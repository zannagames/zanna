# AGENTS.md — Operating Guide for AI Coding Assistants

This file is the tool-agnostic operating guide for AI coding agents (Codex,
Copilot Workspace, Cursor, etc.) working on Zanna. Claude Code users: CLAUDE.md
is the authoritative version of these rules; this file mirrors its essentials
and documents the agent-facing toolchain surface.

## Ground Rules

- **Spec first.** `/docs/il/il-guide.md#reference` (IL v0.3.0) is normative.
  ADRs are required for IL opcode, grammar, verifier-rule, cross-layer
  dependency, runtime C ABI surface, `docs/il/il-guide.md#reference`, and
  `.github/workflows/*` changes.
- **Always green.** Build and tests must pass locally before proposing changes.
- **Zero dependencies.** Zanna is 100% from-scratch. Never introduce external
  libraries or package downloads into the product.
- **Cross-platform always.** Every change must work on macOS, Windows, and
  Linux. Use `src/common/PlatformCapabilities.hpp` or `src/runtime/rt_platform.h`;
  raw `_WIN32`/`__APPLE__`/`__linux__` checks belong only in approved adapter layers.
  Run `./scripts/lint_platform_policy.sh` for cross-platform-sensitive work.
- **Conventional commits**, no AI attribution or generated-by footers.
- **Full Zanna source headers** on all new/modified source files (see CLAUDE.md
  for the template). Format with `.clang-format`; zero warnings.

## Build & Test

Always use the build scripts, never raw cmake for full builds:

```sh
./scripts/build_zanna_linux.sh     # or build_zanna_mac.sh
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```

Fast iteration knobs (environment variables, all platforms):

| Variable | Effect |
|----------|--------|
| `ZANNA_SKIP_CLEAN=1` | Incremental rebuild (skip the clean-all step) |
| `ZANNA_SKIP_TESTS=1` | Build only; skip ctest |
| `ZANNA_TEST_LABEL=<label>` | Run only tests with that ctest label |
| `ZANNA_CTEST_TIMEOUT=<seconds>` | Bound tests without an explicit CMake timeout (Windows default: 600 seconds) |
| `ZANNA_SKIP_LINT=1 ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1` | Skip non-test stages |
| `ZANNA_SKIP_STUDIO=1` | Skip the multi-minute Zanna Studio native compile |
| `ZANNA_CMAKE_GENERATOR=Ninja` | Use Ninja |

ccache is auto-detected (disable with `ZANNA_NO_CCACHE=1`).

Targeted test runs after an incremental build:

```sh
ctest --test-dir build -L codegen --output-on-failure   # by label
ctest --test-dir build -R test_zia_lexer                # by name
./build/src/tests/test_foo --filter=Suite.Name          # single case
./scripts/update_goldens.sh [filter]                    # regenerate goldens
```

See `src/tests/CMakeLists.txt` and `docs/internals/testing.md` for the label taxonomy.

## Agent-Facing Toolchain Surface

These commands are designed for programmatic use; prefer them over parsing
human-oriented output.

| Command | Purpose |
|---------|---------|
| `zanna check <file\|dir> --diagnostic-format=json` | Type-check + verify a file or project without running. Exit 0 = clean, 1 = usage/target error, 2 = compile errors. JSON diagnostics (stderr) carry code, stage, range, notes, and machine-applicable `fixits`. |
| `zanna eval 'expr' [--json] [--type] [--il]` | One-shot snippet evaluation (Zia default, `--lang basic`; `--type`/`--il` are Zia-only). Exit 0/1/2/3 = ok/usage/compile/trap. Reads stdin when no code argument. |
| `zanna explain <CODE> [--json]` | Describe a diagnostic code (e.g., `V-ZIA-UNDEFINED`, `B2001`, `W008`). `zanna explain --list --json` dumps the catalog. |
| `zanna --print-error-codes [--json]` | Full diagnostic-code catalog. |
| `zanna --dump-runtime-api` | JSON inventory of the entire runtime surface (functions + classes with members), generated from the live registry. |
| `zanna --dump-opcodes` | JSON registry of all IL opcodes with arity, operand types, and effects. |
| `zanna run program.zia --diagnostic-format=json` | Compile and execute; same structured diagnostics. |
| `zanna run program.il --max-steps N` | Step-budgeted execution (safe for untrusted generated code on the VM). |
| `--dump-tokens / --dump-ast / --dump-sema-ast / --dump-il / --dump-il-opt / --dump-il-passes` | Pipeline introspection at every stage (see docs/tools/debugging.md). |
| `zanna run --debug-adapter` | VM-backed debugger over newline-delimited JSON (breakpoints, stepping, call stacks, locals). |

## MCP Language Server

`zia-server --mcp` exposes the Zia compiler over the Model Context Protocol
(11 tools: check, compile, completions, hover, symbols, IL/AST/token dumps,
runtime-classes/-methods/-search). The repo's `.mcp.json` points at the build
output (`build/src/tools/zia-server/zia-server`); build the project first.
See docs/tools/zia-server.md and docs/tools/zia-server-mcp-tools.md. A BASIC equivalent
ships as `zbasic-server`.

## Key Docs

- Architecture: `docs/internals/architecture.md` · Code map: `docs/internals/codemap.md` (+ `docs/internals/codemap/`)
- Languages: `docs/languages/zia-reference.md`, `docs/languages/basic-reference.md`
- IL: `docs/il/il-guide.md#quickstart`, `docs/il/il-guide.md` (normative)
- Debugging & diagnostics: `docs/tools/debugging.md` · Tools: `docs/tools/cli.md`
- Testing: `docs/internals/testing.md`
