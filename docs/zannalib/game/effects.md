---
status: active
audience: public
last-verified: 2026-07-26
---

# Visual Effects

> `ParticleEmitter`, `ScreenFX`, and `Lighting2D`

**Part of [Zanna Runtime Library](../README.md) › [Game Utilities](README.md)**

These three APIs use different packed-color conventions. Mixing them is a common source of
invisible or incorrectly colored effects.

| API | Color convention | Example: opaque red |
|---|---|---|
| `ParticleEmitter.Color` | `0xAARRGGBB`; plain `0x00RRGGBB` is treated as opaque | `0xFFFF0000` |
| `ScreenFX.Flash` / `FadeIn` / `FadeOut` | raw `0xRRGGBBAA`; alpha is the low byte | `0xFF0000FF` |
| `ScreenFX` transitions | Canvas RGB, `0x00RRGGBB` | `0xFF0000` |
| `Lighting2D` | Canvas RGB, `0x00RRGGBB`; higher bits are discarded | `0xFF0000` |

`Zanna.Graphics.Color.Rgba()` produces a tagged ARGB value for the Canvas/Pixels pipeline. It is
safe for `ParticleEmitter`, but it is **not** the raw-RGBA representation expected by ScreenFX
flash and fade methods. Use an explicit raw-RGBA literal or pack the bytes yourself for those
methods.

---

## Zanna.Game.ParticleEmitter

`ParticleEmitter` owns a fixed-capacity pool of disc-shaped particles. Simulation is measured in
update ticks, not elapsed time: call `Update()` once for each particle frame you want to advance.

- **Type:** instance class
- **Constructor:** `ParticleEmitter.New(maxParticles)`
- **Capacity:** `maxParticles` is clamped to 1–1024

### Defaults

| Setting | Default |
|---|---|
| Position | `(0.0, 0.0)` |
| Continuous rate | `1.0` particle per update |
| Emission state | stopped |
| Lifetime | 30–60 updates |
| Speed | 1.0–3.0 pixels per update |
| Angle | 0–360 degrees |
| Gravity | `(0.0, 0.0)` per update² |
| Size | 2.0–4.0 pixels in diameter |
| Color | opaque white (`0xFFFFFFFF`) |
| Fade / shrink | fade enabled; shrink disabled |

### Properties

| Property | Type | Access | Meaning |
|---|---|---|---|
| `X`, `Y` | `Double` | read | Origin used by newly emitted particles |
| `Rate` | `Double` | read/write | Particles emitted per `Update()`; fractional rates accumulate |
| `IsEmitting` | `Boolean` | read | Whether continuous emission is enabled |
| `Count` | `Integer` | read | Number of live particles |
| `Color` | `Integer` | read/write | Color assigned to newly emitted particles |
| `FadeOut` | `Boolean` | read/write | Scale alpha by remaining lifetime |
| `Shrink` | `Boolean` | read/write | Scale size by remaining lifetime |

### Methods

| Method | Return | Behavior |
|---|---|---|
| `SetPosition(x, y)` | `Void` | Set the origin for future particles |
| `SetLifetime(min, max)` | `Void` | Configure a random inclusive lifetime range in updates |
| `SetVelocity(minSpeed, maxSpeed, minAngle, maxAngle)` | `Void` | Configure random launch speed and angle ranges |
| `SetGravity(gx, gy)` | `Void` | Set acceleration applied after each position update |
| `SetSize(min, max)` | `Void` | Configure random initial diameters |
| `Start()` | `Void` | Enable continuous emission |
| `Stop()` | `Void` | Disable continuous emission without removing live particles |
| `Burst(count)` | `Void` | Emit immediately, up to the remaining capacity |
| `Update()` | `Void` | Move, age, remove, and then continuously emit particles |
| `Clear()` | `Void` | Remove all particles and clear the fractional rate accumulator |
| `ParticleAt(index)` | `Zanna.Option` | Snapshot the indexed live particle; invalid indices return `None` |
| `Draw(canvas)` | `Integer` | Draw at absolute positions and return the number submitted |
| `DrawAt(canvas, offsetX, offsetY)` | `Integer` | Draw with an integer offset and return the number submitted |
| `DrawToPixels(pixels, offsetX, offsetY)` | `Integer` | Straight-alpha source-over draw into a Pixels buffer |
| `Destroy()` | `Void` | Release this handle's reference |

Angles are degrees counter-clockwise from positive X; 90 degrees launches upward because screen Y
normally increases downward. Angles outside one revolution are accepted but are not explicitly
normalized before the trigonometric conversion.

Configuration is normalized as follows:

- `Rate` is clamped to 0–1024; negative and non-finite values become zero.
- Non-finite position components leave that component unchanged. Non-finite gravity components
  become zero.
- Lifetime minimum is raised to 1; maximum is raised to the resulting minimum.
- Negative/non-finite minimum speed becomes zero. An invalid maximum becomes the minimum.
- Size is clamped to 0.1–2,000,000 pixels and the maximum is never below the minimum.
- Non-positive burst counts do nothing.

`Update()` first advances existing particles, then emits new ones. A newly emitted particle is
therefore available for a full render interval before its first age decrement. If the pool was
full at the start of an update, continuous emission skips that update even if particles expire
during it; accumulated backlog is discarded rather than released later as a burst. `Stop()` keeps
the current fractional accumulator, while `Clear()` resets it.

### Particle snapshots

`ParticleAt()` returns a non-generic `Zanna.Option` whose `Some` payload is a
`Zanna.Game.ParticleSnapshot` with read-only `X`, `Y`, `Size`, and `Color` properties. The snapshot
color is effective `0xAARRGGBB` after fade has been applied. Because the Option signature does not
encode its payload type, low-level callers can use the qualified accessors
`Zanna.Game.ParticleSnapshot.get_X(option.Value)` and so on.

An out-of-range index returns `None`. If snapshot construction returns null under a recovering
runtime trap handler, that null is also collapsed to `None`; ordinary allocation failure first
raises the runtime allocation trap. Failure to allocate the enclosing `Some` follows the Option
allocator's trap/null contract rather than becoming `None`.

`ParticleAt(i)` scans the fixed pool to find the *i*th live particle. Repeatedly querying every
particle can therefore be quadratic in the configured capacity. Prefer `Draw`, `DrawAt`, or
`DrawToPixels` for normal rendering.

### Color and drawing details

Particle colors use `0xAARRGGBB`. For compatibility, an untagged `0x00RRGGBB` value is drawn as
fully opaque. A tagged `Color.Rgba(r, g, b, 0)` remains explicitly transparent. `DrawToPixels`
converts to the Pixels `0xRRGGBBAA` storage convention and performs straight-alpha source-over
compositing, preserving destination alpha.

The three draw methods return the number of particles submitted, not the number of visible pixels.
Off-screen particles still count; particles whose positions cannot be converted to integers do
not. Rendered radius is half the configured size, truncated to an integer and clamped to
1–1,000,000 pixels.

### Zia example

```zia
module ParticleDemo;

bind Zanna.Game.ParticleEmitter as PE;
bind Zanna.Graphics.Color as Color;
bind Zanna.Terminal;

func start() {
    var emitter = PE.New(200);
    emitter.SetPosition(400.0, 300.0);
    emitter.SetLifetime(20, 40);
    emitter.SetVelocity(2.0, 8.0, 0.0, 360.0);
    emitter.SetGravity(0.0, 0.1);
    emitter.SetSize(3.0, 6.0);
    emitter.Color = Color.Rgba(255, 160, 0, 255);
    emitter.FadeOut = true;
    emitter.Shrink = true;

    emitter.Burst(50);
    emitter.Update();
    SayInt(emitter.Count);

    emitter.Rate = 0.5;
    emitter.Start();
    emitter.Update();
    emitter.Update(); // one continuously emitted particle across these two updates
    emitter.Stop();
    SayInt(emitter.Count);

    emitter.Clear();
    emitter.Destroy();
}
```

### BASIC rendering sketch

```basic
DIM canvas AS OBJECT = Zanna.Graphics.Canvas.New("Particles", 800, 600)
DIM cameraX AS INTEGER = 0
DIM cameraY AS INTEGER = 0
DIM explosion AS OBJECT = Zanna.Game.ParticleEmitter.New(200)
explosion.SetPosition(400.0, 300.0)
explosion.SetLifetime(20, 40)
explosion.SetVelocity(2.0, 8.0, 0.0, 360.0)
explosion.SetGravity(0.0, 0.1)
explosion.SetSize(3.0, 6.0)
explosion.Color = 4294927872  ' 0xFFFF6600: opaque orange in particle ARGB
explosion.FadeOut = 1
explosion.Shrink = 1
explosion.Burst(100)

' Each game frame:
explosion.Update()
explosion.DrawAt(canvas, -cameraX, -cameraY)
END
```

---

## Zanna.Game.ScreenFX

`ScreenFX` manages camera shake, flash/fade overlays, and Canvas-rendered transitions. Durations
and `Update(dt)` use integer milliseconds. Shake intensity and offsets use fixed-point pixels where
1000 units equal one pixel.

- **Type:** instance handle
- **Constructor:** `ScreenFX.New()`
- **Cleanup:** the registry inventories `Zanna.Game.ScreenFX.Destroy(fx)` as a static
`void(obj)` target, although current Zia also accepts receiver syntax `fx.Destroy()`

### Properties

| Property | Type | Meaning |
|---|---|---|
| `IsActive` | `Boolean` | At least one effect slot is active |
| `IsFinished` | `Boolean` | No effect of any kind is active |
| `ShakeX`, `ShakeY` | `Integer` | Latest fixed-point shake offsets |
| `OverlayColor` | `Integer` | Winning overlay RGB in the upper 24 bits (`0xRRGGBB00`) |
| `OverlayAlpha` | `Integer` | Winning overlay alpha, 0–255 |
| `TransitionProgress` | `Integer` | Progress of the first active transition slot |

### Methods

| Method | Return | Behavior |
|---|---|---|
| `Update(dt)` | `Void` | Advance all active effects by positive milliseconds |
| `Shake(intensity, duration, decay)` | `Void` | Start or replace the one active shake |
| `Flash(color, duration)` | `Void` | Add a raw-RGBA flash that fades to clear |
| `FadeIn(color, duration)` | `Void` | Fade from the raw-RGBA color to clear |
| `FadeOut(color, duration)` | `Void` | Fade from clear to the raw-RGBA color |
| `CancelAll()` | `Void` | Remove all effects and clear cached shake/overlay output |
| `CancelType(type)` | `Void` | Remove every slot with the numeric effect type |
| `IsTypeActive(type)` | `Boolean` | Query a numeric effect type |
| `Wipe(direction, color, duration)` | `Void` | Add an edge wipe using a Canvas RGB color |
| `CircleIn(cx, cy, color, duration)` | `Void` | Add the closing-iris approximation |
| `CircleOut(cx, cy, color, duration)` | `Void` | Add the opening-iris approximation |
| `Dissolve(color, duration)` | `Void` | Add a 4×4 ordered-dither dissolve |
| `Pixelate(maxBlock, duration)` | `Void` | Add the current dark-grid pixelation approximation |
| `Draw(canvas, width, height)` | `Void` | Draw cached flash/fade output and every active transition |
| `Rgba(r, g, b, a)` | `Integer` | Static. Pack an overlay color as `0xRRGGBBAA` for `Flash`/`FadeIn`/`FadeOut` |
| `Rgb(r, g, b)` | `Integer` | Static. Pack a transition color as `0x00RRGGBB` for `Wipe`/`Circle*`/`Dissolve` |
| `Destroy()` | `Void` | Release the handle (instance method; the static `ScreenFX.Destroy(fx)` form also works) |

Non-positive durations are ignored. `Update()` ignores non-positive `dt` without recomputing the
cached shake or overlay. Elapsed addition saturates at the integer maximum.

The manager initially reserves eight effect slots and grows its array when necessary. There is no
normal eight-effect ceiling. If growth allocation fails, the newly requested effect is silently
dropped.

### Effect and direction constants

The class exposes stable named constants for `IsTypeActive`/`CancelType` and the `Wipe` direction,
so callers never copy raw integers or guess the value. Prefer these over literals — `TypeShake`
is `1`, not `0` (`0` is the internal "none" sentinel, which selects no effect).

| Effect constant | Value | Effect constant | Value |
|---|---:|---|---:|
| `TypeShake` | 1 | `TypeCircleIn` | 6 |
| `TypeFlash` | 2 | `TypeCircleOut` | 7 |
| `TypeFadeIn` | 3 | `TypeDissolve` | 8 |
| `TypeFadeOut` | 4 | `TypePixelate` | 9 |
| `TypeWipe` | 5 | | |

| Direction constant | Value | Visual growth |
|---|---:|---|
| `DirLeft` | 0 | left edge toward right |
| `DirRight` | 1 | right edge toward left |
| `DirUp` | 2 | top edge toward bottom |
| `DirDown` | 3 | bottom edge toward top |

Read them off the class, e.g. `fx.IsTypeActive(ScreenFX.TypeShake)` or
`fx.Wipe(ScreenFX.DirLeft, color, 500)`. An invalid direction is treated as `DirLeft`.

### Shake behavior

Negative intensity is clamped to zero. Retriggering shake replaces the existing shake slot; other
effect types continue running. Each positive update computes new random X/Y offsets in the range
allowed by the current intensity.

| `decay` | Falloff |
|---:|---|
| `<= 0` | constant intensity |
| `1` through `1499` | linear remaining-time factor |
| `>= 1500` | quadratic remaining-time factor |

For example, `Shake(5000, 300, 2000)` is a shake of at most about five pixels for 300 ms with
quadratic falloff. The random generator is seeded from Zanna's active runtime RNG, so
`RANDOMIZE`/runtime seeding controls reproducibility.

### Flash and fades

Flash, FadeIn, and FadeOut take raw `0xRRGGBBAA`; their low byte is the peak alpha. Supplying a
Canvas RGB such as `0xFF0000` makes alpha zero and produces no visible red overlay. A new FadeIn or
FadeOut cancels both existing fade types. Flashes do not replace one another. If several overlays
are active, the slot with the greatest current alpha supplies both `OverlayColor` and
`OverlayAlpha`; the earlier slot wins ties.

ScreenFX colors deliberately use their own byte orders — `0xRRGGBBAA` for overlays and `0x00RRGGBB`
for transitions — which are **not** the canonical `Zanna.Graphics.Color` order (`0xAARRGGBB`).
Passing a `Color.Rgba()` value straight into `Flash` reads the wrong alpha channel and shifts the
color. Build ScreenFX colors with the in-namespace constructors so the encoding is explicit:
`ScreenFX.Rgba(r, g, b, a)` for `Flash`/`FadeIn`/`FadeOut`, and `ScreenFX.Rgb(r, g, b)` for the
transitions. Both clamp each channel to `[0, 255]`.

Starting one of these effects changes `IsActive` immediately, but its cached overlay is not
computed until the next positive `Update()`. `Draw()` already renders the cached overlay, so do not
also draw `OverlayColor` manually unless you intentionally want a second blend.

`CancelType()` removes matching slots and immediately recomputes the composited shake and overlay
from the surviving effects, so a canceled flash/fade cannot keep drawing its cached overlay and a
canceled shake cannot leave a stale camera offset. `CancelAll()` clears everything at once.

### Transitions and completion

Transition colors use Canvas `0x00RRGGBB`, not flash/fade raw RGBA. Multiple transitions can run
and draw concurrently. `TransitionProgress` reports the first active transition in slot order, not
the newest or the longest-running one.

Rendering notes:

- `CircleIn`/`CircleOut` fill the overlay outside a genuinely circular opening, scanline by
  scanline, using the Euclidean distance to the farthest corner as the maximum radius. Cost is
  proportional to screen height each draw.
- `Dissolve` tests every screen pixel against a repeated 4×4 Bayer matrix, so its cost is
  proportional to `width × height` each draw.
- `Pixelate` is a stylized mosaic-grid approximation, not true block-averaging. The runtime has no
  Canvas read-back path, so it cannot sample and average the underlying image into larger blocks;
  it instead draws an increasingly spaced, increasingly dark one-pixel grid. Choose `Dissolve` or a
  circle/wipe when you need a faithful covering transition.

The lifecycle separates "finished advancing" from "removed" so the final frame is always drawable.
The update that reaches `elapsed >= duration` clamps the effect to its exact end (progress 1000)
and holds it one extra frame in a terminal state — still active, still drawn — before the slot is
reclaimed on the following `Update()`. A FadeOut, closing wipe, circle-in, or dissolve therefore
exposes its fully covered terminal frame, and `TransitionProgress` reads 1000 on that frame before
returning 0. Waiting for `IsFinished` to become true is safe: it only reports true after the
covered frame has been drawn, so a scene swap gated on it never flashes the underlying scene.

### Zia example

```zia
module ScreenFXDemo;

bind Zanna.Game.ScreenFX as FX;
bind Zanna.Terminal;

func start() {
    var fx = FX.New();

    fx.Shake(5000, 300, 2000);
    fx.Flash(FX.Rgba(255, 0, 0, 255), 200); // opaque red overlay (0xRRGGBBAA)
    fx.Update(16);

    SayBool(fx.IsTypeActive(FX.TypeShake)); // named constant, not raw 1
    SayInt(fx.ShakeX);                      // fixed-point: divide by 1000 for pixels
    SayInt(fx.OverlayAlpha);

    fx.CancelType(FX.TypeShake); // CancelType recomputes cached shake/overlay immediately
    SayInt(fx.ShakeX);           // already 0 — no stale offset survives cancellation

    fx.CancelAll();
}
```

### BASIC drawing example

```basic
DIM canvas AS OBJECT = Zanna.Graphics.Canvas.New("ScreenFX", 800, 600)
DIM fx AS OBJECT = Zanna.Game.ScreenFX.New()

' On player damage: raw 0xRRGGBBAA colors.
fx.Shake(5000, 300, 2000)
fx.Flash(4278190335, 200)  ' 0xFF0000FF: opaque red

' In the frame loop:
fx.Update(16)
DIM cameraOffsetX AS INTEGER = fx.ShakeX / 1000
DIM cameraOffsetY AS INTEGER = fx.ShakeY / 1000
fx.Draw(canvas, 800, 600)  ' Draws flash/fade overlays and transitions
END
```

### Transition sketch

```zia
module TransitionSketch;

bind Zanna.Game.ScreenFX as FX;
bind Zanna.Graphics.Canvas as Canvas;

func start() {
    var screenWidth = 800;
    var screenHeight = 600;
    var playerX = 400;
    var playerY = 300;
    var canvas = Canvas.New("Transitions", screenWidth, screenHeight);
    var fx = FX.New();

    // Flash/fades use raw RGBA, but transition colors use Canvas RGB.
    fx.FadeOut(0x000000FF, 500);
    fx.Wipe(1, 0x000000, 500); // grow black from the right edge
    fx.CircleIn(playerX, playerY, 0x000000, 800);
    fx.Dissolve(0x000000, 1200);
    fx.Pixelate(16, 600);

    // One representative frame, after drawing the scene.
    fx.Update(16);
    fx.Draw(canvas, screenWidth, screenHeight);
}
```

---

## Zanna.Game.Lighting2D

`Lighting2D` draws a full-canvas tinted darkness overlay followed by a pulsing player glow,
world-space dynamic glows, and one-frame screen-space tile glows.

- **Type:** instance class
- **Constructor:** `Lighting2D.New(maxDynamicLights)`
- **Dynamic capacity:** constructor value clamped to 0–128
- **Tile-light capacity:** 128 per draw, independent of dynamic capacity

### Properties

| Property | Type | Access | Meaning |
|---|---|---|---|
| `Darkness` | `Integer` | read/write | Full-screen overlay alpha, clamped to 0–255 |
| `TintColor` | `Integer` | read/write | Overlay RGB; only the low 24 bits are retained |
| `LightCount` | `Integer` | read | Active dynamic lights; excludes player and tile lights |
| `PlayerRadius` | `Integer` | read | Player light base radius; `0` means the player light is disabled |

### Methods

| Method | Return | Behavior |
|---|---|---|
| `SetPlayerLight(radius, color)` | `Void` | Set the always-present player glow's base radius and RGB |
| `AddLight(x, y, radius, color, lifetime)` | `Void` | Add a world-space dynamic glow |
| `AddTileLight(screenX, screenY, radius, color)` | `Void` | Queue a screen-space glow for the next `Draw()` |
| `ClearLights()` | `Void` | Clear both dynamic and queued tile lights |
| `Update()` | `Void` | Advance the player pulse and dynamic lifetimes by one tick |
| `Draw(canvas, camX, camY, playerScreenX, playerScreenY)` | `Void` | Draw overlay and glows, then consume tile lights |
| `Destroy()` | `Void` | Release this handle's reference |

Defaults are darkness 0, tint `0x00000A`, player radius 180, and player color `0x303240`.
`SetPlayerLight` clamps a non-positive radius to zero and masks color to RGB. Dynamic/tile calls
ignore non-positive radii; `AddLight` also ignores negative lifetimes.

Dynamic lifetime is measured in `Update()` calls. Zero means permanent. Positive lifetimes are
decremented immediately on each update and expire when they reach zero; their glow alpha scales
with remaining lifetime. A lifetime-1 light added before `Update()` is therefore removed before it
can be drawn. A predictable frame order is:

1. Call `lighting.Update()`.
2. Add dynamic and visible tile lights for this frame.
3. Draw the scene.
4. Call `lighting.Draw(...)` last.

When the dynamic pool is full, `AddLight` silently drops the new light; it does not evict an older
one. Tile lights are also silently dropped after 128 have been queued. `Draw()` consumes all queued
tile lights even when `Darkness` is zero or the Canvas is null. World-space dynamic positions are
converted with `screen = world - camera`; tile and player positions are already screen-space.

### Current rendering limitation

Despite the “light” terminology, the current implementation does **not** subtract the darkness
overlay or reveal the already-drawn scene. Canvas alpha primitives perform source-over blending,
so Lighting2D first darkens the scene and then blends colored discs on top. These can look like
glows, but they are not light holes.

The player glow uses a 120-update triangle pulse that adds 0–10 pixels to its base radius. Setting
the player radius to zero disables the player light entirely — `Draw` skips every player-light pass,
including the 40-pixel outer glow (VDOC-271). Read `PlayerRadius` to check the state; `0` means the
player light is off while darkness and the other lights keep working.

### Zia example

```zia
module LightingDemo;

bind Zanna.Game.Lighting2D as Lighting2D;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;

func start() {
    var canvas = Canvas.New("Lighting", 800, 600);
    var lighting = Lighting2D.New(32);
    lighting.Darkness = 160;
    lighting.TintColor = Color.Rgb(0, 0, 10);
    lighting.SetPlayerLight(120, Color.Rgb(255, 230, 160));

    // Recommended frame order.
    lighting.Update();
    lighting.AddLight(500, 300, 90, Color.Rgb(255, 120, 40), 8); // world space
    lighting.AddTileLight(200, 150, 50, Color.Rgb(80, 180, 255)); // screen space

    canvas.Clear(Color.Rgb(30, 30, 40));
    // Draw world and entities here.
    lighting.Draw(canvas, 100, 0, 400, 300);
    canvas.Flip();

    lighting.Destroy();
}
```

---

## See Also

- [Core Utilities](core.md)
- [Physics & Collision](physics.md)
- [Animation & Movement](animation.md)
- [Canvas](../graphics/canvas.md)
- [Game Utilities Overview](README.md)
- [Generated Game API](../../generated/runtime/game.md)
