---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Code Map

Source layout for the Zanna compiler toolchain (current tree, kept in sync).

> For auto-generated files, see [generated-files.md](generated-files.md).

## Quick Reference

| Directory              | Purpose                                                                  |
|------------------------|--------------------------------------------------------------------------|
| `src/buildmeta/`       | Version files (`IL_VERSION`, `VERSION`) used at build time               |
| `src/bytecode/`        | Bytecode VM: compiler, module format, and high-performance interpreter   |
| `src/codegen/`         | Native code generation backends (`x86_64/`, `aarch64/`, `common/`)       |
| `src/common/`          | Cross-cutting utils (mangling, integer helpers, process runner)          |
| `src/frontends/`       | Language frontends: `basic/`, `zia/`, `common/`                          |
| `src/il/`              | IL core types, builder, I/O, verifier, analysis, transforms, linker, API, shared scalar semantics |
| `src/lib/`             | Optional C libraries: ZannaGFX 2D graphics (`graphics/`), ZannaAUD audio (`audio/`), ZannaGUI (`gui/`) |
| `src/parse/`           | Cursor utilities used by frontends (`include/zanna/parse/Cursor.h`)      |
| `src/pass/`            | Generic pass manager façade (`include/zanna/pass/PassManager.hpp`)       |
| `src/repl/`            | Interactive REPL: line editor, input classifier, session, meta-commands, BASIC adapter |
| `src/runtime/`         | C runtime library (strings, collections, I/O, math, graphics, audio, input, networking, threading, text, time, crypto, GC, serialization, physics, async) |
| `src/support/`         | Shared support: diagnostics, arena, source manager, symbols, result      |
| `src/tests/`           | Unit, golden, e2e, and perf tests by area                                |
| `src/tools/`           | CLI tools (`zanna`, `vbasic`, `zia`, `zia-server`, `ilrun`, `il-verify`, `il-dis`, the native Windows installer, etc.) |
| `src/tui/`             | Terminal UI library + demo app (`src/tui/apps/tui_demo.cpp`)             |
| `src/zannastudio/`        | Zanna Studio application (written in Zia) — editor, language services, build/debug workbench |
| `src/vm/`              | Virtual Machine interpreter, opcode handlers, debug, runtime bridge      |
| `include/`             | Public headers (`zanna/il`, `zanna/vm`, `zanna/runtime`, `zanna/...`)    |

## Subsystems

- Bytecode: `src/bytecode` (compiler, module, VM implementation)
- Codegen: `src/codegen/{aarch64,common,x86_64}`
- Frontends: `src/frontends/{basic,common,zia}`
- IL: `src/il/{analysis,api,build,core,internal,io,link,runtime,semantics,transform,utils,verify}`
- Libraries: `src/lib/{graphics,audio,gui}`, `src/tui`
- Runtime: `src/runtime` (C sources and headers)
- Support & Infra: `src/{common,parse,pass,support}`
- REPL: `src/repl` (line editor, input classifier, session, meta-commands)
- Tests: `src/tests/{e2e,golden,perf,smoke,unit,...}`
- Tools: `src/tools/{basic,basic-ast-dump,basic-lex-dump,common,il-dis,il-verify,ilrun,lsp-common,macos,rtgen,vbasic,vbasic-server,windows_installer,zanna,zia,zia-server}`
- VM: `src/vm` (+ `ops/{common,generated}` and `debug/`)

For architecture and layering, see [architecture.md](architecture.md).

## Subsystem Deep-Dives

### IL

| Component                                     | Description                                   |
|-----------------------------------------------|-----------------------------------------------|
| [IL Analysis](codemap/il-analysis.md)         | BasicAA, CallGraph, CFG, dominators           |
| [IL API](codemap/il-api.md)                   | Public API facades                            |
| [IL Build](codemap/il-build.md)               | IR builder for programmatic construction      |
| [IL Core](codemap/il-core.md)                 | IR types, opcodes, modules, functions, values |
| [IL I/O](codemap/il-i-o.md)                   | Text parser/serializer for `.il` files        |
| [IL Link](codemap/il-link.md)                 | Cross-language module linker, boolean thunks  |
| [IL Runtime](codemap/il-runtime.md)           | Runtime signatures, helper effects            |
| [IL Transform](codemap/il-transform.md)       | Optimization passes and pass infra            |
| [IL Utilities](codemap/il-utilities.md)       | Shared IL utilities                           |
| [IL Verification](codemap/il-verification.md) | Verifier (types, control flow, EH)            |

### Frontends

| Component                                      | Description                                |
|------------------------------------------------|--------------------------------------------|
| [BASIC Frontend](codemap/front-end-basic.md)   | Lexer, parser, semantic analysis, lowering |
| [Frontend Common](codemap/front-end-common.md) | Shared utilities across frontends          |
| [Zia Frontend](codemap/front-end-zia.md)       | Zia compiler with classes, generics        |

### Execution & Codegen

| Component                                | Description                               |
|------------------------------------------|-------------------------------------------|
| [Bytecode VM](codemap/bytecode-vm.md)    | Bytecode compiler, module format, interpreter risk map |
| [Codegen](codemap/codegen.md)            | x86_64 and AArch64 native code generation |
| [Virtual Machine](codemap/vm-runtime.md) | IL VM, opcode handlers, debug, bridge     |

### Runtime & Support

| Component                                           | Description                                                  |
|-----------------------------------------------------|--------------------------------------------------------------|
| [Runtime Library (C)](codemap/runtime-library-c.md) | C runtime: strings, arrays, I/O, OOP                         |
| [Support](codemap/support.md)                       | Diagnostics, arena, source manager, symbols, parsing, passes |

### Tools & Libraries

| Component                       | Description                                              |
|---------------------------------|----------------------------------------------------------|
| [Docs](codemap/docs.md)        | Documentation subsystem                                  |
| [Graphics](codemap/graphics.md) | ZannaGFX 2D graphics library                             |
| [Graphics Stubs](codemap/runtime-graphics-stubs.md) | Graphics-disabled runtime surface policy       |
| [Tools](codemap/tools.md)       | CLI tools (zanna, vbasic, zia, ilrun, il-verify, il-dis) |
| [Zia Server](codemap/zia-server.md) | Language server: MCP + LSP protocol handlers         |
| [TUI](codemap/tui.md)           | Terminal UI library + tests                              |
