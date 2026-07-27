---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Virtual Machine

The Virtual Machine (`src/vm/`) interprets Zanna IL with configurable dispatch strategies.
For the separate bytecode interpreter in `src/bytecode/`, see
[CODEMAP: Bytecode VM](bytecode-vm.md).

## Overview

- **Total source files**: 61 (.hpp/.cpp)
- **Subdirectories**: debug/, ops/, ops/common/, ops/generated/, ops/schema/

## Core Infrastructure

| File              | Purpose                                                                  |
|-------------------|--------------------------------------------------------------------------|
| `Runner.cpp`      | Simple facade for running modules with config                            |
| `VM.cpp`          | Main interpreter: frame management, instruction dispatch, execution loop |
| `VM.hpp`          | VM class declarations                                                    |
| `VMConfig.hpp`    | Configuration flags and constants (dispatch mode, limits)                |
| `VMConstants.hpp` | VM-wide constants and limits                                             |
| `VMContext.cpp`   | Execution context: function/block maps, global string tables             |
| `VMContext.hpp`   | VMContext declarations                                                   |
| `VMInit.cpp`      | VM construction, function table setup, per-execution initialization      |
| `FunctionExecCache.cpp` | Pre-resolved operand arrays per (function, block) for fast dispatch |

## Dispatch Strategies

| File                  | Purpose                                               |
|-----------------------|-------------------------------------------------------|
| `DispatchMacros.hpp`  | Dispatch macro definitions for handler generation     |
| `DispatchStrategy.cpp`| Dispatch strategy selection (switch, table, threaded) |
| `DispatchStrategy.hpp`| Dispatch strategy declarations                        |

## Opcode Handler Infrastructure

| File                      | Purpose                                          |
|---------------------------|--------------------------------------------------|
| `OpcodeHandlerHelpers.hpp`| Common handler helper declarations               |
| `OpHandlerAccess.hpp`     | Restricted access layer for slot read/write      |
| `OpHandlers.cpp`          | Opcode handler table construction and accessor   |
| `OpHandlers.hpp`          | Opcode handler declarations                      |
| `OpHandlerUtils.cpp`      | Shared helpers: `storeResult`, slot manipulation |
| `OpHandlerUtils.hpp`      | Handler utility declarations                     |
| `OpHelpers.cpp`           | Frequently used helper routines                  |

## Opcode Handler Headers by Domain

| File                    | Purpose                           |
|-------------------------|-----------------------------------|
| `OpHandlers_Control.hpp`| Control flow handler declarations |
| `OpHandlers_Float.hpp`  | Floating-point handler declarations|
| `OpHandlers_Int.hpp`    | Integer handler declarations      |
| `OpHandlers_Memory.hpp` | Memory handler declarations       |

## Opcode Handler Implementations

| File                 | Purpose                                     |
|----------------------|---------------------------------------------|
| `control_flow.cpp`   | Branch, call, return, trap handlers         |
| `control_flow.hpp`   | Control flow handler declarations           |
| `fp_ops.cpp`         | Floating-point arithmetic and comparisons   |
| `int_ops_arith.cpp`  | Integer arithmetic and bitwise operations   |
| `int_ops_cmp.cpp`    | Integer comparisons                         |
| `int_ops_convert.cpp`| Type conversions and casts                  |
| `IntOpSupport.hpp`   | Overflow/divide-by-zero helpers             |
| `mem_ops.cpp`        | Memory operations: alloca, load, store, gep |

## Specialized Operations (`ops/`)

| File                   | Purpose                    |
|------------------------|----------------------------|
| `Op_BranchSwitch.cpp`  | Switch instruction handler |
| `Op_CallRet.cpp`       | Call and return handlers   |
| `Op_TrapEh.cpp`        | Trap and exception handling|

## Common Operations (`ops/common/`)

| File           | Purpose                    |
|----------------|----------------------------|
| `Branching.cpp`| Shared branching utilities |
| `Branching.hpp`| Branching declarations     |

## Generated Files (`ops/generated/`)

| File                      | Purpose                               |
|---------------------------|---------------------------------------|
| `HandlerTable.hpp`        | Generated dispatch table              |
| `InlineHandlersDecl.inc`  | Generated inline handler declarations |
| `InlineHandlersImpl.inc`  | Generated inline handler bodies       |
| `OpSchema.hpp`            | Generated opcode schema               |
| `SwitchDispatchDecl.inc`  | Generated switch dispatch declarations|
| `SwitchDispatchImpl.inc`  | Generated switch dispatch bodies      |
| `ThreadedCases.inc`       | Generated threaded dispatch cases     |
| `ThreadedLabels.inc`      | Generated threaded dispatch labels    |

## Schema Definition (`ops/schema/`)

| File       | Purpose                                     |
|------------|---------------------------------------------|
| `ops.yaml` | Opcode schema source driving code generation|

## Runtime Bridge

| File                | Purpose                                    |
|---------------------|--------------------------------------------|
| `err_bridge.cpp`    | Runtime error to VM diagnostic adapter     |
| `err_bridge.hpp`    | Error bridge declarations                  |
| `Marshal.cpp`       | Slot to C ABI type marshalling             |
| `Marshal.hpp`       | Marshal declarations                       |
| `RuntimeBridge.cpp` | C runtime invocation with trap diagnostics |
| `RuntimeBridge.hpp` | Runtime bridge declarations                |

## Trap Handling

| File                | Purpose                           |
|---------------------|-----------------------------------|
| `Trap.cpp`          | Trap kinds and reporting          |
| `Trap.hpp`          | Trap declarations                 |
| `TrapInvariants.hpp`| Trap invariant checking utilities |

## Tail Call Optimization

| File     | Purpose                        |
|----------|--------------------------------|
| `tco.cpp`| Tail call optimization support |
| `tco.hpp`| TCO declarations               |

## Diagnostics

| File            | Purpose                       |
|-----------------|-------------------------------|
| `DiagFormat.cpp`| Diagnostic message formatting |
| `DiagFormat.hpp`| DiagFormat declarations       |

## String Handling

| File                  | Purpose                                |
|-----------------------|----------------------------------------|
| `ZannaStringHandle.hpp`| String handle type for runtime interop|

## Threading

| File               | Purpose                                       |
|--------------------|-----------------------------------------------|
| `ThreadsRuntime.cpp`| Runtime threading support and synchronization|

## Networking

| File                | Purpose                                                       |
|---------------------|---------------------------------------------------------------|
| `NetworkRuntime.cpp`| VM-aware helpers for `Zanna.Network.HttpServer` handler binding |

## Debug Infrastructure (`debug/`)

| File               | Purpose                         |
|--------------------|---------------------------------|
| `Debug.cpp`        | Breakpoint and watch management |
| `DebugScript.cpp`  | Scripted debugger actions       |
| `Trace.cpp`        | Execution tracing and logging   |
| `VM_DebugUtils.cpp`| Debug helper utilities          |
| `VMDebug.cpp`      | Debug hook implementation       |
