# Zanna Examples

Showcase programs demonstrating the Zanna compiler toolchain, [runtime library](../docs/zannalib/README.md), and language frontends. All examples can be run via the VM or compiled to native binaries.

---

## 🚀 Quick Reference

```sh
zanna run examples/games/chess/       # Run a project directory
zanna build examples/apps/paint/ -o paint  # Compile to native binary
./scripts/build_demos.sh              # Build all demos (outputs to examples/bin/)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 # Windows equivalent
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --run # Build and smoke-run on Windows
```

---

## 🖥️ Applications

Full-featured applications built with [Zia](../docs/languages/zia-reference.md).

| Project | Description | Highlights |
|---------|-------------|------------|
| [ZannaSQL](apps/zannasql/) | PostgreSQL-compatible SQL database server | MVCC, WAL, B-tree indexes, PG wire protocol, vsql client, 70K+ lines |
| [Paint](apps/paint/) | Drawing application (MS Paint-style) | 8 tools, runtime actions, file dialogs, zoomable canvas, undo/redo, layers |
| [WebServer](apps/webserver/) | Multi-threaded HTTP server | Routing, static files, JSON API, [thread pool](../docs/zannalib/threads.md) |
| [Varc](apps/varc/) | Archive utility | DEFLATE compression, AES [encryption](../docs/zannalib/crypto.md), checksums |
| [Telnet](apps/telnet/) | Telnet client and server | [TCP sockets](../docs/zannalib/network.md), session management, threading |

---

## 🎮 Games

Playable games showcasing [graphics](../docs/zannalib/graphics/README.md), AI, and [game engine](../docs/zannalib/game/README.md) patterns.

### Zia Games

| Project | Description | Highlights |
|---------|-------------|------------|
| [Ridgebound](games/ridgebound/) | Open-world Game3D sample | Game3D world loop, procedural terrain, water, skybox, PBR materials, beacon objectives, post-FX, final overlay |
| [Chess](games/chess/) | Polished chess game with AI opponent | Alpha-beta AI, save slots, clocks, puzzles, themes, audio, FEN/PGN, drag-and-drop GUI |
| [XENOSCAPE](games/xenoscape/) | Release-quality action Metroidvania | 10-region nonlinear campaign, 80 rooms, hub/economy, profiles, accessibility, ranks, Time Trials, Boss Rush, New Game+, authored/fallback art and adaptive audio |
| [Crackman](games/crackman/) | Maze chase game with ghost AI | BFS pathfinding, scatter/chase/frightened modes, [Canvas](../docs/zannalib/graphics/canvas.md) rendering |
| [Centipede](games/centipede/) | Arcade centipede game | Multiple enemy types, particle effects, Canvas graphics |
| [Frogger](games/frogger/) | Frogger clone | Single-file implementation, ANSI terminal |
| [Graphics Show](games/graphics-show/) | Visual effects showcase | Fireworks, plasma, Mandelbrot, starfield, matrix rain, snake |
| [Fade Test](games/fade-test/) | Graphics transition test | Screen fade and transition effects |

### BASIC Games

| Project | Description | Highlights |
|---------|-------------|------------|
| [Pac-Man](games/pacman-basic/) | Pac-Man with ghost AI | Chase/scatter/frightened FSM, ANSI rendering |
| [Centipede](games/centipede-basic/) | Terminal centipede | OOP entities, level progression |
| [Frogger](games/frogger-basic/) | Frogger clone | Multi-lane traffic, river mechanics, OOP |
| [VTris](games/vtris/) | Tetris | All 7 pieces with rotation, line clearing, level progression |

### Shared Game Library

The [`games/lib/`](games/lib/) directory provides reusable base classes for Zia games:

- **`GameBase`** (221 LOC) — Game loop, scene management, input handling, frame timing
- **`IScene`** — Scene interface for state-driven game architecture

---

## 🗃️ SQL Engine — BASIC

A [SQL database engine](sqldb-basic/) written entirely in [BASIC](../docs/languages/basic-reference.md) (9,600 lines). Implements a complete pipeline: lexer, parser, executor, indexes, schema management, and a test suite.

---

## 🔍 API Audit

The [`apiaudit/`](apiaudit/) directory provides **systematic coverage of Zanna runtime classes** across hundreds of source files. Most APIs have both a Zia and BASIC version; `graphics3d` is still more Zia-heavy for rendering demos, but shared runtime surfaces such as `SceneAsset`, `AnimController3D`, and `SceneNode` binding sync now ship with both Zia and BASIC samples.

Organized by namespace:

| Namespace | Coverage |
|-----------|----------|
| [collections](apiaudit/collections/) | StringSet, Bytes, Heap, List, Map, Queue, Ring, Seq, Stack, SortedMap, Set, ... |
| [core](apiaudit/core/) | Box, Object, String |
| [crypto](apiaudit/crypto/) | Hash, KeyDerive, Rand, Aes, Cipher, Tls |
| [functional](apiaudit/functional/) | Option, Result, Lazy, LazySeq |
| [game](apiaudit/game/) | Collision, Grid2D, ObjectPool, ParticleEmitter, Pathfinder, ... |
| [graphics](apiaudit/graphics/) | Canvas, Color, Pixels, Sprite, Tilemap, Camera, ... |
| [gui](apiaudit/gui/) | App, Button, Checkbox, Label, Slider, TextInput, ... |
| [input](apiaudit/input/) | Keyboard, Mouse, Pad |
| [io](apiaudit/io/) | File, Dir, Path, Archive, Compress, ... |
| [math](apiaudit/math/) | Math, Random, Vec2, Vec3, Bits, BigInt, ... |
| [network](apiaudit/network/) | Http, Tcp, TcpServer, Udp, Dns, WebSocket, ... |
| [sound](apiaudit/sound/) | Audio, Music, Sound, Voice |
| [text](apiaudit/text/) | Csv, Json, Pattern, StringBuilder, Template, Uuid, ... |
| [threads](apiaudit/threads/) | Thread, Barrier, Gate, Monitor, RwLock, Channel, ... |
| [time](apiaudit/time/) | Clock, DateTime, Stopwatch, Countdown |

> Run the full audit: `./examples/apiaudit/run_audit.sh`

Example sources are classified by [`smoke_manifest.tsv`](smoke_manifest.tsv). The local
smoke lane keeps tutorial examples and runnable IL samples compiling:

```sh
./scripts/example_smoke.sh --audit
./scripts/example_smoke.sh --fast
ctest --test-dir build -L examples --output-on-failure
```

---

## 📘 Zia Language Examples

The [`zia/`](zia/) directory contains compact source examples for specific Zia
language features. `constrained_generics.zia` demonstrates a single interface
constraint (`T: Named`) and a generic function that calls through the bound
interface.

---

## 📘 BASIC Language Examples

The [`basic/`](basic/) directory contains 28 BASIC programs demonstrating language features:

- **Namespaces** — `namespace_demo.bas`: USING directives, cross-namespace inheritance
- **Control flow** — `select_case.bas`, `ex_elseif.bas`, `ex_not.bas`
- **I/O** — `ex_input_prompt_min.bas`, `ex_print_commas.bas`, `ex_print_semicolons.bas`
- **OOP** — [`oop/`](basic/oop/): collections, text processing, mixed reports
- **Math** — `monte_carlo_pi.bas`, `random_walk.bas`
- **Basics** — `ex1` through `ex6`: hello world, loops, arrays, conditionals

---

## ⚙️ IL Examples

The [`il/`](il/) directory contains 22 [Zanna IL](../docs/il/il-guide.md) programs for VM development and testing:

- **Tutorials** — `ex1` through `ex6`: progressive IL feature demonstrations (hello, loops, tables, factorial, strings, heap arrays)
- **Benchmarks** — [`benchmarks/`](il/benchmarks/): VM and [optimizer](../docs/il/il-passes.md) performance tests (fib, arithmetic, branching, strings)
- **IL v1.2** — [`1.2/`](il/1.2/): block parameter features
- **Advanced** — `break_label.il`, `random_three.il`, `summary.il`, `trace_min.il`
- **Debugging** — `debug_script.il` + `debug_script.txt`: debugger integration demo

---

## 🔗 C++ Embedding

The [`embedding/`](embedding/) directory demonstrates embedding the Zanna VM in C++ host applications:

| File | Description |
|------|-------------|
| `stepping_example.cpp` | Single-step debugger API |
| `register_times2.cpp` | Registering native C++ functions as IL externs |
| `combined.cpp` | TCO, extern registration, opcode counters, polling |

> See the [VM Guide](../docs/internals/vm.md) for the full embedding API.

---

## 🔨 Building and Running

### Run a demo directly

```sh
zanna run examples/games/chess/          # Zia game
zanna run examples/apps/paint/           # Zia app
./build/src/tools/ilrun/ilrun examples/il/ex1_hello_cond.il  # IL program
```

### Build all demos to native binaries

```sh
# macOS / Linux
./scripts/build_demos.sh
./scripts/build_demos.sh --clean   # Clean rebuild

# Windows
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean --run
```

On Windows, each native binary and its declared assets are output to an owned
`examples/bin/<demo>/` directory, avoiding cross-demo filename collisions.
Before publication, Windows validates each staged PE32+ image and its requested
machine architecture. `--run` copies that stage and only its declared assets
into a private temporary directory, accepts a clean exit or a healthy timeout,
and terminates the complete process tree before removing the isolated run directory.
Set `ZANNA_DEMO_TIMEOUT` to a positive number of seconds to change the default
five-second launch window. The Windows driver uses `-O0` for demos affected by
the tracked native checked-integer optimizer issue so its published binaries
remain runnable.

### Build a single demo

```sh
zanna build examples/apps/paint/ -o paint
./paint
```
