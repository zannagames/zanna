---
status: active
audience: public
last-verified: 2026-08-31
---

# CLI Tools Reference

Reference documentation for the Zanna command-line tools.

## User-Facing Tools

### zia

Run or compile Zia programs.

```bash
# Run a Zia program
zia program.zia

# Emit IL
zia program.zia --emit-il

# Save IL to file
zia program.zia -o program.il
```

### zbasic

Run or compile single-file Zanna BASIC programs. Its command shape mirrors
`zia`; running it without a source file prints command help. Use
`zanna repl basic` for an interactive BASIC session.

```bash
# Run a BASIC program
zbasic program.bas

# Emit IL
zbasic program.bas --emit-il

# Save IL to file
zbasic program.bas -o program.il

# Compile a native binary
zbasic program.bas -o program
```

### zia-server

Language server for Zia — serves MCP (for AI assistants) and LSP (for editors).
The LSP side includes diagnostics, completion, hover, document symbols, definition,
references, rename, signature help, workspace symbols, and full semantic tokens.

```bash
# Start in MCP mode (for Claude Code, Copilot, etc.)
zia-server --mcp

# Start in LSP mode (for VS Code)
zia-server --lsp

# Auto-detect protocol from first message
zia-server
```

See [Language Server Reference](zia-server.md) for configuration and tool documentation.

### zbasic-server

Language server for Zanna BASIC, with the same MCP tool schemas as `zia-server`
under the `basic/` prefix. Its LSP mode currently provides diagnostics,
completion, hover, and document symbols.

```bash
zbasic-server --mcp
zbasic-server --lsp
```

See [Language Server Reference](zia-server.md) for the shared protocol and the
per-language capability matrix.

### ilrun

Execute IL programs. The IL file must come first; options follow it.

```bash
# Run an IL program
ilrun program.il

# With tracing
ilrun program.il --trace

# With breakpoints
ilrun program.il --break main:10
```

### il-verify

Verify IL correctness.

```bash
il-verify program.il
```

### il-dis

Disassemble IL modules.

```bash
il-dis program.il
```

---

## Advanced Tool: zanna

The unified compiler driver provides advanced functionality.

`zanna --help` is intentionally concise and limited to common command shapes. Use `zanna <subcommand> --help` or `zanna help <subcommand>` for operational flags, and this reference for release-only packaging, signing, and manifest details that are too noisy for the default help screen.

### Overview

The CLI is organized around primary entry points:

- `zanna init <name> [--lang zia|basic]` — Scaffold a new project
- `zanna run <file|dir>` — Build and run a source file or project
- `zanna build <file|dir> [-o out]` — Emit IL or build a native binary
- `zanna build-many --output-dir DIR name=project [...]` — Build several projects in one process
- `zanna check <file|dir>` — Type-check and verify without running or emitting
- `zanna eval [code]` — Evaluate a one-line snippet and print the result
- `zanna explain <code>` — Describe a diagnostic code from the central catalog
- `zanna -run <file.il>` — Execute an IL module
- `zanna front zia -emit-il <file.zia>` — Legacy low-level Zia frontend entry point
- `zanna front zia -run <file.zia>` — Legacy low-level Zia frontend execution path
- `zanna front basic -emit-il <file.bas>` — Legacy low-level BASIC frontend entry point
- `zanna front basic -run <file.bas>` — Legacy low-level BASIC frontend execution path
- `zanna il-opt <in.il> -o <out.il>` — Run optimization passes
- `zanna codegen x64 <in.il> -o <out>` — Compile to x86-64 native code
- `zanna codegen arm64 <in.il> -S <out.s>` — Generate ARM64 assembly
- `zanna package <dir>` — Package a project for distribution (.app, .dmg, .deb, .rpm, .run, .exe, .tar.gz)
- `zanna install-package` — Package the staged Zanna toolchain itself (.exe, .pkg, .deb, .rpm, .tar.gz)
- `zanna asset bake|validate <model> [out.scene3d]` — Offline 3D asset conditioning
- `zanna bench <file.il>` — Benchmark VM dispatch strategies (see the [Debugging Guide](debugging.md))
- `zanna repl` — Launch the interactive REPL

### zanna init

Scaffold a new Zanna project.

```bash
zanna init <project-name> [--lang zia|basic]
```

| Option              | Description                                         |
|---------------------|-----------------------------------------------------|
| `--lang zia\|basic` | Source language for the entry file (default: `zia`) |
| `--`                | Treat the next token as the project name            |
| `-h`, `--help`      | Show help for `zanna init`                          |

Creates a project directory containing:
- `zanna.project` — Project manifest (name, version, language, entry point, `profile balanced`, `optimize O1`)
- `main.zia` or `main.bas` — Entry-point source file with a hello-world template

```bash
zanna init my-app
zanna run my-app
```

### zanna run / build

Build and run a source file or project, or build an IL/native artifact.

```bash
zanna run program.zia
zanna run program.zia --no-strict-diagnostics
zanna run program.zia --no-bounds-checks
zanna build program.zia -o program.il
zanna build program.zia -o program
```

| Flag | Description |
|------|-------------|
| `--strict-diagnostics` | For Zia, promote safety-critical warnings to errors before execution (default) |
| `--no-strict-diagnostics` | Keep safety-critical Zia diagnostics as warnings |
| `--quiet-warnings`, `--no-warnings` | Do not print warnings when compilation succeeds |
| `--diagnostic-format text\|json` | Select text or machine-readable JSON diagnostics |
| `--no-runtime-namespaces` | Disable automatic runtime namespace imports |
| `--bounds-checks` | Enable generated bounds checks where supported (default for source lowering) |
| `--no-bounds-checks` | Disable generated bounds checks for source lowering |
| `--verify-each` | Verify IL after every optimizer pass for debugging pass failures |
| `--paranoid-verify` | Run every frontend verifier checkpoint, including intermediate optimized-build checks |
| `--time-compile` | Print project resolution, source read, frontend phase, final verifier, asset, backend pass, and native-codegen/link timings |
| `--pass-stats` | Print detailed IL optimizer pass statistics; kept separate from `--time-compile` because it scans the module around each pass |
| `--fast-link` | Skip non-essential native-link size reductions and coalesce generated arm64 function sections; enabled automatically for debug/O0 native builds (`zanna build` only) |
| `--build-profile debug\|balanced\|release` | Override the manifest build profile (`debug`=`O0`, `balanced`=`O1`, `release`=`O2`) |
| `-O0`, `-O1`, `-O2` | Override the final optimization level; this takes precedence over the build profile |
| `--profile` | Print execution profile data after the program exits (`zanna run` only) |
| `--debug-vm` | Run on the standard VM rather than the default execution path (`zanna run` only) |
| `--debug-adapter` | Serve the JSON debug adapter protocol on stdio (`zanna run` only) |
| `--dump-trap` | Print detailed trap diagnostics (`zanna run` only) |
| `--stack-size <bytes>` | Set the native executable stack size, decimal or `0x` hex (`zanna build` only) |
| `--strip-symbols` | Omit function/data names from the executable's symbol table; by default every definition is named so `sample`, Instruments, and `perf` can attribute time (`zanna build` only) |
| `--windows-debug-runtime`, `--windows-release-runtime` | Select the Windows CRT to link (`zanna build` only) |
| `--arch arm64\|x64` | Override the native target architecture (`zanna build` only) |

Pipeline dump flags (`--dump-tokens`, `--dump-ast`, `--dump-sema-ast`, `--dump-il`,
`--dump-il-opt`, `--dump-il-passes`) and execution tracing (`--trace[=il\|src]`,
`--max-steps`, `--stdin-from`) are accepted by both commands and documented in the
[Debugging Guide](debugging.md).

Both Zia and BASIC source paths print successful warnings by default. Zia O0/debug builds verify after lowering; optimized Zia builds normally verify the final optimized IL and skip the intermediate lower-stage verifier to keep large builds fast. `--paranoid-verify` restores every frontend verifier checkpoint, and `--verify-each` verifies after every optimizer pass when debugging an optimizer regression. If any verifier run fails, the command stops with diagnostics instead of running or building the result.

`zanna build` defaults to the `balanced` profile and `O1` optimization when no explicit directive is present. `zanna run` defaults to `debug`/`O0` for convention projects and manifests that do not set `profile`, `build-profile`, or `optimize`, keeping edit-run cycles fast. An explicit manifest profile, explicit manifest optimization level, `--build-profile`, or `-O*` flag is respected by both commands.

Native builds hand the already-verified, already-optimized frontend IL module directly to the backend and pass the selected optimization level to MIR/codegen passes such as pre-regalloc cleanup, block layout, scheduling, and peephole optimization. Safe per-function IL optimizer passes, x86-64 peephole work, and arm64 binary function emission may run in parallel unless a diagnostic dump or `--verify-each` requires strict serial instrumentation.

### zanna build-many

Build several projects in one compiler process. Targets are named with
`output-name=project-path`; output names must be single path components. The
command continues after a failed target, returns failure if any target failed,
and preserves argument order in its diagnostics. Keeping the process alive
allows parsed runtime archives and other immutable process caches to be reused.

```bash
zanna build-many --output-dir examples/bin --arch x64 -O1 --fast-link \
  paint=examples/apps/paint chess=examples/games/chess
```

`--arch x64|arm64`, `-O0|-O1|-O2`, `--fast-link`, and `--time-compile`
apply to every target in the batch.

### zanna check

Type-check and verify a source file or project without executing or emitting
anything. This is the fast verification gate for editors, scripts, and AI
coding agents: it runs the frontend and then stops before emitting or executing.

> **`check` is not currently as strict as `run`/`build`.** It compiles with
> optimization enabled (the manifest/default build profile, not `O0`), and it
> re-runs the IL verifier only when the frontend did not already mark the module
> verified. A module can therefore pass `zanna check` with exit `0` and still
> fail IL verification under `zanna run` or `zanna build`. When you need the
> check to match what a build will accept, pass `-O0`, `--build-profile debug`,
> or `--paranoid-verify` — each of those restores the lowering-stage verifier
> and reports exit `2`.

```bash
zanna check program.zia
zanna check my-project/ --diagnostic-format=json
```

| Flag | Description |
|------|-------------|
| `--diagnostic-format text\|json` | Select text or machine-readable JSON diagnostics (stderr) |
| `--strict-diagnostics` / `--no-strict-diagnostics` | Control safety-warning promotion (strict by default) |
| `--quiet-warnings`, `--no-warnings` | Suppress warning output |
| `--build-profile`, `-O0/-O1/-O2`, `--bounds-checks`, `--no-bounds-checks` | Same meaning as `zanna run` |

Exit codes are differentiated so callers can branch without parsing output:

| Code | Meaning |
|------|---------|
| `0` | No errors (warnings allowed) |
| `1` | Usage error, or the target could not be resolved |
| `2` | Compile or verification errors (diagnostics printed) |

JSON diagnostics include the stable `code`, pipeline `stage`, the underlined
`range`, related `notes`, and machine-applicable `fixits` (for example,
did-you-mean replacements for undefined identifiers).

### zanna eval

Evaluate a single Zia or BASIC snippet through a fresh REPL session and print
the result. Reads the snippet from stdin when no code argument is given.

```bash
zanna eval '2 + 3 * 4'                       # prints 14
zanna eval --json --type 'Zanna.Math.Sqrt(2.0)'
echo 'Say("hi")' | zanna eval
zanna eval --lang basic 'PRINT 2+2'
```

| Flag | Description |
|------|-------------|
| `--lang zia\|basic` | Snippet language (default `zia`) |
| `--json` | Emit one JSON object on stdout: `{success, trapped, resultType, output, error, type?, il?}` |
| `--type` | Include the inferred expression type (Zia only) |
| `--il` | Include the generated IL (Zia only) |

Exit codes: `0` success, `1` usage error, `2` compile/eval error, `3` runtime
trap. The snippet is evaluated as a single REPL input with expression
auto-printing, so `zanna eval` behaves exactly like typing the snippet into
`zanna repl`.

### zanna explain

Describe a diagnostic code from the central catalog.

```bash
zanna explain V-ZIA-UNDEFINED
zanna explain B2001 --json
zanna explain --list --json      # full catalog as a JSON array
zanna --print-error-codes --json # same catalog via a driver flag
```

Cataloged codes print their subsystem and a one-sentence summary. Codes that
are not yet cataloged but match a known prefix family (e.g., `B21xx`,
`V-BC-*`) still resolve to their subsystem description; completely unknown
codes exit `1`.

### Machine-readable registry dumps

Two driver flags emit JSON inventories generated from the live in-process
registries, so they can never drift from the binary:

```bash
zanna --dump-runtime-api   # schema v4 runtime contract and ABI catalog
zanna --dump-opcodes       # {ilVersion, opcodes:[{mnemonic,resultArity,resultType,operandsMin,operandsMax,operandTypes,sideEffects,successors,terminator}]}
```

These complement the human-oriented `--dump-runtime-descriptors` and
`--dump-runtime-classes` text dumps.

`--dump-runtime-api` preserves the original compact fields (`version`,
`functions[].name`, `functions[].signature`, `classes[].name`,
`classes[].constructor`, `properties`, and `methods`) and adds
`schema_version: 4`, `signature_dialect: "runtime-def-v1"`, parsed
`return_type`/`params`, `kind`, `owner`, `class_kind`, `is_static`,
`stability`, `capabilities`, `fallibility`, `ownership`, and `docs_anchor`
metadata. Class rows also carry authored Markdown documentation with a short
`summary` and long `details` field. The new metadata is additive so older tools
can continue reading the fields they already understand.

Schema v4 declares `public_boundary: "registry"` and
`c_abi_status: "internal-embedding"`. Every registry function includes a
`c_symbol` field; generated public rows contain the backing symbol and legacy
hand-authored bridge-only rows may report `null`. Non-empty class constructors,
property accessors, and methods include resolved constructor/getter/setter/method
C symbols (an absent constructor or accessor is reported as `null`). Graphics3D and Game3D
rows additionally carry explicit return nullability, ownership, and
fallibility contracts with `contract_source: "three-d-boundary-policy"`.
`fallibility` and `ownership` are otherwise derived from name/signature
heuristics with explicit overrides for entries the heuristics cannot infer: a
concretely typed `seq<…>` / `obj<…>` return is reported `ownership: "owned"`
(a freshly allocated value the caller owns, not a borrowed handle), and IO
conversion/open/allocation entries that trap on failure — such as
`Stream.AsBinFile` / `AsMemStream` / `ToBytes`, `LineWriter.Append`,
`Watcher.New` / `Start`, and `Archive.Create` — report `fallibility: "traps"`
rather than the heuristic default of `infallible`. The same explicit overrides
cover the trapping `Zanna.Math.*` operations (`BigInt.Div` / `Mod` / `Pow` /
`PowMod` / `Sqrt` / `ToStringBase`, `Mat3.Inverse` / `Mat4.Inverse`,
`Quat.Inverse` / `Slerp`, and `Vec2.Div` / `Vec3.Div`), and every `Zanna.Math.*`
operation that returns an object reports `ownership: "owned"` (math values are
immutable, freshly allocated results). The `Zanna.System.*` surface is covered
the same way: process spawns (`Process.Start` / `StartWithEnv`) mark their
handle return `nullable: true` because a failed spawn yields `NULL` rather than
a live object, and `PtySession.Resize` reports `fallibility: "status"` because it
returns a boolean success indicator instead of an infallible void. The unmanaged
`Zanna.Runtime.Unsafe.Release` / `ReleaseStr` primitives likewise report
`fallibility: "traps"` (they trap on an invalid or already-freed handle). The `Zanna.Time.*` surface is
likewise annotated: the sentinel parsers (`DateTime.ParseIso8601` / `ParseDate` /
`ParseTime`, `DateOnly.Parse`) report `fallibility: "sentinel"` rather than the
heuristic's default `traps`, every `Zanna.Time.*` object return is `owned` (a
freshly allocated value; the sole borrowed exception, `TimeZone.Find`'s static
handle, is annotated separately), and the object operations that yield `NULL` on
ordinary failure — `DateOnly.FromParts` / `Today` / `Parse` / `FromDays` /
`AddDays` / `AddMonths` / `AddYears` and `DateRange.Intersection` / `Union` —
report `nullable: true` so tools emit the null branch. Overflow-on-extreme-input
and null/wrong-class-receiver traps are deliberately left `infallible`, matching
the runtime-wide convention that `fallibility` describes normal-operation failure
modes rather than arithmetic edges or programming errors.
`--dump-runtime-api` is the **full registry inventory**, which is a superset of
the public language surface: it includes hand-authored descriptor rows in
`src/il/runtime/RuntimeSignatures.cpp` that exist only as front-end lowering
targets. For example `Zanna.Math.Randomize` / `Zanna.Math.Rnd` back BASIC's
`RANDOMIZE` / `RND()`, and `Zanna.String.RetainMaybe` / `ReleaseMaybe` are
refcount helpers — none are callable from Zia (`Runtime class 'Zanna.Math' has
no method 'Rnd'`). The curated public surface is
[`docs/generated/runtime/`](../generated/runtime/README.md), generated by `rtgen`
from `src/il/runtime/runtime.def`; use that when you want "what can user code
call", and `--dump-runtime-api` when you want "what is registered".

Together, the canonical name, compact signature, C symbol, and complete class
member bindings form the live ABI manifest used by contract tests. The C
symbols are available to Zanna's embedding and VM layers, but they are not a
separately versioned public SDK ABI and expose no stable object layouts.

### zanna -run

Execute IL modules with debugging controls.

```bash
zanna -run <file.il> [flags]
```

| Flag                         | Description                                  |
|------------------------------|----------------------------------------------|
| `--trace=il`                 | Emit line-per-instruction trace              |
| `--trace=src`                | Show source file, line, column for each step |
| `--stdin-from <file>`        | Feed program stdin from file                 |
| `--max-steps <N>`            | Limit execution to N VM steps                |
| `--bounds-checks`            | Rejected here — bounds checks are generated at compile time, so recompile the source with `zanna build --bounds-checks` instead |
| `--break <Label\|file:line>` | Halt before executing instruction            |
| `--break-src <file:line>`    | Explicit source-line breakpoint              |
| `--debug-cmds <file>`        | Read debugger actions from file              |
| `--step`                     | Enter debug mode, break at entry             |
| `--continue`                 | Ignore breakpoints and run to completion     |
| `--watch <name>`             | Print when scalar changes                    |
| `--count`                    | Print executed instruction count at exit     |
| `--time`                     | Print wall-clock execution time              |
| `--bytecode`                 | Run through the bytecode VM with checked bytecode compilation |
| `--diagnostic-format text\|json` | Select text or JSON diagnostics for load/compile failures |

Debugger command files accept `continue`, `step`, `step N`, `step-over`, and `step-out`. Step-over and step-out are
frame-depth based and are intended for VM debugging workflows where source-level line stepping is not required.

### zanna front

Low-level frontend entry points retained for direct compiler testing and
compatibility. Prefer `zia`, `zbasic`, or `zanna run` / `zanna build` for normal
workflows.

```bash
# Zia
zanna front zia -emit-il <file.zia> [--bounds-checks|--no-bounds-checks] [--strict-diagnostics|--no-strict-diagnostics] [--diagnostic-format text|json]
zanna front zia -run <file.zia> [--strict-diagnostics|--no-strict-diagnostics] [--trace=il|src] [--stdin-from <file>] [--max-steps N]

# BASIC
zanna front basic -emit-il <file.bas> [--bounds-checks|--no-bounds-checks] [--diagnostic-format text|json]
zanna front basic -run <file.bas> [--trace=il|src] [--stdin-from <file>] [--max-steps N] [--quiet-warnings]
```

### zanna il-opt

Run optimization passes on IL modules.

```bash
zanna il-opt <in.il> -o <out.il> [flags]
```

| Flag                  | Description                                          |
|-----------------------|------------------------------------------------------|
| `--passes a,b,c`      | Run an explicit comma-separated pass list            |
| `--pipeline NAME`     | Run a registered pipeline: `O0`, `O1`, `O2`, or `rehab-*` |
| `--no-mem2reg`        | Drop mem2reg from the selected pipeline when present |
| `--mem2reg-stats`     | Print counts of promoted variables                   |
| `-print-before`, `-print-after` | Print IL before or after each pass         |
| `--verify-each`       | Verify the module between passes                     |
| `--pass-stats`        | Print pass statistics                                |
| `--bisect-pipeline`   | Run and report every pipeline prefix, to isolate a bad pass |

Default pipeline: O1 (`simplify-cfg, mem2reg, simplify-cfg, sccp, constfold, peephole, dce, simplify-cfg, sccp, inline, peephole, dce, simplify-cfg`)

### zanna codegen

Compile IL to native code.

```bash
# x86-64
zanna codegen x64 <in.il> -o <executable>
zanna codegen x64 <in.il> -S <out.s>  # Assembly only
zanna codegen x64 <in.il> -o <executable> --asset-blob assets.zpak --extra-obj assets.o
zanna codegen x64 <in.il> --native-asm -o <out.o>
zanna codegen x64 <in.il> --native-asm --debug-lines -o <out.o>
zanna codegen x64 <in.il> --native-asm --target-linux -o <out.o>
zanna codegen x64 <in.il> --native-asm --target-windows -o <out.obj>

# ARM64 (Apple Silicon validated)
zanna codegen arm64 <in.il> -S <out.s>
zanna codegen arm64 <in.il> -o <executable> --asset-blob assets.zpak --extra-obj assets.o
zanna codegen arm64 <in.il> --native-asm -o <out.o>
zanna codegen arm64 <in.il> --native-asm --debug-lines -o <out.o>
zanna codegen arm64 <in.il> --native-asm --target-linux -o <out.o>
zanna codegen arm64 <in.il> --native-asm --target-windows -o <out.obj>
```

On x86-64, `--asset-blob` embeds the ZPAK payload directly when using `--native-asm`. If you force `--system-asm`, pair it with `--extra-obj <asset.o>` so the asset symbols are still linked into the final binary.

On x86-64, `--target-linux` and `--target-windows` select the assembly dialect, native object
format, and native-link platform together. (`--target-darwin` selects the Darwin assembly dialect and Mach-O object
format only; macOS x86-64 is not a supported native-link target — macOS support is Apple Silicon/arm64.) `--target-win64` still switches the calling convention to Win64 and also
selects the Windows platform policy. When you use `--native-asm` with `-o <file.o>` or `-o <file.obj>`, the compiler
writes a relocatable object instead of linking an executable.

On arm64, target selection is explicit: `--target-darwin`, `--target-linux`, and `--target-windows` select the assembly dialect, native object format, and native-link platform together. When you use `--native-asm` with `-o <file.o>` or `-o <file.obj>`, the compiler writes a relocatable object instead of linking an executable.

File-based `zanna codegen` loads and verifies the input IL once before backend lowering. Project builds through `zanna build` skip the textual IL round trip and transfer the verified in-memory module to the backend. Executable builds using the native assembler and linker also transfer the generated relocatable object in memory, avoiding a temporary `.o` write and read; object-only builds continue to write the requested file. Native assembler debug line emission is disabled by default for faster object generation and smaller native-link executables; pass `--debug-lines` when you need DWARF `.debug_line` content in native objects and linked outputs. `--fast-link` skips string deduplication and identical-code folding in the native linker; on arm64 it also emits one generated text section instead of per-function sections for faster debug links.

Both backend drivers accept `--time-passes` for per-pass codegen timings and
`--skip-il-optimization` when an upstream frontend has already optimized the
IL. The latter is intended for staged build tooling; ordinary direct codegen
should leave backend IL optimization enabled.

### Demo build driver

The platform demo drivers read `scripts/demo_projects.list`, a shared curated
selection of Zia showcase projects. The default selection contains three games,
four 3D demos, and one application; BASIC and smaller feature examples remain
available for individual builds but are not part of the showcase build. On Linux,
`scripts/build_demos_linux.sh` uses the in-process project-to-native path and
defaults to O1, the fast linker, host CPU parallelism, and dependency stamps.
Useful controls are `--release` (O2), `--opt O1|O2`, `--jobs N`, `--timings`,
`--rebuild`, and the diagnostic legacy path `--linker system`. `--run`
intentionally serializes smoke launches because the graphical demos share
display, audio, and output-directory state.

### zanna package

Build a native payload and package a project for distribution.

```bash
zanna package .
zanna package . --target linux
zanna package . --target windows --executable build/myapp.exe
zanna package . --target tarball -o myapp.tar.gz
zanna package . --target linux-bundle -o myapp.run
zanna package . --target rpm --linux-sign-key "Maintainer Key"
zanna package . --dry-run --verbose
```

| Option | Description |
|--------|-------------|
| `--target macos\|linux\|windows\|linux-bundle\|rpm\|dmg\|tarball` | Select output format; default is the host platform. `appimage` is a deprecated alias for `linux-bundle` and warns |
| `--arch x64\|arm64` | Select payload architecture |
| `--executable <path>` | Package a prebuilt native executable; required for non-host installer targets |
| `-o <path>` | Output artifact path |
| `--macos-sign-mode none\|preserve\|adhoc\|developer-id` | Override macOS signing mode |
| `--macos-sign-identity <identity>` | Developer ID Application identity for macOS signing |
| `--macos-entitlements <path>` | Entitlements plist used during macOS signing |
| `--macos-hardened-runtime` | Enable hardened runtime for macOS signing |
| `--macos-notary-profile <profile>` | `notarytool` keychain profile used to notarize a Developer ID signed app |
| `--macos-staple` | Staple the notarization ticket before final ZIP output |
| `--windows-install-scope machine\|user` | Override Windows install scope; `machine` uses Program Files/HKLM, `user` uses LocalAppData\\Programs/HKCU |
| `--windows-install-dir <name>` | Override the Windows install-root directory name |
| `--windows-sign` | Authenticode-sign the generated Windows installer with `signtool` |
| `--windows-sign-pfx <path>` | PFX certificate for Windows signing; the password comes from `ZANNA_WINDOWS_SIGN_PASSWORD` |
| `--windows-sign-thumbprint <sha1>` | Sign with a certificate-store SHA-1 thumbprint; spaces are accepted and normalized |
| `--windows-timestamp-url <url>` | RFC3161 timestamp URL for Windows signing |
| `--windows-signtool <path>` | `signtool.exe` path override |
| `--windows-sign-no-verify` | Skip `signtool verify` after signing |
| `--linux-sign-key <id>` | GPG-sign the generated `.deb`/`.rpm` with `dpkg-sig`/`rpmsign` |
| `--dry-run` | Validate metadata and print resolved package contents without building |
| `--json` | With `--dry-run`, print the resolved package plan as JSON |
| `--keep-failed-artifact` | Preserve generated artifacts after a failed package step for inspection |
| `--verbose` | Print binary, output, asset, and verification details |

`.app`/`.dmg` build on a macOS host (`.dmg` shells to `hdiutil`), `.deb`/`.AppImage`/`.tar.gz` are emitted directly on any host, and `.rpm` requires `rpmbuild`. `--linux-sign-key` applies only to `--target linux`/`rpm` and requires `dpkg-sig`/`rpmsign`; each path fails with a clear diagnostic when its tool is unavailable.

Packaging manifest paths are project-relative. The `--executable` CLI option is a normal command-line path: relative values resolve from the current working directory. Scalar package fields such as `package-name`, `package-author`, `package-homepage`, `package-license`, `package-welcome`, platform minimum versions, `package-category`, `linux-startup-wm-class`, `linux-keywords`, `linux-appstream-id`, `windows-publisher`, and `windows-wizard-summary` must be one manifest token; quote values that contain spaces. `package-icon`, `package-license-file`, `package-readme`, `macos-dmg-background`, `macos-dmg-icon`, `windows-dll`, `asset`, `post-install`, and `pre-uninstall` paths may also be quoted when they contain spaces. Sources are resolved inside the canonical project root and reject absolute paths, `..` traversal, unreadable directory entries, and symlinks that resolve outside the project. Missing icons/assets, non-file icons, and assets that are neither files nor directories are fatal. Archive entry paths are normalized to forward slashes, must remain relative, and must be unique after normalization.

Package names, executable names, Windows shortcut names, macOS bundle identifiers, macOS signing modes, Windows ProgID bases, dotted file-association extensions, MIME types, Debian dependency expressions, RPM dependency expressions, freedesktop desktop categories, Debian Policy-style versions, RPM versions, portable archive versions, platform dotted versions, URLs with non-empty hosts, and single-line metadata fields are validated before writing artifacts. Desktop categories are normalized to semicolon-terminated freedesktop category lists, defaulting to `Utility;` when omitted. Duplicate scalar package directives, duplicate Windows DLL paths, and duplicate file-association extensions are rejected. Invalid metadata fails the package command instead of producing malformed `.desktop`, plist, control, spec, shortcut, tar, ZIP, or installer data. Portable tarballs do not validate Debian-only fields such as `.deb` dependencies or desktop categories because they are not emitted into the tarball.

Prebuilt and compiled payload executables are inspected before packaging. macOS targets require Mach-O, Linux targets require ELF, Windows targets require PE32+, and the detected payload architecture must match `--arch` unless a macOS universal binary contains the requested supported slice; both fat32 and fat64 Mach-O universal headers are accepted. Portable tarballs accept Mach-O, ELF, or PE payloads, but still reject unknown executable formats and architecture mismatches. When a project omits `version`, package metadata and the default output filename both use `0.0.0`. Default output filenames sanitize project, version, and architecture components so manifest metadata cannot create paths outside the working directory.

Linux `.deb` packages embed validated `post-install` and `pre-uninstall` script file contents as maintainer scripts. Linux desktop shortcuts install a normal application entry and, when `shortcut-desktop on` is set, copy it to existing user Desktop directories during `postinst` and remove it during `prerm`. File associations always add MIME XML and a desktop handler with `%f`; if menu and desktop shortcuts are disabled, that handler is installed with `NoDisplay=true` so file opens still work without adding a menu item. MIME and desktop caches are refreshed after package removal in `postrm`. RPM app packages emit matching `Requires:` entries from conservative runtime defaults plus `package-rpm-depends`, and refresh desktop/MIME caches when those payloads are installed. `linux-appstream-id` emits AppStream metainfo, and apps without `package-icon` get a generated fallback icon so `.desktop` launchers do not point at missing theme icons.

Windows application installers default to `windows-install-scope user`, which installs under `%LocalAppData%\Programs`, writes uninstall metadata and safe file associations under HKCU, and creates current-user shortcuts. `windows-install-scope machine` installs under `%ProgramFiles%`, writes metadata under HKLM, registers ProgIDs under `HKLM\Software\Classes`, and requests elevation only after all-users scope is selected. `windows-install-dir <name>` overrides the directory below that scope root; otherwise the package display name is used. `windows-publisher` overrides the Apps & Features publisher; otherwise `package-author` and then `package-name` are used. `windows-wizard-summary` customizes the setup summary, and `windows-dll <path>` adds explicit runtime dependencies to recursive PE import discovery.

Application packages use the same statically linked native setup/maintenance host and detached cleanup helper as the Zanna toolchain package; setup does not invoke PowerShell or download prerequisites. The schema-3 overlay hashes the compressed payload, generated maintenance host, cleanup helper, integrations, and every installed file. Install, upgrade, modify, repair, and uninstall are transactional; arbitrary unowned files survive, exact owned hashes are repairable, and successful uninstall removes the running uninstaller and package-cache leaf without a reboot. `/quiet`, `/passive`, `/log`, `/norestart`, `/inspect`, `/checkForUpdates`, and the documented lifecycle exit codes use the same automation contract as the toolchain installer.

File associations advertise ProgIDs through `OpenWithProgids` without overwriting an existing default handler. A fourth `file-assoc` token supplies Windows-only open-command arguments, producing `"<installed exe>" <windows-open-args> "%1"`. Existing extension `Content Type` values are preserved; owned MIME values, ProgIDs, Open With entries, shortcuts, Apps & Features values, and files are removed only when their ownership markers still match. Desktop and Start Menu shortcuts are generated from the resolved destination rather than fixed paths, and a generated fallback `.ico` is installed when `package-icon` is omitted. Apps & Features exposes Modify and Repair in addition to uninstall and records version, publisher, install location, icon, size, date, homepage, architecture, channel, package digest, maintenance cache, and diagnostic log.

Interactive x64 and ARM64 packages use the same per-monitor-DPI native wizard, license flow, progress/cancellation behavior, Restart Manager handling, and unique redacted `%TEMP%\ZannaInstaller-<package-id>-<UTC-time>-<pid>.log`. Application packaging recursively bundles adjacent non-system PE dependencies and rejects a missing imported DLL before output. `--arch arm64` requires an ARM64 PE32+ payload plus architecture-matched native host and cleanup binaries; wrong-machine content is rejected recursively rather than relabeled.

Release signing can be driven directly by `zanna package --target windows --windows-sign`, `zanna install-package --target windows --windows-sign`, by `scripts/sign-windows-installer.ps1`, or by `.github/workflows/windows-release-installer.yml` (ADRs 0025 and 0073). PFX signing uses `ZANNA_WINDOWS_SIGN_PFX` and `ZANNA_WINDOWS_SIGN_PASSWORD` and requires explicit `ZANNA_WINDOWS_SIGN_PASSWORD_ARGV_OK=1` acknowledgement because `signtool` receives the password in argv; certificate-store signing avoids that exposure and uses `--windows-sign-thumbprint`, `windows-sign-thumbprint`, or `ZANNA_WINDOWS_SIGN_THUMBPRINT`. Both paths require an HTTPS RFC 3161 timestamp and post-sign with `signtool verify /pa /all /tw /v`. Signed `.exe` structural verification ignores only the Authenticode certificate table while still validating the complete embedded ZIP overlay.

The XenoScape demo manifest is configured as a user-scope Windows game package: it installs under `%LocalAppData%\Programs\Xenoscape`, creates Start Menu and desktop shortcuts, declares a Windows 10 minimum, and includes both the `sounds/` WAV payload and `xenoscape.runtime.json`. Its runtime first probes the installed working directory for those assets, then the executable directory, then source-tree locations used by local development runs. The Windows installer smoke test installs the package, launches `xenoscape.exe --zanna-package-smoke` from a non-install working directory to verify executable-directory asset lookup, verifies shortcuts and HKCU uninstall metadata, and runs the generated uninstaller.

macOS app packages are staged as a real `.app` bundle before ZIP emission. On macOS the default signing mode is `adhoc`, which runs `codesign --force --sign -` over the bundle so `Info.plist` and bundled resources are sealed in `Contents/_CodeSignature/CodeResources`; on non-macOS hosts the default is `preserve` because local signing tools are unavailable. `adhoc` signing does not require an Apple Developer account and is suitable for local testing or internal handoff where users can explicitly approve an unidentified developer app. For public distribution to quarantined Macs, use `macos-sign-mode developer-id`, `macos-sign-identity "Developer ID Application: ..."` and `macos-notary-profile <profile>`; notarization requires Apple credentials configured in `notarytool` and is accepted only with Developer ID signing. `macos-staple on` requires `macos-sign-mode developer-id` and `macos-notary-profile <profile>`, then staples the ticket before the final ZIP. `preserve` leaves an already-signed payload untouched, and `none` emits an unsigned bundle. App `.dmg` output accepts `macos-dmg-background` and `macos-dmg-icon` manifest paths for Finder window styling and a volume icon.

`asset <source> <target>` targets are relative to the platform's app resource root: `Contents/Resources/<target>` on macOS, `/usr/share/<package>/<target>` for Linux `.deb`, `<target>` inside the Windows install-root payload, and `<top-dir>/<target>` in portable tarballs. For example, `asset assets assets` packages `assets/fonts/font.bdf` as `Contents/Resources/assets/fonts/font.bdf` in a macOS app. Asset directory symlinks are followed when their resolved targets remain inside the project root, packaged paths preserve the symlink path rather than leaking the canonical target path, and packagers read from the validated resolved path. Linux `.deb` and portable tarball outputs preserve executable bits on asset files. App tarballs include `install.sh`, `uninstall.sh`, `README.install`, and `LICENSE`; `package-readme` adds a project README and `package-license-file` supplies full license text. Portable tarball top directories use a filesystem-safe version component, so Debian epochs such as `2:1.0` become `2_1.0` in the directory name while the package version remains unchanged.

Standalone application bundles (`--target linux-bundle`) use Zanna's FUSE-less self-extracting `.run` runtime — not the AppImage specification. The generated artifact accepts `--appimage-help` and `--appimage-extract` (extracts safely to `./zanna-bundle-root`) as compatibility aliases, plus `ZANNA_APPIMAGE_CLEAN_CACHE=1` to force a cache refresh. These application-only controls are separate from `install-package`'s `.run` toolchain format.

Built artifacts are structurally and payload-verified by default: macOS ZIPs must contain the `.app` Info.plist and executable, `.deb` packages must contain the expected `usr/bin` payload, Windows installers verify the PE structure plus required ZIP overlay entries including `meta/manifest.sha256`, and tarballs verify gzip framing, USTAR headers, duplicate-free paths, and the expected executable. ZIP verification normalizes paths before duplicate checks and rejects central-directory/local-header disagreements. Failed verification removes the generated artifact. On macOS, signing failures are fatal before ZIP output, and the staged app bundle is checked with `codesign --verify --deep --strict`.

### zanna asset

Offline 3D asset conditioning. `zanna asset bake <input> <output.scene3d>` (legacy `.vscn` also accepted) loads a
model (glTF/GLB/FBX/OBJ/STL) through the full runtime import pipeline —
including the meshopt, Draco, and Basis Universal decoders and the import
options — optionally generates LOD chains, and saves the complete `SceneAsset`
in the current VSCN representation for near-instant loading. The baked document retains every immutable scene,
camera-to-node association, native light, enumerable resource, typed node metadata, node/skeletal/
camera animation, material variant, and static morph payload. It also retains
the complete validated KTX2, PNG, JPEG, GIF, or BMP container when the importer
had those bytes; otherwise it stores canonical RGBA8 texels. The command reloads
the written file and compares texture content and metadata before reporting
success. Options:

- `--force-tangents`, `--eight-influences`, and `--compress-anims` select importer conditioning.
- `--lods N` generates 0-8 LOD levels at a halving ratio.
- `--clips LIST` keeps selected animation clips by name, zero-based skeletal-clip index, or inclusive `A-B` index range; node animations with matching names follow their skeletal clip.
- `--simplify-meshes N` decimates every mesh above N triangles toward N before saving (`N >= 8`). `--simplify-lock-seams` keeps open, UV-seam, and material-boundary vertices fixed; a seam-constrained mesh can report a valid partial result above N. `--simplify-max-error F` stops when the next collapse would exceed F times the mesh bounding diameter (`0 < F < 1`; `0.001` is a typical starting point). Both refinement options require `--simplify-meshes`.
- `--max-texture-dim N` downsizes material textures above N texels on either axis (`N >= 64`) and stores compact canonical pixels.
- `--strip-meshes` drops all meshes and materials for animation-only bakes; the saved scene keeps nodes, skeletons, and clips.
- `--json` emits the machine-readable report described below.

In the default output mode, success keeps the historical `baked <input> ->
<output>` line on stdout. Any resource class whose count was reduced by the
round trip produces a warning on stderr with a stable code such as
`[scene-count-reduced]`, `[camera-count-reduced]`,
`[node-animation-count-reduced]`, `[morph-target-count-reduced]`,
`[morph-shape-count-reduced]`, or `[variant-count-reduced]`. A texture mismatch
also warns with its material index, slot, and a stable code such as
`[texture-source-container-lost]` or `[texture-decoded-texels-changed]`.
When textures are present, stdout includes a compact count of the three texture
fidelity states before the historical `baked` line.

`--json` suppresses the human success line and fidelity warnings and writes one
compact report object to stdout. Its stable schema identifier is
`zanna.asset-bake-report/v1`. The report contains `status`, `input`, `output`,
`lossy`, `source`, `baked`, `losses`, `textureFidelity`, and the source loader's
`importReport`.
The source/baked snapshots expose mesh, material, skeleton, skeletal-animation,
node-animation, morph-target, morph-shape, node, scene, camera, and
material-variant counts. Each loss has a stable `code`, the affected `resource`,
both counts, and `dropped`.
`textureFidelity.summary` counts `preserved-source`, `preserved-decoded`,
`changed-after-import`, and `losses`. Its per-material/per-slot entries compare
the unresolved runtime reference kind, exact source-container kind and bytes,
dimensions, mip count, compressed/native format, decoded texels, and shared
reference identity. The states mean:

- `preserved-source`: the same runtime texture kind and exact encoded container
  survived the round trip;
- `preserved-decoded`: no exact encoded source was available, and the canonical
  decoded texels survived; and
- `changed-after-import`: a retained decoded surface changed after import, so
  the current RGBA8 texels correctly replaced stale encoded bytes.

Losing an available exact container makes `lossy` true; the absence of source
bytes at import time does not. A failed JSON bake instead reports
`status: "error"` and the failing `stage` (`load`, `save`, or `verify`) and exits
with code 2. Successful JSON mode does not write to stderr, so build tools can
parse stdout directly.

`zanna asset validate <input>` loads a model and prints the
`AssetDiagnostics3D` import report as JSON (skipped primitives, truncated
influences, ignored extensions, compressed animation keys, warnings). Exit
codes: 0 success, 1 usage, 2 load failure.

Requires a graphics-enabled runtime build; other configurations report that
constraint and exit.

### zanna install-package

Package a staged Zanna developer-tools install tree. A valid toolchain
installer stage must include every installed binary tool:

```text
zanna, zia, zbasic, ilrun, il-verify, il-dis, zia-server,
zbasic-server, basic-ast-dump, basic-lex-dump, zannastudio
```

```bash
zanna install-package --build-dir build --target tarball
zanna install-package --build-dir build --target linux-deb
zanna install-package --build-dir build --target linux-rpm
zanna install-package --build-dir build --target linux-bundle
zanna install-package --build-dir build --target windows
zanna install-package --build-dir build --target macos
zanna install-package --build-dir build --stage-only
zanna install-package --verify-only build/installers/zanna-0.2.7-dev-macos-arm64.tar.gz
```

Typical workflow:

- run `cmake --install <build-dir> --prefix <stage-dir>` yourself, or let `zanna install-package --build-dir <build-dir>` create a temporary staged install
- validate the staged install tree before packaging
- emit one or more native toolchain artifacts from the same staged manifest

| Option | Description |
|--------|-------------|
| `--target windows\|macos\|linux-deb\|linux-rpm\|linux-bundle\|tarball\|all\|all-available\|macos-dmg` | Select artifact format(s); `linux-bundle` emits `.run` and `macos-dmg` aliases `--target macos --macos-dmg` |
| `--build-dir <dir>` | Stage from an existing build tree via `cmake --install` |
| `--stage-dir <dir>` | Package an already-staged install tree |
| `--stage-only` | Validate and print staged metadata without producing artifacts |
| `--verify-only <path>` | Structurally verify an existing installer artifact |
| `--require-checksum` | Require and validate `<artifact>.sha256` with `--verify-only` |
| `--arch x64\|arm64` | Require this architecture; packaging rejects a staged native executable that does not match |
| `--macos-pkg-version <version>` | Dotted numeric package version override when the Zanna version contains Debian/SemVer suffixes |
| `--macos-min-version <version>` | Override the architecture-based minimum supported macOS version |
| `--macos-sign-identity <identity>` | Developer ID Installer identity for signing generated macOS `.pkg` artifacts |
| `--macos-app-sign-identity <identity>` | Developer ID Application identity for every nested Mach-O and helper app |
| `--macos-notary-profile <profile>` | `notarytool` keychain profile used to notarize signed macOS `.pkg` artifacts |
| `--macos-staple` | Staple the notarization ticket after successful macOS package submission |
| `--macos-notary-timeout <seconds>` | Bound the `notarytool --wait` timeout for macOS package submission |
| `--macos-dmg` | Also wrap a generated macOS `.pkg` in a styled `.dmg` |
| `--macos-dmg-background <path>` | PNG background image for the generated toolchain `.dmg` window |
| `--macos-dmg-icon <path>` | `.icns` volume icon for the generated toolchain `.dmg` |
| `--macos-pkg-license <path>` | License text shown by the macOS `.pkg` installer |
| `--macos-pkg-background <path>` | Background image for the macOS `.pkg` installer pane |
| `--license <spdx>` | Toolchain package license metadata override |
| `--maintainer <name>` | Toolchain maintainer/packager metadata override |
| `--maintainer-email <email>` | Debian maintainer email metadata override |
| `--homepage <url>` | Toolchain package homepage metadata override |
| `--windows-sign` | Authenticode-sign generated Windows toolchain installers |
| `--windows-sign-pfx <path>` | PFX certificate for Windows signing; the password comes from `ZANNA_WINDOWS_SIGN_PASSWORD` |
| `--windows-sign-thumbprint <sha1>` | Sign with a certificate-store SHA-1 thumbprint |
| `--windows-timestamp-url <url>` | RFC3161 timestamp URL for Windows signing |
| `--windows-signtool <path>` | `signtool.exe` path override |
| `--windows-sign-no-verify` | Skip `signtool verify` after signing |
| `--linux-sign-key <id>` | GPG-sign generated `.deb`/`.rpm` toolchain packages with `dpkg-sig`/`rpmsign` |
| `--windows-install-scope user\|machine` | Select the toolchain installer scope (default `user`) |
| `--windows-install-dir <name>` | Override the Windows toolchain install-root directory name |
| `--windows-identifier <id>` | Override the Apps & Features identity and integration ownership namespace |
| `--windows-channel <id>` | Set the update/coexistence channel; local output defaults to `development`, while `stable` requires `--release` |
| `--source-commit <hex>` | Supply immutable lowercase source identity for an archive build |
| `--windows-documentation-url <https-url>` | Set the documentation/support link shown by setup and maintenance |
| `--windows-update-manifest-url <https-url>` | Configure signed update discovery; omit it for a deterministic offline/unconfigured package |
| `--windows-update-rsa-modulus <hex>` | Pin the update manifest's RSA public modulus |
| `--windows-update-rsa-exponent <hex>` | Pin the update manifest's RSA public exponent |
| `--windows-no-path` | Do not add the installed `bin/` directory to `PATH` |
| `--windows-file-associations on\|off` | Add safe `.zia`/`.bas`/`.il` Open With entries without replacing defaults (default `on`) |
| `--windows-shortcuts on\|off` | Create Start Menu developer shortcuts (default `on`) |
| `--allow-debug-toolchain` | Permit Windows packages that reference MSVC debug CRTs |
| `--skip-build` | With `--build-dir`, run `cmake --install` without rebuilding first |
| `-o <path>` | Compatibility output path: a file for one target unless it names an existing directory; a directory for multiple targets |
| `--output-file <path>` | Explicit single-artifact output path |
| `--output-dir <path>` | Explicit artifact output directory |
| `--artifact-manifest <path>` | Override the JSON artifact inventory path |
| `--release` | Require reproducibility, a clean immutable source identity, verification, platform trust, collision protection, and atomic release cleanup |
| `--keep-stage-dir` | Preserve the auto-generated staging directory |
| `--no-verify` | Skip post-build structural verification |
| `--verbose` | Print staged source/version/arch/file counts and resolved Windows channel identity |

Developer wrappers:

- `scripts/build_installer.sh`
- `scripts/build_installer.ps1`

Installer builds enable `ZANNA_INSTALL_ZANNASTUDIO=ON`, which builds the native
Zanna Studio binary through the freshly built `zanna` tool and stages
`bin/zannastudio` or `bin/zannastudio.exe` plus `bin/zannastudio.buildinfo`.
`ZANNA_IDE_ARCH=x64|arm64` overrides the IDE native target architecture; when
unset, CMake selects the host architecture. `ZANNA_INSTALL_ZANNASTUDIO` defaults
to `ON` so `cmake --install` and `zanna install-package --build-dir` produce a
complete installer payload by default.
The Windows wrapper rejects Debug-like build configurations, treats explicit
stage/build/verification inputs as caller-owned, and verifies both
`zannastudio.exe` and `zannastudio.buildinfo` whenever Studio is enabled before
it invokes packaging.

Staged toolchain packaging accepts `x64` and `arm64` architecture names, and also accepts `universal` only for a detected macOS fat32/fat64 Mach-O whose bounded slices actually contain both supported architectures. It requires a package version from `lib/cmake/Zanna/ZannaConfigVersion.cmake` or `include/zanna/version.hpp`; CMake package path validation is case-insensitive for staged filesystems that vary directory casing. Linux `.deb` output maps architectures to `amd64` and `arm64`; RPM output maps them to `x86_64` and `aarch64`. RPM generation requires `rpmbuild`; Linux `--target all` includes `.deb`, `.rpm`, the FUSE-less `.run` bundle, and a portable tarball, and fails with an actionable diagnostic if `rpmbuild` is missing. `--target all-available` skips only unavailable RPM output. For Windows and macOS stages, the meta-targets emit the staged platform's native package plus a portable tarball. A target incompatible with the detected staged PE, Mach-O, or ELF binary fails before writing output.

Linux-platform tarballs include `install.sh`, `uninstall.sh`, `README.install`, hicolor app icons, and `share/zanna/install_manifest.txt`. Their scripts honor `PREFIX` and `DESTDIR`, as well as `--dry-run`, `--force`, and `--quiet`; preflight unowned conflicts; stage and journal replacements on the destination filesystem; roll back a failed install or uninstall; remove stale owned files on upgrade; record the actual prefix; refresh caches only for a direct host install; and preserve unrelated content.

Staged toolchain packaging rejects symlinks whose resolved targets leave the staged prefix, including when `--stage-dir` itself is a symlink and CMake's install manifest records paths through that alias. Relative internal symlink targets are preserved as written in tar-based artifacts and in macOS package roots; absolute internal symlinks are converted to archive-relative targets. Linux `.deb`, `.rpm`, `.run`, and Linux-platform tarball outputs preserve staged Unix permission bits, map root-level documentation to `/usr/share/doc/zanna/`, include license/copyright/README metadata, a visible `zannastudio.desktop` launcher, hicolor icons, and hidden desktop/MIME handlers. Runtime libraries are hard package dependencies; CMake, make, a C++ compiler, and desktop/MIME/man cache utilities are recommendations. Optional X11/ALSA dependencies are derived from the staged support libraries. RPM `%install` copies the complete staged tree, including dotfiles, and `%files` entries safely quote special paths.

The `.run` bundle launches `zannastudio` with no arguments and dispatches arguments to the CLI. Its full-payload SHA-256 selects a private XDG cache directory. It verifies owner and permissions, rejects symlink components, serializes concurrent first launches with a stale-lock recovery path, extracts through an atomic temporary root, and validates the hash stamp before reuse. `ZANNA_BUNDLE_QUIET=1` suppresses status text and `ZANNA_BUNDLE_REFRESH=1` forces refresh.

Windows toolchain installers dereference only symlinks to regular files and reject directory symlinks because the Windows payload does not carry POSIX symlink metadata. x64 and ARM64 use the same statically linked native host and detached cleanup helper; the canonical path never invokes PowerShell. The schema-3 overlay inventories and hashes the maintenance host, cleanup helper, compressed inner payload, every selected file, component, integration, architecture, channel, version, and build identity. Recursive verification rejects extra, missing, mismatched, or wrong-architecture content before setup mutates the machine.

The native lifecycle engine preflights the supported Windows floor, canonical Unicode destination, disk requirements, semantic-version policy, unowned conflicts, reparse traversal, concurrent setup, and files in use. Install, upgrade, reinstall, modify, repair, explicit downgrade, and uninstall use recoverable journals and directory swaps. Repair restores exact owned hashes; uninstall delegates to the verified maintenance cache and then removes that cache and the empty install root with the detached helper. Arbitrary unowned developer files and sibling release channels survive.

The installer adds `bin` to the selected user or machine `Path` only when absent, records only the exact entry it owns, removes only that token during uninstall, and preserves unrelated edits. It broadcasts environment changes, creates Start Menu shortcuts for the developer prompt and Zanna Studio (plus the VS Code extension installer only when a validated `.vsix` is staged), and adds safe Open With ProgIDs for `.zia`, `.bas`, and `.il` without taking over their default handler. The developer prompt sets `ZANNA_HOME`, `Zanna_DIR`, and `CMAKE_PREFIX_PATH`, so external CMake projects can use `find_package(Zanna CONFIG REQUIRED)` without an additional prefix argument.

The high-DPI native wizard offers one-click Typical, SDK, and Complete paths plus a scrollable custom surface for runtime scope, destination, components, and integrations. It uses system colors, keyboard navigation, accessible names, explicit license acceptance, Restart Manager, cooperative cancellation, redacted unique logs, and truthful self-checked finish actions. Silent setup supports `/quiet`, `/passive`, `/scope`, `/installDir`, `/type`, `/components`, integration switches, `/closeApplications`, `/allowDowngrade`, `/log`, and `/norestart`; `/?` prints the exact contract and exit codes. `/inspect` prints verified package JSON without mutation, and `/checkForUpdates` verifies a pinned signed same-origin manifest and downloaded SHA-256 when configured. Both accept `/output <path>` to publish complete UTF-8 JSON atomically when a GUI process has no reliable inherited standard-output stream.

Release staging installs the architecture-matched compiler runtime DLLs beside the tools, and package generation rejects any imported MSVC runtime missing from the staged executable directory; setup never downloads a redistributable. Zanna-owned nested PEs and generated maintenance binaries are signed before hashing, Microsoft runtime signatures are preserved, and the outer setup is signed last. Unsigned local packages use a separate development identity; stable identity is reserved for trusted release generation. `scripts/new-windows-update-manifest.ps1` exports the pinned key and authors deterministic signed manifests.

For manual clean-VM Windows validation, run `scripts/validate-windows-toolchain-installer.ps1 -Installer <installer.exe>`. Add `-BaselineInstaller <older.exe>` to exercise transactional upgrade and stale-file cleanup, and `-RequireSignature` for a release candidate. The script derives package identity from `/inspect` and checks every required binary, version, `zanna run`, fresh-process PATH, safe associations, native codegen, an external CMake consumer launched through the developer prompt, preservation of unrelated upgrade content, and residue-free owned-only uninstall. The opt-in Windows toolchain E2E adds deterministic package generation, Unicode paths, component modification, exact-hash repair, concurrency, files in use, direct-root uninstall, and fault-injection recovery.

macOS toolchain packages are generated without `pkgbuild` or `productbuild`: Zanna writes the CPIO/XAR component and product archives and uses `mkbom` only for the bill of materials. The package installs under `/usr/local/zanna`, owns command/manpage symlinks, provides `/usr/local/lib/cmake/Zanna` wrappers, and installs `/Applications/Zanna Toolchain.app` as a LaunchServices handler for `.zia`, `.bas`, and `.il`. The installed uninstaller removes manifest-owned content, unregisters the handler, and forgets the receipt while preserving unrelated paths.

Distribution metadata restricts installation to a root volume, declares install domains, host architectures, and an architecture-based minimum OS; `--macos-min-version` overrides that floor. The Installer UI includes welcome, license, read-me, destination, and conclusion panes with generated light/dark backgrounds. `--macos-app-sign-identity` or `ZANNA_MACOS_APP_SIGN_IDENTITY` signs every nested Mach-O and helper app before `--macos-sign-identity`/`ZANNA_MACOS_SIGN_IDENTITY` signs the product. `--macos-notary-profile` submits with a bounded `notarytool --wait`; `--macos-staple` staples and validates the ticket. A styled DMG gets generated artwork/icon defaults, bounded Finder automation, read-only remount verification, `hdiutil verify`, notarization/stapling, and Gatekeeper `open` assessment. `--release` requires both identities, a notary profile, and stapling.

The macOS GUI's Destination Select step chooses the destination volume; the install prefix remains `/usr/local/zanna` so command and CMake discovery paths are consistent. Privileged macOS and Linux lifecycle tests are opt-in and must run on a disposable clean host. They now cover install, installed tools/CMake/native codegen, upgrade where the package manager supports it, stale owned-file removal, preservation of unrelated content, uninstall, and receipt/package-state cleanup. Use `ZANNA_RUN_MACOS_INSTALLER_SMOKE=1` or `ZANNA_RUN_LINUX_INSTALLER_SMOKE=1`; exact handoff commands and release credential contracts are in [Installer and Package Release Guide](../installer-release.md).

Every successful toolchain package invocation writes `<artifact>.sha256` and a JSON artifact inventory; multi-output invocations also write `SHA256SUMS`. `--verify-only --require-checksum` validates structure and the adjacent digest. `--release` additionally requires numeric `SOURCE_DATE_EPOCH`, refuses verification bypasses and output collisions, serializes writers with an output lock, cleans partial artifact sets, and requires Authenticode on Windows, full Developer ID/notary/staple trust on macOS, or verified OpenPGP package signatures on Linux. Native manual workflows live under `.github/workflows/*-release-installer.yml` and implement those gates.

`install-package --verify-only` infers formats only from supported extensions: `.exe`, `.pkg`, `.dmg`, `.deb`, `.rpm`, `.run`, `.tar.gz`, and `.tgz`. Unknown extensions fail unless a supported `--target` is provided. Post-build verification for generated Windows, Debian, RPM, Linux bundle, macOS, DMG, and tarball toolchain artifacts checks every staged manifest path in the emitted payload where the format exposes a payload listing, including generated icons, Linux desktop/MIME metadata, and macOS command, manpage, manifest, file-handler app, uninstall helper, and CMake-wrapper paths. `.pkg` verification is native: it validates product and component XAR headers/TOCs/checksums, inflates gzip payloads, validates CPIO structure, rejects AppleDouble sidecars, requires `Payload`, `PackageInfo`, `Bom`, `Scripts`, `Distribution`, and script entries, then checks the payload paths directly. `.dmg` verification checks the UDIF trailer signature; `.rpm` verification checks the RPM lead, signature header, main header bounds, that a non-empty payload follows the headers, reads RPM file-list tags from the main header, and compares those paths natively without shelling out to `rpm -qpl`.

---

## Exit Codes

| Code | Meaning                                   |
|------|-------------------------------------------|
| `0`  | Program completed successfully            |
| `10` | Halted at breakpoint with no debug script |
| `>0` | Trap or error                             |

> **Known discrepancy:** the `10` breakpoint code is only produced when the IL
> module's `@main` returns `i64`. A module whose `@main` returns `void` — which
> is what `zanna build` emits for Zia sources — still prints `[BREAK]` but exits
> `0`. Check for the `[BREAK]` line on stderr rather than relying on the exit
> code when scripting against Zia-built IL.

`zanna check` and `zanna eval` define differentiated exit codes for
programmatic callers: see their sections above (`0` ok, `1` usage, `2`
compile error, and `3` runtime trap for `eval`).

---

## CMake Integration

Projects embedding Zanna tooling can consume the exported CMake package:

```cmake
find_package(Zanna CONFIG REQUIRED)
target_link_libraries(mytool PRIVATE zanna::il_core zanna::il_io zanna::il_vm)
```
