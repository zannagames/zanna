---
status: active
audience: public
last-verified: 2026-07-26
---

# Zanna FAQ

Frequently asked questions about the Zanna compiler toolchain.

---

## General

### 1. What is Zanna?

Zanna is a from-scratch compiler toolchain and game development platform. Two language frontends—Zia and BASIC—lower to
a shared intermediate language (IL) that runs on a virtual machine or compiles to native code, all on top of a
comprehensive runtime library. The IL layer cleanly separates language semantics from execution, which makes Zanna a
practical foundation both for building programs and games and for exploring language implementation, compiler design,
and runtime systems.

### 2. What makes Zanna different?

Zanna uses a modern compiler architecture with an IL that separates language semantics
from execution. Programs can run in a VM for development/debugging or be compiled to native code for performance. The IL
layer makes it easy to add new language frontends—Zia and BASIC both compile to the same IL and share a common
runtime.

### 3. Is Zanna suitable for production use?

Zanna is under active development and has not yet reached a stable 1.0 release, so it is not yet recommended for
production systems. It already implements substantial subsets of Zia and BASIC over a broad runtime library, and is
well suited to learning language and compiler internals, experimentation, and building games and tools.

---

## Getting Started

### 4. How do I build Zanna?

```bash
./scripts/build_zanna_linux.sh   # Linux
./scripts/build_zanna_mac.sh     # macOS
```

On Windows, run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1`.

Requirements: CMake 3.20+ and a C++20 compiler (Clang is canonical; GCC and MSVC also
work). See the platform guides for [macOS](getting-started/macos.md),
[Linux](getting-started/linux.md), and [Windows](getting-started/windows.md).

### 5. How do I run a program?

```bash
zanna run myprogram.zia
zanna run myprogram.bas
```

You can also run an entire project directory:

```bash
zanna run examples/games/chess/
zanna run examples/apps/paint/
```

The standalone tools `zbasic`, `zia`, and `ilrun` are also available:

```bash
zbasic myprogram.bas
zia myprogram.zia
ilrun program.il
```

### 6. Where can I find example programs?

**In this repo (curated examples):**

- `/examples/games/crackman/` - Crackman maze-chase game
- `/examples/games/chess/` - Full chess engine with an AI opponent
- `/examples/games/vtris/` - Full Tetris game in BASIC demonstrating OOP, graphics, and game-loop patterns
- `/examples/games/frogger-basic/`, `/examples/games/centipede-basic/` - BASIC games
- `/examples/apps/paint/` - Drawing application
- `/examples/3d/` - The Game3D learning ladder (hello-triangle through a streamed open world)
- `/examples/apiaudit/` - Focused runtime API examples in both Zia and BASIC
- `/src/zannastudio/` - Zanna Studio source, built separately from the examples
- `/src/tests/zia/` - Frontend tests covering specific Zia language features
- `/src/tests/basic/` - Frontend tests covering specific BASIC language features

**In the [zannademos repository](https://github.com/zannagames/zannademos) (large showcase demos):**

- `games/xenoscape/` - Ten-region action Metroidvania
- `games/ashfall/` - Nine-mission sci-fi FPS campaign
- `games/3dbowling/`, `games/ridgebound/`, `games/3dscene/` - 3D games over `Zanna.Game3D` / `Zanna.Graphics3D`
- `games/xenoscape-scenes/`, `games/ashfall-scenes/` - The same games with regions authored as `.scene2d` / `.scene3d` documents instead of code
- `games/frogger/`, `games/centipede/`, `games/pacman-basic/` and more
- `apps/zannasql/`, `apps/webserver/`, `apps/telnet/`, `sqldb-basic/` - Larger applications

### 7. What platforms does Zanna support?

Zanna builds and runs on:

- **macOS** (Apple Silicon)
- **Linux** (x86-64 and AArch64)
- **Windows** (x86-64)

Native code generation targets x86-64 (System V and Windows x64 ABIs) and AArch64.

---

## Zia Language

### 8. What is Zia?

Zia is Zanna's primary language, designed as a modern, clean systems programming language. It includes:

- Module system for code organization
- Classes and structs with methods and fields
- Strong static typing (Integer, Boolean, String, etc.)
- Structured control flow (if/else, while, for, match)
- Functions with type annotations
- Bind system for multi-file projects
- First-class support for the Zanna runtime library

### 9. How do I write a simple Zia program?

```zia
module Main;

bind Zanna.Terminal;

func start() {
    Say("Hello, world!");
}
```

Run it with:

```bash
zanna run hello.zia
```

### 10. What are classes in Zia?

Classes are Zia's object-oriented construct:

```zia
class Counter {
    expose Integer value;

    expose func init(start: Integer) {
        value = start;
    }

    expose func increment() {
        value = value + 1;
    }
}
```

Use `expose` to make fields and methods visible outside the class.

### 11. How do I organize multi-file Zia projects?

Use modules and binds:

```zia
module MyModule;

bind "./OtherModule";

func useOther() {
    OtherModule.someFunction();
}
```

See `/examples/games/chess/` for a complete multi-module Zia game example.

---

## BASIC Language

### 12. What BASIC dialect does Zanna implement?

Zanna BASIC is inspired by classic BASIC (especially QBasic/QuickBASIC) with modern extensions. It includes:

- Structured control flow (If/Then/Else, For/Next, While/Wend, Do/Loop, Select Case)
- Procedures (Sub/Function) with parameters
- Object-oriented programming (Class, Sub New, method calls)
- Arrays (single and multi-dimensional)
- String manipulation
- File I/O
- ANSI terminal graphics (COLOR, LOCATE, CLS)

### 13. Does Zanna BASIC support object-oriented programming?

Yes! Zanna BASIC includes:

- Class definitions with fields and methods
- Constructors (`Sub New`)
- Object instantiation (`New` keyword)
- Reference-based object semantics
- Method calls and field access

See `/examples/games/vtris/` for extensive OOP examples.

### 14. Are there any known language limitations or gotchas?

Key limitations to be aware of:

- **Case-insensitive** identifiers (parameter names can collide with field names)
- **Object assignment creates references**, not copies (use `New` for independent objects)
- **No `CALL` statement**, and no VB-style `SET obj = ...` assignment — assign objects directly. (`SET` does exist, but only as the setter half of a `Property ... Get ... Set ... End Set` block.)
- **Type suffixes required** for string-returning builtins (use `Str$`, `Chr$`, not `Str`, `Chr`)

### 15. How do I use the AddFile keyword for modular programs?

Use `AddFile` to include other BASIC source files:

```basic
AddFile "utilities.bas"
AddFile "../../lib/graphics.bas"

' Now you can use classes/functions from included files
Dim helper As HelperClass
helper = New HelperClass()
```

Paths are relative to the file containing the `AddFile` statement.

---

## IL (Intermediate Language)

### 16. What is the Zanna IL?

The Zanna Intermediate Language is a low-level, typed, control-flow graph representation that sits between frontends (
Zia, BASIC) and backends (VM or native code). It's similar to LLVM IR or .NET CIL but designed specifically for this
project's needs.

See `/docs/il/il-guide.md` for the complete IL specification.

### 17. Can I write IL code directly?

Yes! The IL has a textual assembly syntax. You can write `.il` files and run them:

```bash
zanna run myprogram.il
```

Tools available:

- `il-dis` - Disassembler
- `il-verify` - IL verifier
- `ilrun` - IL runner

### 18. How does the compilation pipeline work?

```text
Source (Zia/BASIC) → Parser → Semantic Analysis → IL Generation → IL Transforms →
  ├─→ VM (for development/debugging)
  └─→ Native Codegen (for performance)
```

The IL layer provides optimization passes, verification, and serialization. Different backends can consume the same IL.

---

## VM and Runtime

### 19. What's the difference between VM and native execution?

- **VM**: Executes IL directly. Slower but includes debugging support (breakpoints, stepping, watches).
  Default execution mode.
- **Native**: Compiles IL to machine code. Much faster but fewer debugging features.

For development, use VM mode. For performance testing, use native compilation.

### 20. How do I debug programs?

The VM supports source-level debugging:

```bash
# Build to IL, then set a source breakpoint in the IL runner
zanna build program.zia -o /tmp/program.il
zanna -run /tmp/program.il --break-src program.zia:42

# Use debugger commands
# (watch variables, step through code, inspect state)
```

See VM debugging tests in `/src/tests/vm/` for examples.

### 21. What runtime functions are available?

Both frontends (Zia, BASIC) share the same runtime library. Sample built-in functions:

- **Math**: BASIC `SIN`/`COS`/`TAN`/`SQR`/`ABS`/`ROUND`/`FIX`/`POW`; Zia `Sin`/`Cos`/`Sqrt`/`Abs`/`Floor`/`Ceil`/`Pow` (under `Zanna.Math.*`)
- **String**: BASIC `LEN`/`MID$`/`LEFT$`/`RIGHT$`/`LCASE$`/`UCASE$`/`TRIM$` (concatenation uses `+`, not a function); Zia `Substring`/`Mid`/`Concat`/`Trim` on `Zanna.String`
- **I/O**: BASIC `PRINT`/`INPUT`/`LINE INPUT`/`OPEN`; Zia `Say`/`Print`/`TryReadLine`/`ReadLineResult` under `Zanna.Terminal`
- **Graphics**: BASIC `COLOR`/`LOCATE`/`CLS`; Zia `Zanna.Graphics.Canvas` and friends
- **Conversion**: BASIC `STR$`/`VAL`/`CINT`/`CLNG`/`CSNG`/`CDBL`; Zia `Zanna.Core.Convert.ToInt64`/`ToStringInt`/`ToStringDouble`

See the respective builtin registries in `/src/frontends/zia/` and `/src/frontends/basic/builtins/`
for language-specific function lists.

---

## Development and Contributing

### 22. How do I add a new built-in function?

1. Add the function signature to the builtin registry for the frontend
2. Implement the lowering logic in the frontend
3. Add the runtime implementation in `/src/runtime/`
4. Add tests

See `/docs/internals/frontend-howto.md` for detailed guidance.

### 23. How do I report bugs or request features?

- **Bugs**: Open a GitHub issue
- **Features**: Open a discussion or create an issue describing the use case
- **Contributing**: Follow the Conventional Commits format for commit messages

### 24. Where can I find more documentation?

Key documentation files:

- `/docs/il/il-guide.md` - Complete IL specification (normative reference)
- `/docs/internals/architecture.md` - System architecture overview
- `/docs/internals/codemap.md` - Source code organization and navigation
- `/docs/internals/frontend-howto.md` - Guide to frontend development
- `/docs/tutorials/zia-tutorial.md` - Zia language tutorial
- `/docs/languages/zia-reference.md` - Zia language reference
- `/docs/tutorials/basic-tutorial.md` - BASIC language tutorial
- `/docs/languages/basic-reference.md` - BASIC language reference
- `/CLAUDE.md` - Development workflow and contribution guidelines

For code-level documentation, see header comments in source files.

---

## Quick Reference

**Build and run a Zia program:**

```bash
zanna run program.zia
```

**Build and run a BASIC program:**

```bash
zanna run program.bas
```

**Run with debugging:**

```bash
ilrun program.il --break main:10
```

**View generated IL:**

```bash
zia program.zia --emit-il
zbasic program.bas --emit-il
```

**Run tests:**

```bash
ctest --test-dir build --output-on-failure
```

**Format code:**

```bash
clang-format -i <files>
```
