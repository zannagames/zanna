---
status: active
audience: public
last-verified: 2026-07-15
---

# Zanna Compiler Platform - Release Notes

> **Development Status**: Pre-Alpha  
> These are early development releases. Zanna is under active development and not ready for production use.  
> Future milestones will define supported releases when appropriate.

## Version 0.1.3 - Pre-Alpha (December 20, 2025)

### Release Overview

Version 0.1.3 adds a new language frontend (Zia), expands the runtime library with additional
classes, and introduces basic threading support.

### New Features

#### Zia Frontend

New language with C-style syntax:

- Entity types (reference semantics) and value types (copy semantics)
- Generic collections: `List[T]`, `Map[K,V]`
- Module system with `bind` (originally named `import` in early v0.1.3 builds)
- String interpolation with `${...}`
- Type inference

Example:

```zia
module Hello;

class User {
    String name;
    Integer age;
    
    func greet() -> String {
        return "Hello, ${name}!";
    }
}

func start() {
    var user = new User();
    user.name = "Alice";
    user.age = 30;
    Zanna.Terminal.Say(user.greet());
}
```

#### Runtime Library Additions

The runtime library now includes 44 classes (up from 14). New additions:

**Collections** (`Zanna.Collections.*`):
- `Bag` — String set with union, intersect, difference
- `Bytes` — Byte array with Base64/Hex conversion
- `Map` — Hash map with string keys
- `Queue` — FIFO collection
- `Ring` — Fixed-size circular buffer
- `Seq` — Growable array with push/pop/slice
- `Stack` — LIFO collection
- `TreeMap` — Sorted map with floor/ceil queries

**I/O** (`Zanna.IO.*`):
- `Dir` — Directory listing, creation, deletion
- `Path` — Path manipulation (join, ext, stem)
- `BinFile` — Binary file stream
- `LineReader` — Line-by-line text reading
- `LineWriter` — Buffered text writing

**Threading** (`Zanna.Threads.*`):
- `Thread` — OS thread creation with Start/Join/Sleep
- `Monitor` — Mutex with Wait/Pause
- `SafeI64` — Atomic integer operations

**Math**:
- `Bits` — Bit manipulation (shift, rotate, popcount)
- `Vec2` — 2D vector operations
- `Vec3` — 3D vector operations

**Text** (`Zanna.Text.*`):
- `Codec` — Base64, Hex, URL encoding
- `Csv` — CSV parsing and formatting
- `Uuid` — UUID generation

**Crypto** (`Zanna.Crypto.*`):
- `Hash` — CRC32, MD5, SHA1, SHA256

**System**:
- `Exec` — External command execution
- `Machine` — System info (cores, memory, OS)
- `Log` — Logging with levels
- `Fmt` — Number formatting

**Time** (`Zanna.Time.*`):
- `Clock` — Sleep and tick counting
- `Countdown` — Interval timer

**Graphics**:
- `Canvas` — 2D graphics (replaces Window)
- `Pixels` — Software pixel buffer

**Diagnostics**:
- `Stopwatch` — Performance timing

#### Enhancements to Existing Classes

- `Zanna.String` — Added `Split`, `Replace`, `Repeat`, `PadLeft`, `PadRight`, `StartsWith`, `EndsWith`
- `Zanna.Math` — Added `Atan2`, `Hypot`, `Log2`, `Log10`, `Clamp`, `Lerp`, `Round`, plus `Pi`, `E`, `Tau` constants
- `Zanna.IO.File` — Added `Append`, `Copy`, `Move`, `Size`, `Modified`, `ReadAllLines`, `ReadAllBytes`
- `Zanna.Environment` — Added `GetVariable`, `SetVariable`, `HasVariable`, `IsNative`
- `Zanna.Collections.List` — Added `Find`, `Has`, `Insert`, `Remove`

#### Common Frontend Infrastructure

Shared components extracted for frontend reuse:

- `BlockManager`, `ConstantFolding`, `InstructionEmitter`
- `KeywordTable`, `LexerBase`, `LoopContext`
- `NameMangler`, `ScopeTracker`

#### New Demos

**BASIC**: Pac-Man, Particles

**Zia**: Frogger, class examples, collection demos

### Project Statistics

| Metric              | v0.1.2  | v0.1.3  | Change   |
|---------------------|---------|---------|----------|
| Total Lines (LOC)   | 243,000 | 369,000 | +126,000 |
| C/C++ Source Files  | 1,372   | 1,638   | +266     |
| Runtime Classes     | 14      | 44      | +30      |
| Language Frontends  | 1       | 2       | +1       |
| Test Files          | 530     | 551     | +21      |
| Documentation Files | 584     | 613     | +29      |

### Documentation

- Zia reference and getting started guide
- Runtime library documentation (12 files covering all namespaces)
- Threading ADR

### Architecture

```text
┌──────────────┐  ┌───────────────┐
│ BASIC Source │  │  Zia Source  │
└──────┬───────┘  └──────┬────────┘
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

---

## Version 0.1.2 - Pre-Alpha (December 6, 2025)

### Release Overview

Version 0.1.2 is a pre-alpha development release that delivers an OOP runtime system and advances the AArch64 native
backend.

### New Features

#### OOP Runtime System

Complete object-oriented runtime exposing C functions as clean classes:

- **Three-layer architecture**: RuntimeClasses.inc (declarative catalog), RuntimeSignatures.inc (IL-to-C bridge), and C
  implementations
- **Core classes fully operational**:
    - `Zanna.String` — Length, Substring, Concat, Trim, ToUpper, ToLower, IndexOf
    - `Zanna.Collections.List` — Add, Clear, RemoveAt, Count, indexed access
    - `Zanna.Collections.Dictionary` — Set, Get, ContainsKey, Remove, Clear
    - `Zanna.Text.StringBuilder` — Append, ToString, Clear, Length, Capacity
    - `Zanna.Math` — Abs, Sqrt, Sin, Cos, Tan, Floor, Ceil, Pow, Min, Max
    - `Zanna.DateTime` — Now, Year, Month, Day, Hour, Format, Create, AddDays
    - `Zanna.IO.File` — Exists, ReadAllText, WriteAllText, Delete
    - `Zanna.Console` — WriteLine, ReadLine
    - `Zanna.Random` — Seed, Next, NextInt
    - `Zanna.Environment` — GetArgumentCount, GetArgument, GetCommandLine
    - `Zanna.Graphics.Window` — Full 2D graphics with drawing primitives

Example:

```basic
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
sb.Append("Hello, ")
sb.Append("World!")
PRINT sb.ToString()

DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()
list.Add(obj1)
list.Add(obj2)
PRINT list.Count
```

#### AArch64 Native Backend

The ARM64 backend has matured from early development to functional:

- **All major demos compile to native binaries**: Chess (357KB), vTris (298KB), Frogger (271KB)
- **35+ runtime symbol mappings** added for terminal, string, parsing, and object operations
- **Full pipeline working**: BASIC → IL → Assembly → Object → Linked Executable
- **Automated regression tests** for native compilation

Example:

```bash
# Compile Frogger to native ARM64
./build/src/tools/zanna/zanna front basic -emit-il demos/basic/frogger/frogger.bas > frogger.il
./build/src/tools/zanna/zanna codegen arm64 frogger.il -S frogger.s
as frogger.s -o frogger.o
clang++ frogger.o build/src/runtime/libzanna_runtime.a -o frogger_native
./frogger_native
```

#### New Demo: Centipede

Classic arcade game implementation showcasing OOP and terminal graphics:

- ANSI color graphics
- Multiple game modules using `AddFile` directive
- Scoreboard with level progression
- Spider and centipede enemy AI

### Project Statistics

| Metric              | v0.1.2  |
|---------------------|---------|
| Total Lines (LOC)   | 242,000 |
| Source Lines (SLOC) | 155,000 |
| C/C++ Source Files  | 1,353   |
| Test SLOC           | 42,000  |

### Bug Fixes

- **BUG-ARM-004**: Stack offset exceeds ARM64 immediate range
- **BUG-ARM-005**: Duplicate trap labels in generated assembly
- **BUG-ARM-009**: Frontend wrong parameter name in IL (root cause of ARM-006, ARM-008)
- Fixed all ARM64 linker symbol resolution issues
- Resolved runtime class method binding edge cases

---

## Version 0.1.1 - Pre-Alpha (November 22, 2025)

### Release Overview

Version 0.1.1 is a pre-alpha development release that introduces native code generation, complete object-oriented
programming support, and a modernized runtime architecture. This release represents significant progress toward an
eventual 1.0 milestone.

### New Features

#### Native Code Generation

Transform BASIC and IL programs into native machine code (experimental):

- **x86-64 backend** implementation
    - Linear scan register allocation
    - Instruction selection with pattern matching
    - Peephole optimizations for generated assembly
    - Integration with system linkers and runtime libraries
    - Direct execution of compiled native binaries
- **AArch64 backend** infrastructure (early development)

Example:

```bash
# Compile to native executable
./vbasic program.bas -o program
./program
```

#### Object-Oriented Programming

Complete OOP implementation for the BASIC frontend:

- Classes with single inheritance
- Interface support with multiple implementation
- Virtual method dispatch with vtable implementation
- Method modifiers: `PUBLIC`, `PRIVATE`, `VIRTUAL`, `OVERRIDE`, `ABSTRACT`, `FINAL`
- Runtime type information with `IS` and `AS` operators
- Base class method invocation via `BASE.Method()`

Example:

```basic
CLASS Shape
  PRIVATE x AS INTEGER
  PRIVATE y AS INTEGER
  
  PUBLIC SUB New(initX AS INTEGER, initY AS INTEGER)
    x = initX
    y = initY
  END SUB
  
  PUBLIC VIRTUAL FUNCTION Area() AS DOUBLE
    RETURN 0.0
  END FUNCTION
END CLASS

CLASS Circle : Shape
  PRIVATE radius AS DOUBLE
  
  PUBLIC OVERRIDE FUNCTION Area() AS DOUBLE
    RETURN 3.14159 * radius * radius
  END FUNCTION
END CLASS
```

#### Namespace System

Organize code with hierarchical namespaces:

- Define nested namespaces with dotted notation
- Import namespaces with `USING` directives
- Create aliases for long namespace paths
- Type resolution through namespace hierarchy
- Case-insensitive matching

Example:

```basic
NAMESPACE Company.Product.Graphics
  CLASS Renderer
    PUBLIC SUB Draw()
      ' Implementation
    END SUB
  END CLASS
END NAMESPACE

' Use the namespace
USING Company.Product.Graphics
DIM r AS Renderer
r.Draw()

' Or create an alias
USING Gfx = Company.Product.Graphics
DIM r2 AS Gfx.Renderer
```

#### Modern Runtime Architecture

Complete redesign of the runtime library:

- **Migration from `rt_*` to `Zanna.*` naming convention**
    - Old: `rt_print_str`, `rt_print_i64`, `rt_file_open`
    - New: `Zanna.Console.PrintStr`, `Zanna.Console.PrintI64`, `Zanna.File.Open`
- **Backward compatibility mode** available via build flag
- **New runtime components**:
    - String builder for efficient text manipulation
    - Enhanced array operations with object support
    - Time and date functionality
    - Command-line argument processing
    - Type registry for runtime reflection

#### Graphics Programming Support

New graphics library for visual applications:

- Cross-platform window management
- Basic drawing primitives (pixels, lines, shapes)
- Foundation for game and visualization development
- Minimal dependencies

Example:

```basic
USING Zanna.Graphics

DIM win AS Window
win = Window.Create(800, 600, "My Game")

' Draw a red rectangle
win.SetColor(255, 0, 0)
win.DrawRect(100, 100, 200, 150)

win.Present()
```

### Developer Experience Improvements

#### Simplified Command-Line Tools

New streamlined tools for common tasks:

- `vbasic` - Direct BASIC interpreter/compiler
- `ilrun` - Direct IL program runner

```bash
# New simplified syntax
./vbasic program.bas              # Run directly
./vbasic program.bas -o output    # Compile to executable
./ilrun program.il                # Run IL code

# Previous syntax (still supported)
./build/src/tools/zanna/zanna front basic -run program.bas
```

#### Enhanced Debugging

- VM stepping debugger for line-by-line execution
- Improved error messages with context
- Nine new diagnostic codes for namespace-related issues
- Stack trace improvements

#### External Function Interface

Register custom C/C++ functions with the VM:

```cpp
// Define a custom function
static void times2_handler(void **args, void *result) {
  const auto x = *reinterpret_cast<const int64_t *>(args[0]);
  *reinterpret_cast<int64_t *>(result) = x * 2;
}

// Register with the VM
il::vm::ExternDesc ext;
ext.name = "times2";
ext.fn = reinterpret_cast<void *>(&times2_handler);
```

### Performance Enhancements

#### VM Optimizations

- Compile-time evaluation via constexpr functions
- Improved inline expansion for hot paths
- Reduced overhead in trap handling
- Optimized opcode dispatch mechanisms

#### Tail Call Optimization

- Automatic detection and optimization of tail-recursive calls
- Stack space savings for functional programming patterns

### Documentation & Examples

#### New Documentation

- Architecture guide
- BASIC frontend implementation details
- Zanna Language Specification v0.1 RC1
- Namespace system reference
- Contributor guide
- VM optimization strategies
- Graphics library integration guide

#### Example Programs

- **vTris** - Complete Tetris implementation showcasing OOP features
- **Frogger** - Arcade game with high score persistence
- External function registration examples
- Namespace usage demonstrations
- OOP design pattern examples

### Build System

#### New CMake Options

```cmake
ZANNA_RUNTIME_NS_DUAL       # Enable dual namespace mode (rt_* and Zanna.*)
ZANNA_VM_TAILCALL           # Enable tail call optimization
IL_SANITIZE_UNDEFINED       # Enable undefined behavior sanitizer
IL_ENABLE_X64_NATIVE_RUN    # Enable native execution support
```

#### Platform Support

- Windows (x86-64)
- Linux (x86-64)
- macOS (Apple Silicon / ARM64)
- Dedicated macOS build script (`buildmac.sh`)

### Project Statistics

| Metric              | v0.1.0   | v0.1.1      | Change |
|---------------------|----------|-------------|--------|
| Lines of Code       | 85,000   | 125,000+    | +47%   |
| Test Cases          | 200      | 500+        | +150%  |
| Documentation Files | 15       | 55+         | +267%  |
| Performance         | Baseline | 2-3x faster | +200%  |

### Bug Fixes

- Fixed numeric overflow handling in arithmetic operations
- Resolved namespace resolution edge cases
- Corrected virtual method dispatch in complex inheritance hierarchies
- Improved memory management in string operations
- Fixed terminal handling across platforms
- Enhanced file I/O error reporting
- Corrected array bounds checking in runtime

### Breaking Changes

1. **Runtime Function Names**: Now use `Zanna.*` namespace by default
    - Enable compatibility: `-DZANNA_RUNTIME_NS_DUAL=ON`

2. **Directory Structure Changes**:
    - Test directory: `/tests` → `/src/tests`
    - TUI subsystem: root → `/src/tui`

### Migration Guide

#### Updating Runtime Calls

Option 1 - Update to new names:

```basic
' Old (v0.1.0)
DECLARE SUB rt_print_str(s AS STRING)
CALL rt_print_str("Hello")

' New (v0.1.1)
DECLARE SUB Zanna.Console.PrintStr(s AS STRING)
CALL Zanna.Console.PrintStr("Hello")
```

Option 2 - Use compatibility mode:

```bash
cmake -S . -B build -DZANNA_RUNTIME_NS_DUAL=ON
```

---

## Version 0.1.0 - Initial Pre-Alpha (October 28, 2025)

### Initial Release Overview

Version 0.1.0 is the first pre-alpha release of the Zanna compiler platform, providing an early working implementation
of a compiler toolchain with a BASIC frontend, typed intermediate language, and virtual machine. This initial release
establishes the foundation for ongoing development toward version 1.0.

### Core Features

#### BASIC Compiler Frontend

Complete BASIC implementation with modern extensions:

- Full lexer, parser, and semantic analyzer
- Type system (integers, floats, strings, arrays)
- Control flow: `IF/THEN/ELSE`, `FOR/NEXT`, `WHILE/WEND`, `DO/LOOP`
- Procedures and functions with parameters
- `SELECT CASE` for multi-way branching
- Line numbers and labels
- `GOSUB/RETURN` for subroutines

#### Zanna Intermediate Language (IL)

Strongly-typed, SSA-inspired intermediate representation:

- Human-readable text format
- Binary format for efficient storage
- Comprehensive type system
- Explicit control flow
- External function declarations

Example:

```llvm
il 0.1
extern @rt_print_str(str) -> void
extern @rt_print_i64(i64) -> void
global const str @.HELLO = "Hello, World!"

func @main() -> i64 {
entry:
  %x = add 2, 3
  %y = mul %x, 2
  call @rt_print_str(const_str @.HELLO)
  call @rt_print_i64(%y)
  ret 0
}
```

#### Virtual Machine

High-performance bytecode interpreter:

- Three dispatch modes:
    - Switch dispatch - Classic switch-based
    - Table dispatch - Function pointer table
    - Threaded dispatch - Direct threading (GCC/Clang)
- Stack-based execution model
- Efficient memory management
- Runtime error handling

#### Runtime Library

Portable C runtime providing:

- String manipulation
- Mathematical operations
- File I/O
- Memory management
- Terminal control
- Random number generation

### Developer Tools

- **zanna** - Compiler driver for BASIC and IL
- **il-verify** - IL validation with diagnostics
- **il-dis** - IL disassembler
- **basic-ast-dump** - AST visualization

### Example Programs

- Text-based Frogger game
- Chess with AI opponent
- Mathematical benchmarks
- File I/O demonstrations
- 180+ test programs

### Architecture

```text
┌──────────────┐
│ BASIC Source │
└──────┬───────┘
       │ Frontend
       ▼
┌──────────────┐
│   Zanna IL   │
└──────┬───────┘
       │ VM
       ▼
┌──────────────┐
│   Execution  │
└──────────────┘
```

### Technical Specifications

- Language support: BASIC
- IL version: 0.1
- Platforms: Windows, Linux, macOS
- Build system: CMake 3.22+
- C++ standard: C++17
- Dependencies: Minimal (standard library only)

---

## Installation

### Prerequisites

- CMake 3.22 or higher
- C++17 compatible compiler (GCC 9+, Clang 10+, MSVC 2019+)
- Git for source code

### Building from Source

```bash
# Clone the repository
git clone https://github.com/your-org/zanna.git
cd zanna

# Configure and build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DZANNA_RUNTIME_NS_DUAL=ON \
  -DZANNA_VM_TAILCALL=ON

cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Install (optional)
cmake --install build --prefix /usr/local
```

### Quick Start

Create `hello.bas`:

```basic
10 PRINT "Hello from Zanna!"
20 FOR i = 1 TO 5
30   PRINT "  Iteration "; i
40 NEXT i
50 END
```

Run it:

```bash
./vbasic hello.bas
```

Compile to native:

```bash
./vbasic hello.bas -o hello
./hello
```

### Example Programs

```bash
# Tetris game (BASIC)
./vbasic demos/basic/vtris/vtris.bas

# Frogger (Zia)
./zia demos/zia/frogger/main.zia
```

---

## Feature Comparison

| Feature           | v0.1.0           | v0.1.1              | v0.1.2          | v0.1.3           |
|-------------------|------------------|---------------------|-----------------|------------------|
| BASIC Compiler    | Complete         | Enhanced with OOP   | Stable          | Stable           |
| Zia         | No               | No                  | No              | Complete         |
| IL Format         | Baseline         | Extended            | Stable          | Stable           |
| VM Execution      | 3 dispatch modes | Optimized           | Stable          | +Threading       |
| Native Codegen    | No               | x86-64 experimental | +AArch64        | Stable           |
| OOP Support       | No               | Classes/Interfaces  | Runtime classes | All frontends    |
| Runtime Classes   | —                | 14                  | 14              | 44               |
| Threading         | No               | No                  | No              | Thread/Monitor   |
| Collections       | —                | List, Dictionary    | +StringBuilder  | +8 types         |
| Graphics          | No               | Basic library       | Stable          | +Canvas, Pixels  |
| Documentation     | 15 files         | 55+ files           | 584 files       | 613 files        |

---

## Future Roadmap

### Path to v1.0 (Milestone)

- Stabilize core APIs and IL format
- Complete test coverage for all components
- Comprehensive error recovery
- Performance optimization and benchmarking
- Reference documentation

### Near Term (v0.2.x - v0.9.x)

- IL optimization passes (mem2reg, SimplifyCFG, LICM)
- Graph coloring register allocation
- Debugger integration with breakpoints
- Generics for Zia

### Post-1.0 Features

- LLVM IR backend option
- WebAssembly target
- Language Server Protocol support
- Package manager for libraries

### Long Term Vision

- Self-hosting compiler
- JIT compilation mode
- Advanced optimizations

---

## Resources

- **Repository**: [GitHub](https://github.com/your-org/zanna)
- **Documentation**: See `docs/` directory
- **Runtime Library**: See `docs/zannalib/` for API reference
- **Examples**: Browse `demos/` for sample programs
- **License**: See LICENSE file

---

*Zanna Compiler Platform v0.1.3 (Pre-Alpha)*  
*Released: December 19, 2025*  
*Note: This is an early development release. Future milestones will define supported releases when appropriate.*
