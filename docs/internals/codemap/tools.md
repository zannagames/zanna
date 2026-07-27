---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Tools

Command-line tools (`src/tools/`) for the Zanna toolchain.

## Overview

- **Total source files**: 125 (.hpp/.cpp; excludes `zia-server/`, mapped separately in [Zia Server](zia-server.md))

## User-Facing Tools

### vbasic (`vbasic/`)

| File             | Purpose                                  |
|------------------|------------------------------------------|
| `cli_compat.cpp` | Compatibility shim for zanna integration |
| `main.cpp`       | BASIC VM/compiler entry point            |
| `usage.cpp`      | Help text implementation                 |
| `usage.hpp`      | Help text and usage information          |

### zia (`zia/`)

| File             | Purpose                                  |
|------------------|------------------------------------------|
| `cli_compat.cpp` | Compatibility shim for zanna integration |
| `main.cpp`       | Zia VM/compiler entry point              |
| `usage.cpp`      | Help text implementation                 |
| `usage.hpp`      | Help text and usage information          |

### ilrun (`ilrun/`)

| File             | Purpose                                  |
|------------------|------------------------------------------|
| `cli_compat.cpp` | Compatibility shim for zanna integration |
| `main.cpp`       | IL program runner entry point            |

### il-verify (`il-verify/`)

| File            | Purpose                     |
|-----------------|-----------------------------|
| `il-verify.cpp` | Standalone IL verifier tool |

### il-dis (`il-dis/`)

| File       | Purpose              |
|------------|----------------------|
| `main.cpp` | IL disassembler demo |

### zia-server (`zia-server/`)

| File | Purpose |
|------|---------|
| `Json.hpp` / `Json.cpp` | Zero-dependency JSON value type, parser, emitter |
| `Transport.hpp` / `Transport.cpp` | MCP (newline) and LSP (Content-Length) transports |
| `JsonRpc.hpp` / `JsonRpc.cpp` | JSON-RPC 2.0 request/response types and builders |
| `CompilerBridge.hpp` / `CompilerBridge.cpp` | Protocol-agnostic facade wrapping fe_zia APIs |
| `McpHandler.hpp` / `McpHandler.cpp` | MCP lifecycle + 11 tool definitions + dispatch |
| `LspHandler.hpp` / `LspHandler.cpp` | LSP capabilities + request/notification handlers |
| `DocumentStore.hpp` / `DocumentStore.cpp` | In-memory document store for LSP open files |
| `main.cpp` | Entry point with --mcp/--lsp/auto-detect |

For full details, see [codemap/zia-server.md](zia-server.md).

### lsp-common (`lsp-common/`)

Shared infrastructure for both Zia and BASIC language servers.

| File | Purpose |
|------|---------|
| `Json.hpp` / `Json.cpp` | Zero-dependency JSON value type, parser, emitter |
| `JsonRpc.hpp` / `JsonRpc.cpp` | JSON-RPC 2.0 request/response types |
| `Transport.hpp` / `Transport.cpp` | MCP and LSP transports |
| `LspHandler.hpp` / `LspHandler.cpp` | Shared LSP protocol handling |
| `McpHandler.hpp` / `McpHandler.cpp` | Shared MCP protocol handling |
| `ICompilerBridge.hpp` / `ICompilerBridge.cpp` | Abstract compiler bridge interface |
| `DocumentStore.hpp` / `DocumentStore.cpp` | In-memory open file tracking |
| `DiagnosticUtils.hpp` / `DiagnosticUtils.cpp` | Diagnostic formatting for LSP |
| `TextUtils.hpp` / `TextUtils.cpp` | Text manipulation helpers |
| `ServerTypes.hpp` | Shared types (CompletionItem, Diagnostic, etc.) |

### vbasic-server (`vbasic-server/`)

BASIC language server (reuses lsp-common infrastructure).

| File | Purpose |
|------|---------|
| `BasicCompilerBridge.hpp` / `BasicCompilerBridge.cpp` | BASIC-specific compiler bridge |
| `main.cpp` | Entry point |

## Advanced Tools

### zanna (`zanna/`)

| File                    | Purpose                             |
|-------------------------|-------------------------------------|
| `break_spec.cpp`        | Breakpoint specification impl       |
| `break_spec.hpp`        | Breakpoint specification parsing    |
| `cli.cpp`               | Shared CLI option parsing impl      |
| `cli.hpp`               | Shared CLI option parsing           |
| `cmd_bench.cpp`         | Benchmarking subcommand             |
| `cmd_codegen_arm64.cpp` | ARM64 codegen implementation        |
| `cmd_codegen_arm64.hpp` | ARM64 codegen subcommand            |
| `cmd_codegen_x64.cpp`   | x86-64 codegen implementation       |
| `cmd_codegen_x64.hpp`   | x86-64 codegen subcommand           |
| `cmd_front_basic.cpp`   | BASIC frontend subcommand           |
| `cmd_front_zia.cpp`     | Zia frontend subcommand             |
| `cmd_il_opt.cpp`        | IL optimization subcommand          |
| `cmd_eval.cpp`          | One-shot snippet evaluation subcommand |
| `cmd_explain.cpp`       | Diagnostic-code catalog subcommand  |
| `cmd_init.cpp`          | Init subcommand implementation      |
| `cmd_install_package.cpp` | Toolchain installer packaging CLI |
| `cmd_package.cpp`       | Package subcommand (ZAPS)           |
| `cmd_repl.cpp`          | Interactive REPL subcommand         |
| `cmd_run.cpp`           | Run/build/check subcommand implementation |
| `cmd_run_il.cpp`        | IL execution subcommand             |
| `main.cpp`              | Unified compiler driver entry point |

### rtgen (`rtgen/`)

| File        | Purpose                                                |
|-------------|--------------------------------------------------------|
| `rtgen.cpp` | Runtime signature generator from runtime.def + headers |

## Debugging Tools

### basic-ast-dump (`basic-ast-dump/`)

| File       | Purpose              |
|------------|----------------------|
| `main.cpp` | BASIC AST visualizer |

### basic-lex-dump (`basic-lex-dump/`)

| File       | Purpose                  |
|------------|--------------------------|
| `main.cpp` | BASIC lexer token dumper |

## Shared Utilities

### common (`common/`)

| File                  | Purpose                                   |
|-----------------------|-------------------------------------------|
| `ArgvView.hpp`        | Argument vector view utility              |
| `CommonUsage.hpp`     | Shared usage/help text utilities          |
| `frontend_tool.hpp`   | Shared frontend tool utilities            |
| `module_loader.cpp`   | Shared IL module loading implementation   |
| `module_loader.hpp`   | Shared IL module loading with diagnostics |
| `native_compiler.cpp` | Native compilation driver implementation  |
| `native_compiler.hpp` | Native compilation driver                 |
| `project_loader.cpp`  | Multi-file project loading implementation |
| `project_loader.hpp`  | Multi-file project loading utilities      |
| `source_loader.cpp`   | Source file loading implementation        |
| `source_loader.hpp`   | Source file loading utilities             |
| `vm_executor.cpp`     | VM execution wrapper implementation       |
| `vm_executor.hpp`     | VM execution wrapper utilities            |

### basic (`basic/`)

| File         | Purpose                          |
|--------------|----------------------------------|
| `common.cpp` | Shared BASIC tool implementation |
| `common.hpp` | Shared BASIC tool utilities      |

### Packaging — builders (`common/packaging/`)

| File                          | Purpose                                                  |
|-------------------------------|----------------------------------------------------------|
| `LinuxPackageBuilder.cpp`     | Linux `.deb`/`.rpm`/`.tar.gz` package builder impl       |
| `LinuxPackageBuilder.hpp`     | Linux package builder                                    |
| `MacOSPackageBuilder.cpp`     | macOS `.pkg` package builder impl                        |
| `MacOSPackageBuilder.hpp`     | macOS package builder                                    |
| `WindowsPackageBuilder.cpp`   | Windows installer/package builder impl                   |
| `WindowsPackageBuilder.hpp`   | Windows installer/package builder                        |
| `PackageConfig.hpp`           | Package configuration types and options                  |
| `ToolchainInstallManifest.cpp`| Toolchain install manifest model impl                    |
| `ToolchainInstallManifest.hpp`| Install manifest (files, symlinks, associations)         |

### Packaging — archive/format writers (`common/packaging/`)

| File             | Purpose                                              |
|------------------|------------------------------------------------------|
| `ArWriter.cpp`   | Unix `ar` archive writer impl (`.deb` control/data)  |
| `ArWriter.hpp`   | Unix `ar` archive writer                             |
| `CpioWriter.cpp` | cpio archive writer impl (RPM payloads)              |
| `CpioWriter.hpp` | cpio archive writer                                  |
| `TarWriter.cpp`  | tar archive writer impl                              |
| `TarWriter.hpp`  | tar archive writer                                   |
| `XarWriter.cpp`  | xar archive writer impl (macOS `.pkg`)               |
| `XarWriter.hpp`  | xar archive writer                                   |
| `ZipReader.cpp`  | ZIP archive reader impl                              |
| `ZipReader.hpp`  | ZIP archive reader                                   |
| `ZipWriter.cpp`  | ZIP archive writer impl                              |
| `ZipWriter.hpp`  | ZIP archive writer                                   |
| `LnkWriter.cpp`  | Windows `.lnk` shortcut writer impl                  |
| `LnkWriter.hpp`  | Windows `.lnk` shortcut writer                       |
| `PEBuilder.cpp`  | PE executable builder impl (installer stub `.exe`)   |
| `PEBuilder.hpp`  | PE executable builder                                |

### Packaging — compression & hashing (`common/packaging/`)

| File             | Purpose                                  |
|------------------|------------------------------------------|
| `PkgDeflate.cpp` | DEFLATE compression impl                 |
| `PkgDeflate.hpp` | DEFLATE compression                      |
| `PkgGzip.cpp`    | gzip container impl                       |
| `PkgGzip.hpp`    | gzip container                           |
| `PkgZlib.cpp`    | zlib stream impl                          |
| `PkgZlib.hpp`    | zlib stream                              |
| `PkgHash.cpp`    | Packaging hash helpers impl              |
| `PkgHash.hpp`    | Packaging hash helpers                   |
| `PkgMD5.cpp`     | MD5 digest impl                          |
| `PkgMD5.hpp`     | MD5 digest                              |
| `PkgPNG.cpp`     | PNG encoder impl (generated icons)       |
| `PkgPNG.hpp`     | PNG encoder                             |
| `PkgUtils.hpp`   | Shared packaging utilities               |
| `PkgVerify.cpp`  | Package integrity verification impl      |
| `PkgVerify.hpp`  | Package integrity verification           |

### Packaging — installer & metadata generators (`common/packaging/`)

| File                        | Purpose                                          |
|-----------------------------|--------------------------------------------------|
| `InstallerStub.cpp`         | Self-extracting installer stub impl              |
| `InstallerStub.hpp`         | Self-extracting installer stub                   |
| `InstallerStubGen.cpp`      | Installer stub generator impl                    |
| `InstallerStubGen.hpp`      | Installer stub generator                         |
| `DesktopEntryGenerator.cpp` | Linux `.desktop` entry generator impl            |
| `DesktopEntryGenerator.hpp` | Linux `.desktop` entry generator                 |
| `IconGenerator.cpp`         | Application icon generator impl                  |
| `IconGenerator.hpp`         | Application icon generator                       |
| `PlistGenerator.cpp`        | macOS `Info.plist` generator impl                |
| `PlistGenerator.hpp`        | macOS `Info.plist` generator                     |

### Asset compiler (`common/asset/`)

| File                | Purpose                                            |
|---------------------|----------------------------------------------------|
| `AssetCompiler.cpp` | Project asset → ZPAK compilation impl               |
| `AssetCompiler.hpp` | Project asset compiler (`embed`/`pack` directives) |
| `ZpakWriter.cpp`     | ZPAK (Zanna Pack Archive) writer impl               |
| `ZpakWriter.hpp`     | ZPAK pack archive writer                            |
