---
status: active
audience: public
last-verified: 2026-07-15
---

# Zanna Compiler Platform - Release Notes

> **Development Status**: Pre-Alpha
> These are early development releases. Zanna is under active development and not ready for production use.
> Future milestones will define supported releases when appropriate.

## Version 0.2.1 - Pre-Alpha (February 2026)

### Release Overview

Version 0.2.1 is a broad advancement across every layer of the platform. The runtime library
expands from 70 to 197 classes, the Zia language gains generics and function references, the
compiler backend acquires a new optimizer pipeline and a second bytecode VM, and Windows becomes
a fully supported build target. This release also ships ZannaSQL — a PostgreSQL-compatible SQL
engine written entirely in Zia — as the flagship showcase project.

---

### New Features

#### Runtime Library — 197 Classes (up from 70)

Runtime coverage expands: 127 new classes land
across every namespace, bringing data formats, cryptography, concurrency, and
game development support into the standard library.

**Data Formats (`Zanna.Text.*`)**

Full parsing and serialization for all common structured data formats:

- `Json` — Full JSON parsing, serialization, and **JSONPath** query expressions
- `Xml` — XML DOM parsing, traversal, and **XPath** support
- `Yaml` — YAML 1.2 parser with thread-safe error reporting
- `Toml` — TOML configuration file parsing
- `Csv` — RFC 4180-compliant CSV reader and writer
- `Markdown` — Markdown-to-HTML converter
- `Ini` — INI configuration file parsing
- `Html` — HTML class encoding and decoding

Example — parse JSON and query with JSONPath:

```zia
bind Zanna.Text;

func start() {
    var doc = Json.Parse(`{"users":[{"name":"Alice"},{"name":"Bob"}]}`);
    var names = Json.Path(doc, "$.users[*].name");
    Terminal.Say(names.Get(0));  // "Alice"
}
```

**Cryptography (`Zanna.Crypto.*` and `Zanna.Text.*`)**

- `Hash` — MD5, SHA-1, SHA-256, SHA-512, BLAKE2b/s
- `Aes` — AES-128/192/256 encryption (CBC and GCM modes)
- `Cipher` — Stream ciphers: ChaCha20 and Salsa20
- `Password` — Password hashing: bcrypt and Argon2id
- `KeyDerive` — PBKDF2-SHA256 and HKDF key derivation
- `Rand` (enhanced) — Cryptographically-secure random bytes, integers, and floats

**Collections (`Zanna.Collections.*`)**

New specialized collections join the existing `List`, `Map`, `Queue`, `Stack`, `Ring`, and `Seq`:

- `Set` — Hash set with content-aware equality
- `SortedSet` — Ordered set with range queries
- `Bimap` — Bidirectional mapping (look up by key or value)
- `Countmap` — Key-to-count aggregation map
- `Defaultmap` — Map with default-value initialization
- `Frozenmap` / `Frozenset` — Immutable snapshot collections
- `IntMap` — Integer-keyed map optimized for dense ranges
- `Orderedmap` — Insertion-order-preserving map
- `Weakmap` — Weak-reference value map (does not prevent GC)
- `Trie` — Prefix tree for string lookups and autocomplete
- `SparseArray` — Compact storage for sparse integer-indexed data
- `UnionFind` — Disjoint-set union with path compression
- `BloomFilter` — Probabilistic membership testing
- `LRUCache` — Least-recently-used eviction cache
- `Deque` — Double-ended queue

Functional sequence operations are now available on `Seq`:

```zia
var evens = Seq.Of(1, 2, 3, 4, 5, 6)
    .Filter(x => x % 2 == 0)
    .Map(x => x * x)
    .ToList();   // [4, 16, 36]
```

**Arbitrary-Precision Integers (`Zanna.Math.BigInt`)**

Full arbitrary-precision integer arithmetic: addition, subtraction, multiplication, division,
modulo, power, GCD, bitwise operations, and decimal/hex string conversion.

**Matrix and Quaternion Math**

- `Mat3` — 3×3 matrix (2D transforms: translate, rotate, scale)
- `Mat4` — 4×4 matrix (3D transforms, projection)
- `Quat` — Quaternion rotations with slerp interpolation

**Content-Aware Equality**

`List`, `Map`, `Set`, and `Seq` now implement structural equality: two collections are equal
when they contain structurally equal elements. This enables correct `==` comparisons and use
as map keys.

**Game Development Abstractions**

A complete toolkit for 2D game logic:

- `Grid2D` — 2D spatial grid for tilemap and region queries
- `Timer` — Frame-based countdown and repeating timer
- `StateMachine` — State graph with guarded transitions
- `Tween` — Keyframe animation with 20+ easing curves (ease-in-out, cubic, elastic, bounce, spring…)
- `SmoothValue` — Exponential smoothing for camera follow and UI animations
- `Collision` — AABB rectangles and overlap detection
- `Particle` / `ParticleEmitter` — Configurable particle system (velocity, lifetime, respawn)
- `Quadtree` — Spatial partitioning for broad-phase collision queries
- `PathFollow` — Entity movement along waypoint paths
- `ScreenFX` — Full-screen effects: fade, flash, shake
- `SpriteAnimation` — Frame-based animation with named clip definitions
- `ButtonGroup` — Mutually-exclusive button state management
- `ObjPool` — Generic typed object pool

Example — sprite animation with easing:

```zia
var tween = Tween.New(0.0, 100.0, 0.5, Easing.ElasticOut);
var timer = Timer.New(2.0);  // fires every 2 seconds

func update(dt: Float) {
    tween.Update(dt);
    timer.Update(dt);
    sprite.X = tween.Value();
}
```

**Action Mapping System (`Zanna.Input.ActionMap`)**

Device-agnostic input abstraction: bind logical actions (Jump, Fire, MoveLeft) to any
combination of keyboard keys, mouse buttons, or gamepad buttons. Bindings are hot-reloadable
at runtime.

```zia
var actions = ActionMap.New();
actions.Bind("Jump", Key.Space, PadButton.A);
actions.Bind("Fire", Mouse.Left, PadButton.RightTrigger);

func update() {
    if actions.JustPressed("Jump") { player.Jump(); }
}
```

**Object Pool (`Zanna.Core.Pool`)**

Generic free-list pool with configurable initial capacity. Reduces allocation pressure for
high-frequency object creation (particles, projectiles, UI elements).

#### Zia Language

**Generic Functions and Types**

Full parametric polymorphism with type inference and interface constraints:

```zia
func max<T: Comparable>(a: T, b: T) -> T {
    return a > b ? a : b;
}

class Pair<A, B> {
    A first;
    B second;
}

func start() {
    var m = max(3, 7);            // T inferred as Integer
    var s = max("cat", "dog");    // T inferred as String
    var p = new Pair<String, Integer>();
    p.first  = "answer";
    p.second = 42;
}
```

Generic entities, interfaces, and functions are fully supported. The compiler monomorphizes
each instantiation and emits stable mangled names (`List$I64`, `Pair$String$Integer`, etc.).

**Function References**

First-class function values using the `&` operator:

```zia
func double(x: Integer) -> Integer { return x * 2; }

func apply(f: &(Integer) -> Integer, v: Integer) -> Integer {
    return f(v);
}

func start() {
    var result = apply(&double, 21);  // 42
}
```

**Short-Circuit Evaluation**

`and` / `or` operators now short-circuit. The right operand is only evaluated when the left
does not determine the result — essential for safe nil guards:

```zia
if user != null and user.IsActive() { ... }      // IsActive() not called if user is null
if cache.Has(key) or loadFromDisk(key) { ... }   // loadFromDisk() skipped on cache hit
```

**Type Narrowing**

Flow-sensitive type narrowing for optional values. After a null check, the type of the
variable is automatically narrowed within the truthy branch:

```zia
func greet(name: String?) {
    if name != null {
        Terminal.Say("Hello, " + name);  // name is String here, not String?
    }
}
```

**`bind` Module Keyword**

The module import keyword is now `bind` (renamed from `import`). All demos, tests, and
documentation have been updated:

```zia
bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Math.BigInt;
```

**`zanna init` — Project Scaffolding**

New subcommand creates a complete project skeleton:

```bash
zanna init myapp --lang zia
```

Generates:

```text
myapp/
├── zanna.project      # name, version, lang, entry
└── main.zia           # starter module with bind + start()
```

**`--` argument separator**

Pass command-line arguments to the running program without the tool intercepting them:

```bash
zia myserver.zia -- --port 8080 --config prod.toml
```

#### Compiler — Bytecode VM

A second, independent execution engine is now available alongside the existing IL VM. The
bytecode VM uses a stack-based evaluation model designed for lightweight interpretation and
embedded use cases:

- **Stack-based architecture** with per-frame locals, operand stack, and alloca buffer
- **Two dispatch modes**: portable switch dispatch and faster threaded dispatch (computed goto on Clang/GCC)
- **Exception handling**: typed trap kinds (Overflow, DivByZero, IndexOutOfBounds, NullPointer, StackOverflow, InvalidCast)
- **Debug support**: breakpoints, single-stepping, source-line mapping per frame
- **Native handler integration**: can bypass or route through RuntimeBridge
- **Full Windows support**: switch-based dispatch available for MSVC targets

The bytecode VM coexists with the IL VM; both share the same runtime library and native
code generation backends.

#### Compiler — IL Optimizer

Several new analyses and passes join the optimization pipeline:

**MemorySSA + Dead Store Elimination**

`MemorySSA` builds def-use chains for loads, stores, and calls with a key precision
improvement over conservative analysis: calls are *transparent* for non-escaping allocas.
DSE uses MemorySSA to eliminate dead stores in O(uses) time rather than O(blocks × stores).

**Post-Dominator Tree**

`PostDomTree` via Cooper-Harvey-Kennedy iterative analysis on the reversed CFG. Used by
advanced control-flow analyses and future optimization passes.

**Improved Inliner**

The inliner's cost model is significantly relaxed:

| Parameter | Before | After |
|---|---|---|
| Instruction threshold | 32 | 80 |
| Block budget | 4 | 8 |
| Inlining depth | 2 | 3 |
| Constant-argument bonus | — | +4 |
| Single-use callee bonus | — | +10 |
| Tiny function bonus (≤8 instrs) | — | +16 |

CallGraph SCC analysis (Tarjan algorithm) detects mutually-recursive functions so they are
never inlined, preventing unbounded code growth.

**SCCP Improvements**

Sparse Conditional Constant Propagation now treats block parameters as SSA ϕ-nodes that
merge only from *executable* predecessors, pruning unreachable branches earlier in the pipeline.

**Mem2Reg — Non-Entry Allocas**

Memory-to-register promotion is no longer restricted to the entry block. Any alloca whose
address does not escape is now promoted to SSA temporaries throughout the function body.

**CheckOpt — Constant Elimination**

Bounds and division checks with all-constant operands are eliminated after SCCP rewrites
constants: `IdxChk(5, 0, 10)` and `SDivChk0(x, 3)` are statically proved safe and removed.

**New IL Opcodes**

Three new opcodes for precise floating-point semantics:

- `FCmpOrd` — Ordered comparison: true only when neither operand is NaN
- `FCmpUno` — Unordered comparison: true when either operand is NaN
- `ConstF64` — Load a 64-bit float constant from its I64 bit-pattern payload

#### Compiler — Backend Improvements

**x86-64 Peephole Optimizer**

New passes in the peephole pipeline:

- **Dead Code Elimination** — Pre-computed register liveness at block exits enables
  elimination of provably dead instructions without per-instruction set operations in the hot loop
- **Cold block layout** — Trap-only and error-handling blocks are moved to the end of the
  function body, keeping the hot path contiguous in the instruction cache

**AArch64 Block Layout**

A new `BlockLayoutPass` runs after register allocation and applies greedy trace-based
reordering: for each unconditional branch, the target block is placed immediately after the
branching block. This enables the peephole optimizer to eliminate the resulting fall-through
branches and improves branch predictor effectiveness.

**Stack Safety**

Native-compiled programs now install a platform-appropriate stack overflow handler in the
`main` function prologue:

- **macOS / Linux** — `sigaltstack` + `SIGSEGV`/`SIGBUS` handler on an alternate signal stack
- **Windows** — Vectored exception handler for `EXCEPTION_STACK_OVERFLOW` using I/O primitives
  safe for depleted-stack conditions

On overflow, a human-readable diagnostic is printed and the process exits cleanly.

#### Windows Platform Support

Windows (x86-64) is now a fully supported build target for both MSVC and Clang-cl:

- **`rt_platform.h`** — Unified platform detection and compatibility shim layer:
  - Thread-local storage (`__declspec(thread)` vs `__thread`)
  - Full 32-bit, 64-bit, and pointer atomics via `_Interlocked*` intrinsics
  - Memory barriers: `_mm_mfence()` / `_ReadWriteBarrier()` / `__dmb()`
  - POSIX shims: `access`, `getcwd`, `mkdir`, `strcasecmp`, `lseek`, `strtok_r`, `localtime_r`
  - File-type macros: `S_ISREG`, `S_ISDIR`, `S_ISBLK`, `S_ISCHR`, `S_ISFIFO`
  - High-resolution timer via `FILETIME` (`rt_windows_time_ms`, `rt_windows_time_us`)
- **Windows build script** (`scripts/build_zanna_win.ps1`)
- **x86-64 codegen fixes** — Correct Windows calling convention (RCX/RDX/R8/R9, shadow space)
- **Test suite** — Golden files and exit-code handling updated for Windows differences

#### VM — Performance Optimizations

**`FunctionExecCache`** — Pre-resolved operand cache per (function, block) pair:

- **Reg** (Temp values): direct register ID without map lookup
- **ImmI64** (ConstInt): immediate value stored inline
- **ImmF64** (ConstFloat): bit-identical I64 representation, zero allocation
- **Cold path** (ConstStr, GlobalAddr, NullPtr): routed to `VM::eval()` only when needed

**`SwitchCache`** — Lazily-built per-ExecState switch target map; eliminates per-frame rebuilds.

**Value struct union** — Mutually exclusive fields (`i64`, `f64`, `id`) share storage, saving
8+ bytes per operand value in the IL representation.

**String interning** — FNV-1a open-addressing hash table; equal strings share a single
canonical pointer. Equality becomes an O(1) pointer comparison. Exposed as
`Zanna.Core.StringIntern`.

**Typed i64 list** (`rt_list_i64`) — Unboxed `int64_t` dynamic list with separate `len`/`cap`
fields and amortized O(1) push. Eliminates per-element boxing overhead vs the generic `rt_list`.

**Vec2 / Vec3 object pool** — Thread-local LIFO free list (32 slots). The object finalizer
recycles deallocated vectors back into the pool via `rt_obj_resurrect()`, eliminating repeated
heap allocation in tight game loops.

#### Runtime API Consistency

A comprehensive naming audit brings uniform conventions across all 197 classes:

- **Verb consistency**: `List.Add` → `Push`, `GetItem` → `Get`, `Queue.Enqueue` → `Push`, `Dequeue` → `Pop`
- **Simplified names**: `GetHashCode` → `HashCode`, `ReferenceEquals` → `RefEquals`, `Guid` → `Uuid`
- **Set verbs**: `Put` → `Add`, `Drop` → `Remove`
- **Namespace**: `Zanna.Strings` renamed to `Zanna.String`
- **C internal prefix**: All internal string helpers normalized to `rt_str_*`

#### Runtime Directory Reorganization

The 390-file flat `src/runtime/` directory is restructured into 11 logical subdirectories
matching the library namespace hierarchy:

```text
src/runtime/
├── core/         heap, strings, math, datetime, formatting, GC
├── arrays/       typed arrays and lists (i64, f64, str, obj)
├── oop/          object model, boxing, Option, Result, Lazy
├── collections/  maps, sets, queues, game abstractions
├── text/         JSON, XML, YAML, regex, crypto, codecs
├── io/           files, streams, directories, archives
├── system/       process execution, machine info
├── graphics/     GUI, rendering, input, vectors, physics
├── audio/        playback and playlists
├── threads/      async, channels, futures, threadpool
└── network/      HTTP, WebSocket, TLS, REST
```

All consumers (`#include "rt_foo.h"`) require zero changes — CMake PUBLIC include-path
propagation makes all subdirectories visible to every consumer target automatically.

#### ZannaSQL — Showcase Project

`demos/zia/sqldb/` is a PostgreSQL-compatible relational database engine written entirely in
Zia, demonstrating the platform's capability for production-scale systems programming:

- **60,000+ lines** of Zia source across 109 files
- **Full SQL surface**: SELECT, INSERT, UPDATE, DELETE, DDL, CTEs, window functions, subqueries, triggers, sequences
- **Storage engine**: disk-based B-tree indexes, WAL-based crash recovery (ARIES-style), MVCC snapshot isolation
- **Multi-user server**: PostgreSQL wire protocol v3, MD5/SCRAM authentication, system catalog views, tempdb
- **4,985+ test assertions** covering correctness, concurrency, and crash recovery

```bash
# Interactive REPL
zia demos/zia/sqldb/main.zia

# Server mode — connect with psql, DBeaver, or any PostgreSQL client
zia demos/zia/sqldb/server.zia -- --port 5432
psql -h localhost -p 5432 -U admin zanna
```

#### New Demos

| Demo | Language | Description |
|---|---|---|
| `demos/zia/sqldb/` | Zia | PostgreSQL-compatible SQL engine (60K+ lines) |
| `demos/zia/webserver/` | Zia | HTTP/1.1 server with routing, config, thread-safe logging |
| `demos/zia/zannaide/` | Zia | IDE application with editor, build, and services panels |
| `demos/zia/pacman/` | Zia | Pac-Man rewritten using new game abstraction classes |
| `demos/basic/particles/` | BASIC | Particle system showcase |

---

### Project Statistics

| Metric              | v0.2.0  | v0.2.1  | Change   |
|---------------------|---------|---------|----------|
| Total Lines (LOC)   | 566,000 | ~900,000 | +~334,000 |
| C/C++ Source Files  | —       | 1,542   | —        |
| Runtime Classes     | 70      | 197     | +127     |
| Demo Source Files   | 130     | 664     | +534     |
| Language Frontends  | 2       | 2       | —        |
| Test Count          | —       | 1,196   | All pass |
| ZannaSQL            | —       | 60,000+ lines | New |

---

### Bug Fixes

- **BUG-FE-008**: Chained method calls returned `Function` type instead of the method's return type — fixed in `Sema_Expr_Call.cpp`
- **BUG-FE-009**: `List[Boolean]` unboxing emitted wrong IL return type (`I1` instead of `I64`) — `kUnboxI1` handler corrected
- **BUG-FE-010**: Cross-class `Ptr` type annotation lookup failed for foreign runtime classes — Sema now searches all runtime classes when primary lookup fails
- **BUG-NAT-001**: AArch64 regalloc double-incremented instruction counter, causing wrong spill victim selection under register pressure — duplicate `++currentInstrIdx_` removed from `RegAllocLinear.cpp`
- **BUG-NAT-006**: AArch64 callee stack-parameter loading used hardcoded physical registers (X9/X10) conflicting with regalloc — replaced with virtual registers throughout
- **BUG-005**: Race condition in webserver request parsing — fixed with correct mutex scoping
- Fixed `Pow` VM handler ABI mismatch (hidden `bool*` output parameter)
- Fixed `rt_concat` use-after-free via `ConsumingStringHandler`
- Fixed GUID generation thread race with double-checked locking
- Fixed `rt_time` high-resolution counter overflow on Windows (QPC multiplication)
- Fixed `rt_numfmt` INT64_MIN saturation (negation was undefined behavior)
- Fixed AArch64 peephole copy propagation (over-aggressive register aliasing)
- Fixed WebSocket frame masking on big-endian hosts
- Fixed VM block-parameter transfer for TCO and nested calls
- Fixed `void` main: programs with a `start()` / `main()` that returns void now exit with code 0
- 54 additional runtime API bug fixes from comprehensive audit

---

### Breaking Changes

1. **`bind` replaces `import` in Zia**

   ```zia
   // Old
   import Zanna.Terminal;
   // New
   bind Zanna.Terminal;
   ```

2. **`Zanna.Strings` renamed to `Zanna.String`**

   ```zia
   // Old
   bind Zanna.Strings;
   // New
   bind Zanna.String;
   ```

3. **Collection API verb changes**

   | Old | New |
   |---|---|
   | `List.Add(x)` | `List.Push(x)` |
   | `List.GetItem(i)` | `List.Get(i)` |
   | `List.SetItem(i, x)` | `List.Set(i, x)` |
   | `Queue.Enqueue(x)` | `Queue.Push(x)` |
   | `Queue.Dequeue()` | `Queue.Pop()` |
   | `Set.Put(x)` | `Set.Add(x)` |
   | `Set.Drop(x)` | `Set.Remove(x)` |
   | `GetHashCode()` | `HashCode()` |
   | `Guid` type | `Uuid` type |

4. **Pascal frontend removed** — Pascal source files are no longer compiled

5. **`ilc` renamed to `zanna`** — Update any scripts that invoke `ilc` directly

---

### Migration from v0.2.0

#### Update `import` to `bind`

```bash
# Bulk rename (macOS/Linux)
find . -name "*.zia" -exec sed -i '' 's/^import /bind /g' {} +
```

#### Update `Zanna.Strings`

```bash
find . -name "*.zia" -exec sed -i '' 's/Zanna\.Strings/Zanna.String/g' {} +
```

#### Update collection method names

Replace `List.Add(` → `List.Push(`, `Queue.Enqueue(` → `Queue.Push(`, etc. A full mapping
is in the Breaking Changes section above.

---

### Architecture

```text
┌──────────────┐  ┌──────────────┐
│ BASIC Source │  │  Zia Source  │
│    (.bas)    │  │    (.zia)    │
└──────┬───────┘  └──────┬───────┘
       │                 │
       ▼                 ▼
┌─────────────────────────────────────────────────────┐
│                     Zanna IL                        │
│      SimplifyCFG → SCCP → EarlyCSE → Mem2Reg       │
│         → DSE (MemorySSA) → CheckOpt → Inliner      │
└─────────────────────┬───────────────────────────────┘
                      │
      ┌───────────────┼───────────────┐
      ▼               ▼               ▼
┌──────────┐    ┌──────────┐    ┌──────────┐
│  IL VM   │    │  x86-64  │    │ AArch64  │
│ Bytecode │    │  Native  │    │  Native  │
│    VM    │    └──────────┘    └──────────┘
└──────────┘
```

---

### Feature Comparison

| Feature               | v0.2.0           | v0.2.1                      |
|-----------------------|------------------|-----------------------------|
| Runtime Classes       | 70               | 197 (+127)                  |
| Zia Generics          | No               | Full (type params + constraints) |
| Zia Function Refs     | No               | Yes (`&func` operator)      |
| Zia Short-Circuit     | No               | Yes (`and` / `or`)          |
| Zia Type Narrowing    | No               | Flow-sensitive optionals    |
| Zia Module Keyword    | `import`         | `bind`                      |
| `zanna init`          | No               | Project scaffolding         |
| IL Optimizer          | Baseline         | MemorySSA DSE, post-doms, improved inliner |
| Bytecode VM           | No               | Stack-based, threaded dispatch |
| Windows Support       | Partial          | Full (MSVC + Clang-cl)      |
| Stack Safety          | No               | Platform guard pages        |
| New IL Opcodes        | —                | FCmpOrd, FCmpUno, ConstF64  |
| String Interning      | No               | O(1) canonical pointer eq.  |
| Game Abstractions     | Basic            | Grid2D, Tween, StateMachine, Particle, … |
| BigInt                | No               | Arbitrary-precision         |
| JSON / XML / YAML     | No               | Full (with JSONPath, XPath) |
| ZannaSQL              | Prototype        | 60K+ lines, PostgreSQL wire protocol |
| Documentation         | 613 files        | 800+ files                  |

---

### v0.2.x Roadmap

Remaining v0.2.x focus areas:

- Zia debugger integration with breakpoints and watch expressions
- GUI library maturation (more widget types, accessibility)
- BASIC frontend OOP interop with Zia modules
- Extended native code generation coverage
- Continuous runtime API stability improvements

---

*Zanna Compiler Platform v0.2.1 (Pre-Alpha)*
*Released: February 2026*
*Note: This is an early development release. Future milestones will define supported releases when appropriate.*

---

## Version 0.2.0 - Pre-Alpha (January 2026)

### Release Overview

Version 0.2.0 opens a new development phase focused on runtime expansion and frontend stability. This release renames
ZannaLang to **Zia**, adds a **GUI widget library**, introduces **simplified CLI tools**, and expands the runtime
with networking, input handling, and game development infrastructure.

### Platform Changes

#### ZannaLang Renamed to Zia

The ZannaLang frontend has been renamed to **Zia**. This includes:

- New file extension: `.zia` (replaces `.zanna`)
- New compiler tool: `zia`
- Updated documentation: `zia-getting-started.md`, `zia-reference.md`

Example:

```zia
module Hello;

func start() {
    Zanna.Terminal.Say("Hello from Zia!");
}
```

Run with:

```bash
./build/src/tools/zia/zia hello.zia
```

#### Simplified CLI Tools

New user-friendly compiler drivers replace verbose `zanna` subcommands:

| Old Command | New Command |
|-------------|-------------|
| `zanna front basic -run file.bas` | `zbasic file.bas` |
| `zanna front zia -run file.zia` | `zia file.zia` |
| `zanna -run file.il` | `ilrun file.il` |

The `zanna` tool remains available for advanced use cases.

### New Features

#### GUI Widget Library (`Zanna.GUI.*`)

A new cross-platform GUI widget library (~26,000 lines):

- `App` — Application window with event loop
- `VBox` / `HBox` — Vertical and horizontal layout containers
- `Button` — Clickable buttons with labels
- `Slider` — Value sliders
- `Theme` — Light and dark theme support
- IDE-oriented widgets for building development tools

Example:

```zia
func main() {
    var app = Zanna.GUI.App.New("My App", 800, 600);
    Zanna.GUI.Theme.SetDark();

    var container = Zanna.GUI.VBox.New();
    container.SetSpacing(8.0);
    app.Root.AddChild(container);

    var button = Zanna.GUI.Button.New("Click Me");
    container.AddChild(button);

    app.Run();
}
```

#### Audio Library

Basic audio playback support (~3,000 lines) for sound effects and music.

#### Networking (`Zanna.Network.*`)

TCP and UDP networking with HTTP client support:

- `Tcp` — TCP client with send/receive, timeouts, line-based protocols
- `TcpServer` — TCP server with listen/accept
- `Udp` — UDP sockets with multicast support
- `Dns` — Name resolution, reverse lookup, local address queries
- `Http` — Simple HTTP GET/POST/HEAD requests
- `HttpReq` / `HttpRes` — Full HTTP client with headers and body handling
- `Url` — URL parsing, encoding, query parameter manipulation

#### Input Handling (`Zanna.Input.*`)

Keyboard, mouse, and gamepad input for interactive applications:

- `Keyboard` — Polling and event-based keyboard input, modifier state, text input mode
- `Mouse` — Position, button state, wheel, capture/release, delta movement
- `Pad` — Gamepad support with analog sticks, triggers, buttons, vibration

#### Game Development (`Zanna.Graphics.*`)

Sprite, tilemap, and camera support for 2D games:

- `Sprite` — Animated sprites with multiple frames, collision detection, origin point
- `Tilemap` — Tile-based level rendering with configurable tile size
- `Camera` — Viewport scrolling, class following, world/screen coordinate conversion
- `Color` — RGB/HSL conversion, lerp, brighten/darken utilities

#### I/O Additions (`Zanna.IO.*`)

- `Archive` — ZIP file creation, extraction, and inspection
- `Compress` — Gzip and deflate compression/decompression
- `MemStream` — In-memory binary stream with typed read/write
- `Watcher` — Filesystem change notifications

#### Threading Additions (`Zanna.Threads.*`)

- `Gate` — Semaphore with permit counting
- `Barrier` — N-party synchronization barrier
- `RwLock` — Reader-writer lock with writer preference

#### Crypto Additions (`Zanna.Crypto.*`)

- `KeyDerive` — PBKDF2-SHA256 key derivation
- `Rand` — Cryptographically secure random bytes and integers

#### Text Processing (`Zanna.Text.*`)

- `Pattern` — Regular expression matching, replacement, and splitting
- `Template` — Simple template rendering with placeholder substitution

### New Demos

**Zia:**
- `paint/` — Full-featured paint application with brushes, tools, color palette, and layers
- `vedit/` — Text editor demonstrating the GUI widget library
- `sql/` — Embedded SQL database with REPL
- `telnet/` — Telnet client and server
- `gfx_centipede/` — Graphical Centipede game
- `ladders/` — Platform game demo

**BASIC + Zia:**
- `sqldb/` — SQL database implementation in both languages

### Project Statistics

| Metric              | v0.1.3  | v0.2.0  | Change   |
|---------------------|---------|---------|----------|
| Total Lines (LOC)   | 369,000 | 566,000 | +197,000 |
| Runtime Classes     | 44      | 70      | +26      |
| GUI Library         | —       | 26,000  | New      |
| Audio Library       | —       | 3,000   | New      |
| Zia Source Files    | —       | 130     | New ext  |

### Documentation

- Zia language reference and getting started guide
- "The Zanna Bible" — comprehensive programming book (in progress)
- Network library reference (TCP, UDP, HTTP, DNS, URL)
- Input handling guide (keyboard, mouse, gamepad)
- Graphics additions (sprite, tilemap, camera, color)
- Performance analysis documentation
- CLI redesign documentation

### Architecture

```text
┌──────────────┐  ┌──────────────┐
│ BASIC Source │  │  Zia Source  │
│    (.bas)    │  │    (.zia)    │
└──────┬───────┘  └──────┬───────┘
       │                 │
       ▼                 ▼
┌─────────────────────────────────────────────────────┐
│                     Zanna IL                        │
└─────────────────────────┬───────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │    VM    │    │  x86-64  │    │ AArch64  │
    └──────────┘    └──────────┘    └──────────┘
```

### Breaking Changes

1. **ZannaLang renamed to Zia**: Update file extensions from `.zanna` to `.zia`
2. **New CLI tools**: Use `zbasic`, `zia`, `ilrun` instead of `zanna` subcommands

### Migration from v0.1.3

#### Updating ZannaLang Files

Rename your files and update the tool:

```bash
# Old
./build/src/tools/zanna/zanna front zannalang -run program.zanna

# New
mv program.zanna program.zia
./build/src/tools/zia/zia program.zia
```

#### Updating BASIC Invocations

```bash
# Old
./build/src/tools/zanna/zanna front basic -run program.bas

# New
./build/src/tools/zbasic/zbasic program.bas
```

### v0.2.x Roadmap

This release opens the v0.2.x development phase, which will focus on:

- Runtime library expansion and stability
- BASIC and Zia frontend hardening
- GUI library maturation
- macOS native code generation improvements
- Additional test coverage

---

*Zanna Compiler Platform v0.2.0 (Pre-Alpha)*
*Released: January 2026*
*Note: This is an early development release. Future milestones will define supported releases when appropriate.*
