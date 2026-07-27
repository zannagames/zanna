---
status: active
audience: public
last-verified: 2026-07-26
---

# Input

> Keyboard, mouse, and gamepad input handling for interactive applications.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Input.Key](#zannainputkey)
- [Zanna.Input.Keyboard](#zannainputkeyboard)
- [Zanna.Input.KeyChord](#zannainputkeychord)
- [Zanna.Input.Mouse](#zannainputmouse)
- [Zanna.Input.Pad](#zannainputpad)
- [Zanna.Input.Action](#zannainputaction)
- [Zanna.Input.Manager](#zannainputmanager)

---

## Zanna.Input.Key

Canonical keyboard key-code constants for input APIs.

**Type:** Static constants class

Use `Zanna.Input.Key` anywhere an API expects an integer key code, including
`Keyboard.IsDown`, `Keyboard.WasPressed`, and `Action.BindKey`. `Zanna.Input.Key`
holds the constants; `Zanna.Input.Keyboard` holds the state and query methods.

| Group | Properties |
|---|---|
| Sentinel | `Unknown` (value 0) |
| Letters | `A`-`Z` |
| Digits | `Digit0`-`Digit9` |
| Function keys | `F1`-`F12` |
| Arrows | `Up`, `Down`, `Left`, `Right` |
| Navigation | `Home`, `End`, `PageUp`, `PageDown`, `Insert`, `Delete` |
| Editing | `Backspace`, `Tab`, `Enter`, `Space`, `Escape` |
| Modifiers | `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`, `LeftSuper`, `RightSuper` |
| Punctuation | `Minus`, `Equals`, `LeftBracket`, `RightBracket`, `Backslash`, `Semicolon`, `Quote`, `Grave`, `Comma`, `Period`, `Slash` |
| Numpad | `Numpad0`-`Numpad9`, `NumpadAdd`, `NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadEnter`, `NumpadDecimal` |

### Values

Codes follow the GLFW numbering used by the window backends. Prefer the named
constants — the numbers are listed only for debugging and for reading existing
code that hard-codes them.

#### Letters

| Property | Value | Property | Value | Property | Value |
|----------|-------|----------|-------|----------|-------|
| `A` | 65 | `J` | 74 | `S` | 83 |
| `B` | 66 | `K` | 75 | `T` | 84 |
| `C` | 67 | `L` | 76 | `U` | 85 |
| `D` | 68 | `M` | 77 | `V` | 86 |
| `E` | 69 | `N` | 78 | `W` | 87 |
| `F` | 70 | `O` | 79 | `X` | 88 |
| `G` | 71 | `P` | 80 | `Y` | 89 |
| `H` | 72 | `Q` | 81 | `Z` | 90 |
| `I` | 73 | `R` | 82 |  |  |

#### Digits

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `Digit0` | 48 | `Digit5` | 53 |
| `Digit1` | 49 | `Digit6` | 54 |
| `Digit2` | 50 | `Digit7` | 55 |
| `Digit3` | 51 | `Digit8` | 56 |
| `Digit4` | 52 | `Digit9` | 57 |

#### Function Keys

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `F1` | 290 | `F7` | 296 |
| `F2` | 291 | `F8` | 297 |
| `F3` | 292 | `F9` | 298 |
| `F4` | 293 | `F10` | 299 |
| `F5` | 294 | `F11` | 300 |
| `F6` | 295 | `F12` | 301 |

#### Navigation

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `Up` | 265 | `Home` | 268 |
| `Down` | 264 | `End` | 269 |
| `Left` | 263 | `PageUp` | 266 |
| `Right` | 262 | `PageDown` | 267 |
| `Insert` | 260 | `Delete` | 261 |

#### Editing and Sentinel

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `Unknown` | 0 | `Space` | 32 |
| `Tab` | 258 | `Enter` | 257 |
| `Backspace` | 259 | `Escape` | 256 |

#### Modifiers

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `LeftShift` | 340 | `RightShift` | 344 |
| `LeftControl` | 341 | `RightControl` | 345 |
| `LeftAlt` | 342 | `RightAlt` | 346 |
| `LeftSuper` | 343 | `RightSuper` | 347 |

`LeftSuper` and `RightSuper` identify Command on macOS and the Windows/Super key
on Windows and Linux. For a platform-neutral primary shortcut modifier, test
either Control key or either Super key.

#### Punctuation

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `Minus` | 45 | `Semicolon` | 59 |
| `Equals` | 61 | `Quote` | 39 |
| `LeftBracket` | 91 | `Comma` | 44 |
| `RightBracket` | 93 | `Period` | 46 |
| `Backslash` | 92 | `Slash` | 47 |
| `Grave` | 96 |  |  |

#### Numpad

| Property | Value | Property | Value |
|----------|-------|----------|-------|
| `Numpad0` | 320 | `Numpad6` | 326 |
| `Numpad1` | 321 | `Numpad7` | 327 |
| `Numpad2` | 322 | `Numpad8` | 328 |
| `Numpad3` | 323 | `Numpad9` | 329 |
| `Numpad4` | 324 | `NumpadAdd` | 334 |
| `Numpad5` | 325 | `NumpadSubtract` | 333 |
| `NumpadDecimal` | 330 | `NumpadMultiply` | 332 |
| `NumpadEnter` | 335 | `NumpadDivide` | 331 |

---

## Zanna.Input.Keyboard

Comprehensive keyboard input handling for games and interactive applications.

**Type:** Static utility class

The Keyboard class provides two complementary input models:

- **Polling**: Check the current state of any key at any time
- **Event-based**: Query which keys were pressed or released since the last frame

Keyboard state is updated by the active window event poll (`Canvas.Poll()`,
`Canvas3D.Poll()`, or GUI application polling).

### Polling Methods (Current State)

| Method        | Signature           | Description                                                |
|---------------|---------------------|------------------------------------------------------------|
| `AnyDown()`   | `Boolean()`         | Returns true if any key is currently pressed               |
| `GetDown()`   | `Integer()`         | Returns the key code of the first pressed key, or 0        |
| `IsDown(key)` | `Boolean(Integer)`  | Returns true if the specified key is currently held down   |
| `IsUp(key)`   | `Boolean(Integer)`  | Returns true if the specified key is currently released    |

### Event Methods (Since Last Poll)

| Method              | Signature          | Description                                          |
|---------------------|--------------------|------------------------------------------------------|
| `GetPressed()`      | `Seq()`            | Returns boxed integer key codes pressed this frame   |
| `GetReleased()`     | `Seq()`            | Returns boxed integer key codes released this frame  |
| `WasPressed(key)`   | `Boolean(Integer)` | Returns true if the key was pressed this frame       |
| `WasReleased(key)`  | `Boolean(Integer)` | Returns true if the key was released this frame      |

### Text Input Methods

| Method               | Signature  | Description                                     |
|----------------------|------------|-------------------------------------------------|
| `DisableTextInput()` | `Void()`   | Disable text input mode                         |
| `EnableTextInput()`  | `Void()`   | Enable text input mode (for text fields)        |
| `GetText()`          | `String()` | Returns UTF-8 text received during the last poll|

### Modifier State

There are no dedicated modifier predicates. Test the left and right key codes
with `IsDown` and combine them:

```zia
var shift = KB.IsDown(Key.LeftShift) || KB.IsDown(Key.RightShift);
var ctrl  = KB.IsDown(Key.LeftControl) || KB.IsDown(Key.RightControl);
var alt   = KB.IsDown(Key.LeftAlt) || KB.IsDown(Key.RightAlt);
```

Caps Lock state is not exposed; `GetText()` already reflects the active layout
and shift/caps state for text entry.

### Helper Methods

| Method         | Signature          | Description                                        |
|----------------|--------------------|----------------------------------------------------|
| `KeyName(key)` | `String(Integer)`  | Returns human-readable name for a key code         |

### Key Code Constants

Key codes live on [`Zanna.Input.Key`](#zannainputkey). See its
[Values](#values) tables for every constant and its numeric code.

### Zia Example: Basic Game Input

```zia
module GameInput;

bind Zanna.Terminal;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;
bind Zanna.Input.Keyboard as KB;
bind Zanna.Input.Key as Key;

func start() {
    var c = Canvas.New("Game", 800, 600);
    var px = 400;
    var py = 300;

    while !c.get_ShouldClose() {
        c.Poll();

        // Movement using polling (smooth, held keys)
        if KB.IsDown(Key.W) { py = py - 5; }
        if KB.IsDown(Key.S) { py = py + 5; }
        if KB.IsDown(Key.A) { px = px - 5; }
        if KB.IsDown(Key.D) { px = px + 5; }

        // Action on single press
        if KB.WasPressed(Key.Space) { Say("Action!"); }

        // Escape to quit
        if KB.WasPressed(Key.Escape) { return; }

        c.Clear(Color.Rgb(0, 0, 0));
        c.Box(px - 10, py - 10, 20, 20, Color.Rgb(255, 0, 0));
        c.Flip();
    }
}
```

### Example: Basic Game Input

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Game", 800, 600)

DIM playerX AS INTEGER = 400
DIM playerY AS INTEGER = 300
DIM speed AS INTEGER = 5

DO WHILE NOT canvas.ShouldClose
    ' Poll events and update keyboard state
    canvas.Poll()

    ' Movement using polling (smooth, held keys)
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.W) THEN
        playerY = playerY - speed
    END IF
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.S) THEN
        playerY = playerY + speed
    END IF
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.A) THEN
        playerX = playerX - speed
    END IF
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.D) THEN
        playerX = playerX + speed
    END IF

    ' Action using event (single press)
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Space) THEN
        ' Fire weapon or jump - only triggers once per press
        PRINT "Action!"
    END IF

    ' Escape to quit
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Escape) THEN
        EXIT DO
    END IF

    ' Draw
    canvas.Clear(0)
    canvas.Box(playerX - 10, playerY - 10, 20, 20, 16711680)
    canvas.Flip()
LOOP
```

### Example: Displaying Pressed Keys

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Key Test", 400, 300)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Get all keys pressed this frame
    DIM pressed AS Zanna.Collections.Seq
    pressed = Zanna.Input.Keyboard.GetPressed()

    DIM i AS INTEGER
    FOR i = 0 TO Zanna.Collections.Seq.get_Count(pressed) - 1
        DIM key AS INTEGER
        key = Zanna.Core.Box.ToI64(pressed.Get(i))
        PRINT "Pressed: "; Zanna.Input.Keyboard.KeyName(key)
    NEXT i

    canvas.Clear(0)
    canvas.Flip()
LOOP
```

### Example: Text Input Field

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Text Input", 400, 100)

DIM inputText AS STRING = ""

' Enable text input mode
Zanna.Input.Keyboard.EnableTextInput()

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Get typed text
    DIM typed AS STRING
    typed = Zanna.Input.Keyboard.GetText()
    inputText = inputText + typed

    ' Handle backspace
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Backspace) THEN
        IF LEN(inputText) > 0 THEN
            inputText = LEFT$(inputText, LEN(inputText) - 1)
        END IF
    END IF

    ' Submit on Enter
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Enter) THEN
        PRINT "Submitted: "; inputText
        inputText = ""
    END IF

    canvas.Clear(0)
    ' Draw text field (you'd use a text rendering method)
    canvas.Flip()
LOOP

Zanna.Input.Keyboard.DisableTextInput()
```

### Notes

- Keyboard state advances when the active Canvas, Canvas3D, or GUI application polls events
- `WasPressed()` and `WasReleased()` only return true for the poll that receives the event
- Use polling (`IsDown()`) for continuous input like movement
- Use events (`WasPressed()`) for discrete actions like menu selection or jumping
- Key codes are GLFW-compatible values for portability
- `GetPressed()` and `GetReleased()` contain boxed integers; BASIC callers must use
  `Zanna.Core.Box.ToI64()` when reading a `Seq` element
- Text input mode should be enabled for text fields and disabled otherwise. `GetText()` contains
  UTF-8 text only while the mode is enabled, and its per-poll buffer is cleared before the next
  event collection

### Integration with Canvas

The Keyboard class automatically integrates with each windowing frontend. During an event poll:

1. Platform keyboard events are collected
2. Key state arrays are updated
3. Pressed/released event lists are populated
4. Text input buffer is updated

You do not initialize the keyboard separately; creating a Canvas, Canvas3D, or GUI application
provides the event source.

---

## Zanna.Input.KeyChord

Key chord (simultaneous) and combo (sequential) detection for complex input patterns. Supports named chords with
configurable timing windows.

**Type:** Instance class
**Constructor:** `Zanna.Input.KeyChord.New()`

### Properties

| Property | Type    | Access | Description                             |
|----------|---------|--------|-----------------------------------------|
| `Count`  | Integer | Read   | Number of registered chords and combos  |

### Methods

| Method                             | Signature                          | Description                                           |
|------------------------------------|------------------------------------|-------------------------------------------------------|
| `Active(name)`                      | `Boolean(String)`                  | Check if a chord is currently active (all keys held)  |
| `Clear()`                           | `Void()`                           | Remove all registered chords and combos               |
| `Define(name, keys)`                | `Void(String, Seq)`                | Register a named simultaneous chord (1-16 keys)       |
| `DefineCombo(name, keys, frames)`   | `Void(String, Seq, Integer)`       | Register a named sequence (1-16 keys; maximum gap)    |
| `Progress(name)`                    | `Integer(String)`                  | Get combo progress (number of keys matched so far)    |
| `Remove(name)`                      | `Boolean(String)`                  | Remove a named chord or combo; returns true if found  |
| `Triggered(name)`                   | `Boolean(String)`                  | Check if a chord/combo was triggered this frame       |
| `Update()`                          | `Void()`                           | Update detection state (call once per frame)          |

### Notes

- **Chords** detect simultaneous key presses (e.g., Ctrl+Shift+S). `Active` remains true while
  every key is held; `Triggered` is true only when the chord transitions from inactive to active.
- **Combos** detect ordered key presses (e.g., up-up-down-down). `frames` is the maximum gap
  between consecutive matches, not a limit on the entire sequence. Values less than or equal to
  zero select the 15-frame default. Pressing another key that also occurs in the sequence but is
  not the next expected key resets progress.
- Each definition must contain 1-16 boxed integer key codes. Defining an existing name replaces
  its prior chord or combo.
- Call `Update()` once per frame after `Canvas.Poll()` to refresh detection state.
- A completed combo has `Active` and `Triggered` set only until the next `Update()`; its progress
  resets to zero. For a chord, `Progress` is its key count while active and zero otherwise.

### Zia Example

```zia
module KeyChordDemo;

bind Zanna.Input;
bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Input.Key as Key;

func start() {
    var kc = KeyChord.New();

    // Define a chord (Ctrl+S)
    var saveKeys = Seq.New();
    saveKeys.Push(Key.LeftControl);
    saveKeys.Push(Key.S);
    kc.Define("save", saveKeys);

    // Define a combo (sequential keys with frame window)
    var konamiKeys = Seq.New();
    konamiKeys.Push(Key.Up);
    konamiKeys.Push(Key.Up);
    konamiKeys.Push(Key.Down);
    konamiKeys.Push(Key.Down);
    kc.DefineCombo("konami", konamiKeys, 60);

    SayInt(kc.Count);  // 2

    // Check state (no keys pressed headlessly)
    kc.Update();
    SayBool(kc.Active("save"));      // false
    SayBool(kc.Triggered("save"));   // false
    SayInt(kc.Progress("konami"));   // 0

    // Manage chords
    kc.Remove("save");
    SayInt(kc.Count);  // 1
    kc.Clear();
    SayInt(kc.Count);  // 0
}
```

### BASIC Example

```basic
DIM detector AS OBJECT = Zanna.Input.KeyChord.New()
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Key Chords", 400, 300)

' Register Ctrl+S chord
DIM saveKeys AS OBJECT = NEW Zanna.Collections.Seq()
saveKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.LeftControl))
saveKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.S))
detector.Define("save", saveKeys)

' Register Konami code combo (timing window: 30 frames)
DIM konamiKeys AS OBJECT = NEW Zanna.Collections.Seq()
' Up, Up, Down, Down
konamiKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.Up))
konamiKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.Up))
konamiKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.Down))
konamiKeys.Push(Zanna.Core.Box.I64(Zanna.Input.Key.Down))
detector.DefineCombo("konami", konamiKeys, 30)

' In game loop
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    detector.Update()

    IF detector.Triggered("save") THEN
        PRINT "Save chord triggered"
    END IF
    IF detector.Triggered("konami") THEN
        PRINT "Cheat activated!"
    END IF

    ' Show combo progress
    DIM prog AS INTEGER = detector.Progress("konami")
    IF prog > 0 THEN
        PRINT "Konami progress: "; prog; " / 4"
    END IF

    canvas.Flip()
LOOP
```

---

## Zanna.Input.Mouse

Comprehensive mouse input handling for games and interactive applications.

**Type:** Static utility class

The Mouse class provides:

- **Position tracking**: Current position and movement delta
- **Button state polling**: Check if buttons are currently pressed
- **Button events**: Query presses, releases, clicks, and double-clicks since last frame
- **Scroll wheel**: Horizontal and vertical scroll amounts
- **Cursor control**: Show, hide, mark as captured, and position the cursor

Mouse state is updated by the active window event poll. Coordinates use the canvas's logical
coordinate system: the origin is at the top left and positive Y points down.

### Position Methods

| Method      | Signature   | Description                                           |
|-------------|-------------|-------------------------------------------------------|
| `DeltaX()`  | `Integer()` | Horizontal movement since last frame                  |
| `DeltaY()`  | `Integer()` | Vertical movement since last frame                    |
| `DeltaXFloat()` | `Float()`   | Sub-pixel horizontal movement (relative mouse mode)   |
| `DeltaYFloat()` | `Float()`   | Sub-pixel vertical movement (relative mouse mode)     |
| `X()`       | `Integer()` | Current X position relative to the canvas             |
| `Y()`       | `Integer()` | Current Y position relative to the canvas             |

### Button State Methods (Polling)

| Method            | Signature          | Description                                        |
|-------------------|--------------------|----------------------------------------------------|
| `IsDown(button)`  | `Boolean(Integer)` | Returns true if the button is currently held down  |
| `IsUp(button)`    | `Boolean(Integer)` | Returns true if the button is currently released   |

There are no per-button predicates. Pass a
[button constant](#button-constants) instead — `Mouse.IsDown(Mouse.ButtonLeft)`.

### Button Event Methods (Since Last Poll)

| Method                     | Signature          | Description                                           |
|----------------------------|--------------------|-------------------------------------------------------|
| `WasClicked(button)`       | `Boolean(Integer)` | True on release after a press lasting at most 300 ms  |
| `WasDoubleClicked(button)` | `Boolean(Integer)` | True when that click follows one within 400 ms        |
| `WasPressed(button)`       | `Boolean(Integer)` | Returns true if the button was pressed this frame     |
| `WasReleased(button)`      | `Boolean(Integer)` | Returns true if the button was released this frame    |

### Scroll Wheel Methods

| Method      | Signature   | Description                                           |
|-------------|-------------|-------------------------------------------------------|
| `WheelX()`  | `Integer()` | Horizontal scroll sum, truncated toward zero          |
| `WheelY()`  | `Integer()` | Vertical scroll sum, truncated toward zero (+ = up)   |
| `WheelXFloat()` | `Double()`  | Horizontal scroll sum with fractional precision       |
| `WheelYFloat()` | `Double()`  | Vertical scroll sum with fractional precision (+ = up)|

### Cursor Control Methods

| Method           | Signature                 | Description                                         |
|------------------|---------------------------|-----------------------------------------------------|
| `Capture()`      | `Void()`                  | Set the capture flag and hide the cursor             |
| `Hide()`         | `Void()`                  | Hide the cursor                                     |
| `IsCaptured()`   | `Boolean()`               | Return the runtime capture flag                      |
| `IsHidden()`     | `Boolean()`               | Return whether the cursor is hidden                  |
| `Release()`      | `Void()`                  | Clear the capture flag and show the cursor           |
| `SetPosition(x, y)`   | `Void(Integer, Integer)`  | Update the stored position and warp the bound cursor |
| `Show()`         | `Void()`                  | Show the cursor                                     |

`Capture()` is not an operating-system pointer lock or confinement API. By itself it leaves
absolute motion bounded by the desktop/window. Use relative mode for unbounded camera input.

### Relative (Raw) Mouse Mode — FPS Mouse-Look

`SetRelativeMode(true)` requests unbounded mouse motion and implies `Capture()`;
`SetRelativeMode(false)` releases capture. The request is applied by both `Canvas3D.Poll()` and
the 2D `Canvas.Poll()` — whichever poll drives input reconciles the request against its window.
The GUI application poll records the request and hides the cursor but does not engage native
relative input or the center-warp fallback.

While relative mode is engaged, the absolute position freezes and motion is exposed through
`DeltaXFloat()`/`DeltaYFloat()`; `DeltaX()`/`DeltaY()` return rounded integer deltas.

Both canvas polls use Windows raw input, macOS cursor dissociation, or Linux XInput2 when
available. If native relative input cannot be enabled, they fall back to warping to the window
center, with integer precision. `RelativeModeNative` distinguishes the native path from the
fallback.

| Method / Property         | Signature        | Description                                       |
|---------------------------|------------------|---------------------------------------------------|
| `SetRelativeMode(on)`     | `Void(Boolean)`  | Enable/disable relative mode (implies Capture)    |
| `RelativeMode`            | `Boolean`        | True while relative mode is requested             |
| `RelativeModeNative`      | `Boolean`        | True when native raw deltas are active            |

```zia
module RelativeMouseRequest;

bind Zanna.Input.Mouse as Mouse;

func start() {
    Mouse.SetRelativeMode(true);  // request mouse-look
    // Each frame after Canvas.Poll() or Canvas3D.Poll():
    var lookX = Mouse.DeltaXFloat();  // sub-pixel, unbounded
    var lookY = Mouse.DeltaYFloat();
    Mouse.SetRelativeMode(false); // back to a normal cursor
}
```

In `Zanna.Game3D`, prefer `Input3D.SetRelativeLook(true)` — it enables the
same mode and feeds `Input3D.LookAxis()`/`MouseDelta()` with the sub-pixel
deltas automatically. Focus loss suspends relative mode (the desktop gets a
normal cursor back); it resumes when the window regains focus.

### Button Constants

| Property        | Value | Description              |
|-----------------|-------|--------------------------|
| `ButtonLeft`   | 0     | Left mouse button        |
| `ButtonMiddle` | 2     | Middle mouse button      |
| `ButtonRight`  | 1     | Right mouse button       |
| `ButtonX1`     | 3     | Extra button 1 (back)    |
| `ButtonX2`     | 4     | Extra button 2 (forward) |

### Zia Example: Mouse Drawing

```zia
module MouseDraw;

bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;
bind Zanna.Input.Mouse as Mouse;

func start() {
    var c = Canvas.New("Draw", 800, 600);
    c.Clear(Color.Rgb(0, 0, 0));

    while !c.get_ShouldClose() {
        c.Poll();

        // Draw while left button held
        if Mouse.IsDown(Mouse.ButtonLeft) {
            c.Disc(Mouse.X(), Mouse.Y(), 5, Color.Rgb(255, 0, 0));
        }

        // Clear on right click
        if Mouse.WasClicked(Mouse.get_ButtonRight()) {
            c.Clear(Color.Rgb(0, 0, 0));
        }

        c.Flip();
    }
}
```

### Example: Drawing with Mouse

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Draw", 800, 600)

DIM drawing AS INTEGER = 0

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Start drawing when left button pressed
    IF Zanna.Input.Mouse.WasPressed(Zanna.Input.Mouse.ButtonLeft) THEN
        drawing = 1
    END IF

    ' Stop drawing when released
    IF Zanna.Input.Mouse.WasReleased(Zanna.Input.Mouse.ButtonLeft) THEN
        drawing = 0
    END IF

    ' Draw at mouse position while button held
    IF drawing = 1 THEN
        DIM mx AS INTEGER = Zanna.Input.Mouse.X()
        DIM my AS INTEGER = Zanna.Input.Mouse.Y()
        canvas.Disc(mx, my, 5, 16711680)
    END IF

    ' Clear on right click
    IF Zanna.Input.Mouse.WasClicked(Zanna.Input.Mouse.ButtonRight) THEN
        canvas.Clear(0)
    END IF

    canvas.Flip()
LOOP
```

### Example: Drag and Drop

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Drag Box", 800, 600)

DIM boxX AS INTEGER = 350
DIM boxY AS INTEGER = 250
DIM boxW AS INTEGER = 100
DIM boxH AS INTEGER = 100
DIM dragging AS INTEGER = 0
DIM offsetX AS INTEGER = 0
DIM offsetY AS INTEGER = 0

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    DIM mx AS INTEGER = Zanna.Input.Mouse.X()
    DIM my AS INTEGER = Zanna.Input.Mouse.Y()

    ' Check if mouse is over the box
    DIM overBox AS INTEGER = 0
    IF mx >= boxX AND mx < boxX + boxW THEN
        IF my >= boxY AND my < boxY + boxH THEN
            overBox = 1
        END IF
    END IF

    ' Start dragging on press
    IF Zanna.Input.Mouse.WasPressed(Zanna.Input.Mouse.ButtonLeft) THEN
        IF overBox = 1 THEN
            dragging = 1
            offsetX = mx - boxX
            offsetY = my - boxY
        END IF
    END IF

    ' Stop dragging on release
    IF Zanna.Input.Mouse.WasReleased(Zanna.Input.Mouse.ButtonLeft) THEN
        dragging = 0
    END IF

    ' Update position while dragging
    IF dragging = 1 THEN
        boxX = mx - offsetX
        boxY = my - offsetY
    END IF

    ' Draw
    canvas.Clear(2236962)

    ' Draw box (highlight if hovering or dragging)
    DIM color AS INTEGER = 4473924
    IF overBox = 1 OR dragging = 1 THEN
        color = 6710886
    END IF
    canvas.Box(boxX, boxY, boxW, boxH, color)
    canvas.Frame(boxX, boxY, boxW, boxH, 16777215)

    canvas.Flip()
LOOP
```

### Example: First-Person Camera Input

```basic
DIM canvas AS Zanna.Graphics3D.Canvas3D
canvas = Zanna.Graphics3D.Canvas3D.New("FPS Camera", 800, 600)

DIM cameraYaw AS DOUBLE = 0.0
DIM cameraPitch AS DOUBLE = 0.0
DIM sensitivity AS DOUBLE = 0.002

' Canvas3D.Poll applies native relative mode or its center-warp fallback
Zanna.Input.Mouse.SetRelativeMode(TRUE)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Use mouse delta for camera rotation
    DIM dx AS DOUBLE = Zanna.Input.Mouse.DeltaXFloat()
    DIM dy AS DOUBLE = Zanna.Input.Mouse.DeltaYFloat()

    cameraYaw = cameraYaw + dx * sensitivity
    cameraPitch = cameraPitch - dy * sensitivity

    ' Clamp pitch
    IF cameraPitch > 1.55 THEN cameraPitch = 1.55
    IF cameraPitch < -1.55 THEN cameraPitch = -1.55

    ' Escape to release cursor and exit
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Escape) THEN
        Zanna.Input.Mouse.SetRelativeMode(FALSE)
        EXIT DO
    END IF

    canvas.Clear(0.0, 0.0, 0.0)
    ' Render the scene using cameraYaw and cameraPitch here
    canvas.Flip()
LOOP
```

### Example: Scroll Zoom

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Zoom", 800, 600)

DIM zoom AS INTEGER = 100  ' percentage

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Scroll wheel to zoom
    DIM scroll AS INTEGER = Zanna.Input.Mouse.WheelY()
    zoom = zoom + scroll * 10

    ' Clamp zoom level
    IF zoom < 10 THEN zoom = 10
    IF zoom > 500 THEN zoom = 500

    canvas.Clear(0)

    ' Draw something at zoom level
    DIM size AS INTEGER = zoom \ 2
    DIM cx AS INTEGER = 400 - size \ 2
    DIM cy AS INTEGER = 300 - size \ 2
    canvas.Box(cx, cy, size, size, 65280)

    canvas.Flip()
LOOP
```

### Notes

- Mouse state advances when the active Canvas, Canvas3D, or GUI application polls events
- `WasPressed()`, `WasReleased()`, `WasClicked()`, and `WasDoubleClicked()` are per-poll flags
- Delta and wheel values are reset at the start of each poll
- A click is a press/release of at most 300 ms; a second click within 400 ms also sets the
  double-click flag
- Use `Left()`, `Right()`, `Middle()` as shortcuts for common button checks
- `Capture()` only hides and records state; Canvas3D relative mode provides FPS-style motion
- **Known issue:** in ordinary absolute mode, `DeltaX()`/`DeltaY()` are calculated before current
  events are pumped and therefore trail `X()`/`Y()` by one poll (see
  [VDOC-006](../../misc/reviews/documentation-review-findings.md#vdoc-006--absolute-mouse-deltas-lag-one-poll-behind))

### Integration with Canvas

The Mouse class automatically integrates with each windowing frontend. During an event poll:

1. Per-poll edge, click, delta, and wheel state is reset
2. Platform events update position, buttons, click detection, and scroll accumulators
3. A Canvas3D poll applies requested native or fallback relative motion

You do not initialize the mouse separately; creating a Canvas, Canvas3D, or GUI application
provides the event source.

---

## Zanna.Input.Pad

Gamepad/controller input handling for games and interactive applications.

**Type:** Static utility class

The Pad class maps supported controllers to one standard logical layout:

- **Controller enumeration**: Four fixed slots with indices 0-3
- **Button state polling**: Check if buttons are currently pressed
- **Button events**: Query presses and releases since last frame
- **Analog sticks**: Left and right stick X/Y axes (-1.0 to 1.0)
- **Triggers**: Left and right trigger values (0.0 to 1.0)
- **Deadzone handling**: Configurable stick deadzone
- **Vibration/rumble**: Control haptic feedback motors

Gamepad state is updated by Canvas or Canvas3D polling.

### Controller Enumeration Methods

| Method               | Signature          | Description                                     |
|----------------------|--------------------|------------------------------------------------------|
| `Count()`            | `Integer()`        | Number of connected slots (0-4)                 |
| `IsConnected(index)` | `Boolean(Integer)` | Returns true if controller is connected         |
| `Name(index)`        | `String(Integer)`  | Controller name/description (empty if invalid)  |

### Button State Methods (Polling)

| Method                  | Signature                   | Description                                        |
|-------------------------|-----------------------------|-----------------------------------------------------|
| `IsDown(index, button)` | `Boolean(Integer, Integer)` | Returns true if the button is currently held down  |
| `IsUp(index, button)`   | `Boolean(Integer, Integer)` | Returns true if the button is currently released   |

### Button Event Methods (Since Last Poll)

| Method                       | Signature                   | Description                                        |
|------------------------------|-----------------------------|----------------------------------------------------|
| `WasPressed(index, button)`  | `Boolean(Integer, Integer)` | Returns true if button was pressed this frame      |
| `WasReleased(index, button)` | `Boolean(Integer, Integer)` | Returns true if button was released this frame     |

### Analog Input Methods

| Method                | Signature         | Description                                            |
|-----------------------|-------------------|--------------------------------------------------------|
| `LeftTrigger(index)`  | `Double(Integer)` | Left trigger (0.0 to 1.0, released to fully pressed)   |
| `LeftX(index)`        | `Double(Integer)` | Left stick X axis (-1.0 to 1.0, left to right)         |
| `LeftY(index)`        | `Double(Integer)` | Left stick Y axis (-1.0 to 1.0, up to down)            |
| `RightTrigger(index)` | `Double(Integer)` | Right trigger (0.0 to 1.0, released to fully pressed)  |
| `RightX(index)`       | `Double(Integer)` | Right stick X axis (-1.0 to 1.0, left to right)        |
| `RightY(index)`       | `Double(Integer)` | Right stick Y axis (-1.0 to 1.0, up to down)           |

### Deadzone Methods

| Method                | Signature       | Description                                      |
|-----------------------|-----------------|--------------------------------------------------|
| `GetDeadzone()`       | `Double()`      | Get current deadzone radius (default 0.1)        |
| `SetDeadzone(radius)` | `Void(Double)`  | Set stick deadzone radius (0.0 to 1.0)           |

The runtime clamps the configured radius to `[0.0, 1.0]`. It applies a radial deadzone to each
stick: values inside the radius become zero and values outside it are rescaled over the remaining
range. Triggers are clamped to `[0.0, 1.0]` but do not use this deadzone.

### Vibration Methods

| Method                        | Signature                       | Description                              |
|-------------------------------|---------------------------------|------------------------------------------|
| `StopVibration(index)`        | `Void(Integer)`                 | Stop vibration on controller             |
| `Vibrate(index, left, right)` | `Void(Integer, Double, Double)` | Set motor intensities (0.0 to 1.0)       |

Finite vibration values are clamped to `[0.0, 1.0]`. Unsupported devices/platforms ignore the
request.

### Button Constants

Standard gamepad layout compatible with Xbox and PlayStation controllers:

| Property        | Value | Xbox          | PlayStation    |
|-----------------|-------|---------------|----------------|
| `ButtonA`         | 0     | A             | Cross (X)      |
| `ButtonB`         | 1     | B             | Circle (O)     |
| `ButtonX`         | 2     | X             | Square         |
| `ButtonY`         | 3     | Y             | Triangle       |
| `ButtonLeftBumper`        | 4     | Left Bumper   | L1             |
| `ButtonRightBumper`        | 5     | Right Bumper  | R1             |
| `ButtonBack`      | 6     | Back/View     | Share          |
| `ButtonStart`     | 7     | Start/Menu    | Options        |
| `ButtonLeftStick`    | 8     | Left Stick    | L3             |
| `ButtonRightStick`    | 9     | Right Stick   | R3             |
| `ButtonUp`        | 10    | D-pad Up      | D-pad Up       |
| `ButtonDown`      | 11    | D-pad Down    | D-pad Down     |
| `ButtonLeft`      | 12    | D-pad Left    | D-pad Left     |
| `ButtonRight`     | 13    | D-pad Right   | D-pad Right    |
| `ButtonGuide`     | 14    | Xbox Button   | PS Button      |

### Zia Example: Controller Movement

```zia
module PadDemo;

bind Zanna.Terminal;
bind Zanna.Input.Pad as Pad;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var c = Canvas.New("Controller", 800, 600);
    var px = 400.0;
    var py = 300.0;

    while !c.get_ShouldClose() {
        c.Poll();

        if Pad.IsConnected(0) {
            // Left stick movement
            px = px + Pad.LeftX(0) * 5.0;
            py = py + Pad.LeftY(0) * 5.0;

            if Pad.WasPressed(0, Pad.get_ButtonA()) { Say("Jump!"); }

            // Trigger for shooting
            if Pad.RightTrigger(0) > 0.5 { Say("Shooting!"); }
        }

        c.Clear(Color.Rgb(0, 0, 0));
        c.Flip();
    }
}
```

### Example: Basic Controller Movement

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Controller Demo", 800, 600)

DIM playerX AS DOUBLE = 400.0
DIM playerY AS DOUBLE = 300.0
DIM speed AS DOUBLE = 5.0

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Check if controller 0 is connected
    IF Zanna.Input.Pad.IsConnected(0) THEN
        ' Movement with left stick
        DIM lx AS DOUBLE = Zanna.Input.Pad.LeftX(0)
        DIM ly AS DOUBLE = Zanna.Input.Pad.LeftY(0)

        playerX = playerX + lx * speed
        playerY = playerY + ly * speed

        ' Action on A button press
        IF Zanna.Input.Pad.WasPressed(0, Zanna.Input.Pad.ButtonA) THEN
            PRINT "Jump!"
        END IF

        ' Shoot while holding right trigger
        DIM rt AS DOUBLE = Zanna.Input.Pad.RightTrigger(0)
        IF rt > 0.5 THEN
            PRINT "Shooting! Power: "; rt
        END IF
    END IF

    canvas.Clear(0)
    canvas.Disc(INT(playerX), INT(playerY), 20, 16711680)
    canvas.Flip()
LOOP
```

### Example: Controller Vibration

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Rumble Test", 400, 300)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    IF Zanna.Input.Pad.IsConnected(0) THEN
        ' Strong rumble on left bumper
        IF Zanna.Input.Pad.WasPressed(0, Zanna.Input.Pad.ButtonLeftBumper) THEN
            Zanna.Input.Pad.Vibrate(0, 1.0, 0.3)
        END IF

        ' Light rumble on right bumper
        IF Zanna.Input.Pad.WasPressed(0, Zanna.Input.Pad.ButtonRightBumper) THEN
            Zanna.Input.Pad.Vibrate(0, 0.3, 1.0)
        END IF

        ' Stop on B button
        IF Zanna.Input.Pad.WasPressed(0, Zanna.Input.Pad.ButtonB) THEN
            Zanna.Input.Pad.StopVibration(0)
        END IF
    END IF

    canvas.Clear(0)
    canvas.Flip()
LOOP
```

### Example: Dual Stick Camera Control

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Twin Stick", 800, 600)

DIM posX AS DOUBLE = 400.0
DIM posY AS DOUBLE = 300.0
DIM aimAngle AS DOUBLE = 0.0

' Adjust deadzone for precision
Zanna.Input.Pad.SetDeadzone(0.15)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    IF Zanna.Input.Pad.IsConnected(0) THEN
        ' Movement with left stick
        posX = posX + Zanna.Input.Pad.LeftX(0) * 3.0
        posY = posY + Zanna.Input.Pad.LeftY(0) * 3.0

        ' Aiming with right stick
        DIM rx AS DOUBLE = Zanna.Input.Pad.RightX(0)
        DIM ry AS DOUBLE = Zanna.Input.Pad.RightY(0)
        IF ABS(rx) > 0.1 OR ABS(ry) > 0.1 THEN
            aimAngle = Zanna.Math.Atan2(ry, rx)
        END IF
    END IF

    canvas.Clear(0)

    ' Draw player
    canvas.Disc(INT(posX), INT(posY), 15, 65280)

    ' Draw aim direction
    DIM aimX AS INTEGER = INT(posX + COS(aimAngle) * 30)
    DIM aimY AS INTEGER = INT(posY + SIN(aimAngle) * 30)
    canvas.Line(INT(posX), INT(posY), aimX, aimY, 16711680)

    canvas.Flip()
LOOP
```

### Example: Multi-Controller Support

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Multiplayer", 800, 600)

' Colors for each player
DIM colors(3) AS INTEGER
colors(0) = 16711680  ' Red
colors(1) = 255  ' Blue
colors(2) = 65280  ' Green
colors(3) = 16776960  ' Yellow

DIM x(3) AS DOUBLE
DIM y(3) AS DOUBLE

' Initial positions
x(0) = 200 : y(0) = 300
x(1) = 600 : y(1) = 300
x(2) = 400 : y(2) = 150
x(3) = 400 : y(3) = 450

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    DIM i AS INTEGER
    FOR i = 0 TO 3
        IF Zanna.Input.Pad.IsConnected(i) THEN
            x(i) = x(i) + Zanna.Input.Pad.LeftX(i) * 4.0
            y(i) = y(i) + Zanna.Input.Pad.LeftY(i) * 4.0
        END IF
    NEXT i

    canvas.Clear(2236962)

    ' Draw status text
    PRINT "Controllers: "; Zanna.Input.Pad.Count()

    ' Draw each player
    FOR i = 0 TO 3
        IF Zanna.Input.Pad.IsConnected(i) THEN
            canvas.Disc(INT(x(i)), INT(y(i)), 20, colors(i))
        END IF
    NEXT i

    canvas.Flip()
LOOP
```

### Notes

- Gamepad state advances when Canvas or Canvas3D polls; button edges last for that poll
- The four valid slot indices are 0-3. `Count()` counts connected slots, but slots need not be
  contiguous, so enumerate 0-3 and call `IsConnected()`
- Invalid or disconnected slots return an empty name and zero/false for most queries;
  `IsUp()` returns true
- Raw `Pad` methods do not treat `-1` as “any controller”; that convention belongs to selected
  `Action` and `Manager` button methods
- Vibration intensity may vary by controller model
- macOS rumble requests are ignored

### Platform Support

| Platform | API Used                    | Notes                                              |
|----------|-----------------------------|----------------------------------------------------|
| Windows  | XInput                      | Xbox-compatible controllers; rumble supported             |
| Linux    | evdev (`/dev/input/event*`) | Requires read permission; rumble also needs writable FF support |
| macOS    | IOHIDManager (HID)          | Generic HID gamepads; rumble is a no-op                   |

### Integration with Canvas

The Pad class automatically integrates with Canvas and Canvas3D. During polling:

1. Platform gamepad APIs are queried
2. Controller connection state is updated
3. Button state arrays are updated
4. Analog values are read and deadzone applied
5. Press/release events are detected

You do not initialize gamepads separately.

---

## Zanna.Input.Action

High-level action mapping system for device-agnostic input handling.

**Type:** Static utility class

The Action class provides an abstraction layer over raw input devices. Instead of checking individual keys, mouse buttons, or gamepad buttons, you define named "actions" and bind them to multiple input sources. This enables:

- **Input remapping** without code changes
- **Multi-device support** - query a single action that works across keyboard, mouse, and gamepad
- **Axis actions** for analog input (movement, camera control)
- **Consistent state tracking** - pressed, released, and held states

Action state is updated automatically after Canvas or Canvas3D polling.

### Action System Lifecycle

| Method        | Signature | Description                                     |
|---------------|-----------|------------------------------------------------|
| `Clear()`     | `Void()`  | Remove all defined actions and bindings         |
| `LoadPreset(name)` | `Boolean(String)` | Load a predefined set of actions with standard key bindings |

### Action Definition Methods

| Method             | Signature          | Description                                                |
|--------------------|--------------------|------------------------------------------------------------|
| `Define(name)`     | `Boolean(String)`  | Define a new button action; returns false if already exists|
| `DefineAxis(name)` | `Boolean(String)`  | Define a new axis action; returns false if already exists  |
| `Exists(name)`     | `Boolean(String)`  | Check if an action is defined                              |
| `IsAxis(name)`     | `Boolean(String)`  | Check if an action is an axis action                       |
| `Remove(name)`     | `Boolean(String)`  | Remove an action and all its bindings                      |

### Keyboard Binding Methods

| Method                              | Signature                          | Description                                      |
|-------------------------------------|------------------------------------|--------------------------------------------------|
| `BindKey(action, key)`              | `Boolean(String, Integer)`         | Bind a key to a button action                    |
| `BindKeyAxis(action, key, value)`   | `Boolean(String, Integer, Double)` | Bind a key to an axis action with value          |
| `UnbindKey(action, key)`            | `Boolean(String, Integer)`         | Remove a key binding from an action              |

### Key Chord Binding Methods

| Method                        | Signature                 | Description                                           |
|-------------------------------|---------------------------|-------------------------------------------------------|
| `BindChord(action, keys)`     | `Boolean(String, Seq)`    | Bind a simultaneous 2-8-key chord to a button action  |
| `ChordCount(action)`          | `Integer(String)`         | Get the number of chord bindings for an action        |
| `UnbindChord(action, keys)`   | `Boolean(String, Seq)`    | Remove an exact ordered chord definition              |

Action chords are simultaneous only; use `KeyChord` when you need a sequential combo. Detection
does not care which order chord keys are pressed, but `UnbindChord()` matches the same keys in the
same sequence order supplied to `BindChord()`.

### Mouse Binding Methods

| Method                             | Signature                 | Description                                      |
|------------------------------------|---------------------------|--------------------------------------------------|
| `BindMouse(action, button)`        | `Boolean(String, Integer)` | Bind a mouse button to a button action          |
| `BindMouseX(action, sensitivity)`  | `Boolean(String, Double)`  | Bind mouse X delta to an axis action            |
| `BindMouseY(action, sensitivity)`  | `Boolean(String, Double)`  | Bind mouse Y delta to an axis action            |
| `BindScrollX(action, sensitivity)` | `Boolean(String, Double)`  | Bind scroll wheel X to an axis action           |
| `BindScrollY(action, sensitivity)` | `Boolean(String, Double)`  | Bind scroll wheel Y to an axis action           |
| `UnbindMouse(action, button)`      | `Boolean(String, Integer)` | Remove a mouse button binding                   |

### Gamepad Binding Methods

| Method                                          | Signature                                   | Description                                      |
|-------------------------------------------------|---------------------------------------------|--------------------------------------------------|
| `BindPadAxis(action, pad, axis, scale)`         | `Boolean(String, Integer, Integer, Double)` | Bind a gamepad axis to an axis action            |
| `BindPadButton(action, pad, button)`            | `Boolean(String, Integer, Integer)`         | Bind a gamepad button to a button action         |
| `BindPadButtonAxis(action, pad, button, value)` | `Boolean(String, Integer, Integer, Double)` | Bind a gamepad button to an axis action          |
| `UnbindPadAxis(action, pad, axis)`              | `Boolean(String, Integer, Integer)`         | Remove a gamepad axis binding                    |
| `UnbindPadButton(action, pad, button)`          | `Boolean(String, Integer, Integer)`         | Remove a gamepad button binding                  |

**Note:** Use pad index `-1` to match any connected controller. An any-pad axis binding uses the
first nonzero value found in slot order; it does not sum the same axis across controllers.

### Button Action Query Methods

| Method             | Signature          | Description                                                  |
|--------------------|--------------------|------------------------------------------------------------- |
| `Held(action)`     | `Boolean(String)`  | Returns true if any bound input is currently held            |
| `Pressed(action)`  | `Boolean(String)`  | Returns true if any bound input was pressed this frame       |
| `Released(action)` | `Boolean(String)`  | Returns true if any bound input was released this frame      |
| `Strength(action)` | `Double(String)`   | Returns 1.0 if held, 0.0 otherwise                           |

### Axis Action Query Methods

| Method             | Signature         | Description                                                   |
|--------------------|-------------------|---------------------------------------------------------------|
| `Axis(action)`     | `Double(String)`  | Returns combined axis value, clamped to -1.0 to 1.0           |
| `AxisRaw(action)`  | `Double(String)`  | Returns combined axis value, not clamped                      |

### Introspection Methods

| Method                  | Signature          | Description                                              |
|-------------------------|--------------------|---------------------------------------------------------|
| `BindingCount(action)`  | `Integer(String)`  | Returns the number of bindings for an action             |
| `BindingsStr(action)`   | `String(String)`   | Returns human-readable description of bindings           |
| `List()`                | `Seq()`            | Returns action-name strings in newest-defined-first order|

### Conflict Detection Methods

| Method                          | Signature                  | Description                                    |
|---------------------------------|----------------------------|------------------------------------------------|
| `KeyBoundTo(key)`               | `String(Integer)`          | Returns action name if key is bound, else ""   |
| `MouseBoundTo(button)`          | `String(Integer)`          | Returns action name if button is bound, else ""|
| `PadButtonBoundTo(pad, button)` | `String(Integer, Integer)` | Returns action name if bound, else ""          |

### Persistence Methods

| Method        | Signature          | Description                                                           |
|---------------|--------------------|-----------------------------------------------------------------------|
| `Load(json)`  | `Boolean(String)`  | Atomically replace all actions from JSON; false preserves current state |
| `Save()`      | `String()`         | Serialize all actions and bindings to a JSON string                   |

### Action Presets

`LoadPreset()` loads a predefined action set and returns false for an unknown preset name. Existing
actions are retained; compatible preset bindings are added to them instead of replacing them.

#### Available Presets

| Preset Name | Actions Defined | Principal Bindings |
|-------------|-----------------|--------------------|
| `"standard_movement"` | Buttons `move_up/down/left/right`; axes `move_x/y` | WASD, arrows, D-pad, left stick |
| `"menu_navigation"` | `menu_up/down/left/right`, `confirm`, `back` | WASD/arrows/D-pad; Enter or Space/A; Escape/B |
| `"platformer"` | `move_left/right`, `jump`, `shoot`, `pause`; axis `move_x` | A/D or arrows/D-pad/stick; Space/W/Up or A; J/X or pad X; Escape/Start |
| `"topdown"` | `move_up/down/left/right`, `fire`, `pause`; axes `move_x/y` | WASD/arrows/D-pad/stick; Space/J or A; Escape/Start |
| `"fps3d"` | `jump`, `sprint`, `crouch`, `interact`, `fire`, `aim`, `pause`; axes `move_x/y` | Space/Shift/Ctrl/E, mouse buttons, WASD; A/LB/B/X/Start, left stick |

#### Example
```zia
module PresetDemo;

bind Zanna.Input.Action as Action;
bind Zanna.Input.Key as Key;

func start() {
    Action.Clear();
    Action.LoadPreset("platformer");
    Action.LoadPreset("menu_navigation");

    // Compatible preset bindings are added to existing actions.
    Action.Define("special_attack");
    Action.BindKey("special_attack", Key.Q);
    Action.Clear();
}
```

### Axis Constants

| Property            | Value | Description                     |
|---------------------|-------|---------------------------------|
| `AxisLeftX`       | 0     | Left stick horizontal           |
| `AxisLeftY`       | 1     | Left stick vertical             |
| `AxisRightX`      | 2     | Right stick horizontal          |
| `AxisRightY`      | 3     | Right stick vertical            |
| `AxisLeftTrigger` | 4     | Left trigger (0.0 to 1.0)       |
| `AxisRightTrigger`| 5     | Right trigger (0.0 to 1.0)      |

### Zia Example: Action Mapping

```zia
module ActionDemo;

bind Zanna.Terminal;
bind Zanna.Input.Action as Action;
bind Zanna.Input.Key as Key;
bind Zanna.Input.Pad as Pad;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var c = Canvas.New("Action Demo", 800, 600);

    // Define actions
    Action.Define("jump");
    Action.Define("fire");
    Action.DefineAxis("move_x");
    Action.DefineAxis("move_y");

    // Bind keyboard
    Action.BindKey("jump", Key.Space);
    Action.BindKey("fire", Key.Z);
    Action.BindKeyAxis("move_x", Key.Left, -1.0);
    Action.BindKeyAxis("move_x", Key.Right, 1.0);
    Action.BindKeyAxis("move_y", Key.Up, -1.0);
    Action.BindKeyAxis("move_y", Key.Down, 1.0);

    // Bind gamepad (any controller)
    Action.BindPadButton("jump", -1, Pad.get_ButtonA());
    Action.BindPadButton("fire", -1, Pad.get_ButtonX());

    Say("Jump bindings: " + Action.BindingsStr("jump"));

    var px = 400.0;
    var py = 300.0;

    while !c.get_ShouldClose() {
        c.Poll();

        // Device-agnostic movement
        px = px + Action.Axis("move_x") * 5.0;
        py = py + Action.Axis("move_y") * 5.0;

        if Action.Pressed("jump") { Say("Jump!"); }
        if Action.Held("fire") { Say("Firing"); }

        c.Clear(Color.Rgb(0, 0, 0));
        c.Flip();
    }

    Action.Clear();
}
```

### Example: Basic Game Actions

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Action Demo", 800, 600)

' Define actions
Zanna.Input.Action.Define("jump")
Zanna.Input.Action.Define("fire")
Zanna.Input.Action.DefineAxis("move_x")
Zanna.Input.Action.DefineAxis("move_y")

' Bind keyboard
Zanna.Input.Action.BindKey("jump", Zanna.Input.Key.Space)
Zanna.Input.Action.BindKey("fire", Zanna.Input.Key.Z)
Zanna.Input.Action.BindKeyAxis("move_x", Zanna.Input.Key.Left, -1.0)
Zanna.Input.Action.BindKeyAxis("move_x", Zanna.Input.Key.Right, 1.0)
Zanna.Input.Action.BindKeyAxis("move_y", Zanna.Input.Key.Up, -1.0)
Zanna.Input.Action.BindKeyAxis("move_y", Zanna.Input.Key.Down, 1.0)

' Bind gamepad (any controller)
Zanna.Input.Action.BindPadButton("jump", -1, Zanna.Input.Pad.ButtonA)
Zanna.Input.Action.BindPadButton("fire", -1, Zanna.Input.Pad.ButtonX)
Zanna.Input.Action.BindPadAxis("move_x", -1, Zanna.Input.Action.AxisLeftX, 1.0)
Zanna.Input.Action.BindPadAxis("move_y", -1, Zanna.Input.Action.AxisLeftY, 1.0)

DIM playerX AS DOUBLE = 400.0
DIM playerY AS DOUBLE = 300.0
DIM speed AS DOUBLE = 5.0

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Movement using axis actions (works with keyboard OR gamepad)
    playerX = playerX + Zanna.Input.Action.Axis("move_x") * speed
    playerY = playerY + Zanna.Input.Action.Axis("move_y") * speed

    ' Jump action (works with Space OR gamepad A)
    IF Zanna.Input.Action.Pressed("jump") THEN
        PRINT "Jump!"
    END IF

    ' Fire action (works with Z OR gamepad X)
    IF Zanna.Input.Action.Held("fire") THEN
        PRINT "Firing"
    END IF

    canvas.Clear(0)
    canvas.Disc(INT(playerX), INT(playerY), 20, 16711680)
    canvas.Flip()
LOOP
```

### Example: Mouse Look with Action Mapping

```basic
DIM canvas AS Zanna.Graphics3D.Canvas3D
canvas = Zanna.Graphics3D.Canvas3D.New("FPS Camera", 800, 600)

' Define look actions
Zanna.Input.Action.DefineAxis("look_x")
Zanna.Input.Action.DefineAxis("look_y")

' Bind mouse delta with sensitivity
Zanna.Input.Action.BindMouseX("look_x", 0.002)
Zanna.Input.Action.BindMouseY("look_y", 0.002)

' Also allow gamepad right stick
Zanna.Input.Action.BindPadAxis("look_x", -1, Zanna.Input.Action.AxisRightX, 0.05)
Zanna.Input.Action.BindPadAxis("look_y", -1, Zanna.Input.Action.AxisRightY, 0.05)

DIM yaw AS DOUBLE = 0.0
DIM pitch AS DOUBLE = 0.0

' Relative mode is applied by Canvas3D.Poll()
Zanna.Input.Mouse.SetRelativeMode(TRUE)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    yaw = yaw + Zanna.Input.Action.Axis("look_x")
    pitch = pitch + Zanna.Input.Action.Axis("look_y")

    ' Clamp pitch
    IF pitch > 1.57 THEN pitch = 1.57
    IF pitch < -1.57 THEN pitch = -1.57

    canvas.Clear(0.0, 0.0, 0.0)
    ' Render the scene using yaw and pitch here
    canvas.Flip()
LOOP

Zanna.Input.Mouse.SetRelativeMode(FALSE)
```

### Example: Rebindable Controls

```basic
' Query current bindings
DIM jumpBindings AS STRING
jumpBindings = Zanna.Input.Action.BindingsStr("jump")
PRINT "Jump is bound to: "; jumpBindings

' Check for conflicts
DIM conflict AS STRING
conflict = Zanna.Input.Action.KeyBoundTo(Zanna.Input.Key.Space)
IF conflict <> "" THEN
    PRINT "Space is already bound to: "; conflict
END IF

' Rebind at runtime
Zanna.Input.Action.UnbindKey("jump", Zanna.Input.Key.Space)
Zanna.Input.Action.BindKey("jump", Zanna.Input.Key.W)
```

### Notes

- Action state is updated after Canvas or Canvas3D polling
- `Pressed()`/`Released()` report any bound input edge received during that poll
- Multiple bindings on a button action trigger if ANY binding is active
- Axis bindings are combined (summed) and clamped to -1.0 to 1.0
- Use `AxisRaw()` to get the unclamped sum for advanced use cases
- Mouse-axis bindings use rounded integer mouse deltas; scroll-axis bindings preserve fractional
  wheel input
- Binding the same input to multiple actions is allowed
- Use pad index `-1` on Action gamepad bindings for “any controller”

### Design Philosophy

The Action system follows the principle of "define once, query everywhere":

1. **Define** your game's actions at startup (jump, fire, move_x, etc.)
2. **Bind** multiple inputs to each action (keyboard + mouse + gamepad)
3. **Query** actions in your game logic, not raw inputs

This separation means:
- Players can rebind controls without code changes
- Supporting new input devices only requires adding bindings
- Game logic remains clean and device-agnostic

---

## Zanna.Input.Manager

High-level input wrapper with a per-key edge gate and unified direction input.

**Type:** Instance class (requires `New()`)

Unlike the low-level Keyboard, Mouse, and Pad classes that require checking specific keys/buttons, InputManager provides
unified "direction" input that automatically combines keyboard arrows, WASD, D-pad, and analog sticks.

### Constructor

| Method  | Signature        | Description                      |
|---------|------------------|----------------------------------|
| `New()` | `InputManager()` | Create a new input manager       |

### Properties

| Property       | Type    | Description                                    |
|----------------|---------|------------------------------------------------|
| `DebounceDelay`| Integer | Edge-gate timer in frames (r/w; default 12)    |

### Core Methods

| Method     | Signature | Description                                                      |
|------------|-----------|------------------------------------------------------------------|
| `Update()` | `Void()`  | Decrement edge-gate timers; call after the window poll          |

### Unified Direction Properties

These properties check ALL input sources (keyboard, D-pad, analog sticks) and return true if ANY is active:

| Property  | Type    | Access | Description                                              |
|-----------|---------|--------|----------------------------------------------------------|
| `Up`      | Boolean | Read   | Held: Arrow Up, W, D-pad Up, or left stick below -0.5    |
| `Down`    | Boolean | Read   | Held: Arrow Down, S, D-pad Down, or left stick above 0.5 |
| `Left`    | Boolean | Read   | Held: Arrow Left, A, D-pad Left, or left stick below -0.5|
| `Right`   | Boolean | Read   | Held: Arrow Right, D, D-pad Right, or left stick above 0.5|
| `Confirm` | Boolean | Read   | Press edge: Enter, Space, or gamepad A                    |
| `Cancel`  | Boolean | Read   | Press edge: Escape or gamepad B                           |
| `AxisX`   | Double  | Read   | Unified horizontal axis, clamped to -1.0 through 1.0     |
| `AxisY`   | Double  | Read   | Unified vertical axis, clamped to -1.0 through 1.0       |

The direction properties are level-triggered and therefore remain true every poll while held.
`AxisX`/`AxisY` combine keyboard, D-pad, and all four left sticks by selection rather than by
summing their values. Digital input can force `-1.0` or `1.0`; vertical negative is up.

### Keyboard Methods

| Method                     | Signature           | Description                                          |
|----------------------------|---------------------|------------------------------------------------------|
| `KeyHeld(key)`             | `Boolean(Integer)`  | True if key is currently held down                   |
| `KeyPressed(key)`          | `Boolean(Integer)`  | True if key was pressed this frame (edge detection)  |
| `KeyPressedDebounced(key)` | `Boolean(Integer)`  | Accept a down edge only while that key's timer is zero|
| `KeyReleased(key)`         | `Boolean(Integer)`  | True if key was released this frame                  |

### Mouse Methods

| Method                  | Signature           | Description                                     |
|-------------------------|---------------------|-------------------------------------------------|
| `MouseHeld(button)`     | `Boolean(Integer)`  | True if button is currently held                |
| `MousePressed(button)`  | `Boolean(Integer)`  | True if button was pressed this frame           |
| `MouseReleased(button)` | `Boolean(Integer)`  | True if button was released this frame          |

### Mouse Properties

| Property     | Type    | Access | Description                              |
|--------------|---------|--------|------------------------------------------|
| `MouseX`     | Integer | Read   | Current mouse X position                 |
| `MouseY`     | Integer | Read   | Current mouse Y position                 |
| `MouseDeltaX`| Integer | Read   | Mouse X movement since last frame        |
| `MouseDeltaY`| Integer | Read   | Mouse Y movement since last frame        |
| `ScrollX`    | Integer | Read   | Horizontal scroll delta                  |
| `ScrollY`    | Integer | Read   | Vertical scroll delta                    |
| `ScrollHorizontalFloat` | Double | Read | Horizontal scroll delta with fractional precision |
| `ScrollVerticalFloat`   | Double | Read | Vertical scroll delta with fractional precision   |

### Gamepad Methods

| Method                     | Signature                   | Description                           |
|----------------------------|-----------------------------|---------------------------------------|
| `PadHeld(pad, button)`     | `Boolean(Integer, Integer)` | True if button is currently held      |
| `PadLeftTrigger(pad)`      | `Double(Integer)`           | Left trigger (0.0 to 1.0)             |
| `PadLeftX(pad)`            | `Double(Integer)`           | Left stick X (-1.0 to 1.0)            |
| `PadLeftY(pad)`            | `Double(Integer)`           | Left stick Y (-1.0 to 1.0)            |
| `PadPressed(pad, button)`  | `Boolean(Integer, Integer)` | True if button was pressed this frame |
| `PadReleased(pad, button)` | `Boolean(Integer, Integer)` | True if button was released this frame|
| `PadRightTrigger(pad)`     | `Double(Integer)`           | Right trigger (0.0 to 1.0)            |
| `PadRightX(pad)`           | `Double(Integer)`           | Right stick X (-1.0 to 1.0)           |
| `PadRightY(pad)`           | `Double(Integer)`           | Right stick Y (-1.0 to 1.0)           |

**Note:** `PadPressed`, `PadReleased`, and `PadHeld` accept exactly `-1` for any connected
controller. Axis and trigger methods pass the index directly to `Pad`; `-1` returns zero.

### Zia Example: Menu Navigation

```zia
module MenuDemo;

bind Zanna.Terminal;
bind Zanna.Input.Manager as IM;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var c = Canvas.New("Menu", 800, 600);
    var input = IM.New();
    input.set_DebounceDelay(12);

    var selected = 0;
    var directionReady = true;

    while !c.get_ShouldClose() {
        c.Poll();
        input.Update();

        // Unified directions are held-state properties, so latch one move per press.
        var up = input.get_Up();
        var down = input.get_Down();
        if directionReady && up {
            selected = selected - 1;
            directionReady = false;
        }
        if directionReady && down {
            selected = selected + 1;
            directionReady = false;
        }
        if !up && !down { directionReady = true; }
        if selected < 0 { selected = 3; }
        if selected > 3 { selected = 0; }

        if input.get_Confirm() {
            Say("Selected item: " + Fmt.Int(selected));
        }
        if input.get_Cancel() { return; }

        c.Clear(Color.Rgb(0, 0, 0));
        c.Flip();
    }
}
```

### Example: Menu Navigation

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Menu Demo", 800, 600)

DIM mgr AS OBJECT = Zanna.Input.Manager.New()

DIM selectedItem AS INTEGER = 0
DIM directionReady AS INTEGER = 1
DIM menuItems(3) AS STRING
menuItems(0) = "New Game"
menuItems(1) = "Continue"
menuItems(2) = "Options"
menuItems(3) = "Exit"

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    mgr.Update()

    ' Unified directions are properties and stay true while held.
    IF directionReady = 1 AND mgr.Up THEN
        selectedItem = selectedItem - 1
        IF selectedItem < 0 THEN selectedItem = 3
        directionReady = 0
    END IF

    IF directionReady = 1 AND mgr.Down THEN
        selectedItem = selectedItem + 1
        IF selectedItem > 3 THEN selectedItem = 0
        directionReady = 0
    END IF

    IF NOT mgr.Up AND NOT mgr.Down THEN directionReady = 1

    ' Confirm selection
    IF mgr.Confirm THEN
        SELECT CASE selectedItem
            CASE 0: PRINT "New Game"
            CASE 1: PRINT "Continue"
            CASE 2: PRINT "Options"
            CASE 3: EXIT DO
        END SELECT
    END IF

    ' Back/Cancel
    IF mgr.Cancel THEN
        EXIT DO
    END IF

    ' Render the menu background
    canvas.Clear(0)
    DIM i AS INTEGER
    FOR i = 0 TO 3
        DIM color AS INTEGER = 16777215
        IF i = selectedItem THEN color = 16776960
        ' Draw menuItems(i) at its row here
    NEXT i
    canvas.Flip()
LOOP
```

### Example: Game Movement

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Game", 800, 600)

DIM mgr AS OBJECT = Zanna.Input.Manager.New()

DIM playerX AS DOUBLE = 400.0
DIM playerY AS DOUBLE = 300.0
DIM speed AS DOUBLE = 5.0

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    mgr.Update()

    ' Smooth movement using axis values (works with WASD, arrows, AND analog stick)
    playerX = playerX + mgr.AxisX * speed
    playerY = playerY + mgr.AxisY * speed

    ' Or use digital direction for grid-based movement
    IF mgr.Left THEN playerX = playerX - speed
    IF mgr.Right THEN playerX = playerX + speed
    IF mgr.Up THEN playerY = playerY - speed
    IF mgr.Down THEN playerY = playerY + speed

    canvas.Clear(0)
    canvas.Disc(INT(playerX), INT(playerY), 20, 16711680)
    canvas.Flip()
LOOP
```

### Example: Debounced Key Input

```basic
DIM mgr AS OBJECT = Zanna.Input.Manager.New()
mgr.DebounceDelay = 15
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Input Gate", 400, 300)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    mgr.Update()

    ' Held is level-triggered and fires every poll while held.
    IF mgr.KeyHeld(Zanna.Input.Key.Space) THEN
        PRINT "Fire"
    END IF

    ' This accepts only a down edge while the per-key timer is zero.
    ' Calling it while P is up resets the timer; holding P never repeats.
    IF mgr.KeyPressedDebounced(Zanna.Input.Key.P) THEN
        PRINT "Toggle pause"
    END IF

    canvas.Clear(0)
    canvas.Flip()
LOOP
```

### Notes

- Call `Update()` after each window poll when using `KeyPressedDebounced`; all other members read
  global input state directly
- Negative `DebounceDelay` assignments are ignored. The manager tracks at most 32 key codes and
  reuses the slot with the shortest remaining timer when saturated
- `KeyPressedDebounced` still requires a `WasPressed` down edge; it does not generate held-key
  repeat. Calling it while the key is up clears that key's timer
- Unified directions inspect keyboard, D-pad, and all four gamepad slots automatically
- `AxisX`/`AxisY` select among digital and analog contributions and clamp the result; they do not
  sum all devices
- Use `-1` for any gamepad only with the three Manager button methods
- **Known issue:** the edge gate does not implement the commonly expected repeat-delay behavior;
  see [VDOC-010](../../misc/reviews/documentation-review-findings.md#vdoc-010--inputmanager-debounce-delay-does-not-implement-held-key-repeat)

### InputManager vs Low-Level Classes

| Feature                | InputManager                    | Keyboard/Mouse/Pad            |
|------------------------|---------------------------------|-------------------------------|
| Unified directions     | Yes (Up/Down/Left/Right)        | No (check each device)        |
| Per-key edge gate      | Yes (`KeyPressedDebounced`)     | No                            |
| Device-agnostic axes   | Yes (`AxisX`, `AxisY`)          | No                            |
| Confirm/Cancel actions | Yes (built-in mappings)         | No                            |
| Per-key control        | Yes (falls through to low-level)| Yes                           |
| Input state ownership  | Delegates to global low-level state | Global low-level state    |

### Use Cases

- **Menu navigation:** Unified held directions plus edge-triggered Confirm/Cancel; add a latch or
  repeat policy for one-step selection
- **Character movement:** Combined axis input from all devices
- **Dialog systems:** Per-key edge gating to prevent duplicate consumption
- **Inventory screens:** Device-agnostic selection
- **Quick prototyping:** Less boilerplate than raw input classes

---

## See Also

- [Graphics](graphics/README.md) - `Canvas` class for windowing and rendering that drives input polling
- [Collections](collections/README.md) - `Seq` type returned by `GetPressed()` and `GetReleased()` methods
- [Time](time.md) - `Timer` class for frame-based timing that pairs well with InputManager
