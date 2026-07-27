---
status: active
audience: public
last-verified: 2026-07-26
---

# Engine Architecture

> How all the game engine systems connect — and the zero-dependency philosophy behind them.

**Part of [Zanna Game Engine](README.md)**

---

## Zero Dependencies, From Scratch

Every component of the Zanna Game Engine is implemented in pure C with **no external libraries**. This is not a wrapper around SDL, GLFW, OpenAL, or any other framework. The engine implements:

| What | From Scratch |
|------|-------------|
| **Window & input** | Native platform APIs (Cocoa/Win32/X11) |
| **2D rendering** | Software rasterizer with pixel-level control |
| **3D rendering** | Metal, Direct3D 11, OpenGL 3.3 backends + software fallback |
| **Image I/O** | BMP/PNG encoders, BMP/PNG/JPEG/GIF decoders |
| **Audio mixing** | 32-voice mixer with LRU eviction |
| **Audio codecs** | WAV, MP3, OGG Vorbis decoders |
| **Sound synthesis** | Sine, square, sawtooth, triangle, noise generators |
| **Physics** | AABB + circle rigid bodies, joints, collision response |
| **Pathfinding** | A* grid search with configurable costs and diagonals |
| **Font rendering** | BDF and PSF bitmap font parsers |
| **3D model loading** | OBJ, FBX, glTF 2.0 parsers |
| **Skeletal animation** | Bone hierarchies, skinning, animation blending |
| **Networking** | TCP/UDP sockets, HTTP server/client, WebSocket, TLS |
| **Compression** | gzip, zstd, lz4 |
| **Math** | Vectors, matrices, quaternions, noise, easing |

This means the entire engine compiles on a fresh system with just a C compiler — no package managers, no vcpkg, no brew installs.

---

## System Architecture

The engine is organized in layers. Each layer depends only on layers below it:

```text
┌─────────────────────────────────────────────────────────────┐
│                     Game Code (Zia / BASIC)                  │
├─────────────────────────────────────────────────────────────┤
│  GameBase / IScene / SceneManager         (Application)      │
├─────────┬───────────┬───────────┬───────────┬───────────────┤
│  Entity │ Physics2D │ Pathfinder│ Animation │ ScreenFX      │
│ Behavior│ Collision │ Quadtree  │ Tween     │ Particles     │
│  GameUI │ Raycast2D │ Grid2D    │ Timer     │ Lighting2D    │
│         │ Joints    │           │ StateMach │               │
├─────────┴───────────┴───────────┴───────────┴───────────────┤
│  Canvas    │ SpriteBatch │ Tilemap  │ Camera2D │ BitmapFont │
│  Pixels    │ TextureAtlas│          │          │            │
│  Canvas3D  │ SceneGraph     │ Mesh3D   │ Camera3D │ Material3D │
├────────────┴─────────────┴──────────┴──────────┴────────────┤
│  Sound / Music / Synth / SoundBank / Playlist    (Audio)     │
├─────────────────────────────────────────────────────────────┤
│  Keyboard / Mouse / Pad / Action / InputManager  (Input)     │
├─────────────────────────────────────────────────────────────┤
│  ZannaGFX (platform layer)         SaveData / Assets (I/O)   │
│  Window management, event loop     ZPAK archives, file I/O    │
├─────────────────────────────────────────────────────────────┤
│  Platform: Cocoa (macOS) │ Win32 (Windows) │ X11 (Linux)     │
└─────────────────────────────────────────────────────────────┘
```

---

## Layer Descriptions

### Platform Layer (ZannaGFX)

The lowest layer wraps platform-specific APIs into a common interface:

- **macOS:** Cocoa (`NSWindow`, `NSView`, `NSApplication`) + Metal for 3D
- **Windows:** Win32 (`CreateWindowEx`, `WndProc`, `GetMessage`) + Direct3D 11 for 3D
- **Linux:** native Wayland (preferred), X11 fallback, or headless + EGL/GLX OpenGL 3.3 for 3D

This layer provides: window creation, event polling, keyboard/mouse/gamepad input, pixel buffer display, OpenGL/Metal/D3D context management, audio device output.

Game code never touches this layer directly — it's wrapped by Canvas, Input, and Audio.

### Rendering Layer

**2D (Canvas + Pixels):** Software-rendered into a pixel buffer. Canvas provides 40+ drawing primitives (Box, Disc, Line, Text, Gradient, Polygon, Triangle, Arc, Bezier, etc.). Pixels provides direct pixel manipulation plus BMP/PNG save and BMP/PNG/JPEG/GIF load. SpriteBatch records sprite draws with optional depth sorting and shared tint/alpha before flushing them in `End()`.

**3D (Canvas3D + SceneGraph):** GPU-accelerated with automatic backend selection. The software rasterizer serves as a universal fallback. SceneGraph provides a hierarchical scene graph with transform inheritance, LOD, and AABB culling.

### Audio Layer

The mixer runs on a separate thread, blending up to 32 simultaneous voices. When voices exceed 32, the oldest is evicted (LRU). Music streams from disk (MP3/OGG) to avoid loading entire files into memory. Synth generates tones and preset sound effects procedurally — no audio files needed for prototyping.

### Input Layer

Input flows through Canvas event polling. `Keyboard.IsDown()` provides raw key state. The **Action** system adds a mapping layer — define named actions (`"jump"`, `"shoot"`) and bind them to keys, mouse buttons, or gamepad buttons. Actions support `Pressed()` (edge-triggered) and `Held()` (level-triggered) queries.

### Game Utilities Layer

Built on top of rendering and input:

- **Entity** — lightweight game object with position (centipixel precision), velocity, health, direction, and built-in tilemap collision via `MoveAndCollide()`
- **Behavior** — composable AI presets (patrol, chase, gravity, edge-reverse, shoot) applied to entities via a single `Update()` call
- **Physics2D** — standalone rigid body simulation with AABB/circle bodies, forces, friction, restitution, and 4 joint types
- **Tilemap** — grid-based level storage with collision flags, layers, auto-tiling, and tile animation
- **AnimStateMachine** — maps named states to animation clips with edge flags (`JustEntered`, `JustExited`) and frame events
- **Quadtree** — spatial partitioning for efficient collision queries over large entity counts
- **Pathfinder** — A* grid search, optionally initialized from a Tilemap's collision data

### Application Layer

**GameBase** is an optional base class that eliminates ~100 lines of game loop boilerplate (canvas creation, frame timing, DeltaTime clamping, scene management). **IScene** defines the lifecycle contract (`update`, `draw`, `onEnter`, `onExit`). **SceneManager** handles transitions between scenes with optional visual effects.

Most example games use GameBase + IScene. Simpler programs (demos, tests) use the raw `while canvas.BeginFrame()` loop.

---

## Data Flow: One Frame

```text
1. canvas.BeginFrame() / canvas.Poll()
   └─ ZannaGFX polls OS events → keyboard/mouse/gamepad state updated

2. Input phase
   └─ Game reads Keyboard.IsDown() / Action.Pressed() / Action.Axis()

3. Update phase
   └─ Entity.UpdatePhysics()     — apply gravity, move, collide with tilemap
   └─ Behavior.Update()          — run AI presets on entities
   └─ Physics2D.World.Step()     — advance rigid body simulation
   └─ AnimStateMachine.Update()  — advance animation frames
   └─ ScreenFX.Update(dt)        — advance screen effects
   └─ Timer / Tween updates      — advance timers and interpolations

4. Draw phase
   └─ canvas.Clear()             — fill background
   └─ Tilemap.Draw()             — draw level tiles (camera-aware)
   └─ SpriteBatch.Begin/Draw/End — draw sprites (depth-sorted)
   └─ GameUI.Draw()              — draw HUD (labels, bars, panels)
   └─ ScreenFX.Draw()            — draw overlays (fade, flash)

5. canvas.Flip()
   └─ ZannaGFX presents the pixel buffer to the OS window
   └─ DeltaTime computed for next frame
```

---

## Source Code Layout

```text
src/runtime/
├── graphics/           # Canvas, Pixels, SpriteBatch, Tilemap, Camera, BitmapFont
│   ├── rt_canvas.c     #   2D Canvas implementation
│   ├── rt_pixels.c     #   Pixel buffer + image I/O
│   ├── rt_canvas3d.c   #   3D Canvas + GPU backend dispatch
│   ├── rt_scene3d.c    #   3D scene graph
│   ├── rt_mesh3d.c     #   3D mesh + primitive generation
│   ├── rt_camera.c     #   2D camera
│   ├── rt_camera3d.c   #   3D camera
│   ├── rt_input.c      #   Keyboard + mouse input
│   ├── rt_input_pad.c  #   Gamepad input (41KB — full HID support)
│   └── ...
├── game/               # Game utilities
│   ├── rt_entity.c     #   Entity with built-in physics
│   ├── rt_behavior.c   #   Composable AI behaviors
│   ├── rt_animstate.c  #   Animation state machine
│   ├── rt_physics2d.c  #   2D rigid body physics
│   ├── rt_collision.c  #   Collision detection
│   ├── rt_quadtree.c   #   Spatial partitioning
│   ├── rt_pathfinder.c #   A* pathfinding
│   ├── rt_screenfx.c   #   Screen effects
│   ├── rt_particle.c   #   Particle emitter
│   ├── rt_gameui.c     #   In-game UI widgets
│   └── ...
├── audio/              # Audio system
│   ├── rt_audio.c      #   Mixer, Sound, Music
│   ├── rt_synth.c      #   Procedural sound synthesis
│   ├── rt_mp3.c        #   MP3 decoder
│   ├── rt_vorbis.c     #   OGG Vorbis decoder
│   └── ...
└── io/                 # File and asset I/O
    ├── rt_asset.c      #   Asset loading (embed/pack/filesystem)
    ├── rt_zpak_reader.c #   ZPAK archive format
    └── ...
```

---

## GPU Backend Architecture (3D)

Canvas3D automatically selects the best available GPU backend:

```text
Canvas3D.New()
    │
    ├─ macOS?  → try Metal   → success? use Metal
    ├─ Windows?→ try D3D11   → success? use Direct3D 11
    ├─ Linux?  → try OpenGL  → success? use OpenGL 3.3
    │
    └─ All platforms: fallback to Software Rasterizer
```

The software rasterizer is always available and produces identical output. It's slower but guarantees that 3D code works everywhere, including headless environments and CI.

All four backends implement the same internal interface (`vgfx3d_backend_*.c`), so game code is identical across platforms.

For details, see [3D Graphics Architecture](../internals/graphics3d-architecture.md).

---

## Cross-Platform Strategy

The engine targets cross-platform parity through:

1. **Platform abstraction in C** — `#ifdef` blocks isolate platform-specific code to a few files (`rt_platform.c`, `vgfx_window_*.c`)
2. **POSIX-first APIs** — file I/O, threading, and networking use POSIX with Win32 shims
3. **No external dependencies** — nothing to install, configure, or version-match per platform
4. **Host-capability smoke tests** — `scripts/run_cross_platform_smoke.sh` runs the checks available on the current host; full parity claims require current macOS, Linux, and Windows results

See [Platform Behavioral Differences](../cross-platform/platform-differences.md) for
platform-specific details.

---

## See Also

- [Zanna Architecture](../internals/architecture.md) — Full system architecture (compiler, IL, VM, codegen, runtime)
- [Graphics Library (ZannaGFX)](../graphics-library.md) — Low-level 2D platform API documentation
- [3D Architecture](../internals/graphics3d-architecture.md) — GPU backend internals
- [Code Map](../internals/codemap.md) — Complete source directory layout
