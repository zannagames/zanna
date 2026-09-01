---
status: active
audience: public
last-verified: 2026-08-31
---

# Getting Started with Zanna

Welcome to Zanna! This guide will help you build and run the Zanna compiler toolchain.

---

## Platform Guides

For detailed, platform-specific installation instructions (prerequisites, exact install commands, and troubleshooting):

- **[macOS](getting-started/macos.md)**
- **[Linux](getting-started/linux.md)**
- **[Windows](getting-started/windows.md)**

---

## Prerequisites

Before you begin, ensure you have:

- **CMake** ≥ 3.20
- **C++20 Compiler**: Clang (canonical), GCC, or MSVC
- **Ninja** (optional): Faster builds — select it with `ZANNA_CMAKE_GENERATOR=Ninja`
- **Python 3.x** (optional): Required by some helper scripts in `scripts/`

---

## Building Zanna

The platform build scripts configure, compile, test, and install Zanna in one step:

```sh
# macOS
./scripts/build_zanna_mac.sh

# Linux
./scripts/build_zanna_linux.sh

# Windows
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```
The macOS and Linux scripts are thin wrappers over `./scripts/build_zanna_unix.sh`, which you can also run directly on any Unix system.

### Build Directory Layout

The canonical day-to-day build tree is `build/`, produced by the platform build scripts.
Specialized local lanes use separate gitignored trees so stale instrumented binaries do
not get confused with the normal toolchain:

| Directory | Produced by |
|-----------|-------------|
| `build/` | `build_zanna_mac.sh`, `build_zanna_linux.sh`, `build_zanna_win.ps1` |
| `build-coverage/` | `scripts/coverage.sh` |
| `build-fuzz/` | `scripts/fuzz_smoke.sh` |
| `build_asan_full/`, `build_ubsan_full/`, `build_tsan_full/` | `scripts/ci_full_sanitizer.sh` |
| `cmake-build-*` | IDEs such as CLion |
| `coverage/` | HTML/text coverage reports |

Use `./scripts/clean.sh` or `scripts\clean.ps1` to remove generated build/report
directories after confirming the printed list. If a command behaves unexpectedly, verify
which binary you are running with `which zanna` (or `where zanna` on Windows).

---

## Verify the Installation

After building, confirm the primary tool works correctly:

```sh
zanna --version
```

This should display the Zanna version and IL version without errors.

---

## Create a New Project

Use `zanna init` to scaffold a new project:

```sh
zanna init my-app
```

This creates a project directory with a manifest and entry-point source file:

```text
my-app/
  zanna.project    # Project manifest (name, version, language, entry point, profile, optimize level)
  main.zia         # Entry-point source file
```

Run the new project:

```sh
zanna run my-app
```

### Options

| Option              | Description                                        |
|---------------------|----------------------------------------------------|
| `--lang zia\|basic` | Source language for the entry file (default: `zia`) |
| `--`                | Treat the next token as the project name           |
| `-h`, `--help`      | Show help for `zanna init`                         |

### Examples

```sh
# Create a Zia project (default)
zanna init my-app

# Create a BASIC project
zanna init calculator --lang basic
```

---

## Quick Start: Run Your First Program

### BASIC

```sh
zanna run examples/zbasic/ex1_hello_cond.bas
```

**Expected output:**

```text
HELLO
READY
10
10
```

### Zia

Create a file `hello.zia`:

```zia
module Hello;

bind Zanna.Terminal;

func start() {
    Say("Hello, World!");
}
```

Run it:

```sh
zanna run hello.zia
```

**Expected output:**

```text
Hello, World!
```

---

## Try the Interactive REPL

For quick experimentation, launch the interactive REPL:

```sh
zanna repl
```

The REPL lets you type Zia code and see results immediately:

```text
zia> "Hello from the REPL"
Hello from the REPL
zia> 2 + 3 * 4
14
zia> var x = 42
zia> x
42
zia> func square(n: Integer) -> Integer { return n * n; }
zia> square(7)
49
```

Type `.help` for available commands and `.quit` to exit. See the [REPL Guide](tools/repl.md) for full documentation.

---

## Working with IL Programs

You can inspect the generated IL or run IL programs directly:

```sh
# Emit IL from BASIC
zanna build examples/zbasic/ex1_hello_cond.bas

# Emit IL from Zia
zanna build hello.zia

# Save IL to a file
zanna build examples/zbasic/ex1_hello_cond.bas -o hello.il

# Run the IL program directly
ilrun hello.il
```

For more examples, see the **[BASIC Tutorial](tutorials/basic-tutorial.md)**,
**[Zia Tutorial](tutorials/zia-tutorial.md)**, and the `examples/` directory.

---

## Command Reference

### Primary Tools

| Tool        | Purpose                                   | Example                     |
|-------------|-------------------------------------------|-----------------------------|
| `zanna`     | Unified compiler driver — run and build   | `zanna run program.zia`     |
| `zanna init`| Scaffold a new project                    | `zanna init my-app`         |
| `zbasic`    | Run/compile BASIC programs                | `zbasic script.bas`         |
| `zia`       | Run/compile Zia programs                  | `zia program.zia`           |
| `ilrun`     | Execute IL programs                       | `ilrun program.il`          |
| `il-verify` | Verify IL correctness                     | `il-verify program.il`      |
| `il-dis`    | Disassemble IL                            | `il-dis program.il`         |

> **Note:** `zanna run` is the recommended way to run programs. It auto-detects the language
and can run entire project directories.

---

## Key Concepts

### Unified Error Model

All frontends, IL, and the VM share a consistent error and trap model. Diagnostics remain uniform regardless of entry
point.

> **Learn more:** See [specs/errors.md](specs/errors.md) for trap kinds, handler semantics, and BASIC `ON ERROR` lowering rules.

### Deterministic Numerics

Zanna guarantees consistent numeric behavior across all platforms and execution modes:

- **Overflow checking**: Both frontends use checked integer arithmetic — `+`, `-`, and `*` trap with `Overflow` rather than wrapping
- **Division**: Zia `/` on integers is integer division; BASIC has separate `/` (float) and `\` (integer)
- **Modulo**: Remainder preserves the dividend's sign (C11 semantics)
- **Conversions**: Casts use checked variants that trap when values are out of range
- **Rounding**: All rounding uses round-to-nearest-even (IEEE 754 default)

> **Learn more:** See [specs/numerics.md](specs/numerics.md) for complete numeric semantics.

---

## What to Read Next

**Language Documentation:**

- **[BASIC Tutorial](tutorials/basic-tutorial.md)** — Learn Zanna BASIC by example
- **[BASIC Reference](languages/basic-reference.md)** — Complete BASIC language reference
- **[Zia Tutorial](tutorials/zia-tutorial.md)** — Learn Zia by example
- **[Zia Reference](languages/zia-reference.md)** — Complete Zia language reference
- **[IL Guide](il/il-guide.md)** — Comprehensive IL documentation

**Implementation Guides:**

- **[Frontend How-To](internals/frontend-howto.md)** — Build your own language frontend

**Developer Documentation:**

- [architecture.md](internals/architecture.md) — System architecture overview
- [vm.md](internals/vm.md) — VM and runtime internals
- [contributor-guide.md](internals/contributor-guide.md) — Contribution guidelines
