---
status: active
audience: public
last-verified: 2026-07-26
---

# Game

> 2D game primitives: physics, scenes, cameras, tilemaps, particles, animations, pathfinding, and more.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Graphics.Camera](#zannagraphicscamera)
- [Zanna.Game2D.SceneDocument](#zannagame2dscenedocument)
- [Zanna.Graphics2D.Tilemap](#zannagraphics2dtilemap)
- [Zanna.Game.Physics2D class family](#zannagamephysics2d-class-family)
- [Zanna.Game.Quadtree](#zannagamequadtree)
- [Zanna.Game.ParticleEmitter](#zannagameparticleemitter)
- [Zanna.Graphics.SpriteBatch](#zannagraphicsspritebatch)
- [Zanna.Game.SpriteAnimation](#zannagamespriteanimation)
- [Zanna.Game.StateMachine](#zannagamestatemachine)
- [Zanna.Game.ObjectPool](#zannagameobjectpool)
- [Zanna.Game.PathFollower](#zannagamepathfollower)
- [Zanna.Game.Timer](#zannagametimer)
- [Zanna.Game.Tween](#zannagametween)
- [Zanna.Game.SmoothValue](#zannagamesmoothvalue)
- [Zanna.Game.ButtonGroup](#zannagamebuttongroup)
- [Zanna.Game.ScreenFX](#zannagamescreenfx)
- [Zanna.Game.DebugOverlay](#zannagamedebugoverlay)
- [Zanna.Game.Collision](#zannagamecollision)
- [Zanna.Game.WorldToScreenProjection](#zannagameworldtoscreenprojection)
- [Zanna.Game.Grid2D](#zannagamegrid2d)
- [Zanna.Game.Lighting2D](#zannagamelighting2d)
- [Zanna.Game.PlatformerController](#zannagameplatformercontroller)
- [Zanna.Game.AchievementTracker](#zannagameachievementtracker)
- [Zanna.Game.Typewriter](#zannagametypewriter)
- [Zanna.Game.UI.*](game/ui.md) — Label, Bar, Panel, NineSlice, MenuList, Dialogue (in-game UI widgets)
- [Zanna.Game.Pathfinder](game/pathfinding.md) — A* grid pathfinding for AI navigation
- [Current Limits](#current-limits)
- [Coordinate Systems](#coordinate-systems)

---

## Zanna.Graphics.Camera

The 2D camera is part of `Zanna.Graphics`, not `Zanna.Game`. Its current API covers
position, center, viewport size, zoom, rotation, following and movement, per-axis
world/screen conversion, movement bounds, dead zones, smooth following, and parallax
layers.

**Type:** Instance (obj)

**Constructor:** `Camera.New(width, height)`

See [Scene & Camera](graphics/scene.md#zannagraphicscamera) for usage and the
[generated API reference](../generated/runtime/graphics.md#zanna-graphics-camera) for the
complete live surface.

---

## Zanna.Game2D.SceneDocument

Editable JSON scene document for IDE scene tools, tile layers, placed objects,
typed scalar properties, diagnostics, asset references, and render/collision
Tilemap copies.

**Type:** Instance (obj)

**Constructor:** `SceneDocument.New(width, height, tileWidth, tileHeight)`

**Detailed docs:** [Editable Scene Documents](game/scene.md)

Core methods include `LoadJsonResult`, `LoadResult`, Tiled JSON/TMX
`ImportTiledResult`/`ImportTiledAssetResult`, their compatibility object-returning
forms, `ToJson`, `Save`, `HasErrors`, `DiagnosticRecords`, typed scene/object
property accessors, `AssetDescriptors`, `AssetPaths`, and `BuildTilemap`.

Prefer `LoadJsonResult` and `LoadResult` when application code needs a clear
success/failure value. The legacy `LoadJson` and `Load` methods still return
diagnostic documents instead of trapping on ordinary user input errors such as
malformed JSON, missing files, unsupported versions, invalid dimensions, and
tile-count mismatches. `Diagnostics()` remains a compatibility `Seq<str>`;
`DiagnosticRecords()` returns structured maps.

Tile ID `0` is reserved for empty space. Tile ID `N > 0` maps to tileset frame
`N - 1` when rendered by `Tilemap`. The scene document is the serialization
source of truth; a `BuildTilemap()` result is a separate render/collision copy.

---

## Zanna.Graphics2D.Tilemap

Tilemap rendering and collision live in `Zanna.Graphics2D.Tilemap`. Game scene
documents can build isolated Tilemap render/collision copies with
`SceneDocument.BuildTilemap()`.

**Detailed docs:** [Tilemaps and 2D Rendering](graphics/pixels.md#zannagraphics2dtilemap)

---

## Zanna.Game.Physics2D class family

A 2D rigid-body physics engine. Bodies can be static (immovable) or dynamic. Collision
detection uses an axis-aligned broad phase followed by AABB/circle narrow-phase and swept
collision checks for fast bodies.

**Type:** Instance (obj)
**Constructor:** `Physics2D.World.New(gravityX, gravityY)`

Bodies are objects, not integer handles. Create a `Physics2D.Body`, add it to a world,
then read and write its properties directly. The full API — world, body, joint, and
projectile surfaces — is documented in
[Physics & Collision](game/physics.md#zannagamephysics2d-class-family).

### World Surface (Summary)

| Member | Description |
|---|---|
| `BodyCount`, `JointCount`, `ContactCount` | Read-only counts |
| `Step(dt)` | Advance the simulation by `dt` seconds |
| `Add(body)` / `Remove(body)` | Attach or detach a `Physics2D.Body` |
| `AddJoint(joint)` / `RemoveJoint(joint)` | Attach or detach a joint |
| `SetGravity(gx, gy)` | Change the gravity vector |
| `ContactBodyA/B(i)`, `ContactNX/NY(i)`, `ContactDepth(i)` | Inspect contacts recorded by the last `Step` |

### Collision Layers and Masks

Physics2D uses a 64-bit layer/mask bitfield system, exposed as body properties:

- **`CollisionLayer`**: which bit (0-63) this body occupies. New bodies default to bit `1`.
- **`CollisionMask`**: bitmask of layers this body will collide with. New bodies default
  to `-1` (all 64 bits set).

To create non-colliding groups:

```zia
// Player on layer 0, enemies on layer 1, terrain on layer 2
player.CollisionLayer = 0;
player.CollisionMask = 0b110;  // collides with enemies + terrain, not self

enemy.CollisionLayer = 1;
enemy.CollisionMask = 0b101;   // collides with player + terrain, not other enemies
```

> **Layer 63 note:** The default mask is `-1` (all 64 bits set). A body placed on layer 63
> will collide with any body whose default mask is used.

### Current Limits

| Limit | Value | Notes |
|---|---|---|
| Bodies | Dynamic | `PH_MAX_BODIES` is the initial reservation |
| Joints | Dynamic | `PH_MAX_JOINTS` is the initial reservation |
| Contacts | Dynamic | `PH_MAX_CONTACTS` is the initial reservation; `ContactOverflowed()` reports allocation failure |
| Broad-phase grid | Adaptive 8×8 / 12×12 / 16×16 | Per-world step scratch |
| `BPG_CELL_MAX` | 32 | Dense-cell overflow falls back to an exhaustive pair pass |

---

## Zanna.Game.Quadtree

A spatial quadtree for O(log n) range queries. Insert entities by ID with their AABB, then
query a rectangle to find all overlapping IDs.

**Type:** Instance (obj)
**Constructor:** `Quadtree.New(x, y, w, h)` — defines the world bounds

### Properties

| Property | Type | Description |
|---|---|---|
| `ItemCount` | `Integer` | Total items in the tree |

### Methods

| Method | Signature | Description |
|---|---|---|
| `Insert(id, x, y, w, h)` | `Boolean(Integer,Integer,Integer,Integer,Integer)` | Insert an AABB; returns false if the ID is a duplicate or the AABB is out of bounds |
| `Update(id, x, y, w, h)` | `Boolean(Integer,Integer,Integer,Integer,Integer)` | Move an existing item's AABB; returns false if the ID is unknown |
| `Remove(id)` | `Boolean(Integer)` | Remove by ID; returns false if the ID is unknown |
| `QueryRect(x, y, w, h)` | `QueryResult(Integer,Integer,Integer,Integer)` | Query overlapping items as a stable result snapshot |
| `QueryPoint(x, y, radius)` | `QueryResult(Integer,Integer,Integer)` | Query a circular area as a stable result snapshot |
| `QueryPairs()` | `QuadtreePairResult()` | Collect broad-phase collision pairs as a stable snapshot |
| `Clear()` | `Void()` | Remove every item, keeping the world bounds |
| `Destroy()` | `Void()` | Free the quadtree |

### Query and Pair Results

Queries return immutable result objects that stay valid even after the quadtree is
queried or mutated again.

`Zanna.Game.QueryResult` — returned by `QueryRect` and `QueryPoint`:

| Member | Type | Description |
|---|---|---|
| `Count` | `Integer` | Number of matching IDs |
| `Truncated` | `Boolean` | True when result storage could not grow and the snapshot is incomplete |
| `GetId(i)` | `Integer(Integer)` | The i-th matching ID |
| `Contains(id)` | `Boolean(Integer)` | Whether an ID is in the snapshot |
| `Ids()` | `Seq()` | All matching IDs as a sequence |

`Zanna.Game.QuadtreePairResult` — returned by `QueryPairs`:

| Member | Type | Description |
|---|---|---|
| `Count` | `Integer` | Number of overlapping pairs |
| `Truncated` | `Boolean` | True when pair storage could not grow |
| `First(i)` | `Integer(Integer)` | First ID of the i-th pair |
| `Second(i)` | `Integer(Integer)` | Second ID of the i-th pair |

```zia
var result = tree.QueryRect(x, y, w, h);
if result.Truncated {
    // Handle allocation pressure: results are incomplete.
}
var i = 0;
while i < result.Count {
    HandleEntity(result.GetId(i));
    i = i + 1;
}
```

### Duplicate ID Guard

Inserting the same ID twice returns 0 and leaves the tree unchanged. Use distinct IDs
(e.g., entity indices) for all inserts.

### Current Limits

| Limit | Value | Notes |
|---|---|---|
| Query results | Dynamic | `RT_QUADTREE_MAX_RESULTS` is the initial reservation; `QueryResult.Truncated` reports allocation failure |
| Total items | Dynamic | Item storage grows on insert; duplicate and out-of-bounds inserts return false |
| Overlap pairs | Dynamic | Pair storage grows from the historical 1024-slot reservation; `QuadtreePairResult.Truncated` reports allocation failure |

---

## Zanna.Game.ParticleEmitter

The current emitter supports configured continuous emission and bursts, particle snapshots,
Canvas/Pixels drawing, and explicit cleanup. See [Effects](game/effects.md#zannagameparticleemitter)
for the complete API and examples.

---

## Zanna.Graphics.SpriteBatch

Sprite batching is part of `Zanna.Graphics`, not `Zanna.Game`. A batch is opened with
`Begin()`, populated with the `Draw*` or `DrawAtlas*` methods, and flushed with `End()`.
It also supports depth sorting, tint, and alpha settings.

**Type:** Instance (obj)

**Constructor:** `SpriteBatch.New()`

See [Scene & Camera](graphics/scene.md#zannagraphicsspritebatch) and the
[generated API reference](../generated/runtime/graphics.md#zanna-graphics-spritebatch).

---

## Zanna.Game.SpriteAnimation

Frame-based animation with configurable frame duration, looping, ping-pong playback,
speed, pause/resume, and read-only playback-state properties such as `Frame` and
`IsFinished`.

**Type:** Instance (obj)

**Constructor:** `SpriteAnimation.New()`

See [Animation](game/animation.md#zannagamespriteanimation) and the
[generated API reference](../generated/runtime/game.md#zanna-game-spriteanimation).

---

## Zanna.Game.StateMachine

A finite state machine with up to **256 numeric states**. Supports one-frame edge flags
(`JustEntered`/`JustExited`) for triggering enter/exit logic, a per-state frame counter,
and transition validation. Traps if the state limit is exceeded.

**Type:** Instance (obj)
**Constructor:** `StateMachine.New()`

### Properties

| Property | Type | Description |
|---|---|---|
| `Current` | Integer | Current state ID (-1 if none set) |
| `Previous` | Integer | Previous state ID (-1 if none) |
| `JustEntered` | Boolean | True on the frame a state was entered |
| `JustExited` | Boolean | True on the frame a state was exited |
| `FramesInState` | Integer | Frames since entering current state (incremented by `Update()`) |
| `StateCount` | Integer | Number of registered states |

### Methods

| Method | Signature | Description |
|---|---|---|
| `AddState(id)` | `Boolean(Integer)` | Register a state ID. Returns false if duplicate |
| `SetInitial(id)` | `Boolean(Integer)` | Set initial state (sets `JustEntered` flag) |
| `Transition(id)` | `Boolean(Integer)` | Transition to a registered state. Sets edge flags, resets frame counter |
| `IsState(id)` | `Boolean(Integer)` | True if current state equals `id` |
| `HasState(id)` | `Boolean(Integer)` | True if state `id` is registered |
| `Update()` | `Void()` | Increment `FramesInState` counter (call once per frame) |
| `ClearFlags()` | `Void()` | Clear `JustEntered` and `JustExited` flags |

`FramesInState` saturates at the maximum integer value instead of overflowing during very
long-running states.

### Example

```zia
bind Zanna.Game;

// Register states and set initial
var sm = StateMachine.New();
sm.AddState(0);  // MENU
sm.AddState(1);  // PLAYING
sm.AddState(2);  // PAUSED
sm.SetInitial(0);

// In game loop:
sm.Update();
sm.ClearFlags();

if sm.IsState(0) {
    // Menu logic...
    if startPressed { sm.Transition(1); }
} else if sm.IsState(1) {
    // Gameplay logic...
    if pausePressed { sm.Transition(2); }
}
```

**Reference implementation:** [Crackman](../../examples/games/crackman/game.zia) — uses StateMachine for app flow across gameplay, profile, and summary states.

### Current Limits

| Limit | Value | Notes |
|---|---|---|
| `RT_STATE_MAX` | 256 | Maximum states. Traps if exceeded |

---

## Zanna.Game.ObjectPool

A fixed-capacity pool for reusing integer slot IDs. Acquire a slot from the pool; release it
when done. Iteration over active slots is O(capacity) and returns stable slot indices.

**Type:** Instance (obj)
**Constructor:** `ObjectPool.New(capacity)`

### Methods

| Method | Signature | Description |
|---|---|---|
| `Acquire()` | `Integer()` | Get an inactive slot (-1 if pool exhausted) |
| `Release(slot)` | `Boolean(Integer)` | Return a slot to the pool |
| `FirstActive()` | `Integer()` | First active slot (-1 if none) |
| `NextActive(slot)` | `Integer(Integer)` | Next active slot after `slot` |
| `ActiveCount()` | `Integer()` | Number of active slots |
| `GetData(slot)` | `Integer(Integer)` | Get slot user data, or 0 if inactive or invalid |
| `SetData(slot, data)` | `Boolean(Integer, Integer)` | Associate user data with an active slot |

Releasing or clearing a slot clears its user data.

---

## Zanna.Game.PathFollower

Moves an object along waypoints using fixed-point coordinates where 1000 units equal one
world unit. Speed and playback mode are writable properties; position and playback state are
read-only properties. See [Animation](game/animation.md#zannagamepathfollower) for the
complete API and examples.

---

## Zanna.Game.Timer

A countdown timer supporting both frame-based and millisecond-based modes. Frame mode counts
frames (call `Update()` once per frame). Ms mode counts delta time (call `UpdateMs(dt)` with
the frame's delta time in ms) for frame-rate-independent timing.

**Type:** Instance (obj)
**Constructor:** `Timer.New()`

### Frame-Based Methods

| Method | Signature | Description |
|---|---|---|
| `Start(frames)` | `none(Integer)` | Start one-shot countdown in frames |
| `StartRepeating(frames)` | `none(Integer)` | Start repeating timer in frames |
| `Update()` | `Boolean()` | Advance by 1 frame; returns true on expiry |
| `Stop()` | `none()` | Stop the timer |
| `Reset()` | `none()` | Reset elapsed to 0 without changing state |

### Millisecond-Based Methods

| Method | Signature | Description |
|---|---|---|
| `StartMs(durationMs)` | `none(Integer)` | Start one-shot countdown in milliseconds |
| `StartRepeatingMs(intervalMs)` | `none(Integer)` | Start repeating timer in milliseconds |
| `UpdateMs(dt)` | `Boolean(Integer)` | Advance by dt ms; returns true on expiry |

### Properties

| Property | Type | Description |
|---|---|---|
| `IsRunning` | `Boolean` | True if timer is counting |
| `IsExpired` | `Boolean` | True if timer finished |
| `IsRepeating` | `Boolean` | True if auto-restarting |
| `Elapsed` | `Integer` | Frames elapsed (frame mode) |
| `Remaining` | `Integer` | Frames remaining (frame mode) |
| `ElapsedMs` | `Integer` | Milliseconds elapsed (ms mode) |
| `RemainingMs` | `Integer` | Milliseconds remaining (ms mode) |
| `Progress` | `Integer` | 0-100 completion percentage (both modes) |
| `Duration` | `Integer` | Total duration (read/write) |

---

## Zanna.Game.Tween

Frame-based numeric tweening supports floating-point or integer starts, 19 numeric easing
types, pause/resume, reset/stop, and read-only progress and value properties. There is no
registered `TweenChain` runtime class. See [Animation](game/animation.md#zannagametween) for
the complete current API and examples.

---

## Zanna.Game.SmoothValue

Smooths a value toward the writable `Target` property and exposes current value, velocity,
smoothing, and completion state as properties. See
[Game Core](game/core.md#zannagamesmoothvalue) for the complete API and examples.

---

## Zanna.Game.ButtonGroup

Manages a group of mutually exclusive button IDs (like radio buttons). At most one button
can be selected at a time. Button IDs are arbitrary integers, including `-1`; use
`HasSelection()` to distinguish no selection from selecting ID `-1`. Traps if the button limit is exceeded.

**Type:** Instance (obj)
**Constructor:** `ButtonGroup.New()`

### Methods

| Method | Signature | Description |
|---|---|---|
| `Add(id)` | `Integer(Integer)` | Register a button. Returns 0 if duplicate |
| `Remove(id)` | `Integer(Integer)` | Unregister a button. Clears selection if selected |
| `Has(id)` | `Integer(Integer)` | 1 if button is registered |
| `Count()` | `Integer()` | Number of registered buttons |
| `Select(id)` | `Integer(Integer)` | Select a button (deselects all others) |
| `ClearSelection()` | `none()` | Deselect all buttons |
| `Selected()` | `Integer()` | Currently selected button ID, or -1 |
| `IsSelected(id)` | `Integer(Integer)` | 1 if `id` is the current selection |
| `HasSelection()` | `Integer()` | 1 if any button is selected |
| `SelectionChanged()` | `Integer()` | 1 if selection changed since last frame |
| `ClearChangedFlag()` | `none()` | Reset the changed flag (call each frame) |
| `GetAt(index)` | `Integer(Integer)` | Button ID at position `index` |
| `SelectNext()` | `Integer()` | Advance selection forward (wraps) |
| `SelectPrevious()` | `Integer()` | Advance selection backward (wraps) |

### Current Limits

| Limit | Value | Notes |
|---|---|---|
| `RT_BUTTONGROUP_MAX` | 256 | Maximum buttons per group. Traps if exceeded |

**Reference implementation:** [Crackman](../../examples/games/crackman/game.zia) — uses ButtonGroup for menu navigation with `SelectNext()`/`SelectPrevious()` wraparound.

---

## Zanna.Game.ScreenFX

Post-process screen effects: camera shake, flash, fade-in, and fade-out.

**Type:** Instance (obj)
**Constructor:** `ScreenFX.New()`

> **Color format:** All color parameters use packed **`0xRRGGBBAA`** (alpha in the
> least-significant byte). This differs from the `0x00RRGGBB` format used by Canvas
> drawing methods.
> - Full-opacity white: `0xFFFFFFFF`
> - Half-opacity red: `0xFF000080`

### Properties

| Property | Type | Description |
|---|---|---|
| `IsActive` | `Boolean` | True while any effect is active |
| `ShakeX` | `Integer` | Current horizontal shake offset |
| `ShakeY` | `Integer` | Current vertical shake offset |
| `OverlayAlpha` | `Integer` | Current overlay alpha (0–255) |
| `OverlayColor` | `Integer` | Current overlay color |

### Methods

| Method | Signature | Description |
|---|---|---|
| `Shake(intensity, duration, decay)` | `none(Integer, Integer, Integer)` | Camera shake |
| `Flash(color, duration)` | `none(Integer, Integer)` | Flash overlay |
| `FadeIn(color, duration)` | `none(Integer, Integer)` | Fade from color to clear |
| `FadeOut(color, duration)` | `none(Integer, Integer)` | Fade from clear to color |
| `Update(dt)` | `none(Integer)` | Advance all active effects |
| `IsTypeActive(type)` | `Integer(Integer)` | 1 if the given effect type is active |
| `CancelAll()` | `none()` | Stop all effects immediately |
| `CancelType(type)` | `none(Integer)` | Stop effects of a specific type |

`Update(dt)` ignores non-positive `dt` values. Effect progress uses saturating arithmetic,
so very large elapsed durations clamp to completion instead of overflowing.

### Shake Decay Model

The `decay` parameter controls how quickly shake intensity falls off:

| `decay` value | Behavior |
|---|---|
| `0` | No decay — constant intensity throughout |
| `1000` | Linear decay — intensity falls proportionally to remaining time |
| `≥ 1500` | **Quadratic (trauma model)** — intensity falls as `(remaining_time)²`, producing a natural-feeling hard-stop near the end |

```zia
// Subtle hit feedback with quadratic falloff
fx.Shake(5000, 300, 2000);
// Earthquake: constant rumble for 3 seconds
fx.Shake(3000, 3000, 0);
```

---

## Zanna.Game.DebugOverlay

Real-time debug information overlay. Shows FPS, delta time, and custom watch variables in a semi-transparent panel. Toggle with a key binding during development.

**Type:** Instance (obj)
**Constructor:** `DebugOverlay.New()`

### Properties

| Property    | Type    | Access | Description                              |
|-------------|---------|--------|------------------------------------------|
| `IsEnabled` | Boolean | Read   | Whether the overlay is currently visible |
| `Fps`       | Integer | Read   | Current FPS (rolling 16-frame average)   |

### Methods

| Method              | Signature                   | Description                                     |
|---------------------|-----------------------------|-------------------------------------------------|
| `Enable()`          | `Void()`                    | Show the overlay                                |
| `Disable()`         | `Void()`                    | Hide the overlay                                |
| `Toggle()`          | `Void()`                    | Toggle visibility                               |
| `Update(dt)`        | `Void(Integer)`             | Update FPS tracking (call once per frame with dt in ms) |
| `Watch(name, value)`| `Void(String, Integer)`     | Add/update a named watch variable (max 16)      |
| `Unwatch(name)`     | `Void(String)`              | Remove a watch variable                         |
| `Clear()`           | `Void()`                    | Remove all watch variables                      |
| `Draw(canvas)`      | `Void(Canvas)`              | Render the overlay on top of everything         |

> FPS is color-coded: **green** (≥55), **yellow** (30–54), **red** (<30).

For full documentation see [Debug Overlay](game/debug.md).

---

## Zanna.Game.Collision

Static helpers for geometric overlap tests.

**All methods are static** (`Zanna.Game.Collision.*`)

| Method | Signature | Description |
|---|---|---|
| `RectsOverlap(ax,ay,aw,ah, bx,by,bw,bh)` | `Boolean(Number × 8)` | Whether two rectangles overlap |
| `PointInRect(px,py, rx,ry,rw,rh)` | `Boolean(Number × 6)` | Whether a point is inside a rectangle |
| `CirclesOverlap(ax,ay,ar, bx,by,br)` | `Boolean(Number × 6)` | Whether two circles overlap |
| `PointInCircle(px,py, cx,cy,cr)` | `Boolean(Number × 5)` | Whether a point is inside a circle |
| `CircleRect(cx,cy,cr, rx,ry,rw,rh)` | `Boolean(Number × 7)` | Whether a circle overlaps a rectangle |
| `LineRect(x1,y1,x2,y2, rx,ry,rw,rh)` | `Boolean(Number × 8)` | Whether a line segment crosses a rectangle |
| `LineCircle(x1,y1,x2,y2, cx,cy,cr)` | `Boolean(Number × 7)` | Whether a line segment crosses a circle |
| `Distance(x1,y1, x2,y2)` | `Number(Number × 4)` | Euclidean distance between two points |
| `DistanceSquared(x1,y1, x2,y2)` | `Number(Number × 4)` | Squared distance, avoiding the square root |

All arguments and results are floating point. Circle helpers reject non-positive or
non-finite radii. Point-in-circle and circle-rectangle checks include boundary contact
as a hit.

---

## Zanna.Game.Grid2D

A 2D integer grid. Stores one integer value per cell with get/set/fill operations.

**Type:** Instance (obj)
**Constructor:** `Grid2D.New(cols, rows, defaultValue)`

### Properties

| Property | Type | Description |
|---|---|---|
| `Width` | `Integer` | Number of columns |
| `Height` | `Integer` | Number of rows |

### Methods

| Method | Signature | Description |
|---|---|---|
| `Get(col, row)` | `Integer(Integer, Integer)` | Read cell value |
| `Set(col, row, value)` | `none(Integer, Integer, Integer)` | Write cell value |
| `Fill(value)` | `none(Integer)` | Set all cells to `value` |
| `InBounds(col, row)` | `Integer(Integer, Integer)` | 1 if position is within grid bounds |

---

## Zanna.Game.Lighting2D

A 2D darkness overlay system with a pulsing player light and pooled dynamic point lights.
Renders a full-screen tinted overlay, then punches bright "holes" for light sources. Suitable
for cave, dungeon, and horror game atmospheres.

**Type:** Instance (obj)
**Constructor:** `Lighting2D.New(maxDynamicLights)`

### Properties

| Property | Type | Description |
|---|---|---|
| `Darkness` | `Integer` | Overlay alpha (0 = fully lit, 255 = pitch black) |
| `TintColor` | `Integer` | Darkness tint color (0xRRGGBB) — e.g., blue for underwater |
| `LightCount` | `Integer` | Number of active dynamic lights |

### Methods

| Method | Signature | Description |
|---|---|---|
| `SetPlayerLight(radius, color)` | `none(Integer, Integer)` | Configure the player's light radius and color |
| `AddLight(x, y, radius, color, lifetime)` | `none(Integer, Integer, Integer, Integer, Integer)` | Add a dynamic light; lifetime 0 is permanent |
| `AddTileLight(screenX, screenY, radius, color)` | `none(Integer, Integer, Integer, Integer)` | Add a single-frame tile glow at screen position |
| `ClearLights()` | `none()` | Remove all dynamic lights |
| `Update()` | `none()` | Tick light lifetimes, advance player light pulse |
| `Draw(canvas, camX, camY, playerScreenX, playerScreenY)` | `none(Canvas, Integer, Integer, Integer, Integer)` | Render darkness overlay + all lights |

`maxDynamicLights` is clamped to 0-128. Non-positive light radii are ignored, darkness alpha
is clamped to 0-255, and light colors are masked to `0xRRGGBB`. `AddTileLight` stores a
screen-space light for the next `Draw` call and clears it after drawing; add tile lights after
`Update()` and before `Draw()` each frame.

---

## Zanna.Game.PlatformerController

Encapsulates standard 2D platformer input mechanics: jump buffering, coyote time, variable
jump height, and separate ground/air acceleration curves with apex gravity bonus. All timing
is ms-based for frame-rate independence.

**Type:** Instance (obj)
**Constructor:** `PlatformerController.New()`

### Configuration Methods

| Method | Signature | Description |
|---|---|---|
| `SetJumpBuffer(ms)` | `none(Integer)` | Grace window for early jump press (default: 100ms) |
| `SetCoyoteTime(ms)` | `none(Integer)` | Grace window after leaving ledge (default: 80ms) |
| `SetAcceleration(ground, air, decel)` | `none(Integer, Integer, Integer)` | Horizontal acceleration rates (x100 units) |
| `SetJumpForce(full, cut)` | `none(Integer, Integer)` | Full jump and early-release cut force (negative = up) |
| `SetMaxSpeed(normal, sprint)` | `none(Integer, Integer)` | Speed caps for walk and sprint (x100 units) |
| `SetGravity(gravity, maxFall)` | `none(Integer, Integer)` | Gravity strength and terminal velocity |
| `SetApexBonus(threshold, gravityPct)` | `none(Integer, Integer)` | Apex hang: reduce gravity when |vy| < threshold |

### Per-Frame Update

| Method | Signature | Description |
|---|---|---|
| `Update(dt, left, right, jumpPressed, jumpHeld, onGround, sprint)` | `none(Integer, Boolean, Boolean, Boolean, Boolean, Boolean, Boolean)` | Process input and compute velocities |

### Output Properties

| Property | Type | Description |
|---|---|---|
| `VelocityX` | `Integer` | Computed horizontal velocity (x100, read/write) |
| `VelocityY` | `Integer` | Computed vertical velocity (x100, read/write) |
| `ShouldJump` | `Boolean` | True when jump buffer + coyote resolve (one-shot, consumed on read) |
| `JumpForce` | `Integer` | Configured full jump force |
| `Facing` | `Integer` | 1 = right, -1 = left |
| `IsMoving` | `Boolean` | True when `VelocityX != 0` |

---

## Zanna.Game.AchievementTracker

Achievement system with bitmask-based unlock tracking, stat counters, and animated slide-in
notification popups. Supports up to 64 achievements and 32 stat counters. The unlock mask is
a single Integer suitable for save/load via SaveData.

**Type:** Instance (obj)
**Constructor:** `AchievementTracker.New(maxAchievements)`

### Properties

| Property | Type | Description |
|---|---|---|
| `Mask` | `Integer` | Bitmask of unlocked achievements (read/write for save/load) |
| `UnlockedCount` | `Integer` | Number of achievements unlocked |
| `TotalCount` | `Integer` | Number of achievements defined |
| `NotifyDuration` | `Integer` | Notification display time in ms (write-only, default: 3000) |
| `HasNotification` | `Boolean` | True when a notification popup is visible |

### Methods

| Method | Signature | Description |
|---|---|---|
| `Add(id, name, description)` | `none(Integer, String, String)` | Define an achievement |
| `Unlock(id)` | `Boolean(Integer)` | Unlock achievement; returns true if newly unlocked |
| `IsUnlocked(id)` | `Boolean(Integer)` | Check if achievement is unlocked |
| `IncrementStat(statId, amount)` | `none(Integer, Integer)` | Add to a stat counter |
| `GetStat(statId)` | `Integer(Integer)` | Read a stat counter |
| `SetStat(statId, value)` | `none(Integer, Integer)` | Set a stat counter directly |
| `Update(dt)` | `none(Integer)` | Tick notification timer |
| `Draw(canvas)` | `none(Canvas)` | Render notification popup (top-right corner) |

---

## Zanna.Game.Typewriter

Character-by-character text reveal effect for dialogue, lore terminals, tutorials, and
narrative sequences. Accumulates milliseconds and reveals one UTF-8 character per configured
interval. `Skip()` instantly reveals all remaining text.

**Type:** Instance (obj)
**Constructor:** `Typewriter.New()`

### Properties

| Property | Type | Description |
|---|---|---|
| `VisibleText` | `String` | Text revealed so far |
| `FullText` | `String` | Complete source text |
| `IsActive` | `Boolean` | True while revealing |
| `IsComplete` | `Boolean` | True when fully revealed |
| `Progress` | `Integer` | 0-100 reveal percentage |
| `CharCount` | `Integer` | Characters revealed |
| `TotalChars` | `Integer` | Total characters in source |

### Methods

| Method | Signature | Description |
|---|---|---|
| `Say(text, rateMs)` | `none(String, Integer)` | Start revealing text at rateMs per character |
| `Update(dt)` | `Boolean(Integer)` | Advance reveal; returns true on completion |
| `Skip()` | `none()` | Reveal all remaining text instantly |
| `Reset()` | `none()` | Clear text and state |

`Update(dt)` ignores non-positive `dt` values. Reveal progress is clamped to 0-100 and uses
saturating arithmetic for long-running reveals.

---

## Zanna.Game.WorldToScreenProjection

Stateless helpers for converting world coordinates to screen coordinates. Use these when a game
keeps simulation units separate from draw pixels and you want one consistent projection formula.

**Type:** Static helper class

| Method | Signature | Description |
|---|---|---|
| `LinearX(worldX, cameraX, originX, pixelsPerUnit)` | `Number(Number, Number, Number, Number)` | Linear world X to screen X |
| `LinearY(worldY, cameraY, originY, pixelsPerUnit, flipY)` | `Number(Number, Number, Number, Number, Boolean)` | Linear world Y to screen Y; `flipY=true` makes positive world Y go upward |
| `IsometricX(worldX, worldY, originX, tileWidth)` | `Number(Number, Number, Number, Number)` | Diamond isometric X using half tile width |
| `IsometricY(worldX, worldY, originY, tileHeight)` | `Number(Number, Number, Number, Number)` | Diamond isometric Y using half tile height |
| `PerspectiveScale(depth, nearDepth, farDepth, nearScale, farScale)` | `Number(Number, Number, Number, Number, Number)` | Clamped depth interpolation for faux-perspective sprite scale |
| `PerspectiveX(worldX, centerX, depth, focalLength)` | `Number(Number, Number, Number, Number)` | Simple perspective X projection |
| `PerspectiveY(worldY, centerY, depth, focalLength, flipY)` | `Number(Number, Number, Number, Number, Boolean)` | Simple perspective Y projection |

```zia
var sx = Zanna.Game.WorldToScreenProjection.LinearX(playerX, cameraX, 400.0, 16.0);
var sy = Zanna.Game.WorldToScreenProjection.LinearY(playerY, cameraY, 300.0, 16.0, true);

var isoX = Zanna.Game.WorldToScreenProjection.IsometricX(tileX, tileY, 400.0, 64.0);
var isoY = Zanna.Game.WorldToScreenProjection.IsometricY(tileX, tileY, 80.0, 32.0);
```

---

## Coordinate Systems

Zanna Game APIs use **integer world coordinates** with no fixed unit. Choose a coordinate
scale that suits your game and stay consistent:

| System | Scale | Used by |
|---|---|---|
| World pixels | 1 = 1 pixel | Camera, Physics2D, Quadtree, Tilemap, ScreenFX |
| PathFollower | 1000 = 1 world unit | PathFollower only |

When mixing PathFollower with Camera or Physics2D, convert between scales:

```zia
// PathFollower uses ×1000 scale; camera uses pixel scale
var camX = follower.X / 1000;
var camY = follower.Y / 1000;
cam.Follow(camX, camY);
```

> **ScreenFX color** uses `0xRRGGBBAA`; **Canvas drawing** uses `0x00RRGGBB`.
> These formats are incompatible — always check which is expected.

## Current Limits

| Class | Limit | Constant | Notes |
|---|---|---|---|
| Physics2D | Dynamic bodies/joints/contacts | `PH_MAX_*` initial reservations | `ContactOverflowed()` only reports contact allocation failure |
| Physics2D | Adaptive 8×8 / 12×12 / 16×16 broad-phase cells | Internal | Per-world step scratch |
| Physics2D | 32 bodies/cell | `BPG_CELL_MAX = 32` | Dense-cell overflow falls back to an exhaustive pair pass |
| Quadtree | Dynamic query results | `RT_QUADTREE_MAX_RESULTS` initial reservation | `QueryWasTruncated()` only reports allocation failure |
| Quadtree | Dynamic total items | Internal | Grows on insert |
| Quadtree | Dynamic overlap pairs | Internal 1024-slot initial reservation | `PairsWasTruncated()` only reports allocation failure |
| StateMachine | 256 states | `RT_STATE_MAX` | Traps on overflow |
| ButtonGroup | 256 buttons | `RT_BUTTONGROUP_MAX` | Traps on overflow |
| ScreenFX | Dynamic active effects | `RT_SCREENFX_MAX_EFFECTS` initial reservation | Storage grows when all slots are active |
| SpriteAnim | No limit | — | Start/end frame are plain integers |

## See Also

- [Game Engine Documentation](../gameengine/README.md) — Guides, tutorials, example games
- [Game Module Index](game/README.md) — Per-topic documentation pages
- [Graphics](graphics/README.md) — Canvas, Sprites, Tilemap
- [Input](input.md) — Keyboard, Mouse, Gamepad
- [Audio](audio.md) — Sound effects and music
- [Zanna Runtime Library](README.md)
