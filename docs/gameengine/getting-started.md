---
status: active
audience: public
last-verified: 2026-07-26
---

# Your First Game in 15 Minutes

> From zero to a playable game with movement, sprites, collision, and sound.

**Part of [Zanna Game Engine](README.md)**

---

This guide builds a complete mini-game: a paddle that bounces a ball, with sound effects and score tracking. You'll learn the core game loop pattern, drawing, input handling, and audio — the foundation for every Zanna game.

---

## Prerequisites

- Zanna built and installed ([Getting Started](../getting-started.md))
- A text editor
- A terminal

---

## Step 1: Create a Window

Every game starts with a **Canvas** — a window you draw to.

### Zia

```zia
module mygame;

bind Zanna.Graphics.Canvas;

func start() {
    var canvas = Canvas.New("My First Game", 640, 480);

    while canvas.BeginFrame() != 0 {
        canvas.Clear(0x1a1a2e);
        canvas.Text(10, 10, "Hello, Zanna!", 0xffffff);
        canvas.Flip();
    }
}
```

### BASIC

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("My First Game", 640, 480)

DO WHILE canvas.ShouldClose = 0
    canvas.Poll()
    canvas.Clear(&H001A1A2E)
    canvas.Text(10, 10, "Hello, Zanna!", &H00FFFFFF)
    canvas.Flip()
LOOP
```

**What's happening:**
- `Canvas.New()` opens a window with a title and pixel dimensions
- `BeginFrame()` (Zia) or `Poll()` (BASIC) processes input events
- `Clear()` fills the canvas with a background color (hex `0xRRGGBB`)
- `Text()` draws a string at pixel coordinates
- `Flip()` presents the frame (double-buffered rendering)

---

## Step 2: Add Movement

Read keyboard input to move a paddle.

### Zia

```zia
module mygame;

bind Zanna.Graphics.Canvas;

final W = 640;
final H = 480;
final PADDLE_W = 80;
final PADDLE_H = 12;
final PADDLE_SPEED = 5;

func start() {
    var canvas = Canvas.New("Paddle Game", W, H);
    var px = W / 2 - PADDLE_W / 2;
    var py = H - 40;

    while canvas.BeginFrame() != 0 {
        // Input
        if canvas.KeyHeld(263) != 0 { px = px - PADDLE_SPEED; }  // LEFT
        if canvas.KeyHeld(262) != 0 { px = px + PADDLE_SPEED; }  // RIGHT

        // Clamp to screen
        if px < 0 { px = 0; }
        if px > W - PADDLE_W { px = W - PADDLE_W; }

        // Draw
        canvas.Clear(0x1a1a2e);
        canvas.Box(px, py, PADDLE_W, PADDLE_H, 0xe94560);
        canvas.Text(10, 10, "Arrow keys to move", 0xffffff);
        canvas.Flip();
    }
}
```

### BASIC

```basic
CONST W = 640
CONST H = 480
CONST PADDLE_W = 80
CONST PADDLE_H = 12
CONST PADDLE_SPEED = 5

DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Paddle Game", W, H)
DIM px AS INTEGER = W / 2 - PADDLE_W / 2
DIM py AS INTEGER = H - 40

DO WHILE canvas.ShouldClose = 0
    canvas.Poll()
    IF canvas.KeyHeld(263) <> 0 THEN px = px - PADDLE_SPEED  ' LEFT
    IF canvas.KeyHeld(262) <> 0 THEN px = px + PADDLE_SPEED  ' RIGHT

    IF px < 0 THEN px = 0
    IF px > W - PADDLE_W THEN px = W - PADDLE_W

    canvas.Clear(&H001A1A2E)
    canvas.Box(px, py, PADDLE_W, PADDLE_H, &H00E94560)
    canvas.Text(10, 10, "Arrow keys to move", &H00FFFFFF)
    canvas.Flip()
LOOP
```

**Key concepts:**
- `KeyHeld(keycode)` returns non-zero while a key is pressed
- Key codes: 262 = RIGHT, 263 = LEFT, 264 = DOWN, 265 = UP, 32 = SPACE, 256 = ESCAPE
- Drawing order matters — later calls draw on top of earlier ones

---

## Step 3: Add a Bouncing Ball

Add a ball with velocity that bounces off walls and the paddle.

### Zia

```zia
module mygame;

bind Zanna.Graphics.Canvas;

final W = 640;
final H = 480;
final PADDLE_W = 80;
final PADDLE_H = 12;
final PADDLE_SPEED = 5;
final BALL_R = 8;

func start() {
    var canvas = Canvas.New("Bounce!", W, H);
    var px = W / 2 - PADDLE_W / 2;
    var py = H - 40;

    var bx = W / 2;
    var by = H / 2;
    var bvx = 3;
    var bvy = -3;
    var score = 0;

    while canvas.BeginFrame() != 0 {
        // --- Input ---
        if canvas.KeyHeld(263) != 0 { px = px - PADDLE_SPEED; }
        if canvas.KeyHeld(262) != 0 { px = px + PADDLE_SPEED; }
        if px < 0 { px = 0; }
        if px > W - PADDLE_W { px = W - PADDLE_W; }

        // --- Update ball ---
        bx = bx + bvx;
        by = by + bvy;

        // Wall bounce
        if bx <= BALL_R or bx >= W - BALL_R { bvx = 0 - bvx; }
        if by <= BALL_R { bvy = 0 - bvy; }

        // Paddle bounce
        if bvy > 0 and by + BALL_R >= py and by + BALL_R <= py + PADDLE_H {
            if bx >= px and bx <= px + PADDLE_W {
                bvy = 0 - bvy;
                score = score + 1;
            }
        }

        // Ball fell off bottom — reset
        if by > H + BALL_R {
            bx = W / 2;
            by = H / 2;
            bvy = -3;
            score = 0;
        }

        // --- Draw ---
        canvas.Clear(0x1a1a2e);
        canvas.Box(px, py, PADDLE_W, PADDLE_H, 0xe94560);
        canvas.Disc(bx, by, BALL_R, 0x0f3460);
        canvas.Text(10, 10, "Score: " + Int(score), 0xffffff);
        canvas.Flip();
    }
}
```

### BASIC

```basic
CONST W = 640
CONST H = 480
CONST PADDLE_W = 80
CONST PADDLE_H = 12
CONST PADDLE_SPEED = 5
CONST BALL_R = 8

DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Bounce!", W, H)
DIM px AS INTEGER = W / 2 - PADDLE_W / 2
DIM py AS INTEGER = H - 40

DIM bx AS INTEGER = W / 2
DIM by AS INTEGER = H / 2
DIM bvx AS INTEGER = 3
DIM bvy AS INTEGER = -3
DIM score AS INTEGER = 0

DO WHILE canvas.ShouldClose = 0
    canvas.Poll()
    IF canvas.KeyHeld(263) <> 0 THEN px = px - PADDLE_SPEED
    IF canvas.KeyHeld(262) <> 0 THEN px = px + PADDLE_SPEED
    IF px < 0 THEN px = 0
    IF px > W - PADDLE_W THEN px = W - PADDLE_W

    ' Update ball
    bx = bx + bvx
    by = by + bvy

    IF bx <= BALL_R OR bx >= W - BALL_R THEN bvx = 0 - bvx
    IF by <= BALL_R THEN bvy = 0 - bvy

    IF bvy > 0 AND by + BALL_R >= py AND by + BALL_R <= py + PADDLE_H THEN
        IF bx >= px AND bx <= px + PADDLE_W THEN
            bvy = 0 - bvy
            score = score + 1
        END IF
    END IF

    IF by > H + BALL_R THEN
        bx = W / 2
        by = H / 2
        bvy = -3
        score = 0
    END IF

    ' Draw
    canvas.Clear(&H001A1A2E)
    canvas.Box(px, py, PADDLE_W, PADDLE_H, &H00E94560)
    canvas.Disc(bx, by, BALL_R, &H000F3460)
    canvas.Text(10, 10, "Score: " & STR$(score), &H00FFFFFF)
    canvas.Flip()
LOOP
```

You now have a playable game with input, physics, collision detection, and scoring.

---

## Step 4: Add Sound Effects

Zanna includes a **Synth** module for procedural sound — no audio files needed.

### Zia

Add these imports and lines to the previous example:

```zia
bind Zanna.Audio;

// In start(), before the game loop:
Audio.Init();

// Where the paddle bounce happens (after score = score + 1):
var bounce = Synth.Sfx(1); // coin
bounce.Play();

// Where the ball resets (after score = 0):
var miss = Synth.Sfx(2); // hit
miss.Play();
```

### BASIC

```basic
' Before the game loop:
Zanna.Audio.Mixer.Init()

' Where the paddle bounce happens:
DIM bounce AS Zanna.Audio.Sound
bounce = Zanna.Audio.Synth.Sfx(1) ' coin
bounce.Play()

' Where the ball resets:
DIM miss AS Zanna.Audio.Sound
miss = Zanna.Audio.Synth.Sfx(2) ' hit
miss.Play()
```

**Synth presets:** `0` jump, `1` coin, `2` hit, `3` explosion, `4` powerup, `5` laser — no audio files required. For custom sounds, load WAV files with `Sound.Load("path/to/file.wav")`.

---

## Step 5: Add Visual Polish

Add screen effects for game feel:

### Zia

```zia
bind Zanna.Game.ScreenFX as FX;

// In start(), before the game loop:
var fx = FX.New();

// Where the paddle bounce happens:
fx.Flash(0xFFFFFFFF, 100);  // white flash for 100ms, 0xRRGGBBAA

// Where the ball resets:
fx.Shake(6000, 300, 1000);  // 6px intensity, 300ms duration, linear decay

// In the update section (before drawing):
fx.Update(canvas.DeltaTime);

// At the end of drawing (before Flip):
fx.Draw(canvas, W, H);
```

---

## What's Next?

You've learned the core loop: **input -> update -> draw -> flip**. From here:

| Want to... | Read... |
|------------|---------|
| Use sprites instead of shapes | [Animation](../zannalib/game/animation.md) |
| Build levels with tiles | [Tilemaps](../zannalib/graphics/tilemaps2d.md) |
| Add enemies with AI | [Entities](../zannalib/game/entity.md) |
| Structure a multi-screen game | [GameBase and IScene](../zannalib/game/gameloop.md) |
| Add a scrolling camera | [Scene and Camera](../zannalib/graphics/scene.md#zannagraphicscamera) |
| Use rebindable input | [Input](../zannalib/input.md) |
| Study complete games | [Game examples](examples/README.md) |

---

## The GameBase Shortcut

Once you're comfortable with the raw game loop, consider using **GameBase** — a reusable base class that eliminates the boilerplate (canvas creation, frame loop, DeltaTime clamping, scene management):

```zia
bind "../lib/gamebase";
bind "../lib/iscene";

class MyGame : GameBase {
    expose func init() {
        initGame("My Game", 640, 480);
        setScene(new PlayScene());
    }
}
```

See [GameBase and IScene](../zannalib/game/gameloop.md) for the full pattern.

---

## See Also

- [2D Graphics](../zannalib/graphics/README.md) — Graphics guide and API index
- [Audio](../zannalib/audio.md) — Sound loading, music streaming, and procedural synthesis
- [Input](../zannalib/input.md) — Keyboard codes, gamepad, and action mapping
- [Canvas API Reference](../zannalib/graphics/canvas.md) — Exhaustive method signatures
- [The Zanna Book, Chapter 19](../book/part4-applications/19-graphics.md) — Deep dive on graphics fundamentals
