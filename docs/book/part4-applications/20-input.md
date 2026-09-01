---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 20: User Input

Graphics let you show things to users. But showing is only half of interactive software. The other half is listening. When the player presses a key, the character should jump. When they move the mouse, the cursor should follow. When they click a button, something should happen.

Without input handling, your program is like a movie — it plays, but the audience can't participate. With input handling, it becomes a conversation. The user speaks through the keyboard, mouse, and controller. Your program listens and responds.

This chapter teaches you how to hear what users are saying.

---

## Why Input Matters

Think about the programs you use every day. A word processor responds to every keystroke. A web browser follows your mouse as you scroll and click. A video game translates your button mashes into on-screen action. All of these programs share something fundamental: they react to you.

Input handling is what separates passive media from interactive software. A photograph doesn't change when you look at it. A book doesn't rewrite itself based on how fast you turn pages. But software can respond to your every action — if it knows how to listen.

This responsiveness creates engagement. When a character jumps the instant you press the button, the game feels good. When there's a delay, it feels sluggish. When the game ignores your input entirely, it feels broken. Great input handling is invisible; you don't notice it because the program simply does what you expect. Poor input handling is immediately obvious and frustrating.

---

## The Input Mental Model

Before we write any code, let's understand how input actually works. This mental model will help you debug problems and design better interactive programs.

### The Mail Carrier Analogy

Imagine you're waiting for an important letter. You have two ways to find out when it arrives:

**Approach 1: Constantly check the mailbox.** Every few minutes, you walk outside, open the mailbox, and look inside. This is *polling* — you repeatedly ask "is there anything new?" Most of the time the answer is no, but you keep checking anyway.

**Approach 2: Listen for the mail carrier.** You install a doorbell that rings when mail is delivered. You go about your business, and when you hear the bell, you know something arrived. This is *event-driven* input — the system notifies you when something happens.

Both approaches work. Polling is simpler to understand but can waste effort. Event-driven systems are more efficient but require you to set up the notification mechanism.

Most game loops use polling because games update every frame anyway — you're already checking everything 60 times per second, so checking input at the same time costs nothing extra. GUI applications often use events because they might sit idle for seconds or minutes between user actions.

### The Input Pipeline

When you press a key on your keyboard, quite a lot happens before your program can respond:

1. **Physical action**: Your finger pushes down a key
2. **Hardware detection**: The keyboard's internal circuitry detects which key moved
3. **Signal transmission**: The keyboard sends a signal to the computer (via USB, Bluetooth, etc.)
4. **Operating system processing**: The OS receives the signal and determines which key was pressed
5. **Event creation**: The OS creates an "input event" and places it in a queue
6. **Delivery to application**: The OS delivers the event to your program's input queue
7. **Program processing**: Your code reads the event and responds

This happens in milliseconds, fast enough to feel instantaneous. But understanding this pipeline helps you debug problems. If a keypress doesn't work, the issue could be anywhere along this chain — from a stuck key to a bug in your event-handling code.

### The Input Queue

When you press multiple keys quickly, the computer doesn't lose any of them. Instead, it stores them in an *input queue* — a waiting line of events that your program processes one at a time.

Think of it like a restaurant's order tickets. When customers order faster than the kitchen can cook, the tickets pile up in a queue. The kitchen works through them in order. Similarly, your program works through input events in the order they arrived.

This is why games feel responsive even during complex scenes. The input is captured immediately and queued; even if a frame takes a bit longer to render, the input events are waiting patiently to be processed.

---

## Input Devices

Before we write code, let's understand what we're working with. Each input device has its own characteristics and quirks.

### The Keyboard

A keyboard is essentially a grid of buttons. Each key is either *up* (not pressed) or *down* (pressed). When you press a key, it generates a "key down" event. When you release it, it generates a "key up" event. Some keys (like Shift or Ctrl) are "modifier" keys that change the meaning of other keys.

Keyboards also have *key repeat*. If you hold down a key, after a short delay, the computer starts generating repeated "key down" events, as if you were typing the key over and over. This is great for text editing (hold backspace to delete multiple characters) but can be confusing for games (you don't want your character to jump repeatedly just because you're holding the button).

### The Mouse

A mouse reports two kinds of information: *position* and *buttons*.

Position is straightforward — the mouse is at coordinates (x, y) on the screen. These coordinates update continuously as you move the mouse.

Mouse buttons work like keyboard keys — they can be up or down, and they generate press and release events. Most mice have at least left, right, and middle buttons. Many also have a scroll wheel, which generates scroll events when you roll it.

One subtlety: mouse position is in *screen coordinates*, but your game probably uses *canvas coordinates*. If your canvas is smaller than the full window, or if it's scaled, you'll need to convert between them.

### Game Controllers

Game controllers (gamepads) have both digital and analog inputs:

**Digital inputs** (buttons) work like keyboard keys — they're either pressed or not. Controllers typically have face buttons (A, B, X, Y), shoulder buttons (bumpers and triggers), and a D-pad (directional pad with up, down, left, right).

**Analog inputs** (sticks and triggers) report a range of values. An analog stick reports its position as two numbers (x and y), typically ranging from -1.0 to 1.0. A trigger reports how far it's pressed, typically 0.0 to 1.0.

Analog inputs introduce a complication: *dead zones*. When you're not touching an analog stick, it should report (0, 0). But physical sticks rarely rest exactly at center — they might report (0.03, -0.02) even when untouched. Without dead zone handling, your character would slowly drift even when you're not touching the controller.

---

## Event Types

Input events come in several flavors. Understanding the difference is crucial for handling input correctly.

### Press vs. Release Events

A key press event occurs once, at the instant a key goes from up to down. A key release event occurs once, at the instant a key goes from down to up.

This distinction matters enormously:

```zia
bind Keyboard = Zanna.Input.Keyboard;

// This fires continuously while the key is held
if Keyboard.IsDown(Key.Space) {
    // This code runs 60 times per second while space is held!
}

// This fires exactly once when the key is first pressed
if Keyboard.WasPressed(Key.Space) {
    // This code runs once, when space goes from up to down
}
```

For a jump action, you almost always want `WasPressed`. If you use `IsDown`, the character will try to jump every single frame while you hold the button, which is almost never what you want.

### Continuous State vs. Discrete Events

Some actions need the key's current state (is it down right now?). Other actions need discrete events (was it just pressed?).

**Use state checks (`Keyboard.IsDown`) for:**
- Continuous movement: "move right while arrow is held"
- Holding actions: "aim while right-click is held"
- Any action that should continue as long as the button is held

**Use event checks (`Keyboard.WasPressed`) for:**
- One-time actions: "jump", "shoot", "pause"
- Menu navigation: "select next item"
- Any action that should happen once per button press

### Text Input Events

Games typically use key codes (like `KeyA`), but text input is more complex. When the user presses Shift+A, they probably want an uppercase 'A', not separate events for Shift and A. When they're typing in a language that requires an input method editor (like Chinese or Japanese), a single character might require multiple key presses.

For text input (like typing a player's name), Zanna provides text input events that handle all this complexity:

```zia
bind Keyboard = Zanna.Input.Keyboard;

var playerName = "";
Keyboard.EnableTextInput();

var text = Keyboard.GetText();  // Gets the actual characters typed this frame
if text.Length() > 0 {
    playerName += text;
}
```

---

## Tracing a Key Press: What Really Happens

Let's trace exactly what happens when a player presses the Space bar to jump. Understanding this flow will help you debug input problems.

**Frame 0:** The player's finger pushes down the Space bar. The keyboard hardware detects this and sends a signal to the computer.

**Frame 1:** The operating system receives the signal and creates a key-down event for Space. This event goes into your program's input queue. At the start of this frame, Zanna's input system reads all queued events. It notices that Space just went from "up" to "down", so it:
- Sets the internal state of Space to "down"
- Sets a flag indicating Space was "just pressed" this frame

Your game loop runs. When it calls `Keyboard.WasPressed(Key.Space)`, the function returns `true` because the "just pressed" flag is set. Your code calls `player.jump()`, and the character begins rising into the air.

**Frame 2:** No new Space events (the player is still holding the key). At the start of this frame, Zanna clears all the "just pressed" flags from the previous frame. The state of Space is still "down", but it's no longer "just pressed."

Your game loop runs. `Keyboard.WasPressed(Key.Space)` now returns `false` because the flag was cleared. But `Keyboard.IsDown(Key.Space)` still returns `true` because the key is still being held. Your physics code continues the jump — the character rises and then falls.

**Frame 10:** The player releases the Space bar. The OS creates a key-up event. Zanna reads it and:
- Sets the internal state of Space to "up"
- Sets a flag indicating Space was "just released" this frame

**Frame 11:** The "just released" flag is cleared. Space is now fully up with no special flags.

This is why `WasPressed` only returns true for one frame. The "just pressed" state is temporary — it exists only to let you detect the transition from up to down.

---

## Keyboard Input

Now let's write some code. We'll start with the keyboard, the most common input device for games and applications.

### Checking Key State

To check if a key is currently held down, use `Keyboard.IsDown`:

```zia
bind Keyboard = Zanna.Input.Keyboard;

while gameRunning {
    if Keyboard.IsDown(Key.Left) {
        player.x -= speed * dt;
    }
    if Keyboard.IsDown(Key.Right) {
        player.x += speed * dt;
    }
    if Keyboard.IsDown(Key.Up) {
        player.y -= speed * dt;
    }
    if Keyboard.IsDown(Key.Down) {
        player.y += speed * dt;
    }
}
```

Let's trace through this code:

- `Keyboard.IsDown(Key.Left)` returns `true` if the left arrow key is currently pressed, `false` otherwise.
- If it's pressed, we subtract from `player.x`, moving the player leftward.
- We multiply by `speed` (how fast to move) and `dt` (delta time — how many seconds passed since the last frame). This makes movement consistent regardless of frame rate.
- We check each direction independently, so the player can move diagonally by holding two arrows.

Notice we're checking every frame. This is polling in action — we're constantly asking "is this key down?" sixty times per second.

### Detecting Key Presses

For one-time actions, use `Keyboard.WasPressed`:

```zia
bind Keyboard = Zanna.Input.Keyboard;

if Keyboard.WasPressed(Key.Space) {
    player.jump();
}

if Keyboard.WasPressed(Key.Escape) {
    pauseGame();
}

if Keyboard.WasPressed(Key.R) {
    restartLevel();
}
```

Why use `WasPressed` instead of `IsDown`?

- `WasPressed` returns `true` only on the frame the key went from up to down. The next frame, it returns `false` (even if the key is still held).
- `IsDown` returns `true` on every frame while the key is held.

For jumping, you want one jump per button press. If you used `IsDown`, the character would try to jump continuously while you hold the button — usually resulting in a single jump (since you can't jump while in the air), but the code would be wastefully checking every frame.

### Detecting Key Release

Sometimes you need to know when a key is released:

```zia
bind Keyboard = Zanna.Input.Keyboard;

if Keyboard.WasReleased(Key.LeftControl) {
    // Player released the aim button
    releaseArrow();  // Fire the arrow they were aiming
}
```

This is common in "charge and release" mechanics. The player holds a button to charge an attack, then releases to fire.

### Common Key Codes

Zanna provides named constants for all standard keys:

```zia
// Arrow keys
KeyLeft, KeyRight, KeyUp, KeyDown

// Letters (uppercase names, but detect both cases)
KeyA, KeyB, KeyC, ... KeyZ

// Numbers (top row of keyboard)
Key0, Key1, ... Key9

// Numpad numbers
KEY_NUMPAD_0, KEY_NUMPAD_1, ... KEY_NUMPAD_9

// Function keys
KeyF1, KeyF2, ... KeyF12

// Special keys
KeySpace, KeyEnter, KeyEscape
KeyTab, KeyBackspace, KeyDelete
KeyHome, KeyEnd, KeyPageUp, KeyPageDown

// Modifier keys
KeyLeftShift, KeyLeftControl, KeyLeftAlt
KeyLeft_SHIFT, KeyRight_SHIFT  // Distinguish left from right
KeyLeft_CTRL, KeyRight_CTRL
KeyLeft_ALT, KeyRight_ALT
```

### Checking Modifier Keys

Modifier keys (Shift, Ctrl, Alt) are often used in combination with other keys:

```zia
bind Keyboard = Zanna.Input.Keyboard;

if Keyboard.WasPressed(Key.S) && Keyboard.IsDown(Key.LeftControl) {
    saveGame();  // Ctrl+S to save
}

if Keyboard.IsDown(Key.LeftShift) {
    speed = runSpeed;  // Hold shift to run
} else {
    speed = walkSpeed;
}
```

Notice the pattern: we check if S was *pressed* and if Ctrl is *down*. This correctly handles the timing — the action triggers when S is pressed, not when Ctrl is pressed.

---

## Mouse Input

The mouse provides position and button information. Let's explore both.

### Reading Mouse Position

```zia
bind Mouse = Zanna.Input.Mouse;

var mouseX = Mouse.X();
var mouseY = Mouse.Y();
```

These return the mouse position in canvas coordinates. The top-left corner of your canvas is (0, 0).

You can use mouse position for many things:

```zia
bind Mouse = Zanna.Input.Mouse;
bind Zanna.Math as Math;

// Make something follow the mouse
cursor.x = Mouse.X();
cursor.y = Mouse.Y();

// Check if mouse is over a button
var mx = Mouse.X();
var my = Mouse.Y();
if mx >= button.x && mx < button.x + button.width &&
   my >= button.y && my < button.y + button.height {
    // Mouse is over the button
    button.highlighted = true;
}

// Point a turret at the mouse
var dx = Mouse.X() - turret.x;
var dy = Mouse.Y() - turret.y;
turret.angle = Math.Atan2(dy, dx);
```

### Mouse Buttons

Mouse buttons work like keyboard keys:

```zia
bind Mouse = Zanna.Input.Mouse;

// Check if button is currently held (0 = left, 1 = right, 2 = middle)
if Mouse.IsDown(0) {
    // Left button is being held
}

// Check if button was just clicked
if Mouse.WasClicked(0) {
    // Left button was just clicked
}

// Check if button was just released
if Mouse.WasReleased(1) {
    // Right button was just released
}
```

The button indices are:

```zia
0  // Left button (primary click)
1  // Right button (secondary click / context menus)
2  // Middle button (usually clicking the scroll wheel)
```

### The Scroll Wheel

The scroll wheel reports how much it moved:

```zia
bind Mouse = Zanna.Input.Mouse;

var zoomLevel = 1.0;
var scroll = Mouse.WheelY();  // Positive = up, negative = down
if scroll != 0 {
    zoomLevel += scroll * 0.1;
}
```

Note that scroll values are typically small integers (-1, 0, or 1), though some mice report fractional values for smooth scrolling.

### Example: A Simple Drawing Program

Let's put mouse input together in a practical example:

```zia
module MouseDraw;

bind Zanna.Graphics;
bind Mouse = Zanna.Input.Mouse;
bind Zanna.Time.Clock as Clock;

func start() {
    var canvas = Canvas.New("Draw with Mouse", 800, 600);

    // Start with a white background
    canvas.Box(0, 0, 800, 600, Color.White);

    while !canvas.ShouldClose {
        canvas.Poll();

        // When left button is held, draw at mouse position
        if Mouse.IsDown(0) {
            var x = Mouse.X();
            var y = Mouse.Y();
            canvas.Disc(x, y, 5, Color.Black);
        }

        // Right click clears the canvas
        if Mouse.WasClicked(1) {
            canvas.Box(0, 0, 800, 600, Color.White);
        }

        canvas.Flip();
        Clock.Sleep(16);
    }
}
```

Let's trace through this code:

1. We create an 800x600 canvas and fill it with white.
2. In our main loop, we check if the left mouse button is held (`Mouse.IsDown`, not `Mouse.WasClicked`).
3. While held, we draw a small black circle at the current mouse position. As the user drags, we draw many circles, creating a line.
4. If the right button is clicked, we clear the canvas back to white.
5. We flip the buffer and wait 16 milliseconds (~60 FPS).

This demonstrates a key insight: use `Mouse.IsDown` for continuous actions (drawing while dragging) and `Mouse.WasClicked` for discrete actions (clearing on click).

---

## Game Controllers

Game controllers (gamepads) offer a different experience from keyboard and mouse. Their analog sticks provide smooth, proportional control that's hard to achieve with binary key presses.

### Checking Connection

Before reading from a controller, check if one is connected:

```zia
if Input.isControllerConnected(0) {  // Controller 0 (first controller)
    // Safe to read controller input
}
```

Controllers are numbered starting from 0. Most games support at least 4 controllers for local multiplayer.

### Reading Analog Sticks

Analog sticks report their position as two values (x and y), each ranging from -1.0 to 1.0:

```zia
if Input.isControllerConnected(0) {
    var leftX = Input.controllerAxis(0, Axis.LEFT_X);
    var leftY = Input.controllerAxis(0, Axis.LEFT_Y);

    // Move player based on stick position
    player.x += leftX * speed * dt;
    player.y += leftY * speed * dt;
}
```

The value represents how far the stick is pushed:
- 0.0 = centered
- 1.0 = fully pushed right (for X) or down (for Y)
- -1.0 = fully pushed left (for X) or up (for Y)

Values in between give you proportional control. Pushing the stick halfway right gives about 0.5, letting the player walk slowly. Pushing it fully gives 1.0, for full speed.

### Controller Buttons

Controller buttons work like keyboard keys:

```zia
if Input.isControllerButtonDown(0, ControllerButton.A) {
    // A button is held
}

if Input.wasControllerButtonPressed(0, ControllerButton.A) {
    player.jump();
}
```

### Controller Button Names

```zia
// Face buttons
ControllerButton.A, ControllerButton.B
ControllerButton.X, ControllerButton.Y

// Shoulder buttons
ControllerButton.LEFT_BUMPER, ControllerButton.RIGHT_BUMPER

// Stick clicks
ControllerButton.LEFT_STICK, ControllerButton.RIGHT_STICK

// Menu buttons
ControllerButton.START, ControllerButton.SELECT

// D-pad (digital directional pad)
ControllerButton.DButtonUp, ControllerButton.DButtonDown
ControllerButton.DButtonLeft, ControllerButton.DButtonRight
```

### Controller Axes

```zia
Axis.LEFT_X, Axis.LEFT_Y    // Left stick (-1 to 1)
Axis.RIGHT_X, Axis.RIGHT_Y  // Right stick (-1 to 1)
Axis.LEFT_TRIGGER           // Left trigger (0 to 1)
Axis.RIGHT_TRIGGER          // Right trigger (0 to 1)
```

Note that triggers are 0 to 1 (not -1 to 1) because they only go one direction — from released (0) to fully pressed (1).

---

## Input Patterns

Now that we understand the basics, let's explore patterns that make input handling robust and professional.

### Dead Zones

Remember that analog sticks rarely rest exactly at (0, 0). A dead zone ignores small values near the center:

```zia
bind Zanna.Math as Math;

func applyDeadZone(value: Number, threshold: Number) -> Number {
    if Math.Abs(value) < threshold {
        return 0.0;  // Treat small values as zero
    }
    return value;
}

// Usage
var rawX = Input.controllerAxis(0, Axis.LEFT_X);
var moveX = applyDeadZone(rawX, 0.15);  // Ignore values under 0.15
```

A threshold of 0.15 is typical. Too low, and the character drifts. Too high, and the stick feels unresponsive.

A more sophisticated dead zone smoothly scales the value to avoid a "jump" when crossing the threshold:

```zia
bind Zanna.Math as Math;

func applyDeadZoneSmooth(value: Number, threshold: Number) -> Number {
    var absValue = Math.Abs(value);
    if absValue < threshold {
        return 0.0;
    }
    // Scale remaining range to 0-1
    var sign = if value < 0 { -1.0 } else { 1.0 };
    return sign * (absValue - threshold) / (1.0 - threshold);
}
```

This makes the transition from dead zone to movement smooth rather than abrupt.

### Input Abstraction

Games should abstract input so the same action can come from different sources. This lets players use their preferred input device and makes your code cleaner:

```zia
bind Keyboard = Zanna.Input.Keyboard;
bind Zanna.Math as Math;

class InputManager {
    func getMoveX() -> Number {
        // Check keyboard first
        if Keyboard.IsDown(Key.Left) || Keyboard.IsDown(Key.A) {
            return -1.0;
        }
        if Keyboard.IsDown(Key.Right) || Keyboard.IsDown(Key.D) {
            return 1.0;
        }

        // Then check controller
        if Input.isControllerConnected(0) {
            var axis = Input.controllerAxis(0, Axis.LEFT_X);
            if Math.Abs(axis) > 0.2 {  // Dead zone
                return axis;
            }
        }

        return 0.0;
    }

    func getMoveY() -> Number {
        if Keyboard.IsDown(Key.Up) || Keyboard.IsDown(Key.W) {
            return -1.0;
        }
        if Keyboard.IsDown(Key.Down) || Keyboard.IsDown(Key.S) {
            return 1.0;
        }

        if Input.isControllerConnected(0) {
            var axis = Input.controllerAxis(0, Axis.LEFT_Y);
            if Math.Abs(axis) > 0.2 {
                return axis;
            }
        }

        return 0.0;
    }

    func isJumpPressed() -> Boolean {
        return Keyboard.WasPressed(Key.Space) ||
               Keyboard.WasPressed(Key.W) ||
               Input.wasControllerButtonPressed(0, ControllerButton.A);
    }

    func isActionPressed() -> Boolean {
        return Keyboard.WasPressed(Key.E) ||
               Keyboard.WasPressed(Key.Enter) ||
               Input.wasControllerButtonPressed(0, ControllerButton.X);
    }
}
```

Now your game code becomes clean and device-agnostic:

```zia
var input = InputManager();

while gameRunning {
    // Movement works with keyboard or controller
    player.x += input.getMoveX() * speed * dt;
    player.y += input.getMoveY() * speed * dt;

    // Actions work with any input device
    if input.isJumpPressed() {
        player.jump();
    }
    if input.isActionPressed() {
        player.interact();
    }
}
```

### Key Mapping (Rebindable Controls)

Players appreciate customizable controls. A key map stores the current bindings:

```zia
bind Keyboard = Zanna.Input.Keyboard;

class KeyMap {
    hide bindings: Map[String, Integer];

    expose func init() {
        self.bindings = new Map[String, Integer]();
        // Default bindings
        self.bindings.Set("jump", Key.Space);
        self.bindings.Set("left", Key.Left);
        self.bindings.Set("right", Key.Right);
        self.bindings.Set("up", Key.Up);
        self.bindings.Set("down", Key.Down);
        self.bindings.Set("fire", Key.LeftControl);
        self.bindings.Set("pause", Key.Escape);
    }

    func isActionDown(action: String) -> Boolean {
        var key = self.bindings.Get(action) ?? Key.Unknown;
        return Keyboard.IsDown(key);
    }

    func wasActionPressed(action: String) -> Boolean {
        var key = self.bindings.Get(action) ?? Key.Unknown;
        return Keyboard.WasPressed(key);
    }

    func rebind(action: String, key: Integer) {
        self.bindings.Set(action, key);
    }

    func getBinding(action: String) -> Integer {
        return self.bindings.Get(action) ?? Key.Unknown;
    }
}
```

To let the player rebind a key:

```zia
bind Keyboard = Zanna.Input.Keyboard;
bind Zanna.Time as Time;

func waitForKeyAndRebind(keyMap: KeyMap, action: String) {
    // Wait for any key press
    while true {
        for keyCode in 0..256 {
            if Keyboard.WasPressed(keyCode) {
                keyMap.rebind(action, keyCode);
                return;
            }
        }
        Time.Clock.Sleep(16);
    }
}
```

### Input Buffering

Professional games use *input buffering* to feel responsive. The idea: remember recent inputs and use them when they become valid.

Imagine you're playing a platformer. Your character is falling toward the ground. You press jump slightly before landing. Without buffering, the jump is ignored because you weren't on the ground yet. With buffering, the game remembers you pressed jump and executes it the moment you land.

```zia
class InputBuffer {
    hide jumpBufferTime: Number;
    hide jumpBufferDuration: Number = 0.1;  // 100ms buffer window

    func update(dt: Number) {
        // Decrease buffer timer
        if self.jumpBufferTime > 0 {
            self.jumpBufferTime -= dt;
        }

        // When jump is pressed, start the buffer timer
        if Keyboard.WasPressed(Key.Space) {
            self.jumpBufferTime = self.jumpBufferDuration;
        }
    }

    func consumeJump() -> Boolean {
        // If there's a buffered jump, use it and clear the buffer
        if self.jumpBufferTime > 0 {
            self.jumpBufferTime = 0;
            return true;
        }
        return false;
    }
}
```

Usage in your game:

```zia
var inputBuffer = InputBuffer();

while gameRunning {
    inputBuffer.update(dt);

    // Only try to jump when on the ground
    if player.onGround && inputBuffer.consumeJump() {
        player.jump();
    }
}
```

This small addition makes games feel much more responsive. Players don't realize it's happening — they just feel like the game "gets" what they're trying to do.

### Coyote Time

A related technique is *coyote time* (named after cartoon coyotes who don't fall until they look down). It's the opposite of input buffering: instead of remembering inputs, you remember when the player was last grounded.

```zia
class CoyoteTime {
    hide timeLeftGrounded: Number;
    hide coyoteDuration: Number = 0.1;  // 100ms grace period
    hide wasGrounded: Boolean = false;

    func update(dt: Number, isGrounded: Boolean) {
        if isGrounded {
            self.timeLeftGrounded = self.coyoteDuration;
            self.wasGrounded = true;
        } else if self.wasGrounded {
            // Just left the ground, start counting
            self.timeLeftGrounded -= dt;
            if self.timeLeftGrounded <= 0 {
                self.wasGrounded = false;
            }
        }
    }

    func canJump() -> Boolean {
        return self.timeLeftGrounded > 0;
    }
}
```

Now the player can jump for a brief moment after walking off a ledge, which feels fair and responsive.

### Debouncing

Some inputs shouldn't repeat too quickly. *Debouncing* prevents rapid-fire activation:

```zia
class Debouncer {
    hide cooldowns: Map[String, Number];

    expose func init() {
        self.cooldowns = new Map[String, Number]();
    }

    func update(dt: Number) {
        for action in self.cooldowns.Keys() {
            var remaining = self.cooldowns.Get(action) ?? 0.0;
            if remaining > 0.0 {
                self.cooldowns.Set(action, remaining - dt);
            }
        }
    }

    func canActivate(action: String, cooldown: Number) -> Boolean {
        var remaining = self.cooldowns.Get(action) ?? 0.0;
        if remaining <= 0.0 {
            self.cooldowns.Set(action, cooldown);
            return true;
        }
        return false;
    }
}
```

Use it for things like menu navigation:

```zia
var debouncer = Debouncer();

while inMenu {
    debouncer.update(dt);

    if Keyboard.IsDown(Key.Down) && debouncer.canActivate("menuDown", 0.2) {
        selectedIndex += 1;
    }
    if Keyboard.IsDown(Key.Up) && debouncer.canActivate("menuUp", 0.2) {
        selectedIndex -= 1;
    }
}
```

This lets the player hold the key to scroll through menu items at a reasonable pace, rather than instantly jumping to the end.

---

## A Complete Example: Controllable Character

Let's put everything together in a complete, playable example:

```zia
module CharacterDemo;

bind Zanna.Graphics;
bind Keyboard = Zanna.Input.Keyboard;
bind Key = Zanna.Input.Key;
bind Zanna.Time.Clock as Clock;
bind Convert = Zanna.Core.Convert;

struct Player {
    expose Number x;
    expose Number y;
    expose Number vx;
    expose Number vy;
    expose Boolean onGround;

    expose func init(x: Number, y: Number, vx: Number, vy: Number, onGround: Boolean) {
        self.x = x;
        self.y = y;
        self.vx = vx;
        self.vy = vy;
        self.onGround = onGround;
    }
}

final GRAVITY = 800.0;
final JUMP_SPEED = -400.0;
final MOVE_SPEED = 200.0;
final GROUND_Y = 500.0;

func start() {
    var canvas = Canvas.New("Character Control", 800, 600);

    var player = new Player(400.0, GROUND_Y, 0.0, 0.0, true);

    var lastTime = Clock.NowMs();

    while !canvas.ShouldClose {
        canvas.Poll();

        // Calculate delta time
        var now = Clock.NowMs();
        var dt = (now - lastTime) / 1000.0;
        lastTime = now;

        // --- INPUT ---
        // Horizontal movement (continuous - use IsDown)
        player.vx = 0.0;
        if Keyboard.IsDown(Key.Left) || Keyboard.IsDown(Key.A) {
            player.vx = -MOVE_SPEED;
        }
        if Keyboard.IsDown(Key.Right) || Keyboard.IsDown(Key.D) {
            player.vx = MOVE_SPEED;
        }

        // Jump (one-time action - use WasPressed)
        if Keyboard.WasPressed(Key.Space) && player.onGround {
            player.vy = JUMP_SPEED;
            player.onGround = false;
        }

        // --- PHYSICS ---
        // Apply gravity when in the air
        if !player.onGround {
            player.vy += GRAVITY * dt;
        }

        // Update position
        player.x += player.vx * dt;
        player.y += player.vy * dt;

        // Ground collision
        if player.y >= GROUND_Y {
            player.y = GROUND_Y;
            player.vy = 0.0;
            player.onGround = true;
        }

        // Keep player on screen (horizontal bounds)
        if player.x < 25.0 { player.x = 25.0; }
        if player.x > 775.0 { player.x = 775.0; }

        // --- RENDERING ---
        // Sky
        canvas.Box(0, 0, 800, 550, Color.Rgb(100, 150, 255));

        // Ground
        canvas.Box(0, 550, 800, 100, Color.Rgb(50, 150, 50));

        // Player (centered on position)
        var drawX = Convert.NumToInt(player.x - 25.0);
        var drawY = Convert.NumToInt(player.y - 50.0);
        canvas.Box(drawX, drawY, 50, 50, Color.Red);

        // Instructions
        canvas.Text(10, 25, "Arrow keys or A/D to move, Space to jump", Color.White);

        // Debug info
        canvas.Text(10, 50, "Position: (" + player.x + ", " + player.y + ")", Color.White);
        canvas.Text(10, 75, "On ground: " + player.onGround, Color.White);

        canvas.Flip();
        Clock.Sleep(16);
    }
}
```

Let's trace what happens when you run this:

1. **Initialization**: We create a canvas, initialize the player at ground level, and record the current time.

2. **Main loop**: Each iteration represents one frame (~60 times per second).

3. **Delta time calculation**: We measure how much time passed since the last frame. This ensures consistent movement regardless of frame rate.

4. **Input handling**: We check keyboard state for movement (continuous) and key presses for jumping (one-time).

5. **Physics**: Gravity pulls the player down. Velocity updates position.

6. **Collision**: If the player falls below ground level, we snap them to the ground and reset vertical velocity.

7. **Rendering**: We draw everything in order (background, then foreground objects, then UI).

8. **Wait**: We sleep briefly to limit frame rate and avoid consuming 100% CPU.

---

## Common Mistakes

Learning from mistakes is efficient. Here are problems beginners often encounter with input handling.

### Mistake 1: Using IsDown for One-Time Actions

**Wrong:**
```zia
if Keyboard.IsDown(Key.Space) {
    fireBullet();  // Fires 60 bullets per second!
}
```

**Right:**
```zia
if Keyboard.WasPressed(Key.Space) {
    fireBullet();  // Fires once per button press
}
```

When you hold the Space bar, `IsDown` returns `true` every frame. For actions that should happen once per press, use `WasPressed`.

### Mistake 2: Forgetting to Handle Key Release

**Problem:**
```zia
if Keyboard.WasPressed(Key.LeftShift) {
    player.isRunning = true;
}
// Player runs forever after pressing shift once!
```

**Fix:**
```zia
player.isRunning = Keyboard.IsDown(Key.LeftShift);
// Or:
if Keyboard.WasPressed(Key.LeftShift) {
    player.isRunning = true;
}
if Keyboard.WasReleased(Key.LeftShift) {
    player.isRunning = false;
}
```

For "hold to activate" mechanics, check the key state continuously, or handle both press and release.

### Mistake 3: Checking Input Outside the Game Loop

**Wrong:**
```zia
func checkJump() {
    // This might miss the key press!
    if Keyboard.WasPressed(Key.Space) {
        player.jump();
    }
}

// Called from some other part of the code, not every frame
```

**Right:**
```zia
// In main game loop, called every frame
while gameRunning {
    if Keyboard.WasPressed(Key.Space) {
        player.jump();
    }
    // ...
}
```

`WasPressed` only returns `true` for one frame. If you don't check it during that frame, you miss the input.

### Mistake 4: Not Applying Dead Zones

**Wrong:**
```zia
var stickX = Input.controllerAxis(0, Axis.LEFT_X);
player.x += stickX * speed * dt;  // Character slowly drifts even with stick centered
```

**Right:**
```zia
bind Zanna.Math as Math;

var stickX = Input.controllerAxis(0, Axis.LEFT_X);
if Math.Abs(stickX) < 0.15 {
    stickX = 0.0;  // Dead zone
}
player.x += stickX * speed * dt;
```

Physical analog sticks almost never rest at exactly (0, 0). Always apply a dead zone.

### Mistake 5: Hardcoding Controls

**Problematic:**
```zia
// Scattered throughout your code
if Keyboard.IsDown(Key.W) { moveUp(); }
if Keyboard.IsDown(Key.A) { moveLeft(); }
if Keyboard.WasPressed(Key.Space) { jump(); }
```

**Better:**
```zia
// Centralized input handling
class InputManager {
    func getMoveDirection() -> Vec2 { ... }
    func isJumpPressed() -> Boolean { ... }
}

// Game code uses abstraction
player.move(input.getMoveDirection());
if input.isJumpPressed() { player.jump(); }
```

Centralizing input handling makes it easy to add controller support, rebindable keys, and different control schemes.

### Mistake 6: Checking Input for Unfocused Windows

When your game window isn't focused (the player clicked on another window), you might still receive input events in some situations, or the input state might be stale. Good practice:

```zia
bind Zanna.Time as Time;

while !canvas.ShouldClose {
    canvas.Poll();

    if !canvas.HasFocus() {
        // Window not focused, skip input processing
        // Maybe also pause the game
        Time.Clock.Sleep(100);  // Don't burn CPU while unfocused
        continue;
    }

    // Normal input handling here
    canvas.Flip();
}
```

### Mistake 7: Not Clearing Input Between States

When transitioning between game states (menu to gameplay, for example), leftover input can cause problems:

```zia
func startGame() {
    // The player pressed Enter to start, but Enter might still register
    // as a "just pressed" key this frame
    if Keyboard.WasPressed(Key.Enter) {
        // This triggers immediately, maybe pausing the game!
        togglePause();
    }
}
```

**Fix:** consume the edge before switching state, or simply wait a frame before
processing input in the new state — there is no API to clear the pressed set.

```zia
func startGame() {
    // Drain this frame's edge-triggered events so the Enter that started the
    // game is not observed again by the gameplay state.
    var _ = Keyboard.GetPressed();
}
```

---

## Debugging Input Problems

When input doesn't work as expected, here's how to find the problem.

### Print Input State

The simplest debugging technique — see what the input system is actually reporting:

```zia
// Add to your game loop temporarily
canvas.Text(10, 50, "Space down: " + Keyboard.IsDown(Key.Space), Color.White);
canvas.Text(10, 70, "Space pressed: " + Keyboard.WasPressed(Key.Space), Color.White);
canvas.Text(10, 90, "Mouse: " + Mouse.X() + ", " + Mouse.Y(), Color.White);
```

If the display shows the input is detected but your game doesn't respond, the bug is in your game logic. If the display doesn't show the input, the problem is earlier in the pipeline.

### Check Event Order

Sometimes the order of operations matters:

```zia
// Bug: wasKeyPressed is checked after the action already happened
player.update();  // This might call Input functions internally
if Keyboard.WasPressed(Key.Space) {
    // This never triggers because WasPressed was already
    // consumed (or cleared) during player.update()
}
```

Make sure input is checked before it's used anywhere else in the frame.

### Verify Focus

Is your game window actually focused?

```zia
canvas.Text(10, 110, "Has focus: " + canvas.HasFocus(), Color.White);
```

Some input might not register if the window isn't focused.

### Test with Different Input Devices

If keyboard works but controller doesn't:
- Is the controller actually connected? (`Input.isControllerConnected(0)`)
- Is it the right controller number? (Maybe it's controller 1, not 0)
- Are you using the right button names? (Controllers vary in layout)

### Log Timing Issues

For input buffering and timing-sensitive code:

```zia
bind Keyboard = Zanna.Input.Keyboard;
bind Zanna.Terminal;
bind Zanna.Time as Time;

if Keyboard.WasPressed(Key.Space) {
    Say("Jump pressed at time: " + Time.Clock.NowMs());
}

if player.onGround {
    Say("On ground at time: " + Time.Clock.NowMs());
    if inputBuffer.consumeJump() {
        Say("Jump executed!");
    }
}
```

This helps you see if inputs are arriving at the right times.

---

## The Two Languages

**Zia**
```zia
bind Keyboard = Zanna.Input.Keyboard;
bind Mouse = Zanna.Input.Mouse;

// Keyboard
if Keyboard.IsDown(Key.Space) {
    player.charging = true;
}
if Keyboard.WasPressed(Key.Escape) {
    pauseGame();
}

// Mouse
var mx = Mouse.X();
var my = Mouse.Y();
if Mouse.WasClicked(0) {
    handleClick(mx, my);
}

// Controller
if Input.isControllerConnected(0) {
    var moveX = Input.controllerAxis(0, Axis.LEFT_X);
    player.x += moveX * speed * dt;
}
```

**BASIC**
```text
' Keyboard
IF KEYDOWN(KeySpace) THEN
    player.charging = TRUE
END IF
IF KEYPRESSED(KeyEscape) THEN
    CALL PauseGame()
END IF

' Mouse
DIM mx AS INTEGER, my AS INTEGER
mx = MOUSEX
my = MOUSEY
IF MOUSEPRESSED(ButtonLeft) THEN
    CALL HandleClick(mx, my)
END IF

' Controller
IF CONTROLLERCONNECTED(0) THEN
    DIM moveX AS SINGLE
    moveX = CONTROLLERAXIS(0, AxisLeftX)
    player.x = player.x + moveX * speed * dt
END IF
```

---

## Summary

- **Input is communication** — it's how users talk to your program
- **The input pipeline** goes from physical device through OS to your code
- **Input queues** store events so nothing is lost
- **Key state** (`Keyboard.IsDown`) checks if a key is currently held
- **Key events** (`Keyboard.WasPressed`, `Keyboard.WasReleased`) detect transitions
- Use **state** for continuous actions, **events** for one-time actions
- **Mouse** provides position and button state
- **Controllers** have analog sticks (need dead zones) and digital buttons
- **Abstract input** to support multiple control schemes
- **Input buffering** and **coyote time** make games feel responsive
- **Debouncing** prevents unwanted rapid-fire activation
- **Common mistakes**: using the wrong function for the action type, missing key releases, no dead zones

---

## Exercises

**Exercise 20.1** (Mimic): Create a cursor that follows the mouse position. Draw a small crosshair or circle at the mouse coordinates.

**Exercise 20.2** (Extend): Modify the drawing program to support multiple colors. Use number keys 1-5 to select different colors.

**Exercise 20.3** (Create): Build a simple "avoid the obstacles" game where the player character follows the mouse and must avoid randomly moving rectangles. Display a score that increases over time, and end the game when the player touches an obstacle.

**Exercise 20.4** (Create): Create a color picker with three sliders for R, G, B values. Click and drag each slider to adjust. Display the resulting color in a preview box.

**Exercise 20.5** (Create): Implement a simple text input field. Show characters as the user types, handle backspace to delete, and Enter to submit. Display what was typed.

**Exercise 20.6** (Create): Create a simple drawing program with:
- Different brush sizes (number keys 1-5)
- Different colors (letter keys R, G, B, Y, W)
- Clear canvas (C key)
- Undo last stroke (Ctrl+Z) — this requires storing strokes, not just pixels

**Exercise 20.7** (Challenge): Create a two-player game on one keyboard. Player 1 uses WASD and Space. Player 2 uses arrow keys and Enter. Both players control separate characters that can move and jump. Add a simple competition element (race to a goal, collect coins, etc.).

**Exercise 20.8** (Challenge): Implement a complete input system with:
- Support for keyboard, mouse, and controller
- Rebindable keys (press a key to assign it to an action)
- Dead zone handling for controller sticks
- Input buffering for jump actions
- Save and load control bindings to a file

**Exercise 20.9** (Challenge): Create a rhythm game where notes scroll down the screen and the player must press the correct key (D, F, J, K) when notes reach a target line. Score based on timing accuracy. This requires precise input timing and visual feedback.

**Exercise 20.10** (Challenge): Build a simple virtual keyboard on screen. The player uses the mouse to click keys, and what they type appears in a text display. Support shift for uppercase letters.

---

*We can draw graphics and handle input. Now let's build a complete game from scratch, putting together everything we've learned.*

*[Continue to Chapter 21: Building a Game](21-game-project.md)*
