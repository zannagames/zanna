---
status: active
audience: public
last-verified: 2026-07-29
---

# Scene Graph
> SceneNode, SceneGraph, SpriteBatch, Camera, SpriteAnimation

**Part of [Zanna Runtime Library](../README.md) › [Graphics](README.md)**

---

## Zanna.Graphics.Camera

2D camera for viewport management, scrolling, and coordinate transformation.
Camera movement, bounds clamping, world/screen conversion, and parallax scrolling use saturating integer arithmetic at the int64 limits.
Camera methods validate their receiver as a real `Camera` object; invalid handles return safe defaults or no-op.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics.Camera(width, height)`

### Properties

| Property   | Type    | Access | Description                         |
|------------|---------|--------|-------------------------------------|
| `X`        | Integer | R/W    | Viewport left edge in world coordinates |
| `Y`        | Integer | R/W    | Viewport top edge in world coordinates |
| `Zoom`     | Integer | R/W    | Zoom level (100 = 100%)             |
| `Rotation` | Integer | R/W    | Rotation in degrees                 |
| `Width`    | Integer | Read   | Viewport width                      |
| `Height`   | Integer | Read   | Viewport height                     |
| `CenterX`  | Integer | Read   | Viewport center X in world coordinates |
| `CenterY`  | Integer | Read   | Viewport center Y in world coordinates |

### Methods

| Method                           | Signature                              | Description                                      |
|----------------------------------|----------------------------------------|--------------------------------------------------|
| `ClearBounds()`                  | `Void()`                               | Remove camera bounds                             |
| `Follow(x, y)`                   | `Void(Integer, Integer)`               | Center camera on world position                  |
| `Move(dx, dy)`                   | `Void(Integer, Integer)`               | Move camera by delta amounts                     |
| `SetCenter(x, y)`                | `Void(Integer, Integer)`               | Position the viewport so its center is at the world coordinate |
| `SetBounds(minX, minY, maxX, maxY)` | `Void(Integer, Integer, Integer, Integer)` | Limit camera movement range; immediately clamps the current view if needed |
| `SetDeadzone(width, height)`     | `Void(Integer, Integer)`               | Configure the no-move rectangle used by `SmoothFollow`; non-positive values disable it |
| `SmoothFollow(x, y, lerp)`       | `Void(Integer, Integer, Integer)`      | Move toward a target center using 0..1000 interpolation, then clamp to bounds |
| `ToScreenX(worldX)`              | `Integer(Integer)`                     | Convert world X to screen X                      |
| `ToScreenY(worldY)`              | `Integer(Integer)`                     | Convert world Y to screen Y                      |
| `ToWorldX(screenX)`              | `Integer(Integer)`                     | Convert screen X to world X                      |
| `ToWorldY(screenY)`              | `Integer(Integer)`                     | Convert screen Y to world Y                      |

`X` and `Y` are the viewport origin, not the followed target. Use `CenterX`/`CenterY`
when you need the world coordinate currently centered in the view. `Follow` and
`SetCenter` center the viewport on the provided world position.

Camera transforms reduce rotation modulo 360 before trigonometric evaluation, so
very large degree values behave like their reduced angle. `SmoothFollow` makes
one-pixel progress when a small positive interpolation fraction rounds to zero;
a one-unit dead zone therefore contains only the exact center. Transform setters,
`Move`, and `Follow` leave the dirty flag clear when bounds or identical inputs
produce no effective movement.

Parallax rendering covers the larger of the camera viewport and current canvas.
Zoomed/rotated tiles are cached by source-pixel generation and transform, and each
layer is capped at 65,536 counted tile draws.

### Zia Example

```zia
module CameraDemo;

bind Zanna.Terminal;
bind Zanna.Graphics.Camera as Camera;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var cam = Camera.New(800, 600);
    Say("Viewport: " + Fmt.Int(cam.get_Width()) + "x" + Fmt.Int(cam.get_Height()));

    // Position camera
    cam.set_X(100);
    cam.set_Y(200);
    cam.Follow(500, 400);
    Say("Pos: " + Fmt.Int(cam.get_X()) + "," + Fmt.Int(cam.get_Y()));

    // Coordinate conversion
    var sx = cam.ToScreenX(500);
    var sy = cam.ToScreenY(400);
    Say("Screen: " + Fmt.Int(sx) + "," + Fmt.Int(sy));

    // Movement and bounds
    cam.Move(10, 20);
    cam.SetBounds(0, 0, 2000, 1500);
    cam.SetDeadzone(96, 64);
    cam.SmoothFollow(560, 440, 200);
    cam.ClearBounds();
}
```

### Example

```basic
' Create camera matching screen size
DIM camera AS Zanna.Graphics.Camera
camera = NEW Zanna.Graphics.Camera(800, 600)

' Player position (world coordinates)
DIM playerX AS INTEGER = 400
DIM playerY AS INTEGER = 300

' Set camera bounds to prevent showing outside world
camera.SetBounds(0, 0, 2000, 1500)  ' World is 2000x1500

' Game loop
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    canvas.Clear(0)

    ' Move player
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Right) THEN playerX = playerX + 5
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Left) THEN playerX = playerX - 5
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Up) THEN playerY = playerY - 5
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Down) THEN playerY = playerY + 5

    ' Camera follows player
    camera.Follow(playerX, playerY)

    ' Draw tilemap using camera viewport origin
    map.Draw(canvas, -camera.X, -camera.Y)

    ' Draw player at screen position
    DIM screenX AS INTEGER = camera.ToScreenX(playerX)
    DIM screenY AS INTEGER = camera.ToScreenY(playerY)
    canvas.Box(screenX - 16, screenY - 16, 32, 32, 65280)

    ' Convert mouse to world coordinates for clicking
    DIM mx AS INTEGER = Zanna.Input.Mouse.X
    DIM my AS INTEGER = Zanna.Input.Mouse.Y
    DIM worldX AS INTEGER = camera.ToWorldX(mx)
    DIM worldY AS INTEGER = camera.ToWorldY(my)

    canvas.Text(10, 10, "World: " + STR$(worldX) + "," + STR$(worldY), 16777215)

    ' Zoom with +/-
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Equals) THEN camera.Zoom = camera.Zoom + 10
    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Minus) THEN camera.Zoom = camera.Zoom - 10

    canvas.Flip()
LOOP
```

### Camera + Tilemap + Sprite Integration

```basic
' Full game rendering pipeline
DIM camera AS Zanna.Graphics.Camera
DIM map AS Zanna.Graphics2D.Tilemap
DIM player AS Zanna.Graphics.Sprite

' ... initialize all objects ...

' Render in correct order
SUB Render()
    ' 1. Clear screen
    canvas.Clear(0)

    ' 2. Draw tilemap with camera viewport origin
    map.Draw(canvas, -camera.X, -camera.Y)

    ' 3. Draw sprites at screen positions
    player.X = camera.ToScreenX(playerWorldX)
    player.Y = camera.ToScreenY(playerWorldY)
    player.Draw(canvas)

    ' 4. Draw UI (not affected by camera)
    canvas.Text(10, 10, "Score: " + STR$(score), 16777215)

    canvas.Flip()
END SUB
```

---

## Zanna.Graphics2D.SceneNode

Hierarchical scene node for building scene graphs with transform inheritance.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics2D.SceneNode()` (empty node) or `Zanna.Graphics2D.SceneNode.FromSprite(sprite)` (with sprite)

Creates a scene node. Scene nodes support parent-child hierarchies where child transforms are relative to their parent.
Scene nodes validate their receiver and child arguments before mutating hierarchy state. Local/world transform composition uses saturating integer arithmetic, so extreme coordinates clamp instead of wrapping.
Local scale values are normalized to at least `1` before being stored; use `100` for normal size. Reparenting a child temporarily retains it while moving between parents, so `AddChild` keeps parent pointers and child ownership consistent. The `Sprite` property accepts real `Sprite` handles; invalid non-sprite handles are ignored. Dirty propagation, find, draw, camera draw, and update traversals are iterative, so deep node hierarchies do not consume the C call stack.

### Static Methods

| Method               | Signature         | Description                                          |
|----------------------|-------------------|------------------------------------------------------|
| `FromSprite(sprite)` | `SceneNode(Sprite)` | Create a scene node with a sprite attached         |

### Properties

| Property        | Type    | Access | Description                                    |
|-----------------|---------|--------|------------------------------------------------|
| `X`             | Integer | R/W    | Local X position (relative to parent)          |
| `Y`             | Integer | R/W    | Local Y position (relative to parent)          |
| `ScaleX`        | Integer | R/W    | Local X scale, minimum 1 (100 = 100%)          |
| `ScaleY`        | Integer | R/W    | Local Y scale, minimum 1 (100 = 100%)          |
| `Rotation`      | Integer | R/W    | Local rotation in degrees                      |
| `Visible`       | Integer | R/W    | Visibility (1=visible, 0=hidden)               |
| `Depth`         | Integer | R/W    | Z-order for depth sorting (higher = on top)    |
| `Name`          | String  | R/W    | Name/tag for identification and search         |
| `Sprite`        | Object  | R/W    | Sprite attached to this node                   |
| `Parent`        | Object  | Read   | Parent scene node (NULL if root)               |
| `ChildCount`    | Integer | Read   | Number of direct children                      |
| `WorldX`        | Integer | Read   | Computed world X position                      |
| `WorldY`        | Integer | Read   | Computed world Y position                      |
| `WorldScaleX`   | Integer | Read   | Computed world X scale                         |
| `WorldScaleY`   | Integer | Read   | Computed world Y scale                         |
| `WorldRotation` | Integer | Read   | Computed world rotation                        |

### Methods

| Method                            | Signature                      | Description                                    |
|-----------------------------------|--------------------------------|------------------------------------------------|
| `AddChild(child)`                 | `Void(SceneNode)`              | Add a child node                               |
| `Detach()`                        | `Void()`                       | Remove this node from its parent               |
| `Draw(canvas)`                    | `Void(Canvas)`                 | Draw this node and all children to a canvas    |
| `DrawWithCamera(canvas, camera)`  | `Void(Canvas, Camera)`         | Draw with camera transform applied             |
| `Find(name)`                      | `Option[SceneNode](String)`            | Find a descendant node as `Some(node)`, or `None`                 |
| `GetChild(index)`                 | `SceneNode(Integer)`           | Get child by index                             |
| `Move(dx, dy)`                    | `Void(Integer, Integer)`       | Move the node by delta amounts                 |
| `RemoveChild(child)`              | `Void(SceneNode)`              | Remove a child node                            |
| `SetPosition(x, y)`              | `Void(Integer, Integer)`       | Set both X and Y position at once              |
| `SetScale(scale)`                 | `Void(Integer)`                | Set both ScaleX and ScaleY to the same value   |
| `Update()`                        | `Void()`                       | Update node and all children (for animations)  |

### Zia Example

```zia
module SceneNodeDemo;

bind Zanna.Graphics;
bind Zanna.Graphics2D;
bind Zanna.Terminal;

func start() {
    var root = SceneNode.New();
    SceneNode.set_Name(root, "root");
    SceneNode.set_X(root, 100);
    SceneNode.set_Y(root, 200);

    // Add children
    var child1 = SceneNode.New();
    SceneNode.set_Name(child1, "child1");
    SceneNode.set_X(child1, 10);
    SceneNode.set_Y(child1, 20);
    root.AddChild(child1);

    var child2 = SceneNode.New();
    SceneNode.set_Name(child2, "child2");
    SceneNode.set_X(child2, 50);
    SceneNode.set_Y(child2, 60);
    root.AddChild(child2);

    // World coordinates (parent + child)
    SayInt(child1.WorldX);  // 110
    SayInt(child1.WorldY);  // 220

    // Find by name
    var found = root.Find("child2");
    if found.IsSome {
        Say(SceneNode.get_Name(found.Unwrap()));  // child2
    }

    // Transform inheritance
    root.SetScale(200);
    SayInt(child1.WorldScaleX);  // 200

    // Hierarchy management
    root.RemoveChild(child2);
    SayInt(root.ChildCount);  // 1
    child1.Detach();
    SayInt(root.ChildCount);  // 0
}
```

### Example

```basic
' Create sprites
DIM bodySprite AS Zanna.Graphics.Sprite
DIM armSprite AS Zanna.Graphics.Sprite
bodySprite = Zanna.Graphics.Sprite.FromFile("body.bmp")
armSprite = Zanna.Graphics.Sprite.FromFile("arm.bmp")

' Create scene nodes
DIM body AS Zanna.Graphics2D.SceneNode
DIM arm AS Zanna.Graphics2D.SceneNode
body = Zanna.Graphics2D.SceneNode.FromSprite(bodySprite)
arm = Zanna.Graphics2D.SceneNode.FromSprite(armSprite)

' Build hierarchy - arm is child of body
body.AddChild(arm)

' Name nodes for later lookup
body.Name = "body"
arm.Name = "arm"

' Position arm relative to body
arm.X = 20  ' 20 pixels right of body origin
arm.Y = -10 ' 10 pixels above body origin

' When body moves/rotates, arm follows automatically
body.X = 100
body.Y = 200
body.Rotation = 45  ' Arm rotates with body

' Arm inherits transforms - its world position is computed automatically
PRINT "Arm world position: "; arm.WorldX; ", "; arm.WorldY

' Find a descendant by name
DIM found AS OBJECT
found = body.Find("arm")
IF found.IsSome THEN
    PRINT found.Unwrap()
END IF

' Draw entire hierarchy to canvas
body.Draw(canvas)

' Detach arm from body
arm.Detach()
```

---

## Zanna.Graphics2D.SceneGraph

Root container for a scene graph. Manages rendering order and provides scene-level operations.
Scene draws are depth-sorted, and nodes with equal depth preserve traversal order.
Scene APIs validate `SceneGraph`, `SceneNode`, and `Camera` handles before traversal or drawing. Scene-level collection for depth-sorted rendering also uses iterative traversal and preserves depth-first order for equal-depth nodes.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics2D.SceneGraph()`

### Properties

| Property    | Type    | Access | Description                                |
|-------------|---------|--------|--------------------------------------------|
| `Root`      | Object  | Read   | The root SceneNode of the scene            |
| `NodeCount` | Integer | Read   | Number of root-level nodes                 |

### Methods

| Method                           | Signature                      | Description                                    |
|----------------------------------|--------------------------------|------------------------------------------------|
| `Add(node)`                      | `Void(SceneNode)`              | Add a root-level node to the scene             |
| `Clear()`                        | `Void()`                       | Remove all nodes                               |
| `Draw(canvas)`                   | `Void(Canvas)`                 | Render all visible nodes to canvas (depth-sorted; equal depths stay stable) |
| `DrawWithCamera(canvas, camera)` | `Void(Canvas, Camera)`         | Render all visible nodes with camera transform (depth-sorted; equal depths stay stable) |
| `Find(name)`                     | `SceneNode(String)`            | Find a node by name                            |
| `Remove(node)`                   | `Void(SceneNode)`              | Remove a node from the scene                   |
| `Update()`                       | `Void()`                       | Update all nodes (advances animations)         |

### Zia Example

```zia
module SceneDemo;

bind Zanna.Graphics;
bind Zanna.Graphics2D;
bind Zanna.Terminal;

func start() {
    var scene = SceneGraph.New();

    // Add nodes
    var player = SceneNode.New();
    SceneNode.set_Name(player, "player");
    SceneNode.set_X(player, 100);
    SceneNode.set_Depth(player, 50);

    var bg = SceneNode.New();
    SceneNode.set_Name(bg, "background");
    SceneNode.set_Depth(bg, 0);

    scene.Add(player);
    scene.Add(bg);

    // Access root
    var root = scene.Root;
    SayInt(SceneNode.get_ChildCount(root));  // 2

    // Find by name
    var found = scene.Find("player");
    if found.IsSome {
        SayInt(SceneNode.get_X(found.Unwrap()));  // 100
    }

    // Update and manage
    scene.Update();
    scene.Remove(bg);
    scene.Clear();
}
```

### Example

```basic
' Create a scene
DIM scene AS Zanna.Graphics2D.SceneGraph
scene = NEW Zanna.Graphics2D.SceneGraph()

' Create game objects as scene nodes
DIM background AS Zanna.Graphics2D.SceneNode
DIM player AS Zanna.Graphics2D.SceneNode
DIM foreground AS Zanna.Graphics2D.SceneNode

background = Zanna.Graphics2D.SceneNode.FromSprite(bgSprite)
player = Zanna.Graphics2D.SceneNode.FromSprite(playerSprite)
foreground = Zanna.Graphics2D.SceneNode.FromSprite(fgSprite)

' Set depth (lower = rendered first/behind)
background.Depth = 0
player.Depth = 50
foreground.Depth = 100

' Add to scene
scene.Add(background)
scene.Add(player)
scene.Add(foreground)

' Game loop
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    canvas.Clear(0)

    ' Update player position
    player.X = playerX
    player.Y = playerY

    ' Update animations
    scene.Update()

    ' Render entire scene
    scene.Draw(canvas)

    canvas.Flip()
LOOP
```

### Hierarchical Character Example

```basic
' Build a character with multiple parts
DIM character AS Zanna.Graphics2D.SceneNode
DIM head AS Zanna.Graphics2D.SceneNode
DIM body AS Zanna.Graphics2D.SceneNode
DIM leftArm AS Zanna.Graphics2D.SceneNode
DIM rightArm AS Zanna.Graphics2D.SceneNode

' Create nodes
character = NEW Zanna.Graphics2D.SceneNode()  ' Empty parent node
body = Zanna.Graphics2D.SceneNode.FromSprite(bodySprite)
head = Zanna.Graphics2D.SceneNode.FromSprite(headSprite)
leftArm = Zanna.Graphics2D.SceneNode.FromSprite(armSprite)
rightArm = Zanna.Graphics2D.SceneNode.FromSprite(armSprite)

' Build hierarchy
character.AddChild(body)
body.AddChild(head)
body.AddChild(leftArm)
body.AddChild(rightArm)

' Position parts relative to body
head.Y = -40
leftArm.X = -25
leftArm.Y = -10
rightArm.X = 25
rightArm.Y = -10

' Add character to scene
scene.Add(character)

' Moving/rotating the character moves all parts
character.X = 400
character.Y = 300
character.Rotation = 15  ' Entire character tilts
```

---

## Zanna.Graphics.SpriteBatch

Batched sprite rendering for improved performance when drawing many sprites.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics.SpriteBatch(capacity)`

Creates a sprite batch with the given initial capacity (use 0 for default). SpriteBatch records draw calls, optionally sorts them by depth, applies shared tint/alpha state, and flushes them during `End(canvas)`. `End(canvas)` also clears the recorded batch so the same instance can be reused next frame. Use the `Draw` overloads for `Sprite` objects and `DrawPixels`/`DrawRegion` for raw `Pixels` buffers. `DrawPixels` preserves per-pixel alpha, so transparent sprites and overlays blend like `Canvas.BlitAlpha`. `DrawRegion` draws its extracted region at the requested destination top-left; any temporary transform or color copy does not recenter the final blit. When depth sorting is enabled, items with the same depth still preserve their original submission order. Scale values below `1` clamp to `1` for both sprite and raw-pixels batch entries. `Color.Rgba` tint values preserve their explicit alpha channel.
SpriteBatch methods validate the batch receiver before recording or flushing; invalid handles are treated as empty/inactive batches or no-ops. Draw calls also validate their source objects: sprite draw methods require a real `Sprite`, and raw-pixels methods require a real `Pixels` buffer.

### Properties

| Property    | Type    | Access | Description                                      |
|-------------|---------|--------|--------------------------------------------------|
| `Count`     | Integer | Read   | Number of sprites currently in the batch         |
| `Capacity`  | Integer | Read   | Current capacity of the batch                    |
| `IsActive`  | Integer | Read   | Non-zero if the batch is currently recording     |

### Methods

| Method                                          | Signature                                              | Description                                    |
|-------------------------------------------------|--------------------------------------------------------|------------------------------------------------|
| `Begin()`                                       | `Void()`                                               | Begin batch - call before drawing              |
| `Draw(sprite, x, y)`                            | `Void(Sprite, Integer, Integer)`                       | Draw sprite at position                        |
| `Draw(sprite, x, y, scaleX, scaleY, rotation)`  | `Void(Sprite, Integer, Integer, Integer, Integer, Integer)` | Draw with full transform              |
| `DrawPixels(pixels, x, y)`                      | `Void(Pixels, Integer, Integer)`                       | Draw pixels buffer at position                 |
| `DrawRegion(pixels, dx, dy, sx, sy, sw, sh)`    | `Void(Pixels, Integer...)`                             | Draw a sub-region of a Pixels buffer           |
| `DrawScaled(sprite, x, y, scale)`               | `Void(Sprite, Integer, Integer, Integer)`              | Draw sprite with uniform scale (100 = 100%)    |
| `End(canvas)`                                   | `Void(Canvas)`                                         | End batch, flush recorded draws to the canvas, and clear the batch |
| `ResetSettings()`                               | `Void()`                                               | Clear all settings to defaults                 |
| `SetAlpha(alpha)`                               | `Void(Integer)`                                        | Set global alpha (0-255) for all sprites       |
| `SetSortByDepth(enabled)`                       | `Void(Integer)`                                        | Enable/disable depth sorting (1=on, 0=off); equal depths stay stable in submission order |
| `SetTint(color)`                                | `Void(Integer)`                                        | Set tint color (`Color.Rgb`, `Color.Rgba`, or RGB literal); pass `-1` for no tint |
| `DrawAtlas(atlas, name, x, y)`                  | `Void(TextureAtlas, String, Integer, Integer)`         | Draw named atlas region at position            |
| `DrawAtlasScaled(atlas, name, x, y, scale)`     | `Void(TextureAtlas, String, Integer, Integer, Integer)`| Draw named atlas region with uniform scale     |
| `DrawAtlas(atlas, name, x, y, scale, rot, depth)` | `Void(TextureAtlas, String, Integer...)`             | Draw named atlas region with full transform    |

### Zia Example

```zia
module SpriteBatchDemo;

bind Zanna.Graphics;
bind Zanna.Graphics2D;
bind Zanna.Terminal;

func start() {
    var batch = SpriteBatch.New(64);
    SayInt(batch.Count);       // 0
    SayInt(batch.Capacity);    // 64
    SayBool(batch.IsActive);   // false

    // Begin a batch
    batch.Begin();
    SayBool(batch.IsActive);  // true

    // Draw sprites
    var px = Pixels.New(16, 16);
    px.FillColor(Color.Rgb(255, 0, 0));
    batch.DrawPixels(px, 10, 20);
    batch.DrawPixels(px, 30, 40);
    batch.DrawPixels(px, 50, 60);
    SayInt(batch.Count);  // 3

    // Rendering settings
    batch.SetSortByDepth(true);
    batch.SetTint(Color.Rgba(255, 0, 0, 128));
    batch.SetAlpha(200);
    batch.ResetSettings();
}
```

### Example

```basic
' Create sprite batch with default capacity
DIM batch AS Zanna.Graphics.SpriteBatch
batch = NEW Zanna.Graphics.SpriteBatch(0)

' Load sprites
DIM bulletSprite AS Zanna.Graphics.Sprite
bulletSprite = Zanna.Graphics.Sprite.FromFile("bullet.bmp")

' Array of bullet positions
DIM bulletsX(50) AS INTEGER
DIM bulletsY(50) AS INTEGER

' Game loop
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    canvas.Clear(0)

    ' Begin batched rendering
    batch.Begin()

    ' Draw all bullets efficiently
    FOR i = 0 TO 49
        batch.Draw(bulletSprite, bulletsX(i), bulletsY(i))
    NEXT i

    ' End batch - all sprites rendered to canvas in one go
    batch.End(canvas)

    canvas.Flip()
LOOP
```

### Particle System Example

```basic
' Create a simple particle system using SpriteBatch
DIM batch AS Zanna.Graphics.SpriteBatch
batch = NEW Zanna.Graphics.SpriteBatch(512)  ' Pre-allocate for 512 sprites
batch.SetSortByDepth(1)  ' Sort particles by depth

DIM particleSprite AS Zanna.Graphics.Sprite
particleSprite = Zanna.Graphics.Sprite.FromFile("particle.bmp")

' Particle data arrays
DIM px(500) AS INTEGER   ' X positions
DIM py(500) AS INTEGER   ' Y positions
DIM pa(500) AS INTEGER   ' Alpha (0-255)

' Render particles
SUB RenderParticles()
    batch.Begin()

    FOR i = 0 TO 499
        IF pa(i) > 0 THEN
            ' Set alpha for this particle
            batch.SetAlpha(pa(i))
            batch.Draw(particleSprite, px(i), py(i))
        END IF
    NEXT i

    batch.End(canvas)
END SUB
```

### Tinting Example

```basic
' Create batch
DIM batch AS Zanna.Graphics.SpriteBatch
batch = NEW Zanna.Graphics.SpriteBatch(0)

DIM sprite AS Zanna.Graphics.Sprite
sprite = Zanna.Graphics.Sprite.FromFile("enemy.bmp")

batch.Begin()

' Draw normal sprite
batch.Draw(sprite, 100, 100)

' Draw with red tint (damaged enemy)
batch.SetTint(4294901760)
batch.Draw(sprite, 200, 100)

' Draw with blue tint
batch.SetTint(4278190335)
batch.Draw(sprite, 300, 100)

' Reset all settings to defaults
batch.ResetSettings()
batch.Draw(sprite, 400, 100)

batch.End(canvas)
```

`SetTint(0)` applies a black multiplicative tint. Use `SetTint(-1)` or `ResetSettings()` when you want no tint. `SetTint(Color.Rgba(...))` keeps the tint alpha rather than collapsing it to opaque RGB.

### Performance Tips

- **Batch similar sprites:** Draw sprites that share the same texture together
- **Minimize Begin/End calls:** Each Begin/End pair flushes the batch
- **Use depth sorting wisely:** Disable `SetSortByDepth` when not needed for faster rendering
- **Pre-allocate batches:** Create SpriteBatch once with sufficient capacity, reuse each frame

---

## Zanna.Graphics.TextureAtlas

Named-region sprite sheet atlas. Maps string names to rectangular sub-regions of a
backing Pixels buffer, enabling content pipelines where frames are referenced by name
instead of raw pixel coordinates. Added regions must stay inside the backing `Pixels`
bounds and names must be non-empty.
`TextureAtlas` retains its backing `Pixels`; constructors require a real `Pixels`
object. `LoadGrid` publishes each generated region only after the region metadata
and lookup entry are consistent. Drawing a missing region, null name, or invalid
atlas through `SpriteBatch` is a no-op.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics.TextureAtlas(pixels)` or `TextureAtlas.LoadGrid(pixels, frameW, frameH)`

### Properties

| Property      | Type    | Access | Description                                |
|---------------|---------|--------|--------------------------------------------|
| `Pixels`      | Pixels  | Read   | The backing Pixels buffer                  |
| `RegionCount` | Integer | Read   | Number of named regions in the atlas       |

### Methods

| Method                            | Signature                                        | Description                                    |
|-----------------------------------|--------------------------------------------------|------------------------------------------------|
| `Add(name, x, y, w, h)`          | `Void(String, Integer, Integer, Integer, Integer)` | Add a named rectangular region               |
| `Has(name)`                       | `Boolean(String)`                                | Check if a named region exists                 |
| `GetX(name)`                      | `Integer(String)`                                | Get region X coordinate                        |
| `GetY(name)`                      | `Integer(String)`                                | Get region Y coordinate                        |
| `GetWidth(name)`                  | `Integer(String)`                                | Get region width                               |
| `GetHeight(name)`                 | `Integer(String)`                                | Get region height                              |

### Static Methods

| Method                            | Signature                                        | Description                                    |
|-----------------------------------|--------------------------------------------------|------------------------------------------------|
| `LoadGrid(pixels, frameW, frameH)` | `TextureAtlas(Pixels, Integer, Integer)`        | Create atlas by slicing a grid; names are "0", "1", "2", ... |

### Zia Example

```zia
module AtlasDemo;
bind Zanna.Graphics;
bind Zanna.Graphics2D;

func start() {
    var canvas = Canvas.New("Atlas Demo", 640, 480);

    // Load a sprite sheet and create a grid atlas
    var sheet = Pixels.LoadBmp("spritesheet.bmp");
    var atlas = TextureAtlas.LoadGrid(sheet, 32, 32);

    // Add friendly names for specific frames
    atlas.Add("idle", 0, 0, 32, 32);
    atlas.Add("walk1", 32, 0, 32, 32);
    atlas.Add("walk2", 64, 0, 32, 32);
    atlas.Add("jump", 96, 0, 32, 32);

    // Draw using SpriteBatch
    var batch = SpriteBatch.New(64);

    while !canvas.ShouldClose {
        canvas.Poll();
        canvas.Clear(Color.Black);

        batch.Begin();
        batch.DrawAtlas(atlas, "idle", 100, 200);
        batch.DrawAtlas(atlas, "walk1", 150, 200);
        batch.DrawAtlas(atlas, "walk2", 200, 200);
        batch.DrawAtlas(atlas, "jump", 250, 200);
        batch.End(canvas);

        canvas.Flip();
    }
}
```

---

## Zanna.Game.SpriteAnimation

Frame-based sprite animation controller. Use alongside `Zanna.Graphics.Sprite` to drive animated sprites with configurable speed, looping, and ping-pong modes.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Game.SpriteAnimation()`

### Properties

| Property       | Type    | Access | Description                                              |
|----------------|---------|--------|----------------------------------------------------------|
| `Frame`        | Integer | R/W    | Current frame index                                      |
| `FrameCount`   | Integer | Read   | Total number of frames (set via `Setup`)                 |
| `FrameDuration`| Integer | R/W    | Duration of each frame in milliseconds                   |
| `IsPlaying`    | Boolean | Read   | True if animation is currently playing                   |
| `IsPaused`     | Boolean | Read   | True if animation is paused                              |
| `IsFinished`   | Boolean | Read   | True if non-looping animation reached the last frame     |
| `Progress`     | Integer | Read   | Playback progress 0–100 (percent complete)               |
| `Speed`        | Double  | R/W    | Playback speed multiplier (1.0 = normal, 2.0 = double)   |
| `Loop`         | Boolean | R/W    | Loop when last frame is reached (default: true)          |
| `PingPong`     | Boolean | R/W    | Reverse direction at end instead of restarting           |
| `FrameChanged` | Boolean | Read   | True if frame advanced on the last `Update()` call       |

### Methods

| Method                            | Signature                               | Description                                        |
|-----------------------------------|-----------------------------------------|----------------------------------------------------|
| `Setup(startFrame, endFrame, fps)` | `Void(Integer, Integer, Integer)`      | Configure frame range and playback speed            |
| `Play()`                          | `Void()`                                | Start or resume playback                           |
| `Stop()`                          | `Void()`                                | Stop and reset to first frame                      |
| `Pause()`                         | `Void()`                                | Pause at the current frame                         |
| `Resume()`                        | `Void()`                                | Resume from the paused frame                       |
| `Reset()`                         | `Void()`                                | Reset to first frame without stopping              |
| `Update()`                        | `Boolean()`                             | Advance animation by one frame tick; high playback speeds may cross multiple frames and still return true if any visible frame changed |
| `Destroy()`                       | `Void()`                                | Free the animator                                  |

### Zia Example

```zia
module AnimDemo;

bind Zanna.Terminal;
bind Zanna.Graphics.Sprite as Sprite;
bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Game.SpriteAnimation as Anim;

func start() {
    var canvas = Canvas.New("Sprite Animation", 400, 300);
    var hero = Sprite.FromFile("hero_sheet.bmp");

    var walk = Anim.New();
    walk.Setup(0, 7, 12);  // frames 0-7 at 12 FPS
    walk.set_Loop(true);
    walk.Play();

    while !canvas.get_ShouldClose() {
        canvas.Poll();
        canvas.Clear(0x000000);

        if walk.Update() == true {
            hero.set_Frame(walk.get_Frame());
        }
        hero.Draw(canvas);
        canvas.Flip();
    }
}
```

### Notes

- Call `Update()` once per frame in your game loop; it advances the frame timer and returns `true` when the visible frame changes.
- High `Speed` values can consume multiple frame steps in one `Update()` call, so fast effects stay time-correct instead of slowing down.
- `FrameChanged` is a convenience flag — equivalent to the `Update()` return value — useful when calling `Update()` elsewhere but checking in a different location.
- `PingPong` and `Loop` are independent: setting both causes the animation to reverse at the end and loop indefinitely; `PingPong` alone (no `Loop`) plays forward then backward once.

---


## See Also

- [Canvas & Color](canvas.md)
- [Images & Sprites](pixels.md)
- [Graphics Overview](README.md)
- [Zanna Runtime Library](../README.md)
