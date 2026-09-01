---
status: active
audience: public
last-verified: 2026-09-01
---

# Zanna Runtime Library Reference

> **Development line:** 0.3.0
> **Status:** Pre-Alpha — API subject to change
> **Last updated:** 2026-07-14

The Zanna Runtime Library provides built-in classes and utilities available to all Zanna programs. These classes are
implemented in C and exposed through the IL runtime system.

For the exhaustive, source-generated class/member/function inventory, see the
[Runtime API Reference](../generated/runtime/README.md). The pages below are
conceptual guides and curated navigation; canonical signatures and class
descriptions come from the modular `runtime.def` registry.

---

## Quick Navigation

| Module                          | Description                                                               |
|---------------------------------|---------------------------------------------------------------------------|
| [Architecture](architecture.md) | Runtime internals, type reference                                         |
| [Audio](audio.md)               | `Mixer`, `Music`, `MusicGen`, `Playlist`, `Sound`, `SoundBank`, `Synth`, `Voice` — audio playback and procedural generation |
| [Collections](collections/README.md)   | `StringSet`, `Bytes`, `Deque`, `F64Buffer`, `Heap`, `I64Buffer`, `LazySeq`, `List`, `Map`, `Queue`, `Ring`, `Seq`, `Set`, `SortedSet`, `Stack`, `SortedMap`, `Trie`, `WeakMap` |
| [Core Types](core.md)           | `Box`, `Diagnostics`, `MessageBus`, `Object`, `Parse`, `String` — foundational types (`Zanna.Core`) |
| [Cryptography](crypto.md)       | `Aes`, `Cipher`, `Hash`, `KeyDerive`, `Password`, `SecureRandom`, `Tls`; compatibility `Legacy.Aes`, `Legacy.Hash` |
| [Diagnostics](diagnostics.md)   | `Diagnostics`, `Log`, `TrapInfo` — assertions, logging, and current-trap inspection |
| [Functional](functional.md)     | `Lazy`, `Option`, `Result` — lazy evaluation, optionals, and result types  |
| [Game Utilities](game/README.md)       | `AchievementTracker`, `AnimStateMachine`, `ButtonGroup`, `Collision`, `CollisionRect`, `Grid2D`, `Zanna.Game2D.LevelDocument`, `Zanna.Game2D.SceneDocument`, `Lighting2D`, `ObjectPool`, `ParticleEmitter`, `PathFollower`, `Physics2D`, `PlatformerController`, `Quadtree`, `ScreenFX`, `SmoothValue`, `SpriteAnimation`, `StateMachine`, `Timer`, `Tween`, `Typewriter`, `WorldToScreenProjection` |
| [Graphics](graphics/README.md)         | `Camera`, `Canvas`, `Color`, `Pixels`, `Renderer2D`, `RenderTarget2D`, `Zanna.Graphics2D.SceneGraph`, `Zanna.Graphics2D.SceneNode`, `Sprite`, `SpriteBatch`, `SpriteSheet`, `Texture2D`, `TextureAtlas`, `TileLayer2D`, `Zanna.Graphics2D.Tilemap`, production 2D classes |
| [Game3D](graphics/game3d.md) | `World3D`, `Entity3D`, `LayerMask`, `Input3D` — code-first 3D game workflow helpers |
| [Graphics 3D & Physics](graphics/physics3d.md) | `PhysicsWorld3D`, `PhysicsHit3D`, `PhysicsHitList3D`, `CollisionEvent3D`, `ContactPoint3D`, `Collider3D`, `PhysicsBody3D`, `Character3D`, `DistanceJoint3D`, `SpringJoint3D` |
| [GUI](gui/README.md)                   | `App`, `Breadcrumb`, `Button`, `ClipboardText`, `CodeEditor`, `CommandPalette`, `Container`, `Cursor`, `FileDialog`, `Label`, `MessageBox`, `Minimap`, `Shortcuts`, `Toast`, `Tooltip`, widgets — GUI toolkit for applications |
| [Input](input.md)               | `Action`, `Key`, `Keyboard`, `KeyChord`, `Manager`, `Mouse`, `Pad` — input for games and interactive apps |
| [Input/Output](io/README.md)           | `Archive`, `BinaryBuffer`, `BinFile`, `Compress`, `Dir`, `File`, `Glob`, `LineReader`, `LineWriter`, `MemStream`, `Path`, `Stream`, `TempFile`, `Watcher` |
| [Mathematics](math.md)          | `BigInt`, `Bits`, `Easing`, `Mat3`, `Mat4`, `Math`, `PerlinNoise`, `Quat`, `Random`, `Spline`, `Vec2`, `Vec3` |
| [Network](network.md)           | `Dns`, `Http`, `HttpReq`, `HttpRes`, `RateLimiter`, `RestClient`, `RetryPolicy`, `Tcp`, `TcpServer`, `Udp`, `Url`, `WebSocket` |
| [System](system.md)             | `Environment`, `Clipboard`, `Exec`, `Machine`, `Terminal`; `Zanna.Runtime.Unsafe`, `Zanna.Runtime.GC`, compatibility `Zanna.Memory`, and `Zanna.Memory.WeakRef` |
| [Text & Data](text/README.md)          | `Codec`, `CompiledPattern`, `Csv`, `Diff`, `Html`, `Ini`, `Json`, `JsonPath`, `JsonStream`, `Markdown`, `InvariantNumberFormat`, `Pattern`, `Pluralize`, `Scanner`, `StringBuilder`, `Template`, `TextWrapper`, `Toml`, `Uuid`, `Version`; `Zanna.Data`: `Serialize`, `Xml`, `Yaml` |
| [Threads](threads.md)           | `Async`, `Barrier`, `CancelToken`, `Channel`, `ConcurrentMap`, `Debouncer`, `Future`, `Gate`, `Monitor`, `Parallel`, `Pool`, `Promise`, `RwLock`, `SafeI64`, `Scheduler`, `Thread`, `Throttler` |
| [Time & Timing](time.md)        | `Clock`, `Countdown`, `DateOnly`, `DateRange`, `DateTime`, `Duration`, `RelativeTime`, `Stopwatch` |
| [Localization](localization/README.md) | `Locale`, `LocaleInfo`, `LocaleManager`, `NumberFormat`, `DateFormat`, `RelativeTimeFormat`, `MessageBundle`, `PluralRules`, `ListFormat`, `TextDirection` |
| [Utilities](utilities.md)       | `Convert`, `Fmt`, `Log`, `Parse`                                          |
| [Zia Tooling](zia.md)           | `Zanna.Zia.Toolchain` structured diagnostics and compile results          |

---

## Namespace Overview

Class links in this overview point to the source-generated API reference when
the conceptual guide does not provide a class-specific section.

### Zanna (Root)

| Class                                        | Type     | Description                                   |
|----------------------------------------------|----------|-----------------------------------------------|
| [`BigInt`](math.md#zannamathbigint)          | Static   | Arbitrary-precision integer arithmetic        |
| [`Bits`](math.md#zannamathbits)              | Static   | Bit manipulation (shifts, rotates, counting)  |
| [`Box`](core.md#zannacorebox)                | Static   | Boxing helpers for generic collections        |
| [`Convert`](../generated/runtime/core.md#zanna-core-convert)       | Static   | Type conversion utilities                     |
| [`Easing`](math.md#zannamatheasing)          | Static   | Animation easing functions                    |
| [`Environment`](../generated/runtime/system.md#zanna-system-environment)  | Static   | Command-line args and environment             |
| [`Exec`](../generated/runtime/system.md#zanna-system-exec)                | Static   | External command execution                    |
| [`Fmt`](../generated/runtime/text.md#zanna-text-fmt)               | Static   | String formatting                             |
| [`Lazy`](../generated/runtime/functional.md#zanna-functional-lazy)            | Static   | Lazy evaluation wrapper                       |
| [`Log`](../generated/runtime/diagnostics.md#zanna-diagnostics-log)               | Static   | Logging utilities                             |
| [`Machine`](../generated/runtime/system.md#zanna-system-machine)          | Static   | System information queries                    |
| [`System.Clipboard`](system.md#zannasystemclipboard) | Static   | UTF-8 desktop clipboard access                 |
| [`Mat3`](math.md#zannamathmat3)              | Static   | 3×3 matrix for 2D affine transforms           |
| [`Mat4`](math.md#zannamathmat4)              | Static   | 4×4 matrix for 3D transforms and projection   |
| [`Math`](math.md#zannamath)                  | Static   | Mathematical functions (trig, pow, abs, etc.) |
| [`MessageBus`](core.md#zannacoremessagebus)  | Instance | In-process publish/subscribe messaging        |
| [`Object`](core.md#zannacoreobject)          | Base     | Root type for all reference types             |
| [`Option`](functional.md#zannaoption)        | Static   | Optional value type (Some/None)               |
| [`PerlinNoise`](math.md#zannamathperlinnoise) | Instance | Perlin noise for procedural generation       |
| [`Quat`](math.md#zannamathquat)              | Instance | Quaternion math for 3D rotations              |
| [`Random`](math.md#zannamathrandom)          | Static/Instance | Random number generation                 |
| [`Result`](functional.md#zannaresult)        | Static   | Result type for error handling (Ok/Err)       |
| [`Spline`](math.md#zannamathspline)          | Instance | Catmull-Rom spline interpolation              |
| [`String`](core.md#zannastring)              | Instance | Immutable string with manipulation methods    |
| [`Terminal`](system.md#zannaterminal)        | Static   | Terminal input/output                         |
| [`Vec2`](math.md#zannamathvec2)              | Instance | 2D vector math                                |
| [`Vec3`](math.md#zannamathvec3)              | Instance | 3D vector math                                |

### Zanna.Collections

| Class                                                         | Type     | Description                                |
|---------------------------------------------------------------|----------|--------------------------------------------|
| [`StringSet`](../generated/runtime/collections.md#zanna-collections-stringset)                   | Instance | String set with set operations             |
| [`BiMap`](../generated/runtime/collections.md#zanna-collections-bimap)               | Instance | Bidirectional key-value map                |
| [`BitSet`](../generated/runtime/collections.md#zanna-collections-bitset)             | Instance | Compact fixed-size set of bits             |
| [`BloomFilter`](../generated/runtime/collections.md#zanna-collections-bloomfilter)   | Instance | Probabilistic membership test              |
| [`Bytes`](../generated/runtime/collections.md#zanna-collections-bytes)               | Instance | Byte array for binary data                 |
| [`CountMap`](../generated/runtime/collections.md#zanna-collections-countmap)         | Instance | Frequency counter map                      |
| [`DefaultMap`](../generated/runtime/collections.md#zanna-collections-defaultmap)     | Instance | Map with auto-initialized default values   |
| [`Deque`](../generated/runtime/collections.md#zanna-collections-deque)               | Instance | Double-ended queue                         |
| [`FrozenMap`](../generated/runtime/collections.md#zanna-collections-frozenmap)       | Instance | Immutable key-value map                    |
| [`FrozenSet`](../generated/runtime/collections.md#zanna-collections-frozenset)       | Instance | Immutable string set                       |
| [`F64Buffer`](collections/specialized.md#packed-numeric-buffers)      | Instance | Packed 64-bit float buffer                 |
| [`Heap`](../generated/runtime/collections.md#zanna-collections-heap)                 | Instance | Priority queue (min/max heap)              |
| [`I64Buffer`](collections/specialized.md#packed-numeric-buffers)      | Instance | Packed 64-bit integer buffer               |
| [`IntMap`](../generated/runtime/collections.md#zanna-collections-intmap)             | Instance | Integer-keyed hash map                     |
| [`Iterator`](../generated/runtime/collections.md#zanna-collections-iterator)         | Instance | Generic forward iterator                   |
| [`LazySeq`](../generated/runtime/functional.md#zanna-functional-lazyseq)                      | Instance | Lazy on-demand sequence (`Zanna.Functional.LazySeq`)  |
| [`List`](../generated/runtime/collections.md#zanna-collections-list)                 | Instance | Dynamic array of objects                   |
| [`LruCache`](../generated/runtime/collections.md#zanna-collections-lrucache)         | Instance | Least-recently-used cache                  |
| [`Map`](../generated/runtime/collections.md#zanna-collections-map)                   | Instance | String-keyed hash map                      |
| [`MultiMap`](../generated/runtime/collections.md#zanna-collections-multimap)         | Instance | Map allowing multiple values per key       |
| [`OrderedMap`](../generated/runtime/collections.md#zanna-collections-orderedmap)     | Instance | Insertion-ordered key-value map            |
| [`Queue`](../generated/runtime/collections.md#zanna-collections-queue)               | Instance | FIFO collection                            |
| [`Ring`](../generated/runtime/collections.md#zanna-collections-ring)                 | Instance | Fixed-size circular buffer                 |
| [`Seq`](../generated/runtime/collections.md#zanna-collections-seq)                   | Instance | Growable array with stack/queue ops        |
| [`Set`](../generated/runtime/collections.md#zanna-collections-set)                   | Instance | Generic object set with set ops            |
| [`SortedSet`](../generated/runtime/collections.md#zanna-collections-sortedset)       | Instance | Sorted string set with range ops           |
| [`SparseArray`](../generated/runtime/collections.md#zanna-collections-sparsearray)   | Instance | Sparse integer-indexed array               |
| [`Stack`](../generated/runtime/collections.md#zanna-collections-stack)               | Instance | LIFO collection                            |
| [`SortedMap`](../generated/runtime/collections.md#zanna-collections-sortedmap)           | Instance | Sorted key-value map                       |
| [`Trie`](../generated/runtime/collections.md#zanna-collections-trie)                 | Instance | Prefix-tree for string lookups             |
| [`UnionFind`](../generated/runtime/collections.md#zanna-collections-unionfind)       | Instance | Disjoint-set union-find structure          |
| [`WeakMap`](../generated/runtime/collections.md#zanna-collections-weakmap)           | Instance | Weak-reference key-value map               |

### Zanna.Crypto

| Class                                         | Type     | Description                              |
|-----------------------------------------------|----------|------------------------------------------|
| [`Aes`](crypto.md#zannacryptoaes)             | Static   | AES-GCM and password encryption with Result/Option decrypt helpers |
| [`Cipher`](crypto.md#zannacryptocipher)       | Static   | High-level authenticated encryption with explicit failure APIs |
| [`Hash`](crypto.md#zannacryptohash)           | Static   | SHA-256, HMAC-SHA256, fast hashes, and constant-time comparison |
| [`KeyDerive`](crypto.md#zannacryptokeyderive) | Static   | PBKDF2-SHA256 key derivation             |
| [`Legacy.Aes`](crypto.md#zannacryptolegacyaes) | Static  | AES-CBC compatibility helpers            |
| [`Legacy.Hash`](crypto.md#zannacryptolegacyhash) | Static | CRC32, MD5, SHA1, and legacy HMAC compatibility helpers |
| [`Password`](crypto.md#zannacryptopassword)   | Static   | Password hashing and verification        |
| [`SecureRandom`](crypto.md#zannacryptosecurerandom) | Static | Cryptographically secure random bytes  |
| [`Tls`](crypto.md#zannacryptotls)             | Instance | TLS 1.3 secure socket connections        |

### Zanna.Data

| Class                                               | Type   | Description                                           |
|-----------------------------------------------------|--------|-------------------------------------------------------|
| [`Csv`](../generated/runtime/data.md#zanna-data-csv)                       | Static | CSV parsing and formatting                            |
| [`Ini`](../generated/runtime/data.md#zanna-data-ini)                       | Static | INI file parsing and formatting                       |
| [`Json`](../generated/runtime/data.md#zanna-data-json)                     | Static | JSON parsing and formatting                           |
| [`JsonPath`](../generated/runtime/data.md#zanna-data-jsonpath)             | Static | JSONPath query evaluation                             |
| [`JsonStream`](../generated/runtime/data.md#zanna-data-jsonstream)         | Instance | Streaming JSON reader                               |
| [`Serialize`](../generated/runtime/data.md#zanna-data-serialize)           | Static | Unified multi-format serializer (JSON/XML/YAML/TOML/CSV) |
| [`Toml`](../generated/runtime/data.md#zanna-data-toml)                     | Static | TOML parsing and formatting                           |
| [`Xml`](../generated/runtime/data.md#zanna-data-xml)                       | Static | XML document model — parse, navigate, mutate          |
| [`Yaml`](../generated/runtime/data.md#zanna-data-yaml)                     | Static | YAML parse and format                                 |

### Zanna.Diagnostics

| Class                                             | Type     | Description        |
|---------------------------------------------------|----------|--------------------|
| [`Assert`](diagnostics.md#zannadiagnostics)       | Static   | Assertion checking |
| [`Trap`](diagnostics.md#zannadiagnostics)         | Static   | Unconditional trap |

### Zanna.Game

| Class                                                     | Type     | Description                             |
|-----------------------------------------------------------|----------|-----------------------------------------|
| [`ButtonGroup`](game.md#zannagamebuttongroup)             | Instance | Mutually exclusive button selection      |
| [`Collision`](game.md#zannagamecollision)                 | Static   | Static collision detection helpers       |
| [`CollisionRect`](../generated/runtime/game.md#zanna-game-collisionrect)         | Instance | AABB collision detection                 |
| [`Grid2D`](game.md#zannagamegrid2d)                       | Instance | 2D array for tile maps and grids         |
| [`ObjectPool`](game.md#zannagameobjectpool)               | Instance | Efficient object slot reuse              |
| [`ParticleEmitter`](game/effects.md#zannagameparticleemitter) | Instance | Particle effects with batch rendering |
| [`PathFollower`](game/animation.md#zannagamepathfollower) | Instance | Path following along waypoints |
| [`Physics2D`](game.md#zannagamephysics2d-class-family)    | Compound | 2D rigid body physics (World + Body)     |
| [`Quadtree`](game.md#zannagamequadtree)                   | Instance | Spatial partitioning for collision       |
| [`ScreenFX`](game.md#zannagamescreenfx)                   | Instance | Screen effects (shake, flash, fade)      |
| [`SmoothValue`](game.md#zannagamesmoothvalue)             | Instance | Smooth value interpolation               |
| [`SpriteAnimation`](game/animation.md#zannagamespriteanimation) | Instance | Frame-based sprite animation controller |
| [`StateMachine`](game.md#zannagamestatemachine)           | Instance | Finite state machine for game states     |
| [`Timer`](game.md#zannagametimer)                         | Instance | Frame-based game timers                  |
| [`Tween`](../generated/runtime/game.md#zanna-game-tween)                         | Instance | Animation tweening with 19 easing curves |
| [`WorldToScreenProjection`](game.md#zannagameworldtoscreenprojection) | Static | Linear, isometric, and perspective world-to-screen helpers |

### Zanna.Game2D

| Class | Type | Description |
|-------|------|-------------|
| [`LevelDocument`](game/leveldata.md#zannagame2dleveldocument) | Instance | Legacy JSON level loader with tilemap and object spawn data |
| [`SceneDocument`](game/scene.md#editable-scene-documents) | Instance | Editable JSON scene document with layers, objects, properties, diagnostics, and tilemap-copy export |

### Zanna.Audio

| Class                                         | Type     | Description                      |
|-----------------------------------------------|----------|----------------------------------|
| [`Mixer`](audio.md#zannaaudiomixer)            | Static   | Global audio mixer control       |
| [`Music`](audio.md#zannaaudiomusic)            | Instance | Streaming music playback         |
| [`MusicGen`](audio.md#zannaaudiomusicgen)      | Instance | Procedural music composition     |
| [`Playlist`](audio.md#zannaaudioplaylist)      | Instance | Sequential/shuffle track queue   |
| [`Sound`](audio.md#zannaaudiosound)            | Instance | Sound effects for short clips    |
| [`SoundBank`](audio.md#zannaaudiosoundbank)    | Instance | Named sound registry             |
| [`Synth`](audio.md#zannaaudiosynth)            | Static   | Procedural sound synthesis       |
| [`Voice`](audio.md#zannaaudiovoice)            | Static   | Voice control for playing sounds |

### Zanna.Graphics

| Class                                         | Type     | Description                      |
|-----------------------------------------------|----------|----------------------------------|
| [`Camera`](../generated/runtime/graphics.md#zanna-graphics-camera)         | Instance | 2D camera for scrolling/zoom/parallax |
| [`Canvas`](../generated/runtime/graphics.md#zanna-graphics-canvas)         | Instance | 2D graphics canvas with delta time    |
| [`Color`](../generated/runtime/graphics.md#zanna-graphics-color)           | Static   | Color creation                   |
| [`DebugDraw2D`](graphics/shapes2d.md#classes)              | Instance | Retained debug drawing queue     |
| [`Material2D`](graphics/rendering2d.md#classes)            | Instance | 2D tint, alpha, and blend state  |
| [`NineSlice2D`](graphics/shapes2d.md#classes)              | Instance | Stretchable UI image drawing |
| [`ObjectLayer2D`](graphics/tilemaps2d.md#classes)          | Instance | Rect object map layer       |
| [`ParticleEmitter`](graphics/game2d.md#classes)           | Instance | Graphics alias for particle effects |
| [`Pixels`](../generated/runtime/graphics.md#zanna-graphics-pixels)         | Instance | Software image buffer            |
| [`PostProcess2D`](graphics/rendering2d.md#classes)         | Instance | CPU image post-processing pass   |
| [`RenderTarget2D`](graphics/rendering2d.md#render-targets-textures-and-renderer) | Instance | Offscreen alpha-aware render surface |
| [`Renderer2D`](graphics/rendering2d.md#render-targets-textures-and-renderer) | Instance | Retained 2D draw command stream |
| [`Shader2D`](graphics/rendering2d.md#classes)              | Instance | CPU 2D image effect wrapper      |
| [`ShapeRenderer2D`](graphics/shapes2d.md#classes)          | Instance | Lines, rectangles, circles, paths |
| [`SdfFont`](graphics/shapes2d.md#classes)                  | Instance | SDF-ready bitmap font wrapper    |
| [`Sprite`](../generated/runtime/graphics.md#zanna-graphics-sprite)         | Instance | 2D sprite with flip/animation    |
| [`SpriteAnimator`](graphics/pixels.md#zannagraphicsspriteanimator) | Instance | Named sprite clip controller |
| [`SpriteBatch`](../generated/runtime/graphics.md#zanna-graphics-spritebatch)| Instance | Batched sprite rendering        |
| [`SpriteSheet`](../generated/runtime/graphics.md#zanna-graphics-spritesheet)| Instance | Sprite sheet/atlas with named region extraction |
| [`TextRenderer2D`](graphics/shapes2d.md#classes)           | Instance | Text measurement and Canvas draw wrapper |
| [`Texture2D`](graphics/rendering2d.md#classes)             | Instance | Retained texture handle over Pixels |
| [`TileLayer2D`](graphics/tilemaps2d.md#classes)            | Instance | Dense tile ID layer          |
| [`TileSet2D`](graphics/tilemaps2d.md#classes)              | Instance | Uniform grid tileset         |
| [`Viewport2D`](graphics/game2d.md#viewport-scale)          | Instance | Virtual-to-screen scaling and transforms |

### Zanna.Graphics2D

| Class | Type | Description |
|-------|------|-------------|
| [`SceneGraph`](graphics/scene.md#zannagraphics2dscenegraph) | Instance | 2D scene graph container |
| [`SceneNode`](graphics/scene.md#zannagraphics2dscenenode) | Instance | Hierarchical 2D scene graph node |
| [`Tilemap`](graphics/pixels.md#zannagraphics2dtilemap) | Instance | Tile-based map rendering, collision, JSON save/load, and CSV import |

### Zanna.Graphics3D

| Class | Type | Description |
|-------|------|-------------|
| [`Character3D`](graphics/physics3d.md#zannagraphics3dcharacter3d) | Instance | Slide-and-step character controller |
| [`AnimController3D`](../graphics3d-guide.md#animcontroller3d) | Instance | Stateful skeletal animation controller with transitions, events, root motion, and scene-node binding support |
| [`CollisionEvent3D`](graphics/physics3d.md#zannagraphics3dcollisionevent3d) | Instance | Structured collision pair event with contact, speed, and impulse data |
| [`Collider3D`](graphics/physics3d.md#zannagraphics3dcollider3d) | Instance | Reusable 3D collision shape including compound, mesh, and heightfield variants |
| [`ContactPoint3D`](graphics/physics3d.md#zannagraphics3dcontactpoint3d) | Instance | Contact manifold point with position, normal, and signed separation |
| [`DistanceJoint3D`](graphics/physics3d.md#zannagraphics3ddistancejoint3d) | Instance | Fixed-distance constraint between bodies |
| [`SceneAsset`](../graphics3d-guide.md#sceneasset) | Instance | Unified imported asset with shared resources and scene instantiation |
| [`SceneGraph`](../graphics3d-guide.md#scenegraph) | Instance | 3D scene graph with culling, spatial queries, `.vscn` save/load, and binding sync |
| [`SceneNode`](../graphics3d-guide.md#scenenode) | Instance | Hierarchical 3D scene node with transform, mesh, material, LOD, and bindings |
| [`PhysicsBody3D`](graphics/physics3d.md#zannagraphics3dphysicsbody3d) | Instance | 3D rigid body with linear/angular motion, sleep, and CCD |
| [`PhysicsHit3D`](graphics/physics3d.md#zannagraphics3dphysicshit3d) | Instance | World-query hit result with body, collider, point, normal, and fraction |
| [`PhysicsHitList3D`](graphics/physics3d.md#zannagraphics3dphysicshitlist3d) | Instance | List of `PhysicsHit3D` results returned by overlap and multi-hit queries |
| [`PhysicsWorld3D`](graphics/physics3d.md#zannagraphics3dphysicsworld3d) | Instance | 3D simulation world with contact and joint access |
| [`SpringJoint3D`](graphics/physics3d.md#zannagraphics3dspringjoint3d) | Instance | Spring constraint with stiffness and damping |

### Zanna.GUI

| Class                                               | Type     | Description                                      |
|-----------------------------------------------------|----------|--------------------------------------------------|
| [`App`](../generated/runtime/gui.md#zanna-gui-app)                        | Instance | Main application window                          |
| [`Breadcrumb`](../generated/runtime/gui.md#zanna-gui-breadcrumb)                   | Instance | Breadcrumb navigation widget                     |
| [`Button`](../generated/runtime/gui.md#zanna-gui-button)                  | Instance | Clickable button widget                          |
| [`Container`](../generated/runtime/gui.md#zanna-gui-container)            | Instance | Generic layout container widget                  |
| [`Checkbox`](../generated/runtime/gui.md#zanna-gui-checkbox)              | Instance | Boolean toggle widget                            |
| [`ClipboardText`](gui/application.md#clipboardtext)         | Static   | System clipboard access                          |
| [`CodeEditor`](../generated/runtime/gui.md#zanna-gui-codeeditor)          | Instance | Code editing with syntax coloring                |
| [`CommandPalette`](../generated/runtime/gui.md#zanna-gui-commandpalette)           | Instance | Searchable command palette overlay               |
| [`Cursor`](../generated/runtime/gui.md#zanna-gui-cursor)                           | Static   | System cursor type and visibility                |
| [`Dropdown`](../generated/runtime/gui.md#zanna-gui-dropdown)              | Instance | Drop-down selection                              |
| [`FileDialog`](../generated/runtime/gui.md#zanna-gui-filedialog)                   | Static   | Native open/save file dialogs                    |
| [`Font`](../generated/runtime/gui.md#zanna-gui-font)                      | Instance | Font for text rendering                          |
| [`Label`](../generated/runtime/gui.md#zanna-gui-label)                    | Instance | Text display widget                              |
| [`ListBox`](../generated/runtime/gui.md#zanna-gui-listbox)                | Instance | Scrollable list selection                        |
| [`MessageBox`](../generated/runtime/gui.md#zanna-gui-messagebox)                   | Static   | Native info/warning/error/question dialogs       |
| [`Minimap`](../generated/runtime/gui.md#zanna-gui-minimap)                         | Instance | Code minimap sidebar bound to a CodeEditor       |
| [`ProgressBar`](../generated/runtime/gui.md#zanna-gui-progressbar)        | Instance | Progress indicator                               |
| [`RadioButton`](../generated/runtime/gui.md#zanna-gui-radiobutton)        | Instance | Single-select option widget                      |
| [`ScrollView`](../generated/runtime/gui.md#zanna-gui-scrollview)          | Instance | Scrollable container                             |
| [`Shortcuts`](../generated/runtime/gui.md#zanna-gui-shortcuts)                     | Static   | Global keyboard shortcut registration            |
| [`Slider`](../generated/runtime/gui.md#zanna-gui-slider)                  | Instance | Numeric range slider                             |
| [`Spinner`](../generated/runtime/gui.md#zanna-gui-spinner)                | Instance | Numeric spinner control                          |
| [`SplitPane`](../generated/runtime/gui.md#zanna-gui-splitpane)            | Instance | Resizable split container                        |
| [`TabBar`](../generated/runtime/gui.md#zanna-gui-tabbar)                  | Instance | Tabbed container                                 |
| [`TextInput`](../generated/runtime/gui.md#zanna-gui-textinput)            | Instance | Single-line text entry                           |
| [`Theme`](../generated/runtime/gui.md#zanna-gui-theme)                    | Static   | Built-in, system-following, and custom themes    |
| [`ThemePalette`](../generated/runtime/gui.md#zanna-gui-themepalette)      | Instance | Validated logical theme token collection         |
| [`Toast`](../generated/runtime/gui.md#zanna-gui-toast)                             | Instance | Transient notification toasts                    |
| [`Tooltip`](../generated/runtime/gui.md#zanna-gui-tooltip)                         | Static   | Floating tooltip display                         |
| [`TreeView`](../generated/runtime/gui.md#zanna-gui-treeview)              | Instance | Hierarchical tree widget                         |

### Zanna.Input

| Class                                         | Type     | Description                        |
|-----------------------------------------------|----------|------------------------------------|
| [`Action`](input.md#zannainputaction)         | Static   | Device-agnostic action mapping     |
| [`Key`](input.md#zannainputkey)               | Static   | Canonical keyboard key codes       |
| [`Keyboard`](input.md#zannainputkeyboard)     | Static   | Keyboard input for games and UI    |
| [`KeyChord`](input.md#zannainputkeychord)     | Instance | Key chord and combo detection      |
| [`Manager`](input.md#zannainputmanager)       | Instance | Unified input with debouncing      |
| [`Mouse`](input.md#zannainputmouse)           | Static   | Mouse input for games and UI       |
| [`Pad`](input.md#zannainputpad)               | Static   | Gamepad/controller input           |

### Zanna.IO

| Class                                           | Type     | Description                         |
|-------------------------------------------------|----------|-------------------------------------|
| [`Archive`](../generated/runtime/io.md#zanna-io-archive)               | Instance | ZIP archive read/write              |
| [`BinaryBuffer`](../generated/runtime/io.md#zanna-io-binarybuffer)     | Instance | Positioned binary read/write buffer |
| [`BinFile`](../generated/runtime/io.md#zanna-io-binfile)               | Instance | Binary file stream                  |
| [`Compress`](../generated/runtime/io.md#zanna-io-compress)             | Static   | DEFLATE/GZIP compression            |
| [`Dir`](../generated/runtime/io.md#zanna-io-dir)                       | Static   | Directory operations                |
| [`File`](../generated/runtime/io.md#zanna-io-file)                     | Static   | File read/write/delete              |
| [`Glob`](../generated/runtime/io.md#zanna-io-glob)                     | Static   | File globbing and matching          |
| [`LineReader`](../generated/runtime/io.md#zanna-io-linereader)         | Instance | Line-by-line text reading           |
| [`LineWriter`](../generated/runtime/io.md#zanna-io-linewriter)         | Instance | Buffered text writing               |
| [`MemStream`](../generated/runtime/io.md#zanna-io-memstream)           | Instance | In-memory binary stream             |
| [`Path`](../generated/runtime/io.md#zanna-io-path)                     | Static   | Path manipulation                   |
| [`Stream`](../generated/runtime/io.md#zanna-io-stream)                 | Instance | Unified stream interface            |
| [`TempFile`](../generated/runtime/io.md#zanna-io-tempfile)             | Static   | Temporary file/dir creation         |
| [`Watcher`](../generated/runtime/io.md#zanna-io-watcher)               | Instance | File system event monitoring        |

### Zanna.Network

| Class                                             | Type     | Description                           |
|---------------------------------------------------|----------|---------------------------------------|
| [`Dns`](network.md#zannanetworkdns)               | Static   | DNS resolution and validation         |
| [`Http`](network.md#zannanetworkhttp)             | Static   | Simple HTTP helpers                   |
| [`HttpReq`](network.md#zannanetworkhttpreq)       | Instance | HTTP request builder                  |
| [`HttpRes`](network.md#zannanetworkhttpres)       | Instance | HTTP response wrapper                 |
| [`RateLimiter`](network.md#zannanetworkratelimiter)| Instance | Token bucket rate limiting           |
| [`RestClient`](network.md#zannanetworkrestclient) | Instance | REST API client with base URL         |
| [`RetryPolicy`](network.md#zannanetworkretrypolicy)| Instance | Retry with backoff for HTTP requests |
| [`Tcp`](network.md#zannanetworktcp)               | Instance | TCP client connection                 |
| [`TcpServer`](network.md#zannanetworktcpserver)   | Instance | TCP server (listener)                 |
| [`Udp`](network.md#zannanetworkudp)               | Instance | UDP datagram socket                   |
| [`Url`](network.md#zannanetworkurl)               | Instance | URL parsing and building              |
| [`WebSocket`](network.md#zannanetworkwebsocket)   | Instance | WebSocket client (RFC 6455)           |

### Zanna.Text

| Class                                                     | Type     | Description                        |
|-----------------------------------------------------------|----------|------------------------------------|
| [`Char`](../generated/runtime/text.md#zanna-text-char)                           | Static   | Unicode character classification   |
| [`Codec`](../generated/runtime/text.md#zanna-text-codec)                         | Static   | Base64, Hex, URL encoding          |
| [`CompiledPattern`](../generated/runtime/text.md#zanna-text-compiledpattern)     | Instance | Pre-compiled regex pattern         |
| [`Diff`](../generated/runtime/text.md#zanna-text-diff)                           | Static   | Text diff and patch                |
| [`Fmt`](../generated/runtime/text.md#zanna-text-fmt)                             | Static   | Number, byte-size, and value formatting |
| [`FuzzyMatch`](../generated/runtime/text.md#zanna-text-fuzzymatch)               | Static   | Approximate string matching        |
| [`Html`](../generated/runtime/text.md#zanna-text-html)                           | Static   | HTML stripping and entity decode   |
| [`Markdown`](../generated/runtime/text.md#zanna-text-markdown)                   | Static   | Markdown to HTML/text conversion   |
| [`InvariantNumberFormat`](text/formatting.md#zannatextinvariantnumberformat) | Static   | C-locale number formatting       |
| [`Pattern`](../generated/runtime/text.md#zanna-text-pattern)                     | Static   | Regex pattern matching             |
| [`Pluralize`](../generated/runtime/text.md#zanna-text-pluralize)                 | Static   | English pluralization utilities    |
| [`Scanner`](../generated/runtime/text.md#zanna-text-scanner)                     | Instance | Token-based string scanner         |
| [`StringBuilder`](../generated/runtime/text.md#zanna-text-stringbuilder)         | Instance | Mutable string builder             |
| [`Template`](../generated/runtime/text.md#zanna-text-template)                   | Static   | Template rendering                 |
| [`TextWrapper`](../generated/runtime/text.md#zanna-text-textwrapper)             | Static   | Word-wrap and text formatting      |
| [`Uuid`](../generated/runtime/text.md#zanna-text-uuid)                           | Static   | UUID v4 generation                 |
| [`Version`](../generated/runtime/text.md#zanna-text-version)                     | Static   | Semantic version parsing/comparison|

### Zanna.Threads

| Class                                         | Type     | Description                                  |
|-----------------------------------------------|----------|----------------------------------------------|
| [`Async`](threads.md#zannathreadsasync)                   | Static   | Async task combinators (Delay, All, Any)      |
| [`Barrier`](threads.md#zannathreadsbarrier)               | Instance | Synchronization barrier for N participants    |
| [`CancelToken`](threads.md#zannathreadscanceltoken)       | Instance | Cooperative cancellation token                |
| [`Channel`](threads.md#zannathreadschannel)               | Instance | Typed buffered/unbuffered message channel     |
| [`ConcurrentMap`](threads.md#zannathreadsconcurrentmap)   | Instance | Thread-safe string-keyed hash map             |
| [`Debouncer`](threads.md#zannathreadsdebouncer)           | Instance | Debounce rapid calls to single execution      |
| [`Future`](threads.md#zannathreadsfuture)                 | Instance | Read-only handle for async result             |
| [`Gate`](threads.md#zannathreadsgate)                     | Instance | Counting gate/semaphore                       |
| [`Monitor`](threads.md#zannathreadsmonitor)               | Static   | FIFO-fair, re-entrant object monitor          |
| [`Parallel`](threads.md#zannathreadsparallel)             | Static   | Parallel For/ForEach/Map/Reduce               |
| [`Pool`](threads.md#zannathreadspool)                     | Instance | Thread pool for parallel task execution       |
| [`Promise`](threads.md#zannathreadspromise)               | Instance | Write-once async value producer               |
| [`RwLock`](threads.md#zannathreadsrwlock)                 | Instance | Reader-writer lock                            |
| [`SafeI64`](threads.md#zannathreadssafei64)               | Instance | Lock-free atomic integer cell                 |
| [`Scheduler`](threads.md#zannathreadsscheduler)           | Instance | Delayed and periodic task scheduling          |
| [`Thread`](threads.md#zannathreadsthread)                 | Instance | OS thread handle + join/sleep/yield helpers   |
| [`Throttler`](threads.md#zannathreadsthrottler)           | Instance | Rate-limit calls to at most once per interval |

### Zanna.Runtime

| Class                                           | Type   | Description                                     |
|-------------------------------------------------|--------|-------------------------------------------------|
| [`Unsafe`](system.md#zannaruntimeunsafe)       | Static | Explicit retain/release for runtime handles      |
| [`GC`](system.md#zannaruntimegc)               | Static | Garbage collector controls and statistics        |

### Zanna.Memory

| Class                                           | Type   | Description                                     |
|-------------------------------------------------|--------|-------------------------------------------------|
| [`WeakRef`](system.md#zannamemoryweakref)      | Instance | Zeroing reference that does not retain its target |

### Zanna.Time

| Class                                       | Type     | Description                  |
|---------------------------------------------|----------|------------------------------|
| [`Clock`](time.md#zannatimeclock)               | Static   | Sleep and tick counting          |
| [`Countdown`](time.md#zannatimecountdown)       | Instance | Millisecond-based countdown      |
| [`DateOnly`](time.md#zannatimedateonly)         | Instance | Calendar date without time       |
| [`DateRange`](time.md#zannatimedaterange)       | Instance | Time range with start/end        |
| [`DateTime`](time.md#zannatimedatetime)         | Static   | Date and time operations         |
| [`Duration`](time.md#zannatimeduration)         | Static   | Time span manipulation           |
| [`RelativeTime`](time.md#zannatimerelativetime) | Static   | Human-readable relative times    |
| [`Stopwatch`](time.md#zannatimestopwatch)       | Instance | Performance timing               |

---

## Class Types

- **Instance** — Requires instantiation with `NEW` before use
- **Static** — Methods called directly on the class name (no instantiation)
- **Base** — Cannot be instantiated directly; inherited by other types

---

## Which Collection Should I Use?

| Need                        | Use         | Why                                         |
|-----------------------------|-------------|---------------------------------------------|
| 2D tile maps/grids          | `Grid2D`    | Efficient (x,y) access, bounds checking     |
| Binary data                 | `Bytes`     | Efficient byte manipulation                 |
| Both ends access            | `Deque`     | Push/pop from front and back, indexed       |
| FIFO (first-in-first-out)   | `Queue`     | Push/Pop interface                   |
| Fixed-size buffer           | `Ring`      | Overwrites oldest when full                 |
| Indexed array               | `Seq`       | Fast random access, push/pop                |
| Key-value pairs             | `Map`       | O(1) lookup by string key                   |
| Large/infinite sequences    | `LazySeq`   | Memory-efficient on-demand generation       |
| Legacy compatibility        | `List`      | Similar to VB6 Collection                   |
| LIFO (last-in-first-out)    | `Stack`     | Simple push/pop interface                   |
| Priority queue              | `Heap`      | Extract min/max by priority                 |
| Packed numeric batches      | `F64Buffer` / `I64Buffer` | Dense primitive storage without per-element boxing |
| Sorted key-value            | `SortedMap`   | Keys in sorted order, floor/ceil queries    |
| Sorted unique strings       | `SortedSet` | Sorted order, range queries, floor/ceil     |
| Unique objects              | `Set`       | Object set with set operations              |
| Unique strings              | `StringSet`       | String set operations (union, etc.)         |
| Weak references             | `WeakMap`   | GC-friendly caching, avoids memory leaks    |

---

## See Also

- [Zia Language Reference](../languages/zia-reference.md)
- [BASIC Language Reference](../languages/basic-reference.md)
- [IL Guide](../il/il-guide.md)
- [Getting Started](../getting-started.md)
