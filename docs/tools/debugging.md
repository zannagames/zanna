---
status: active
audience: public
last-verified: 2026-08-31
---

# Zanna Debugging Guide

This guide covers all debugging features available in the Zanna platform, including both the VM and the Zia/BASIC frontends.

---

## Quick Reference

| Goal | Command / Flag |
|------|---------------|
| Trace every IL instruction | `--trace=il` |
| Trace source locations | `--trace=src` |
| Break at source line | `--break-src program.il:42` (IL path, source line) |
| Break at block label | `--break entry` |
| Watch a variable | `--watch x` |
| Limit execution steps | `--max-steps 10000` |
| Single-step on entry | `--step` |
| Dump IL on trap | `--dump-trap` |
| Run debug script | `--debug-cmds script.dbg` |
| Dump token stream | `--dump-tokens` |
| Dump AST after parsing | `--dump-ast` |
| Dump AST after sema | `--dump-sema-ast` |
| Dump IL after lowering | `--dump-il` |
| Dump IL after optimization | `--dump-il-opt` |
| Dump IL per-pass | `--dump-il-passes` |
| Keep safety warnings as warnings | `--no-strict-diagnostics` |
| Suppress successful warning output | `--quiet-warnings` |
| Emit diagnostics as JSON | `--diagnostic-format=json` |

---

## 1. Tracing

Tracing prints detailed execution information to stderr as the VM runs each instruction.

### IL Trace Mode

```sh
zanna -run program.il --trace=il
```

Output format:
```text
[IL] fn=@main blk=L3 ip=#5 op=add 10, 20 -> %t5
```

Each line shows: function name, block label, instruction index, opcode, operands, and result register.

### Source Trace Mode

```sh
zanna -run program.il --trace=src
```

Output format:
```text
[SRC] main.zia:42:10  (fn=@main blk=L3 ip=#5)  Say("hello");
```

Shows the source file, line, and column for each executed instruction, followed
by the source text from that column onward. Requires that the IL was compiled
with source location information (Zia and BASIC both propagate source locations
to IL).

> When you run a **source** file (`zanna run program.zia --trace=src`), the file
> name and the echoed text are the real source. When you run a **pre-built `.il`
> module** (`zanna -run program.il --trace=src`), the runner registers the `.il`
> file itself as the source, so the `.loc` line/column numbers still come from
> the original source while the file name and echoed text come from the `.il`
> file — the echoed snippet will not line up. Trace the source directly when you
> want readable source text.

### Notes

- Both trace modes write to stderr, so program output on stdout remains clean.
- Tracing is **all-or-nothing** per mode; there is no per-function or per-file filter.
- In `zanna run` and the legacy `zanna front zia` / `zanna front basic` entry points, source-driven trace runs disable IL optimization to preserve the instruction-to-source mapping.

---

## 2. Breakpoints

Breakpoints pause execution at a specific point and return control to the debug controller.

Source breakpoints are resolved by the IL runner, and only by the IL runner —
`zanna run` does not accept `--break` or `--break-src`. Build the source target
to IL first, then debug the resulting module:

```sh
zanna build program.zia -o /tmp/program.il
zanna -run /tmp/program.il --break-src /tmp/program.il:42
```

> **Pass the `.il` path, not the original source path.** Textual IL carries no
> source-file table, so the runner registers the `.il` file being executed as
> file 1 and every `.loc` record resolves to it. The **line number** is still the
> original source line recorded at lowering time, but the **file name** must be
> the `.il` module. `--break-src program.zia:42` matches nothing and the program
> runs to completion silently.

### Source-Line Breakpoints

```sh
zanna -run program.il --break-src program.il:42
```

Pauses before executing any instruction whose recorded source line is 42. File paths are normalized automatically (forward slashes, case-insensitive on Windows).

### Block-Label Breakpoints

```sh
zanna -run program.il --break entry
```

Pauses when the VM enters a block with the given label. Also supports `file:line` format as a shorthand for `--break-src` — with the same `.il`-path rule:

```sh
zanna -run program.il --break program.il:42
```

### Multiple Breakpoints

Specify multiple `--break` or `--break-src` flags:

```sh
zanna -run program.il --break-src program.il:10 --break-src program.il:25
```

### Breakpoint Coalescing

Repeated hits on the same source line are coalesced — the debugger does not spam notifications for every IL instruction that maps to the same line.

---

## 3. Variable Watches

Watches report every time a named variable is stored to during execution.

```sh
zanna -run program.il --watch x --watch total
```

Output format:
```text
[WATCH] x=I64:42 (fn=@main blk=L3 ip=#5)
```

### How It Works

- Watches use an O(1) fast-path via a symbol-to-watch-ID map.
- Only reports when the value **changes** from the previous watched value (deduplication).
- Multiple watches can be active simultaneously.

### Limitations

- Zia local variables and mutable slots carry source names into IL value metadata, so `--watch x` works for ordinary Zia locals named `x`.
- For compiler-generated temporaries, duplicate shadowed names, and some BASIC lowering paths, you may still need to inspect IL output (`--dump-il` or `-emit-il`) to find the exact name.

---

## 4. Memory Watches

Memory watches monitor reads/writes to specific memory address ranges. This is primarily useful for debugging the runtime or tracking heap corruption.

### Programmatic API

```cpp
Runner runner(module);
runner.addMemWatch(addr, size, "my-tag");
// ... run ...
auto hits = runner.drainMemWatchHits();
for (auto &hit : hits) {
    // hit.tag, hit.addr, hit.size
}
runner.removeMemWatch(addr, size, "my-tag");
```

Memory watches use binary search for efficient lookup when 8+ watches are active.

---

## 5. Stepping

### Single-Step on Entry

```sh
zanna -run program.il --step
```

Executes one instruction at the entry point, then continues to completion. Useful for verifying the program starts correctly.

### Step Budget

```sh
zanna -run program.il --max-steps 10000
```

Limits the total number of IL instructions executed. When exceeded, execution stops with `RunStatus::StepBudgetExceeded`. Set to 0 (default) for unlimited.

### Debug Script Automation

```sh
zanna -run program.il --debug-cmds script.dbg
```

The debug script file contains one command per line:

| Command | Description |
|---------|-------------|
| `continue` | Resume execution until next breakpoint or halt |
| `step` | Execute exactly 1 instruction |
| `step N` | Execute exactly N instructions |
| `step-over` | Execute through the current call without pausing in the callee |
| `step-out` | Continue until the current function returns to its caller |

Example `script.dbg`:
```text
step 5
continue
step-over
step 10
step-out
continue
```

Empty lines and unrecognized commands are ignored with a `[DEBUG]` message to stderr.

### Programmatic Stepping (C++ API)

```cpp
Runner runner(module);
// Execute one instruction
auto result = runner.step();
// result.status: Advanced, Halted, BreakpointHit, Trapped, Paused

// Continue until terminal state
auto status = runner.continueRun();
// status: Halted, BreakpointHit, Trapped, Paused, StepBudgetExceeded
```

### Current Limitations

- **Step = 1 IL instruction**, not 1 source line. A single source line may compile to many IL instructions.
- Step-over and step-out are frame-depth based. They work for VM function calls; they do not yet provide source-line
  granularity or native debugger integration.

---

## 6. Error Reporting

### Diagnostic Format

All compiler diagnostics follow this format:

```text
<path>:<line>:<column>: <severity>[<code>]: <message>
```

For example:
```text
main.zia:42:1: error[V-ZIA-TYPE-MISMATCH]: Type mismatch: expected Integer, got String
 42 | var x: Integer = "hello";
    | ^
  stage: sema
```

The caret marks the start of the offending declaration, and a `stage:` line
names the pipeline phase that produced the diagnostic.

Severity levels: `note`, `warning`, `error`.

Diagnostic codes are prefixed by subsystem:
- `V-ZIA-LEX-*` — Zia lexer
- `V-ZIA-PARSE-*` — Zia parser
- `V-ZIA-*` — Zia semantic analysis
- `V-ZIA-LOWER-*` — Zia lowering invariants
- `B1xxx` — BASIC frontend
- `V-IL-*` — IL verification
- `V-BC-*` — bytecode compiler
- `V-CG-*` — native backend/codegen
- `V-SRC-*` — shared source loading/registration

Use `--diagnostic-format=json` on `zanna` subcommands for machine-readable output. The JSON form writes a compact object with a `diagnostics` array and includes severity, code, message, stage, location, range, source line, help text, fix-its, and notes when available.

Diagnostic codes are documented in a central catalog: `zanna explain <code>`
describes one code (`--json` for structured output), and
`zanna --print-error-codes --json` dumps the whole catalog. Codes not yet
cataloged still resolve to their subsystem family by prefix.

### Source Snippets

When a source manager is available, diagnostics include the offending source line with a caret (`^`) pointing to the error column. Frontends cache in-memory source text, so diagnostics still render snippets when the source came from the CLI, REPL, import resolver, or another non-file buffer. Same-line ranges are underlined with `^~~~`, and related locations can appear as `note:` entries below the primary error.

### Compile-Time Gates

Several checks now run before a binary or VM run can start:

- Zia O0/debug builds verify IL after lowering. Optimized Zia builds normally verify the final optimized IL and skip
  the intermediate lower-stage verifier for faster large builds; use `--paranoid-verify` to restore every frontend
  verifier checkpoint. Use `--verify-each` to add verifier runs between optimizer passes while debugging pass failures.
- BASIC assignment and operator type mismatches, such as assigning a string-valued expression to an integer variable or using `MOD` with a float operand, are reported during semantic analysis with `B2001` before IL is emitted.
- Optimization pipeline failures are surfaced as diagnostics instead of continuing with potentially invalid IL.
- Bytecode compilation uses checked diagnostics for unsupported or malformed IL, so the VM reports compile failure instead of crashing during execution.
- File-based native codegen loads and verifies textual IL before backend lowering. `zanna build` reuses the already
  verified in-memory module from the frontend pipeline.
- Zia lexer errors and semantic errors stop compilation before lowering, so bad token streams cannot still produce IL.
- Literal fixed-array indexes are checked at compile time when the array length is known.

`zanna run`, `zanna build`, and `zanna front zia` enable strict diagnostics by default. Safety-critical Zia warnings are promoted to errors before execution or emission; this currently includes missing returns, division by zero, uninitialized variables, optional access without a check, and non-exhaustive matches. Use `--no-strict-diagnostics` only when you intentionally want those findings to remain warnings.

Warnings are printed even when compilation succeeds. Use `--quiet-warnings` or `--no-warnings` to suppress successful warning output.

### Trap Format

Runtime traps (VM errors) are formatted by the execution path that raised them.

`zanna -run` and `ilrun` (standard VM) use:

```text
Trap @function:block#ip line N: Kind (code=C): path:line:column: message
```

```text
Trap @processRow:L3#2 line 145: Bounds (code=7): src/main.zia:145:12: index out of bounds
```

When no source path is registered, the VM falls back to `file#ID:line:column` after the trap kind while preserving the
stable `line N` token used by existing tooling.

`zanna run` (the default execution path) places the source detail in parentheses
immediately after the instruction pointer and omits the `line N` token:

```text
Trap @main:entry_0#26 (/path/to/main.zia:7): Overflow (code=4): Overflow: integer overflow in add
```

Match on the trap **kind** name rather than on either layout or on `code=`.

### Trap Kinds

These are the values of the `TrapKind` enum (`src/vm/Trap.hpp`). They are **not**
the number printed as `(code=C)` in a trap message — that is a separate
secondary error code, and the same logical failure can carry different secondary
codes on different execution paths.

| Kind | TrapKind value | Description |
|------|------|-------------|
| DivideByZero | 0 | Integer division or remainder by zero |
| Overflow | 1 | Arithmetic or conversion overflow |
| InvalidCast | 2 | Invalid cast or type conversion |
| DomainError | 3 | Semantic domain violation |
| Bounds | 4 | Array index out of bounds |
| FileNotFound | 5 | File not found |
| EOF | 6 | Unexpected end of file |
| IOError | 7 | I/O failure |
| InvalidOperation | 8 | Invalid operation for current state |
| RuntimeError | 9 | General runtime error |
| Interrupt | 10 | External interrupt |
| NetworkError | 11 | Network I/O failure (connection, DNS, TLS) |

Use `--dump-trap` to ensure trap messages are printed to stderr even when the program handles them internally.

---

## 7. Runtime Logging

### Zanna.Diagnostics.Log API (Zia / BASIC)

The runtime provides a leveled logging system accessible from both Zia and BASIC:

```zia
Zanna.Diagnostics.Log.Debug("detailed info")
Zanna.Diagnostics.Log.Info("normal info")
Zanna.Diagnostics.Log.Warn("potential issue")
Zanna.Diagnostics.Log.Error("something failed")
```

Output format:
```text
[INFO] 2026-08-31 14:30:05 normal info
```

Log levels (lowest to highest): DEBUG (0), INFO (1), WARN (2), ERROR (3), OFF (4).

Default level: INFO. Messages below the current level are suppressed.

### Zanna.Core.Diagnostics API

The runtime assertion library provides assertion variants for test and debug use. Every assertion
takes a trailing `msg: str` argument that describes the failure:

| Function | Signature | Description |
|----------|-----------|-------------|
| `Assert(cond, msg)` | `void(i1, str)` | Assert condition is true |
| `AssertEq(a, b, msg)` | `void(i64, i64, str)` | Assert i64 values are equal |
| `AssertNeq(a, b, msg)` | `void(i64, i64, str)` | Assert i64 values are not equal |
| `AssertEqNum(a, b, msg)` | `void(f64, f64, str)` | Assert f64 values are equal |
| `AssertEqStr(a, b, msg)` | `void(str, str, str)` | Assert string equality |
| `AssertNull(val, msg)` | `void(obj, str)` | Assert object is null |
| `AssertNotNull(val, msg)` | `void(obj, str)` | Assert object is non-null |
| `AssertFail(msg)` | `void(str)` | Unconditionally fail with message |
| `AssertGt(a, b, msg)` | `void(i64, i64, str)` | Assert `a > b` |
| `AssertLt(a, b, msg)` | `void(i64, i64, str)` | Assert `a < b` |
| `AssertGte(a, b, msg)` | `void(i64, i64, str)` | Assert `a >= b` |
| `AssertLte(a, b, msg)` | `void(i64, i64, str)` | Assert `a <= b` |
| `Trap(msg)` | `void(str)` | Raise a runtime trap with message |

These are exposed as static methods on `Zanna.Core.Diagnostics` and registered in
`src/il/runtime/defs/api/core_crypto.def`.

### Debug Print

There is no `Zanna.Debug` namespace. For quick debugging, route messages through `Zanna.Diagnostics.Log` (see
above) or directly to the terminal:

```zia
// Goes through the leveled logger
Zanna.Diagnostics.Log.Debug("value=" + Zanna.Core.Convert.ToStringInt(value));
// Prints to stdout
Zanna.Terminal.Print("debug: " + msg);
```

`Zanna.Diagnostics.Log.*` writes to stderr (or the configured log sink) and is suppressed when the active level
is above the call's level.

---

## 8. Pipeline Dump Flags

The compiler supports dump flags for inspecting intermediate results at every stage of the pipeline. All dumps go to **stderr** so they don't interfere with program output or `-emit-il`. These work with `zanna run` and the legacy `zanna front zia` / `zanna front basic` commands.

### Token Stream

```sh
zanna run --dump-tokens program.zia
zanna front basic -run program.bas --dump-tokens
```

Prints every token produced by the lexer with location, kind, text, and literal values:

```text
=== Zia Token Stream ===
1:1     module  "module"
1:8     identifier      "Test"
2:1     func    "func"
2:6     identifier      "start"
3:27    integer "42"    value=42
5:1     eof
=== End Token Stream ===
```

### AST Dump

```sh
zanna run --dump-ast program.zia
```

Prints the parsed AST (abstract syntax tree) as an indented tree. For Zia, this includes source locations, node kinds, operators, and literal values:

```text
=== AST after parsing ===
ModuleDecl "Test" (1:1)
  FunctionDecl "start" (2:1)
    Visibility: private
    Body:
      BlockStmt (2:14)
        ExprStmt (3:5)
          CallExpr (3:26)
            Callee:
              FieldExpr "SayInt" (3:19)
                FieldExpr "Terminal" (3:10)
                  IdentExpr "Zanna" (3:5)
            Args:
              Arg:
                IntLiteral 42 (3:27)
=== End AST ===
```

For BASIC, the AST uses the existing `AstPrinter` format.

### AST Dump After Semantic Analysis (Zia Only)

```sh
zanna run --dump-sema-ast program.zia
```

Prints the AST after semantic analysis has run. This is useful for seeing what sema has annotated or transformed — comparing `--dump-ast` with `--dump-sema-ast` shows what the semantic pass changed.

### IL After Lowering

```sh
zanna run --dump-il program.zia
```

Prints the IL module immediately after lowering from the AST, before any optimization:

```text
=== IL after lowering ===
il 0.3.0
extern @Zanna.Terminal.SayInt(i64) -> void
func @main() -> void {
entry_0:
  .loc 1 3 26
  call @Zanna.Terminal.SayInt(42)
  .loc 1 2 1
  ret
}
=== End IL ===
```

### IL Per-Pass Dump

```sh
zanna run -O1 --dump-il-passes program.zia
```

Prints the full IL module before and after each optimization pass. Requires `-O1` or `-O2` (at `-O0`, the only passes are SimplifyCFG and DCE). Uses the PassManager's built-in instrumentation hooks:

```text
*** IR before pass 'simplify-cfg' ***
...
*** IR after pass 'simplify-cfg' ***
...
*** IR before pass 'mem2reg' ***
...
```

### IL After Optimization

```sh
zanna run -O1 --dump-il-opt program.zia
```

Prints the IL module after the entire optimization pipeline has completed:

```text
=== IL after optimization (O1) ===
...
=== End IL ===
```

### Combining Flags

All dump flags can be combined freely. They print in pipeline order:

```sh
zanna run --dump-tokens --dump-ast --dump-il --dump-il-opt program.zia
```

This prints the token stream, then the AST, then the pre-optimization IL, then the post-optimization IL.

### Programmatic API

The same flags are available in `CompilerOptions` (Zia) and `BasicCompilerOptions` (BASIC):

| Option | CLI Flag | Description |
|--------|----------|-------------|
| `dumpTokens` | `--dump-tokens` | Dump lexer token stream |
| `dumpAst` | `--dump-ast` | Dump AST after parsing |
| `dumpSemaAst` | `--dump-sema-ast` | Dump AST after sema (Zia only) |
| `dumpIL` | `--dump-il` | Dump IL after lowering |
| `dumpILOpt` | `--dump-il-opt` | Dump IL after optimization |
| `dumpILPasses` | `--dump-il-passes` | Dump IL before/after each pass |

### Safety Checks

All enabled by default. Can be toggled via `CompilerOptions`:

| Option | Default | Description |
|--------|---------|-------------|
| `boundsChecks` | true | Emit array bounds checking code |
| `overflowChecks` | true | Emit arithmetic overflow checks |
| `nullChecks` | true | Emit null pointer checks |

When enabled, these generate IL instructions (`IdxChk`, `SDivChk0`, etc.) that trap on violations rather than producing undefined behavior.

### Optimization Levels

| Level | Description |
|-------|-------------|
| O0 | Minimal optimization (SimplifyCFG + DCE only) |
| O1 | Standard optimizations |
| O2 | Aggressive optimizations |

Debug and trace modes automatically disable optimization to preserve the instruction-to-source mapping.

### Compiler Phase Timing

The `debugTime()` helper in the Zia compiler prints elapsed time for each compilation phase:
- Lexing
- Parsing
- Import resolution
- Semantic analysis
- IL lowering

---

## 9. IL Inspection

### Serialization

IL modules can be serialized in two modes:

- **Pretty mode** — Human-readable with indentation and comments
- **Canonical mode** — Deterministic output suitable for golden tests

Use `zia --emit-il`, `zbasic --emit-il`, or the legacy `zanna front ... -emit-il`
commands to output the final IL module to stdout. Use `--dump-il` /
`--dump-il-opt` to print specific pipeline stages to stderr:

```sh
zanna front zia -emit-il program.zia          # Final IL to stdout
zanna front zia -run program.zia --dump-il    # IL after lowering to stderr
```

### Verification

The IL verifier (`Verifier::verify()`) checks:
- Type consistency across instructions
- Control flow graph validity (block connectivity, terminator presence)
- Exception handling structure (`eh.push`/`eh.pop` balancing per CFG path, handler block parameter shape)
- External function declaration correctness
- Global variable definitions

Verification runs automatically during compilation. Invalid IL is reported as diagnostics. `Verifier::verify()` fails only on error-severity diagnostics and attaches additional verifier findings as notes; `Verifier::verifyAll()` is available for tooling that needs the full bounded diagnostic list, including verifier warnings. Independent function-body failures are collected in one run instead of stopping after the first broken function.

---

## 10. VM Debug Hooks

### DebugCtrl

Every VM instance has a `DebugCtrl` member that provides:
- Breakpoint management (block-label and source-line)
- Variable watch tracking
- Memory watch monitoring

Fast-path flags (`fastDebugBreak_`, `fastDebugMemWatch_`) ensure **zero overhead** when no debug features are active.

### TraceSink Callbacks

The trace system fires callbacks during execution:
- `onFramePrepared()` — New function frame created
- `onStep()` — Instruction about to execute
- `onTailCall()` — Tail call optimization activated

### Host Polling

For interactive applications, configure periodic callbacks:

```cpp
RunConfig config;
config.interruptEveryN = 1000;  // Check every 1000 instructions
config.pollCallback = [&]() -> bool {
    return shouldContinue;  // Return false to pause
};
```

---

## 11. Exception Handling

### IL Exception Model

Zanna IL uses structured exception handling with `eh.push` / `eh.pop` opcodes that bracket the
protected region, plus an `eh.entry` marker inside the handler block:

```il
eh.push ^handler         ; install handler on the EH stack
  ; protected code
eh.pop                   ; uninstall on normal exit

handler(%err:Error, %tok:ResumeTok):
  eh.entry               ; handler entry marker
  ; recovery code
  resume.label %tok, ^after_try
```

Handler blocks declare two parameters: `%err: Error` (the active error) and
`%tok: ResumeTok` (the resume token used by `resume.same` / `resume.next` / `resume.label`).

### Resume Variants

- `ResumeSame` — Re-raise the same exception
- `ResumeNext` — Continue after the protected region
- `ResumeLabel` — Jump to a specific block

### VM Exception Handling

When a trap occurs:
1. The VM searches the current frame's exception handler stack
2. If a handler is found, control transfers to it
3. If no handler exists, the trap propagates up the call stack
4. Unhandled traps call `rt_abort()` with diagnostic output

---

## 12. Benchmarking

### `zanna bench` Command

```sh
zanna bench program.il -n 5 --all
```

Runs the program multiple times across different VM dispatch strategies and reports timing:

```text
BENCH "program.il" table instr=50000 time_ms=12.04 insns_per_sec=4152824
```

### Dispatch Strategies

| Flag | Strategy |
|------|----------|
| `--table` | Function table dispatch (standard VM) |
| `--switch` | Switch dispatch (standard VM) |
| `--threaded` | Threaded dispatch (standard VM) |
| `--bc-switch` | Bytecode VM, switch dispatch |
| `--bc-threaded` | Bytecode VM, threaded dispatch |
| `--all` | All strategies (default) |

### JSON Output

```sh
zanna bench program.il --json
```

### Opcode Counting

When compiled with `ZANNA_VM_OPCOUNTS=1` (default in Debug builds):

```cpp
Runner runner(module);
runner.run();
auto counts = runner.opcodeCounts();      // Per-opcode array
auto top = runner.topOpcodes(10);         // Top 10 by frequency
runner.resetOpcodeCounts();               // Reset counters
```

---

## 13. Execution Statistics

### Instruction Count

```sh
zanna -run program.il --count
```

Output:
```text
[SUMMARY] instr=142857
```

### Execution Time

```sh
zanna -run program.il --time
```

Output:
```text
[SUMMARY] time_ms=42.183012
```

Both flags can be combined:

```sh
zanna -run program.il --count --time
```

---

## 14. Programmatic Debugging API

### Complete Example

```cpp
#include "zanna/vm/VM.hpp"

// Load module
auto module = loadModule("program.il");

// Configure runner
Runner runner(module);

// Set breakpoint
runner.setBreakpoint({file_id, 42, 0});

// Run to breakpoint
auto status = runner.continueRun();
if (status == RunStatus::BreakpointHit) {
    // Inspect state
    auto trap = runner.lastTrap();

    // Single-step
    auto step = runner.step();

    // Continue
    status = runner.continueRun();
}

// Check for errors
if (auto msg = runner.lastTrapMessage()) {
    std::cerr << *msg << "\n";
}

// Statistics
std::cout << "Instructions: " << runner.instructionCount() << "\n";
```

### Key Types

| Type | Description |
|------|-------------|
| `Runner` | Main VM execution controller |
| `Runner::StepResult` | Result of `step()` — status enum |
| `Runner::RunStatus` | Result of `continueRun()` — terminal state |
| `Runner::TrapInfo` | Trap details: kind, code, ip, line, function, block, message |
| `DebugCtrl` | Low-level breakpoint/watch controller |
| `TraceSink` | Trace output handler with file caching |
| `TraceConfig` | Trace mode and source manager configuration |
| `DebugScript` | File-based debug automation |

---

## 15. Debug Adapter

`zanna run program.zia --debug-adapter` starts the VM-backed debug adapter on
standard input and output. It exchanges newline-delimited JSON commands for
breakpoints, continue/step control, call stacks, locals, watches, and
evaluation; Zanna Studio uses this path for its integrated debugger. Adapter
protocol output owns stdout, so program stdout/stderr and logpoint records are
carried as debug-session output events rather than mixed into the command
stream.

A `terminated` event means execution has reached a terminal state, not that
the client should immediately close the process pipes. Zanna Studio continues
draining until the child has exited and its pending program, error, and
logpoint output has been delivered, then releases the session handles. This
preserves final output written immediately before exit.

## 16. Known Limitations

| Area | Status | Notes |
|------|--------|-------|
| Source-line stepping | Not implemented | `step()` advances 1 IL instruction, not 1 source line |
| Step-over / step-out | Implemented | Frame-depth based; still instruction-level rather than source-line stepping |
| Full backtrace API | Not implemented | `execStack` is private; only single-frame `TrapInfo` exposed |
| Conditional breakpoints | Not implemented | No expression evaluation on break condition |
| Source-to-IL name mapping | Partial | Zia local slots carry source names; generated temporaries and some BASIC paths still require IL names |
| DWARF debug info | Partial | `--debug-lines` preserves linked DWARF v5 sections; codegen-generated line tables are limited |
| Debug Adapter Protocol | Implemented for Zanna Studio | VM-backed newline-delimited JSON through `zanna run --debug-adapter`; it is not advertised as a general VS Code DAP transport |
| Signal/crash handler | Not implemented | Native crashes produce no diagnostic output |
| In-VM profiling | Not implemented | No function-level timing or allocation tracking |
| Subsystem log filtering | Not implemented | VM trace and Zanna.Diagnostics.Log are all-or-nothing |
