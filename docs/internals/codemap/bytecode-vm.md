---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Bytecode VM

The bytecode VM (`src/bytecode/`) compiles IL modules into compact bytecode and
executes them with a stack-based interpreter. This is separate from the legacy
IL VM in `src/vm/`.

## Current Files

| File | Purpose |
|------|---------|
| `Bytecode.def` | X-macro opcode table: single source of truth for the bytecode opcode set |
| `Bytecode.hpp` / `Bytecode.cpp` | Opcode enum, slot type, constants, opcode names |
| `BytecodeModule.hpp` | Bytecode module/function containers |
| `BytecodeCompiler.hpp` / `BytecodeCompiler.cpp` | IL to bytecode lowering, SSA local mapping, block linearization |
| `BytecodeVM.hpp` / `BytecodeVM.cpp` | Interpreter state, dispatch loop, frames, traps, EH, runtime calls, debug hooks |
| `BytecodeVM_threaded.cpp` | Computed-goto threaded dispatch implementation |

## Review Risk

`BytecodeVM.cpp` is intentionally treated as a high-risk monolith until it is
split. It currently owns multiple concerns that should be independently
testable:

| Concern | Current Owner | Target Owner |
|---------|---------------|--------------|
| Dispatch loop and opcode execution | `BytecodeVM.cpp`, `BytecodeVM_threaded.cpp` | `BytecodeVMDispatch.cpp` |
| Stack slots, value stack, call frames | `BytecodeVM.cpp` | `BytecodeVMStack.cpp` |
| Trap records, EH stack, catch/resume logic | `BytecodeVM.cpp` | `BytecodeVMEH.cpp` |
| Runtime function calls and ABI marshalling | `BytecodeVM.cpp` | `BytecodeVMRuntime.cpp` |
| Global/module state and string ownership | `BytecodeVM.cpp` | `BytecodeVMState.cpp` |
| Debug hooks, breakpoints, stepping | `BytecodeVM.cpp` | `BytecodeVMDebug.cpp` |
| Validation and defensive bounds checks | `BytecodeVM.cpp` | `BytecodeVMValidate.cpp` |

## Invariants

- Bytecode VM output must match the IL VM for defined programs.
- Runtime string handles passed through bytecode must remain valid until the
  runtime side releases them.
- Traps must preserve fault PC, next PC, line number, live value stack, call
  frames, EH stack, and owned alloca storage.
- A trap that is caught in a caller must not resume into a corrupted callee
  frame.
- Worker threads run with separate VM instances and copied execution
  environment, not shared mutable VM stacks.
- Native runtime calls must use the same runtime registry signatures as codegen
  and the IL VM.

## Refactor Criteria

Split `BytecodeVM.cpp` only along behaviorally stable seams. Each extracted
translation unit should land with focused regression tests and should keep
`BytecodeVM.hpp` as the single public state boundary until the split is complete.

Before any extraction is merged, run:

```bash
ctest --test-dir build --output-on-failure -L vm
ctest --test-dir build --output-on-failure -R 'bytecode|runtime_bridge|trycatch|trap'
```
