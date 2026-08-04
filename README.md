<p align="center">
  <img src="misc/images/zannalogo2.png" alt="Zanna" width="160">
</p>

<h1 align="center">Zanna</h1>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
  <img src="https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-brightgreen" alt="Platform">
</p>

**Zanna** is a from-scratch, IL-first compiler toolchain and virtual machine for building platform-native applications and games. Source languages lower to a typed intermediate language, **[Zanna IL](docs/il/il-guide.md)**, which can run on the [VM](docs/internals/vm.md), feed the optimizer, or compile to native code through the built-in backends.

**[Zia](docs/languages/zia-reference.md)** is the flagship language: a modern, statically typed language with classes, generics, enums, lambdas, modules, pattern matching, and direct access to the Zanna runtime. A **[BASIC](docs/languages/basic-reference.md)** frontend is included for education, compatibility experiments, and quick prototypes.

> **Status:** Pre-alpha, active development. The current source tree is `v0.2.99`; the IL reference is `0.3.0`. APIs, diagnostics, IL rules, and tooling are still evolving.

---

## Download

**Latest tagged release:** [v0.2.7-dev](https://github.com/zannagames/zanna/releases/tag/v0.2.7-dev) (2026-06-30)

- [Source (tar.gz)](https://github.com/zannagames/zanna/archive/refs/tags/v0.2.7-dev.tar.gz)
- [Source (zip)](https://github.com/zannagames/zanna/archive/refs/tags/v0.2.7-dev.zip)
- [Release notes](docs/release_notes/Zanna_Release_Notes_0_2_7.md)

**In development:** `v0.2.99` on `master`. See the [draft v0.2.99 release notes](docs/release_notes/Zanna_Release_Notes_0_2_99.md).

```sh
git clone https://github.com/zannagames/zanna.git
cd zanna
```

---

## Quickstart

Build, test, and install the toolchain with the platform scripts:

```sh
# macOS
./scripts/build_zanna_mac.sh

# Linux
./scripts/build_zanna_linux.sh

# Windows
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```

Verify the build:

```sh
zanna --version
```

Create and run a project:

```sh
zanna init my-app              # Zia project (default)
zanna init my-app --lang basic # BASIC project
zanna run my-app
```

Try the [REPL](docs/tools/repl.md):

```sh
zanna repl
```

```text
zia> 2 + 3 * 4
14
zia> Say("Hello from Zanna")
Hello from Zanna
```

See the [Getting Started Guide](docs/getting-started.md) for platform-specific setup, build directory layout, and troubleshooting.

---

## Components

| Component | Description |
|-----------|-------------|
| **[Zia](docs/languages/zia-reference.md)** | Statically typed application language with classes, generics, modules, lambdas, enums, and pattern matching |
| **[BASIC](docs/languages/basic-reference.md)** | Educational and prototyping frontend that lowers to the same IL |
| **[Zanna IL](docs/il/il-guide.md)** | Typed, block-structured SSA-style IR with a normative `0.3.0` reference |
| **[Optimizer](docs/il/il-passes.md)** | Registered O1/O2 pipelines covering SSA promotion, SCCP, GVN, LICM, loop cleanup, inlining, devirtualization, runtime fast paths, and cleanup passes |
| **[VM](docs/internals/vm.md)** | IL execution engine for fast bring-up, tests, debugging, and step-budgeted runs |
| **[Native backends](docs/internals/backend.md)** | AArch64 and x86-64 code generators with backend optimization, register allocation, assembly emission, and executable output |
| **[Assembler](docs/internals/native-assembler.md) / [Linker](docs/internals/native-linker.md)** | In-tree ELF, Mach-O, and PE object/link support with DWARF and platform packaging integration |
| **[Runtime](docs/zannalib/README.md)** | Shared standard library for collections, graphics, 3D, GUI, games, networking, crypto, text, threads, localization, and more |
| **[Language servers](docs/tools/zia-server.md)** | Zia and BASIC servers with LSP and MCP modes for editors and AI coding tools |
| **[Zanna Studio](src/zannastudio/README.md)** | IDE written in Zia on the Zanna GUI runtime: semantic editing, project search/replace, side-by-side diffs, VM debugging with structured inspection, PTY terminal, and Git source control |
| **[Tools](docs/tools/cli.md)** | Unified `zanna` driver plus `zia`, `vbasic`, `ilrun`, `il-verify`, `il-dis`, REPL, package, installer, and benchmark commands |

### Why Zanna?

- **IL thin waist**: Zia, BASIC, and future frontends share one typed IR, verifier, optimizer, VM, and native backend path.
- **Self-contained native toolchain**: Zanna includes its own runtime, assembler, linker, object writers, package generation, and install packaging paths.
- **Machine-readable tooling**: `zanna check`, `zanna eval`, `zanna explain`, `--dump-runtime-api`, and `--dump-opcodes` expose JSON-friendly surfaces for editors, scripts, and AI agents.
- **Cross-platform by design**: macOS, Linux, and Windows are first-class targets; platform checks are centralized in adapter layers.
- **Runtime-first apps and games**: The standard library includes 2D/3D graphics, GUI widgets, game systems, audio, networking, localization, threading, and structured data APIs.

---

## Examples

The [examples](examples/README.md) tree includes curated applications, games, 3D scenes, API audits, language samples, IL programs, and C++ embedding demos.

| Demo | Description |
|------|-------------|
| [Paint](examples/apps/paint/) | Drawing app with tools, layers, file dialogs, zoom, and undo/redo |
| [Chess](examples/games/chess/) | GUI chess with alpha-beta AI and drag-and-drop play |
| [Crackman](examples/games/crackman/) | Maze chase game with pathfinding and mode-driven AI |
| [Game3D starter & scenes](examples/3d/) | The Game3D learning ladder, from hello-triangle to a streamed open world |

```sh
zanna run examples/games/chess/
zanna build examples/apps/paint/ -o paint
./scripts/build_demos.sh
```

The larger showcase titles — XENOSCAPE, Ashfall, 3D Bowling, Ridgebound,
ZannaSQL, and more — live in the
[zannademos repository](https://github.com/zannagames/zannademos), which
tests and builds them against a Zanna checkout.

---

## Architecture

```text
+-------------------------+
|    Source Languages     |
|      Zia / BASIC        |
+-----------+-------------+
            |
            v
+-------------------------+
|  Parser / Sema / Lower  |
+-----------+-------------+
            |
            v
+-------------------------+
|       Zanna IL          |
|  Verifier / Optimizer   |
+------+------------+-----+
       |            |
       v            v
+-------------+  +-----------------+
|     VM      |  | Native Backends |
|  Interpreter|  | AArch64/x86-64  |
+------+------+  +-------+---------+
       |                 |
       +--------+--------+
                v
+-------------------------+
|      Zanna Runtime      |
| Collections / Graphics  |
| GUI / Game / Network    |
| Text / Threads / ...    |
+-------------------------+
```

See the [Architecture Overview](docs/internals/architecture.md) and [Code Map](docs/internals/codemap.md) for subsystem details.

---

## IL at a Glance

Frontends lower to typed [Zanna IL](docs/il/il-guide.md) that is compact, explicit, and inspectable.

**Zia source:**

```rust
module Hello;

bind Zanna.Terminal;
bind Fmt = Zanna.Text.Fmt;

func start() {
    var x = 2 + 3;
    var y = x * 2;
    Say("HELLO");
    Say(Fmt.Int(y));
}
```

**Representative IL:**

```llvm
il 0.3.0
extern @Zanna.Text.Fmt.Int(i64) -> str
extern @Zanna.Terminal.Say(str) -> void
global const str @.L0 = "HELLO"
func @main() -> void {
entry_0:
  %t0 = iadd.ovf 2, 3
  %t1 = imul.ovf %t0, 2
  %t2 = const_str @.L0
  call @Zanna.Terminal.Say(%t2)
  %t3 = call @Zanna.Text.Fmt.Int(%t1)
  call @Zanna.Terminal.Say(%t3)
  ret
}
```

Use `--dump-il`, `--dump-il-opt`, and `zanna il-opt` to inspect the pipeline. The [IL Quickstart](docs/il/il-guide.md#quickstart) is the practical introduction; [IL Guide](docs/il/il-guide.md) is the normative reference.

---

## Runtime Library

All frontends share the [Zanna Runtime Library](docs/zannalib/README.md). The runtime surface is generated from the live registry and spans:

- Collections, core types, functional helpers, math, text, structured data, I/O, time, and utilities
- 2D graphics, 3D graphics, Game3D, GUI widgets, input, audio, and game systems
- Networking, crypto, diagnostics, memory controls, threading, system APIs, and localization

Authoritative runtime inventory:

```sh
zanna --dump-runtime-api
```

Authoritative IL opcode inventory:

```sh
zanna --dump-opcodes
```

---

## Tools

| Command                                | Purpose |
|----------------------------------------|---------|
| `zanna run <file\|dir>` | Build and run a source file, project directory, or manifest |
| `zanna build <file\|dir> -o <out>` | Build IL or a native executable |
| `zanna check <file\|dir> --diagnostic-format=json` | Type-check and verify without running; JSON diagnostics include ranges, notes, and fixits |
| `zanna eval 'expr' --json --type --il` | Evaluate a Zia or BASIC snippet through the REPL pipeline |
| `zanna explain <CODE> --json`          | Explain a diagnostic code from the central catalog |
| `zanna repl [zia & basic]`             | Interactive REPL |
| `zanna -run <file.il>`                 | Execute an IL module directly, with optional tracing and step limits |
| `zanna package <dir>`                  | Package an application for macOS, Linux, Windows, or tarball output |
| `zanna install-package`                | Package the Zanna binary tools and Zanna Studio into a platform installer |
| `zia` / `vbasic`                       | Standalone source compiler entry points |
| `zia-server` / `vbasic-server`         | Language servers with LSP and MCP modes |
| `ilrun`, `il-verify`, `il-dis`         | Direct IL execution, verification, and disassembly |
| `zanna il-opt`                         | Run and inspect optimizer pipelines |
| `zanna bench`                          | IL benchmark runner |

See the [installer and package release guide](docs/installer-release.md) for
native signing, checksums, artifact inventories, release workflows, and clean-VM
install/upgrade/uninstall validation.

Common examples:

```sh
zanna run program.zia
zanna -run program.il --max-steps 100000
zanna build project/ -o app
zanna check project/ --diagnostic-format=json
zanna eval '2 + 3 * 4' --json --type
zanna explain V-ZIA-UNDEFINED --json
zanna --dump-runtime-api
zanna --dump-opcodes
```

See the [Tools Reference](docs/tools/cli.md), [Debugging Guide](docs/tools/debugging.md), and [MCP tool reference](docs/tools/zia-server-mcp-tools.md) for full details.

---

## Building

### Requirements

- CMake 3.20+
- C++20 compiler: Apple Clang, Clang, GCC 11+, or MSVC

### Build Scripts

```sh
# macOS
./scripts/build_zanna_mac.sh

# Linux
./scripts/build_zanna_linux.sh

# Windows
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```

The scripts configure, build, test, and install Zanna. The Unix wrappers delegate to `scripts/build_zanna_unix.sh`.

Useful iteration knobs:

| Variable | Effect |
|----------|--------|
| `ZANNA_SKIP_CLEAN=1` | Incremental rebuild |
| `ZANNA_SKIP_TESTS=1` | Build only |
| `ZANNA_TEST_LABEL=<label>` | Run one CTest label |

Targeted checks after a build:

```sh
ctest --test-dir build -L codegen --output-on-failure
ctest --test-dir build -R test_zia_lexer --output-on-failure
./scripts/example_smoke.sh --fast
```

Platform guides:

- [macOS](docs/getting-started/macos.md)
- [Linux](docs/getting-started/linux.md)
- [Windows](docs/getting-started/windows.md)

---

## Documentation

**Getting Started**: [Setup Guide](docs/getting-started.md), [REPL Guide](docs/tools/repl.md), [Zia Tutorial](docs/tutorials/zia-tutorial.md), [The Zanna Book](docs/book/README.md)

**Language References**: [Zia](docs/languages/zia-reference.md), [BASIC](docs/languages/basic-reference.md), [IL Guide](docs/il/il-guide.md), [IL Quickstart](docs/il/il-guide.md#quickstart)

**Runtime & APIs**: [Runtime Library](docs/zannalib/README.md), [3D Graphics](docs/graphics3d-guide.md), [Game Engine](docs/gameengine/README.md), [GUI](docs/zannalib/gui/README.md)

**Internals**: [Architecture](docs/internals/architecture.md), [VM](docs/internals/vm.md), [Code Map](docs/internals/codemap.md), [Backend](docs/internals/backend.md), [IL Passes](docs/il/il-passes.md)

**Contributors**: [Contributor Guide](docs/internals/contributor-guide.md), [Frontend How-To](docs/internals/frontend-howto.md), [Testing](docs/internals/testing.md)

---

## Contributing

Zanna is in active development and the architecture is still stabilizing. Small fixes, documentation improvements, bug reports, and focused tests are welcome.

Before proposing changes:

- Read the relevant spec or reference first; [IL Guide](docs/il/il-guide.md) is normative for IL.
- Keep the product dependency-free.
- Preserve macOS, Windows, and Linux behavior.
- Use the platform build scripts and keep tests green.
- Follow [Conventional Commits](https://www.conventionalcommits.org/) for commit messages.

See [Contributor Guide](docs/internals/contributor-guide.md) and [Testing](docs/internals/testing.md) for the full workflow.

---

## License

Zanna is licensed under the **GNU General Public License v3.0** (GPL-3.0-only).

See [LICENSE](LICENSE) for the full text.
