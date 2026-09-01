---
status: active
audience: public
last-verified: 2026-09-01
---

# Zanna Game Engine

> Build 2D and 3D games entirely from scratch — zero external dependencies.

**Part of [Zanna Documentation](../README.md)**

---

The Zanna Game Engine is a comprehensive game development platform built into the Zanna runtime. Every system — from PNG decoding and MP3 playback to OpenGL rendering and A* pathfinding — is implemented from scratch in pure C with **zero external dependencies**. The engine targets macOS, Windows, and Linux through platform-specific backends plus documented fallbacks; platform parity claims should be backed by current test output for each host.

Two language frontends are supported: **Zia** (modern, expressive) and **BASIC** (classic, beginner-friendly). Both compile to the same IL and have access to the full engine API.

The engine powers games ranging from a simple bouncing ball to [XENOSCAPE](https://github.com/zannagames/zannademos/tree/main/games/xenoscape), a 13,000-line Metroidvania with 25+ enemy types, 10 levels, parallax scrolling, particle effects, and a full save system.

---

## Quick Start

Create a window, draw shapes, and respond to input — all in under 20 lines.

### Zia

```zia
module MyGame;

bind Zanna.Graphics.Canvas as Canvas;

func start() {
    var c = Canvas.New("My Game", 640, 480);
    var x = 320;

    while c.BeginFrame() != 0 {
        if c.KeyHeld(262) != 0 { x = x + 4; }   // RIGHT arrow
        if c.KeyHeld(263) != 0 { x = x - 4; }   // LEFT arrow

        c.Clear(0x1a1a2e);
        c.Disc(x, 240, 20, 0xe94560);
        c.Text(10, 10, "Arrow keys to move", 0xffffff);
        c.Flip();
    }
}
```

### BASIC

```basic
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("My Game", 640, 480)
DIM x AS INTEGER = 320

DO WHILE canvas.ShouldClose = 0
    canvas.Poll()
    IF canvas.KeyHeld(262) <> 0 THEN x = x + 4  ' RIGHT arrow
    IF canvas.KeyHeld(263) <> 0 THEN x = x - 4  ' LEFT arrow

    canvas.Clear(&H001A1A2E)
    canvas.Disc(x, 240, 20, &H00E94560)
    canvas.Text(10, 10, "Arrow keys to move", &H00FFFFFF)
    canvas.Flip()
LOOP
```

**Next step:** [Your First Game in 15 Minutes](getting-started.md) walks through building a complete mini-game with sprites, sound, and collision.

---

## Engine at a Glance

| System | Classes | Highlights |
|--------|---------|------------|
| **2D Graphics** | Canvas, Pixels, SpriteBatch, TextureAtlas, BitmapFont | 40+ drawing primitives, BMP/PNG save, BMP/PNG/JPEG/GIF load |
| **3D Graphics** | Canvas3D, Mesh3D, SceneGraph, Material3D, Light3D, Skeleton3D | Metal, D3D11, OpenGL 3.3 + software rasterizer |
| **Sprites & Animation** | SpriteAnimation, AnimStateMachine, SpriteBatch | State machines, frame events, ping-pong, depth sorting |
| **Tilemap** | Tilemap | Layers, auto-tiling, tile animation, collision, CSV I/O |
| **Camera** | Camera2D, Camera3D | Smooth follow, deadzone, parallax, bounds clamping |
| **Entity & AI** | Entity, Behavior | Composable AI presets (patrol, chase, shoot), built-in physics |
| **Physics** | Physics2D, CollisionRect, Quadtree | AABB + circle bodies, joints, collision layers, spatial queries |
| **Audio** | Sound, Music, Synth, SoundBank, Playlist, MusicGen | WAV/MP3/OGG, 32 voices, procedural synthesis, spatial audio |
| **Input** | Keyboard, Mouse, Pad, Action, InputManager | 4 gamepads, rumble, rebindable actions, key chords |
| **Scene Management** | GameBase, IScene, SceneManager, LevelDocument, Config | JSON levels, scene transitions, typed configuration |
| **Game UI** | UILabel, UIBar, UIPanel, NineSlice, MenuList, Dialogue | HUD widgets, typewriter text, health bars |
| **Effects** | ScreenFX, ParticleEmitter, Lighting2D | Shake, fade, wipe, dissolve, particles, dynamic lighting |
| **Pathfinding** | Pathfinder, Quadtree, Raycast2D | A* grid, spatial partitioning, line-of-sight |
| **Persistence** | SaveData, Assets, ZPAK | Cross-platform saves, embed/pack assets, ZPAK archives |
| **Utilities** | Timer, Tween, StateMachine, ObjectPool, PlatformerController | Easing curves, coyote time, jump buffer, achievement tracking |
| **Math** | Vec2, Vec3, Mat3, Mat4, Quaternion | Full vector/matrix/quaternion ops, slerp, easing functions |
| **3D Advanced** | Terrain3D, Water3D, NavMesh3D, InstanceBatch3D, Particles3D | Vegetation, decals, post-processing, morph targets |

For a deeper look at how these systems connect, see [Architecture](architecture.md).

---

## Guides by Topic

> **Note:** Guide pages below marked *(planned)* are not yet written. For current API details, see the
> [Runtime Library Reference](../zannalib/README.md) and [Graphics3D Guide](../graphics3d-guide.md).

### Rendering

- 2D Graphics *(planned)* — Canvas drawing, Pixels image buffers, bitmap fonts, color utilities
- [3D Graphics](../graphics3d-guide.md) — Canvas3D, meshes, materials, lighting, scene graph, skeletal animation
- Sprites & Animation *(planned)* — SpriteSheet, SpriteBatch, SpriteAnimation, AnimStateMachine
- Tilemaps *(planned)* — Creation, layers, auto-tiling, tile collision, CSV import/export
- Camera — Camera2D *(planned)*; [Camera3D](../graphics3d-guide.md#camera3d) is covered by the 3D Graphics guide

### Gameplay

- Entity System *(planned)* — Entity (position, velocity, health, collision) and composable Behaviors
- Physics & Collision *(planned)* — Physics2D world, rigid bodies, joints, raycasting, quadtree
- Pathfinding & AI *(planned)* — A* grid pathfinder, quadtree spatial queries, behavior presets
- Scene Management *(planned)* — GameBase, IScene lifecycle, SceneManager, LevelDocument, Config

### Presentation

- Audio *(planned)* — Sound effects, music streaming, procedural synthesis, playlists, spatial audio
- Screen Effects *(planned)* — ScreenFX (shake, fade, wipe), ParticleEmitter, Lighting2D
- Game UI *(planned)* — Labels, health bars, panels, nine-slice, menus, dialogue system

### Infrastructure

- Input *(planned)* — Keyboard, mouse, gamepad, action mapping, InputManager
- Persistence & Assets *(planned)* — SaveData, ZPAK archives, asset embedding and loading
- Game Utilities *(planned)* — Timer, Tween, StateMachine, ObjectPool, PlatformerController, and more
- Math for Games *(planned)* — Vec2/3, Mat3/4, Quaternion, easing functions, noise
- Cross-Platform *(planned)* — macOS/Windows/Linux differences, GPU backend selection

---

## Tutorials

> Step-by-step tutorials are planned. For now, see the [Example Games](examples/README.md).

- Your First Platformer *(planned)* — Entity + Tilemap + Camera + Animation
- Arcade Shooter *(planned)* — ObjectPool + Particles + ScreenFX + Score tracking

---

## Example Games

Zanna ships with 14 example games demonstrating the engine at every scale:

| Game | LOC | Genre | Key Engine Features |
|------|-----|-------|---------------------|
| [XENOSCAPE](https://github.com/zannagames/zannademos/tree/main/games/xenoscape) | 17,005 | Metroidvania | Entity, AnimState, Camera, Tilemap, Particles, ScreenFX, Audio, SceneManager, Save |
| [Graphics Show](https://github.com/zannagames/zannademos/tree/main/games/graphics-show) | 8,000+ | Demo collection | Canvas primitives, particles, physics, fractals (10 sub-demos) |
| [Chess](../../examples/games/chess/) | 4,000+ | Board game | Canvas rendering, AI evaluation, StateMachine, SaveData, SoundBank |
| [Centipede](https://github.com/zannagames/zannademos/tree/main/games/centipede) | 2,553 | Arcade shooter | Grid2D, Timer, ScreenFX, ParticleEmitter, ObjectPool |
| [Crackman](../../examples/games/crackman/) | 2,230 | Arcade | Ghost AI, StateMachine, ButtonGroup, Tween, SmoothValue |
| [Frogger](https://github.com/zannagames/zannademos/tree/main/games/frogger) | ~1,500 | Arcade | Entity, grid movement, collision, AI traffic |
| [VTris](../../examples/games/vtris/) | 1,132 | Puzzle (BASIC) | Terminal rendering, OOP classes, matrix rotation |
| [Frogger BASIC](../../examples/games/frogger-basic/) | 1,320 | Arcade (BASIC) | OOP stress test, nested objects, collision |
| [Pac-Man BASIC](https://github.com/zannagames/zannademos/tree/main/games/pacman-basic) | 450 | Arcade (BASIC) | Ghost pathfinding, ANSI rendering |
| [Centipede BASIC](../../examples/games/centipede-basic/) | 450 | Arcade (BASIC) | Class-based architecture, grid field |
| [Fade Test](https://github.com/zannagames/zannademos/tree/main/games/fade-test) | 168 | Test harness | ScreenFX, Canvas coverage verification |

See the [Example Games Gallery](examples/README.md) for detailed descriptions and engine feature breakdowns.

---

## API Reference

The topical guides above explain concepts and patterns. For exhaustive method signatures, see the runtime library API docs:

- [Game Utilities API](../zannalib/game/README.md) — 28 game classes (Entity, Timer, Tween, Physics2D, etc.)
- [Graphics API](../zannalib/graphics/README.md) — Canvas, Pixels, Scene, Fonts
- [3D Graphics API](../graphics3d-guide.md) — 69 `Zanna.Graphics3D` classes (Canvas3D, Mesh3D, SceneGraph, etc.; generated inventory: [graphics3d](../generated/runtime/graphics3d.md))
- [Game3D API](../zannalib/graphics/game3d.md) — 61 `Zanna.Game3D` classes (World3D, Entity3D, controllers, combat, dialogue, streaming)
- [Audio API](../zannalib/audio.md) — Sound, Music, Synth, SoundBank, Playlist
- [Input API](../zannalib/input.md) — Keyboard, Mouse, Pad, Action, InputManager
- [Math API](../zannalib/math.md) — Vec2/3, Mat3/4, Quaternion, Easing, Noise
- [GUI API](../zannalib/gui/README.md) — 46 desktop GUI widgets (distinct from game UI)

---

## See Also

- [The Zanna Book](../book/README.md) — Chapters 19-21 cover graphics, input, and building a complete game project
- [Zia Reference](../languages/zia-reference.md) — Complete Zia language specification
- [BASIC Reference](../languages/basic-reference.md) — Complete BASIC language specification
- [Architecture Overview](../internals/architecture.md) — Full Zanna system architecture (compiler, VM, codegen, runtime)
