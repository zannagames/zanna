---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna Bytecode VM Reference

The bytecode VM is the implemented stack interpreter in `src/bytecode/`. It is
produced from IL by `BytecodeCompiler` and executed by `BytecodeVM`; it is not a
separate language contract. The normative IL contract remains
[`docs/il/il-guide.md#reference`](../il/il-guide.md#reference).

## Source Of Truth

| Area | Source |
|------|--------|
| Opcode names, values, and category comments | `src/bytecode/Bytecode.def` |
| Opcode enum, encoding helpers, limits, `BCSlot` | `src/bytecode/Bytecode.hpp` |
| Module, function, native, global, debug metadata | `src/bytecode/BytecodeModule.hpp` |
| IL-to-bytecode lowering | `src/bytecode/BytecodeCompiler.hpp`, `src/bytecode/BytecodeCompiler.cpp` |
| Switch interpreter, frames, EH, runtime bridge, globals | `src/bytecode/BytecodeVM.hpp`, `src/bytecode/BytecodeVM.cpp` |
| Computed-goto interpreter | `src/bytecode/BytecodeVM_threaded.cpp` |
| Scalar width and trap adapters | `src/bytecode/BytecodeSemantics.hpp` |
| CLI execution helper for source frontends | `src/tools/common/vm_executor.cpp` |
| IL runner bytecode flags | `src/tools/zanna/cmd_run_il.cpp` |

`src/bytecode/Bytecode.def` is the only hand-maintained bytecode opcode list.
It drives the `BCOpcode` enum, `opcodeName()`, and the threaded-dispatch label
table. Do not copy the full opcode list into docs; use the source file or
`zanna --dump-opcodes` for a live registry.

As of this verification, `src/il/core/Opcode.def` contains 83 IL opcode entries
and `src/bytecode/Bytecode.def` contains 134 bytecode entries, including the
`OPCODE_COUNT` sentinel.

## Execution Model

`BytecodeCompiler::compileChecked()` lowers an IL module into a `BytecodeModule`.
The compiler performs SSA-to-local mapping, block linearization, constant pool
construction, bytecode emission, branch fixup, and derived metadata rebuild for
exception ranges and switch tables.

`BytecodeVM` executes a loaded `BytecodeModule` with a stack-based model:

- parameters occupy the first local slots
- temporaries and block parameters are mapped to additional local slots
- operands live on a per-frame operand stack
- stack allocations use a separate alloca buffer
- globals are materialized as one `BCSlot` of VM-owned storage per bytecode global

The VM has two dispatch engines:

- `BytecodeVM::run()` is the portable switch interpreter.
- `BytecodeVM::runThreaded()` uses GCC/Clang labels-as-values computed goto.

Threaded dispatch is the default in `BytecodeVM` construction. Tests and tools
can select switch dispatch by calling `setThreadedDispatch(false)`.

## Instruction Format

Most bytecode instructions are one 32-bit word:

```text
[ opcode:8 ][ arg0:8 ][ arg1:8 ][ arg2:8 ]
```

Encoding and decoding helpers in `Bytecode.hpp` cover unsigned and signed
8-bit, 16-bit, and 24-bit operands. Some instructions consume extra raw
32-bit words after the opcode word. The VM and compiler explicitly validate
those words before reading them. Current examples include:

- `LOAD_I32`, followed by an inline 32-bit integer value
- `EH_PUSH`, followed by a raw relative handler offset word
- `RESUME_LABEL`, followed by a raw relative target offset word
- `SWITCH`, followed by a raw switch table header and case data

The bytecode module header uses:

- magic: `kBytecodeModuleMagic == 0x01434256`
- version: `kBytecodeVersion == 3`

VM limits from `Bytecode.hpp`:

- `kMaxCallDepth == 4096`
- `kMaxStackSize == 1024` `BCSlot` entries per call frame

## Opcode Categories

The opcode category ranges are defined in `Bytecode.hpp` and populated in
`Bytecode.def`:

| Range | Category |
|-------|----------|
| `0x00-0x0F` | Stack operations |
| `0x10-0x1F` | Local variable operations |
| `0x20-0x2F` | Constant loading |
| `0x30-0x4F` | Integer arithmetic |
| `0x50-0x5F` | Float arithmetic |
| `0x60-0x6F` | Bitwise operations |
| `0x70-0x7F` | Integer comparisons |
| `0x80-0x8F` | Float comparisons |
| `0x90-0x9F` | Type conversions |
| `0xA0-0xAF` | Memory operations |
| `0xB0-0xBF` | Control flow |
| `0xC0-0xCF` | Exception handling |
| `0xD0-0xD7` | Debug operations |
| `0xD8-0xDF` | Runtime fast-path operations |
| `0xE0-0xEF` | String operations |

`TAIL_CALL`, `MAKE_ERROR`, and `OPCODE_COUNT` are declared with
`BC_OPCODE_TRAP` in `Bytecode.def`. They remain enum values and disassemble by
name, but the threaded interpreter leaves them routed to the default trap path,
and module validation rejects them as executable bytecode.

`Bytecode.hpp` also defines `isTerminator()` and `canTrap()`. `canTrap()` covers
checked arithmetic, checked conversions, alloca, bytecode/native/indirect calls,
and explicit trap opcodes.

## Value Representation

`BCSlot` is the VM value cell:

```cpp
union BCSlot {
    int64_t i64;
    double f64;
    void *ptr;
};
static_assert(sizeof(BCSlot) == 8, "BCSlot must be 8 bytes");
```

There is no runtime type tag in `BCSlot`. The bytecode compiler and opcode
semantics determine the active representation. Runtime strings are stored as
`void *` pointers to `rt_string` objects, and string ownership is tracked in
parallel bitmaps rather than in `BCSlot` itself.

## Module Metadata

`BytecodeModule` owns:

- deduplicated constant pools for `i64`, `f64`, and strings
- bytecode functions plus a name-to-index map
- native runtime function references plus a signature-shaped index
- bytecode global descriptors plus a name-to-index map
- source file metadata used by debug and trap diagnostics

`BytecodeFunction` stores:

- function name, parameter count, local count, max stack, alloca size, return flag
- `code`, the vector of 32-bit bytecode words
- `localIsString`, used for local string ownership handling
- derived exception ranges and switch tables
- optional local variable metadata
- line, source-file, and basic-block tables indexed by bytecode PC

`NativeFuncRef` stores the native name, encoded arity, return expectation,
cached runtime descriptor/signature pointers, and string ownership flags for
runtime calls that consume or return managed strings.

`GlobalInfo` stores global name, byte size, alignment, IL type, scalar initial
data, and string initializer payload.

## Loading And Validation

`BytecodeVM::load()` validates the module before binding it. Validation checks
the header, function tables, code stream, constant/global/function/native
indices, inline operand words, width metadata, branch targets, switch tables,
and executable-opcode restrictions.

When load succeeds, the VM:

- enables trusted dispatch only if it was requested and validation succeeded
- initializes global storage
- materializes string globals as owned runtime strings
- initializes the string literal cache

When load fails, the VM records a trap and does not bind the module.

During dispatch, untrusted execution checks PC range and stack shape before each
instruction. Trusted dispatch skips those hot-path guards only after a checked
compile and successful module load.

## Traps And Exceptions

`TrapKind` values `0-11` intentionally match `il::vm::TrapKind`:

| Value | Trap |
|-------|------|
| `0` | `DivideByZero` |
| `1` | `Overflow` |
| `2` | `InvalidCast` |
| `3` | `DomainError` |
| `4` | `Bounds` |
| `5` | `FileNotFound` |
| `6` | `EndOfFile` (the `Trap.hpp` enumerator is spelled `EOF`) |
| `7` | `IOError` |
| `8` | `InvalidOperation` |
| `9` | `RuntimeError` |
| `10` | `Interrupt` |
| `11` | `NetworkError` |

Bytecode-specific trap values start at `100`:

- `NullPointer == 100`
- `StackOverflow == 101`
- `InvalidOpcode == 102`

`None == 255` is the no-trap sentinel.

Exception handlers are pushed with `EH_PUSH` and popped with `EH_POP`.
`dispatchTrap()` searches the handler stack, snapshots resumable trap state in a
`TrapRecord`, unwinds to the handler frame, restores the handler stack pointer,
and transfers execution to the handler PC. `ERR_GET_*` opcodes read the current
trap record and message. `TRAP_KIND` maps bytecode-specific trap kinds to
`RuntimeError` for catch matching.

`RESUME_SAME`, `RESUME_NEXT`, and `RESUME_LABEL` validate the current resume
token. `RESUME_LABEL` consumes the following raw relative target word.

## Runtime Integration

`CALL_NATIVE` encodes an 8-bit argument count and a 16-bit native index. The VM
checks that the encoded argument count matches the `NativeFuncRef` before
calling.

Native calls can run through either:

- the local `registerNativeHandler()` map, when runtime bridge integration is
  disabled
- `il::vm::RuntimeBridge`, when `setRuntimeBridgeEnabled(true)` is used

The standalone `BytecodeVM` constructor leaves the runtime bridge disabled.
CLI execution paths that run source frontends through
`src/tools/common/vm_executor.cpp` enable the runtime bridge and threaded
dispatch before running `main`.

The bytecode VM also installs unified runtime handlers so bytecode callbacks can
participate in `Thread.Start`, `Thread.StartOwned`, `Thread.StartSafe`,
`Async.Run`, HTTP callbacks, game callbacks, and parallel runtime helpers.
Worker bytecode VMs receive an `ExecutionEnvironment` snapshot containing
runtime bridge state, dispatch mode, trusted-dispatch preference, step budget,
and direct native handlers.

## Memory And Strings

The VM validates null or clearly invalid low-page pointers before memory
accesses. For VM-owned regions, accesses overlapping global slots or the alloca
arena must stay inside the corresponding live range.

String values are runtime string handles stored in `BCSlot::ptr`. Ownership is
tracked separately for value stack slots, globals, string literals, and trap
records. Stack operations such as `DUP`, `DUP2`, `POP`, `SWAP`, and `ROT3` use
ownership-aware helpers so retained strings are not leaked or released twice.

`localIsString` classifies string local slots. Frame cleanup releases string
locals, owned stack values, owned globals, cached string literals, and retained
trap-record values through the runtime string API.

## Debug Support

`BytecodeVM` supports PC breakpoints, single stepping, and a debug callback:

- `setBreakpoint(functionName, pc)` stores a per-function PC breakpoint
- `setSingleStep(true)` requests a debug callback before each instruction
- `setDebugCallback()` receives the VM, current function, PC, and breakpoint flag

`LINE` and `WATCH_VAR` are currently no-op dispatch instructions in the switch
interpreter. Source line, source file, and block label lookup use the tables
stored on `BytecodeFunction`.

For existing IL files, `zanna run <file.il> --bytecode` and
`zanna run <file.il> --bc-threaded` reject debugger flags such as `--break`,
`--break-src`, `--watch`, `--debug-cmds`, `--step`, and `--continue`.

## CLI And Benchmark Surface

Source frontend execution goes through `executeBytecodeVM()` in
`src/tools/common/vm_executor.cpp`. That helper compiles IL with
`compileChecked()`, configures the bytecode VM with threaded dispatch, trusted
dispatch, runtime bridge support, runtime arguments, and the shared step budget,
then runs `main`.

Existing IL files can opt into bytecode execution:

```sh
zanna run program.il --bytecode
zanna run program.il --bc-threaded
```

The benchmark command can compare dispatch strategies:

```sh
zanna bench program.il --bc-switch
zanna bench program.il --bc-threaded
zanna bench program.il --bytecode
```

## Tests

The checked-in bytecode test targets are registered in `src/tests/CMakeLists.txt`,
`src/tests/unit/CMakeLists.txt`, and `src/tests/bytecode/CMakeLists.txt`.

| Test | Coverage |
|------|----------|
| `test_bytecode_vm` | Direct compiler and VM behavior, including arithmetic, control flow, calls, runtime bridge, strings, globals, async/thread interactions, and VM state handling |
| `test_bytecode_compiler_diagnostics` | Checked compile diagnostics, malformed IL rejection, debug metadata emission, trap lowering, imports, native arity, and metadata validation cases |
| `test_vm_bytecode_equivalence` | Regular VM and bytecode VM return-value equivalence for constructed IL modules |
| `test_bytecode_full_program_parity` | Shared IL corpus parity across the IL VM, bytecode switch dispatch, and bytecode threaded dispatch |

The full-program parity test compares return value, captured runtime stdout,
trap state, and shared trap kind. It also contains static assertions that keep
the bytecode trap values `0-11` aligned with the tree-walking VM trap enum.

Run bytecode-focused tests with:

```sh
ctest --test-dir build -L bytecode --output-on-failure
```

Run individual bytecode tests with:

```sh
ctest --test-dir build -R test_bytecode_vm --output-on-failure
ctest --test-dir build -R test_bytecode_compiler_diagnostics --output-on-failure
ctest --test-dir build -R test_vm_bytecode_equivalence --output-on-failure
ctest --test-dir build -R test_bytecode_full_program_parity --output-on-failure
```
