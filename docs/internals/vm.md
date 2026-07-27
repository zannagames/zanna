---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna VM — Architecture & Implementation Guide

Comprehensive guide to the Zanna Virtual Machine (VM), which executes Zanna IL
programs. This document covers the VM's design philosophy, architecture, execution model, and source code organization.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture & Design Philosophy](#architecture--design-philosophy)
3. [Key Components](#key-components)
4. [Execution Model](#execution-model)
5. [Dispatch Strategies](#dispatch-strategies)
6. [Memory Model](#memory-model)
7. [Exception & Trap Handling](#exception--trap-handling)
8. [Runtime Integration](#runtime-integration)
9. [Debug & Tracing](#debug--tracing)
10. [Source Code Guide](#source-code-guide)
11. [Performance Features](#performance-features)
12. [Best Practices](#best-practices)
13. [Further Reading](#further-reading)

**Appendices**
- [Performance Tuning](#appendix-performance-tuning)
- [Concurrency Model](#appendix-concurrency-model)
- [Runtime ABI Reference](#appendix-runtime-abi-reference)

---

## Overview

### What is the Zanna VM?

The Zanna VM is the primary execution engine for programs written in Zanna's Intermediate Language (
IL). It serves as the primary execution engine for the Zanna toolchain, providing:

- **Deterministic execution** of IL programs
- **Debugging and tracing** capabilities
- **Exception handling** with structured error recovery
- **Runtime function calls** via the RuntimeBridge
- **Multiple dispatch strategies** optimized for different use cases

### Key Characteristics

| Feature            | Description                                                                    |
|--------------------|--------------------------------------------------------------------------------|
| **Architecture**   | Stack-based interpreter with SSA register file                                 |
| **Dispatch**       | Pluggable (function table, switch, computed goto)                              |
| **Memory**         | Frame-local operand stack with explicit allocation via `alloca` (64KB default) |
| **Error Handling** | Structured exception handling with trap metadata                               |
| **Debugging**      | Built-in breakpoints, stepping, and tracing                                    |
| **Performance**    | Tail-call optimization, opcode counting, inline caching                        |

---

## Architecture & Design Philosophy

### Core Principles

The VM design prioritizes several key principles:

1. **Modularity**: Pluggable dispatch strategies allow optimization without changing the core interpreter
2. **Inspectability**: Comprehensive tracing and debugging support for all execution paths
3. **Correctness**: Deterministic execution with explicit error handling
4. **Performance**: Multiple optimization layers (TCO, inline caching, threaded dispatch)
5. **Simplicity**: Clean separation between interpretation, runtime, and tooling

### High-Level Architecture

```text
┌─────────────────────────────────────────────────────────┐
│                      VM (Interpreter)                    │
├─────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │   Dispatch   │  │   Opcode     │  │    Debug     │ │
│  │   Strategy   │──│   Handlers   │──│   Control    │ │
│  └──────────────┘  └──────────────┘  └──────────────┘ │
│                           │                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │    Frame     │  │    Trap      │  │    Trace     │ │
│  │   Manager    │  │   Handler    │  │     Sink     │ │
│  └──────────────┘  └──────────────┘  └──────────────┘ │
└─────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  IL Module   │    │   Runtime    │    │   External   │
│  (readonly)  │    │    Bridge    │    │   Callbacks  │
└──────────────┘    └──────────────┘    └──────────────┘
```

### Component Relationships

- **VM** owns: Dispatch driver, trace sink, debug controller, function/string maps
- **VM** borrows: IL Module (must outlive VM), optional debug script
- **Frame** owns: Register file, operand stack, exception handlers
- **RuntimeBridge** provides: C runtime function invocation, trap reporting

---

## Key Components

### 1. VM Class (`src/vm/VM.hpp`)

The main interpreter class that orchestrates execution:

```cpp
class VM {
    // Module and configuration
    const il::core::Module& mod;                    // IL module (non-owning)
    std::shared_ptr<ProgramState> programState_;    // Shared globals/context
    TraceSink tracer;                               // Trace output
    DebugCtrl debug;                                // Breakpoint controller
    std::unique_ptr<DispatchDriver> dispatchDriver; // Pluggable dispatch
    DispatchKind dispatchKind;                      // Active strategy type

    // Execution state
    uint64_t instrCount;                // Executed instructions
    uint64_t maxSteps;                  // Step limit (0 = unlimited)
    std::size_t stackBytes_;            // Per-frame stack size (default 64KB)
    std::vector<ExecState*> execStack;  // Active execution stack for unwinding

    // Caching and lookup (string_view keys for zero-copy)
    FnMap fnMap;                                    // Function lookup table
    StrMap inlineLiteralCache;                      // String literal handles (RAII)
    std::unordered_map<const BasicBlock*, const Function*> blockToFunction;
    std::unordered_map<const Function*, size_t> regCountCache_;

    // Buffer pools for allocation reuse
    std::vector<std::vector<uint8_t>> stackBufferPool_;
    std::vector<std::vector<Slot>> regFilePool_;

    // Exception handling
    TrapContext currentContext;         // Active instruction context
    TrapState lastTrap;                 // Last trap for diagnostics
    TrapToken trapToken;                // Error payload for trap.err

    // Polling and profiling
    uint32_t pollEveryN_;               // Host callback frequency
    std::function<bool(VM&)> pollCallback_;
#if ZANNA_VM_OPCOUNTS
    std::array<uint64_t, kNumOpcodes> opCounts_;  // Per-opcode counters
#endif

    // Per-VM extern registry (optional)
    ExternRegistry* externRegistry_;    // Custom function resolution
};
```

**Key responsibilities:**

- Module initialization and function lookup
- Dispatch strategy selection and lifecycle
- String literal caching and lifetime management
- Buffer pooling for recursive call efficiency
- Trap context tracking and formatting
- Debug breakpoint coordination
- Host polling for embedded applications

### 2. Frame (`src/vm/VM.hpp`)

Represents a single function activation record:

```cpp
struct Frame {
    // Nested types for exception handling
    struct HandlerRecord {
        const BasicBlock* handler;  // Handler block
        size_t ipSnapshot;          // IP to restore
    };
    struct ResumeState {
        const BasicBlock* block;    // Faulting block
        size_t faultIp, nextIp;     // Instruction pointers
        bool valid;
    };

    const Function* func;                          // Active function (non-owning)
    std::vector<Slot> regs;                        // SSA register file
    static constexpr size_t kDefaultStackSize = 65536;  // 64KB
    std::vector<uint8_t> stack;                    // Operand stack (alloca)
    size_t sp = 0;                                 // Stack pointer in bytes
    std::vector<std::optional<Slot>> params;       // Pending block parameters
    std::vector<HandlerRecord> ehStack;            // Exception handlers
    VmError activeError{};                         // Current error payload
    ResumeState resumeState{};                     // Resumption metadata
};
```

**Key responsibilities:**

- SSA value storage in register file
- Stack allocation via `alloca` instruction (bump `sp` within `stack`)
- Block parameter passing
- Exception handler stack management
- Resume state for error recovery

### 3. Slot (`src/vm/VM.hpp`)

Tagged union for runtime values:

```cpp
union Slot {
    int64_t i64;      // Integer value
    double f64;       // Floating-point value
    void* ptr;        // Generic pointer
    rt_string str;    // Runtime string handle
};
```

All IL values are represented as `Slot` during execution. Type safety is enforced by the IL verifier and opcode
handlers.

### 4. Dispatch Drivers

Pluggable strategies for instruction fetch-decode-execute:

- **FnTableDispatchDriver**: Uses function pointer table lookup
- **SwitchDispatchDriver**: Uses switch statement (inline handlers)
- **ThreadedDispatchDriver**: Uses computed goto (GCC/Clang only)

Selected at VM construction via `DispatchKind` enum.

### 5. Opcode Handlers (`src/vm/OpHandlers*.hpp`)

Category-organized functions that implement IL instructions:

- **Control** (`OpHandlers_Control.hpp`): `br`, `cbr`, `call`, `ret`, `switch`
- **Integer** (`OpHandlers_Int.hpp`): `add`, `mul`, `icmp_*`, `scmp_*`
- **Float** (`OpHandlers_Float.hpp`): `fadd`, `fmul`, `fcmp_*`
- **Memory** (`OpHandlers_Memory.hpp`): `alloca`, `load`, `store`, `gep`

Each handler has signature:

```cpp
ExecResult handler(VM& vm, Frame& fr, const Instr& in,
                  const BlockMap& blocks,
                  const BasicBlock*& bb, size_t& ip);
```

---

## Execution Model

### Execution Flow

```text
1. VM::run()
   ├─ Lookup "main" function
   └─ Call execFunction()
      ├─ setupFrame() → Initialize registers, stack, block map
      └─ runFunctionLoop()
         └─ dispatchDriver->run()
            └─ Loop:
               ├─ selectInstruction() → Fetch next instruction
               ├─ executeOpcode() → Dispatch to handler
               ├─ handleDebugBreak() → Check breakpoints
               └─ finalizeDispatch() → Update IP, check for return
```

### Instruction Execution Cycle

For each instruction:

1. **Select**: `selectInstruction()` identifies the next instruction
2. **Trace**: `traceInstruction()` emits trace output if enabled
3. **Execute**: Handler updates frame state and returns `ExecResult`
4. **Finalize**: `finalizeDispatch()` processes jumps/returns

### Control Flow

**Basic blocks:**

- Execution starts at the `entry` block
- Terminators (`ret`, `br`, `cbr`, `switch`) transfer control
- Block parameters are transferred before entering a new block

**Function calls:**

- `call` opcode pushes a new frame onto the execution stack
- Arguments are evaluated and passed as block parameters
- Return value is propagated back via `Slot`

**Tail calls:**

- Detected via `call.tail` attribute
- Reuses current frame instead of allocating new one
- Eliminates stack growth for recursive functions

---

## Dispatch Strategies

### 1. Function Table Dispatch (Default)

Uses a compile-time generated array of function pointers:

```cpp
// Generated in HandlerTable.hpp
static const OpcodeHandlerTable& getOpcodeHandlers() {
    static OpcodeHandlerTable table = {
        &handleAdd,    // Opcode::Add
        &handleSub,    // Opcode::Sub
        // ... one entry per opcode
    };
    return table;
}
```

**Pros:** Simple, portable, easy to debug
**Cons:** Indirect call overhead per instruction

### 2. Switch Dispatch

Expands all handlers inline within a giant switch statement:

```cpp
while (true) {
    Opcode op = fetchOpcode(state);
    switch (op) {
        case Opcode::Add: inline_handle_Add(state); break;
        case Opcode::Sub: inline_handle_Sub(state); break;
        // ... case per opcode
    }
}
```

**Pros:** Better branch prediction, potential for inlining
**Cons:** Large code size, longer compile time

### 3. Threaded Dispatch (GCC/Clang)

Uses computed goto with label addresses:

```cpp
static void* kOpLabels[] = { &&LBL_Add, &&LBL_Sub, /* ... */ };

#define DISPATCH_TO(opcode) goto *kOpLabels[opcode]

for (;;) {
    DISPATCH_TO(fetchOpcode(state));

    LBL_Add: inline_handle_Add(state); DISPATCH_TO(fetchNext());
    LBL_Sub: inline_handle_Sub(state); DISPATCH_TO(fetchNext());
    // ... label per opcode
}
```

**Pros:** Fastest dispatch, direct jump to handlers
**Cons:** Compiler-specific, large code size

### Selecting a Dispatch Strategy

The dispatch strategy is selected at VM construction via environment variable:

```bash
# Use function table dispatch (portable, moderate performance)
ZANNA_DISPATCH=table ./zanna -run program.il

# Use switch statement dispatch (good cache locality)
ZANNA_DISPATCH=switch ./zanna -run program.il

# Use threaded dispatch (fastest, requires GCC/Clang)
ZANNA_DISPATCH=threaded ./zanna -run program.il
```

**Default:** Threaded if supported (`ZANNA_THREADING_SUPPORTED=1`), otherwise Switch.

### Shared Dispatch Loop

All strategies share a common dispatch loop (`runSharedDispatchLoop`) that handles:

- State reset per iteration (`beginDispatch`)
- Instruction selection (`selectInstruction`)
- Debug hooks (`ZANNA_VM_DISPATCH_BEFORE/AFTER`)
- Trap handling for threaded dispatch
- Finalization and exit conditions (`finalizeDispatch`)

The strategy only implements `executeInstruction()` to map opcodes to handlers.

### Dispatch Loop Performance Optimizations

The shared dispatch loop includes several optimizations:

1. **Cached strategy properties**: `requiresTrapCatch()` and `handlesFinalizationInternally()`
   are cached once at loop entry to avoid virtual call overhead per instruction.

2. **Branch hints**: `[[likely]]` and `[[unlikely]]` attributes guide code layout for hot paths.

3. **Zero-cost hooks**: `ZANNA_VM_DISPATCH_BEFORE` and `ZANNA_VM_DISPATCH_AFTER` macros
   compile to nothing when disabled. When opcode counting is enabled (`ZANNA_VM_OPCOUNTS=1`),
   the counter increment is gated by a runtime flag (`config.enableOpcodeCounts`).

4. **Efficient polling**: `ZANNA_VM_DISPATCH_AFTER` only increments the poll counter when
   polling is active (`interruptEveryN > 0`), avoiding wasted cycles in the common case.

### Instrumentation Hooks

The VM provides compile-time configurable hooks for profiling and embedding:

```cpp
// In VMConfig.hpp - define before including VM headers
#define ZANNA_VM_DISPATCH_BEFORE(ST, OPCODE) \
    do { myProfiler.onInstruction(ST, OPCODE); } while(0)

#define ZANNA_VM_DISPATCH_AFTER(ST, OPCODE) \
    do { myProfiler.afterInstruction(ST, OPCODE); } while(0)
```

**Predefined behavior:**

- `ZANNA_VM_DISPATCH_BEFORE`: Increments per-opcode counters when `ZANNA_VM_OPCOUNTS=1`
- `ZANNA_VM_DISPATCH_AFTER`: Calls poll callback every N instructions if configured

### Per-Opcode Counters

Enable compile-time opcode counting:

```cpp
#define ZANNA_VM_OPCOUNTS 1  // Default: enabled
```

Access counters at runtime:

```cpp
vm.resetOpcodeCounts();
vm.run();
auto counts = vm.getOpcodeCounts();  // Returns array<uint64_t, kNumOpcodes>
for (auto [opcode, count] : vm.getNonZeroOpcodeCounts()) {
    std::cout << opcodeMnemonic(opcode) << ": " << count << "\n";
}
```

Disable via environment: `ZANNA_ENABLE_OPCOUNTS=0`

### Benchmark Harness

The `zanna bench` command provides a built-in benchmark harness for comparing dispatch strategies:

```sh
# Run all three strategies with 3 iterations each
zanna bench program.il

# Run a specific strategy with 5 iterations
zanna bench program.il -n 5 --table

# Run multiple files with JSON output
zanna bench file1.il file2.il --json

# Limit execution with max-steps
zanna bench program.il --max-steps 1000000
```

**Output format (text):**

```text
BENCH <file> <strategy> instr=<N> time_ms=<T> insns_per_sec=<R>
```

**Output format (JSON):**

```json
[
  {
    "file": "program.il",
    "strategy": "table",
    "success": true,
    "instructions": 7000004,
    "time_ms": 3618.33,
    "insns_per_sec": 1934596,
    "return_value": 0
  }
]
```

**Strategy selection flags:**

- `--table`: Run only FnTable dispatch
- `--switch`: Run only Switch dispatch
- `--threaded`: Run only Threaded dispatch
- (default): Run all three strategies

**Example benchmark IL programs** are available in `examples/il/benchmarks/`:

- `arith_stress.il`: Heavy arithmetic workload
- `branch_stress.il`: Branch-heavy control flow
- `call_stress.il`: Function call overhead testing
- `mixed_stress.il`: Combined workload
- `string_stress.il`: String operations

---

## Memory Model

### Register File

Each frame has an SSA register file sized to the function's register count:

```cpp
frame.regs.resize(func->registerCount);
```

Registers are indexed by SSA value ID. Each register is written once and read many times (SSA property).

### Operand Stack

Each frame has an operand stack for `alloca` allocations. The default capacity
is 64KB (`Frame::kDefaultStackSize`):

```cpp
std::vector<uint8_t> stack; // capacity ~= 64KB by default
size_t sp = 0;  // Stack pointer in bytes
```

**Usage:**

- `alloca N` allocates N bytes on the stack
- Returns a `ptr` pointing into `stack` at offset `sp`
- Stack grows upward (`sp` increases)
- No explicit deallocation (frame-scoped)

**Limits:**

- Default 64KB size per frame (`Frame::kDefaultStackSize`)
- Overflow causes trap
- Suitable for temporaries, strings, and moderate-sized arrays (e.g., 80×25 screen buffers)

### String Handles

Strings are managed by the runtime as opaque handles (`rt_string`):

- **Global strings**: Cached in `strMap`, lifetime = VM lifetime
- **Inline literals**: Cached in `inlineLiteralCache`, supports embedded NULs
- **Runtime strings**: Created by runtime functions, reference-counted

The VM releases all cached handles in its destructor.

---

## Exception & Trap Handling

### Trap Types

Defined in `Trap.hpp`:

```cpp
enum class TrapKind {
    DivideByZero,     // Integer division by zero
    Overflow,         // Arithmetic overflow
    InvalidCast,      // Type conversion failure
    DomainError,      // Semantic violation
    Bounds,           // Array bounds check
    FileNotFound,     // File I/O error
    EOF,              // End of file
    IOError,          // Generic I/O failure
    InvalidOperation, // Invalid state transition
    RuntimeError,     // Catch-all
    Interrupt,        // Ctrl-C or requestInterrupt()
    NetworkError      // Network I/O failure
};
```

### Exception Handler Stack

Each frame maintains an exception handler stack (`Frame::ehStack`) using the `HandlerRecord` type
defined in Frame (see [Key Components](#key-components)).

**IL instructions:**

- `eh.push label handler` — Push handler onto stack
- `eh.pop` — Pop handler from stack
- `eh.entry` — Mark entry point of handler block

### Trap Dispatch

When a trap occurs:

1. **Capture context**: Function, block, instruction, source location
2. **Search for handler**: Walk `ehStack` for active handler
3. **Dispatch or unwind**:
    - **Handler found**: Jump to handler block, set `activeError`
    - **No handler**: Throw `TrapDispatchSignal` to unwind stack
4. **Resume**: Handler uses `resume.same`, `resume.next`, or `resume.label`

### Structured Error Payload

```cpp
struct VmError {
    TrapKind kind;     // Error classification
    int32_t code;      // Secondary code
    uint64_t ip;       // Instruction pointer
    int32_t line;      // Source line (-1 if unknown)
};
```

Accessible via:

- `trap.kind` — Read current trap kind
- `err.get_kind %e` — Extract kind from error value
- `err.get_code %e` — Extract code from error value

---

## Runtime Integration

### RuntimeBridge (`src/vm/RuntimeBridge.hpp`)

Adapter between VM and C runtime library:

```cpp
class RuntimeBridge {
    static Slot call(RuntimeCallContext& ctx,
                    const std::string& name,
                    const std::vector<Slot>& args,
                    ...);

    static void trap(TrapKind kind, const std::string& msg, ...);
    static const RuntimeCallContext* activeContext();
};
```

**Call flow:**

1. IL `call @Zanna.Terminal.PrintI64(args)` instruction (or legacy `@rt_*` alias)
2. Handler evaluates arguments into bytecode/VM slots
3. Bytecode caches known runtime descriptors in the native-function table and calls the resolved-descriptor RuntimeBridge entry point when possible
4. C function is invoked with marshalled arguments
5. Return value is marshalled back to `Slot`

**Note:** The runtime supports both canonical `@Zanna.*` names and legacy `@rt_*` aliases when built with
`-DZANNA_RUNTIME_NS_DUAL=ON`.

### Runtime Call Context

Tracks active runtime call for trap diagnostics:

```cpp
struct RuntimeCallContext {
    SourceLoc loc;                      // Call site location
    std::string function;               // Calling IL function
    std::string block;                  // Calling block
    const RuntimeDescriptor* descriptor; // Runtime function
    Slot* argBegin;                     // Argument array
    size_t argCount;                    // Argument count
};
```

Populated before each runtime call, cleared after.

### External Function Registry

Custom functions can be registered:

```cpp
struct ExternDesc {
    std::string name;
    void* ptr;
    // ... signature metadata
};

RuntimeBridge::registerExtern(desc);
```

Enables embedding applications to extend the runtime.

---

## Debug & Tracing

### Trace Sink

Configurable output for instruction tracing:

```cpp
struct TraceConfig {
    bool enabled;           // Enable tracing
    bool ilTrace;           // Trace IL instructions
    bool boolTrace;         // Trace boolean values
    bool srcTrace;          // Trace source locations
};
```

Output format:

```text
[func:block:ip] opcode operands → result
```

### Debug Controller

Manages breakpoints and stepping:

```cpp
class DebugCtrl {
    // Breakpoints
    void addBreakLabel(std::string label);
    void addBreakSrcLine(std::string file, int line);
    void clearBreaks();

    // Stepping
    void requestStep(uint64_t count);
    bool shouldBreak(/* context */);
};
```

**Breakpoint types:**

- Block label breakpoints
- Source line breakpoints
- Step count breakpoints

### Debug Scripting

Optional command script for automated debugging:

```cpp
class DebugScript {
    virtual Action onBreakpoint(VM& vm, Frame& fr) = 0;
};
```

Allows programmatic control of execution (continue, step, inspect, etc.).

### Memory Watches

Monitor memory access for debugging:

```cpp
debug.addMemWatch(addr, size, "tag");
auto hits = debug.drainMemWatchEvents();
```

Tracks reads/writes to specific memory ranges.

---

## Source Code Guide

### Directory Structure

```text
src/vm/
├── VM.hpp/cpp                  # Main VM class and core interpreter logic
├── VMContext.hpp/cpp           # Execution context helpers
├── VMConfig.hpp                # Build configuration
├── VMConstants.hpp             # VM constants
├── VMInit.cpp                  # VM initialization
├── FunctionExecCache.cpp       # Pre-resolved operand cache per (function, block)
├── Runner.cpp                  # Public API facade
│
├── OpHandlers.hpp/cpp          # Handler aggregation and table generation
├── OpHandlerUtils.hpp/cpp      # Handler utility functions
├── OpHandlerAccess.hpp         # Handler access utilities
├── OpcodeHandlerHelpers.hpp    # Common handler helper functions
├── OpHandlers_Control.hpp      # Control flow handlers
├── OpHandlers_Int.hpp          # Integer arithmetic handlers
├── OpHandlers_Float.hpp        # Float arithmetic handlers
├── OpHandlers_Memory.hpp       # Memory operation handlers
├── IntOpSupport.hpp            # Integer operation support
│
├── DispatchStrategy.hpp/cpp    # Pluggable dispatch strategies
├── DispatchMacros.hpp          # Dispatch loop macros and hooks
│
├── ops/
│   ├── Op_CallRet.cpp          # Call/return implementation
│   ├── Op_BranchSwitch.cpp     # Branch/switch implementation
│   ├── Op_TrapEh.cpp           # Trap/exception handling
│   ├── common/Branching.*      # Branch target resolution helpers
│   ├── schema/ops.yaml         # Opcode schema definitions
│   └── generated/              # Generated dispatch tables and handlers
│       ├── HandlerTable.hpp    # Static handler function table
│       ├── InlineHandlers*.inc # Inline handler implementations
│       ├── SwitchDispatch*.inc # Switch dispatch implementations
│       └── Threaded*.inc       # Threaded dispatch labels/cases
│
├── RuntimeBridge.hpp/cpp       # Runtime integration
├── Marshal.hpp/cpp             # Value marshalling
│
├── Trap.hpp/cpp                # Trap definitions and formatting
├── TrapInvariants.hpp          # Trap assertion helpers
├── DiagFormat.hpp/cpp          # Diagnostic message formatting
├── err_bridge.hpp/cpp          # Error bridge helpers
│
├── control_flow.hpp/cpp        # Control flow utilities
├── tco.hpp/cpp                 # Tail-call optimization
├── ZannaStringHandle.hpp       # RAII string handle wrapper
│
├── int_ops_arith.cpp           # Integer arithmetic implementations
├── int_ops_cmp.cpp             # Integer comparison implementations
├── int_ops_convert.cpp         # Integer conversion implementations
├── fp_ops.cpp                  # Floating-point implementations
├── mem_ops.cpp                 # Memory operation implementations
│
├── ThreadsRuntime.cpp          # Zanna.Threads runtime support
│
└── debug/                      # Debug and tracing subsystem
    └── *.cpp                   # Debug controller, trace, scripting
```

### Key Files by Functionality

**Core Interpreter:**

- `VM.hpp`, `VM.cpp` — Main interpreter class
- `VMContext.hpp` — Shared execution helpers
- `Runner.cpp` — Public API facade

**Dispatch:**

- `VM.cpp` — Dispatch driver implementations
- `ops/generated/` — Generated dispatch tables

**Opcode Handlers:**

- `OpHandlers*.hpp` — Handler declarations by category
- `ops/Op_*.cpp` — Complex handler implementations
- `int_ops_*.cpp`, `fp_ops.cpp`, `mem_ops.cpp` — Arithmetic implementations

**Exception Handling:**

- `Trap.hpp`, `Trap.cpp` — Trap types and formatting
- `err_bridge.hpp` — Error bridge integration
- `ops/Op_TrapEh.cpp` — Exception handler opcodes

**Runtime Integration:**

- `RuntimeBridge.hpp`, `RuntimeBridge.cpp` — C runtime adapter
- `Marshal.hpp`, `Marshal.cpp` — Value marshalling

**Debugging:**

- `debug/Debug.cpp` — Breakpoint management
- `debug/DebugScript.cpp` — Debug scripting support
- `debug/Trace.cpp` — Trace output formatting
- `debug/VM_DebugUtils.cpp` — Debug utility helpers
- `debug/VMDebug.cpp` — Debug integration

---

## Performance Features

### Tail-Call Optimization

Enabled by default (`ZANNA_VM_TAILCALL`):

```cpp
// Detect tail call
if (instr.isTailCall()) {
    // Reuse current frame
    return executeTailCall(fr, callee, args);
}
```

Eliminates stack growth for recursive functions.

### Opcode Counting

Compile-time flag (`ZANNA_VM_OPCOUNTS`):

```cpp
#if ZANNA_VM_OPCOUNTS
std::array<uint64_t, kNumOpcodes> opCounts_;
#endif
```

Tracks execution count per opcode for profiling.

**API:**

```cpp
const auto& counts = vm.opcodeCounts();
auto top = vm.topOpcodes(10);  // Top 10 opcodes
vm.resetOpcodeCounts();
```

### Execution Context Optimization

The VM execution context has been optimized to minimize overhead on the hot path:

**Trusted bytecode dispatch:** Source execution uses `BytecodeCompiler::compileChecked()` and then enables trusted
dispatch in the bytecode VM. Trusted dispatch skips per-instruction PC and operand-stack validation in the interpreter
loop while keeping checked compilation, verifier diagnostics, runtime traps, and branch-target checks available for
debug/unchecked embedding paths.

**ExecState-based dispatch:** The dispatch macros (`ZANNA_VM_DISPATCH_BEFORE`, `ZANNA_VM_DISPATCH_AFTER`) use
`ExecState` directly instead of `VMContext`, avoiding an extra indirection per instruction:

```cpp
// Hot path uses ExecState directly
ZANNA_VM_DISPATCH_BEFORE(state, opcode);  // state is ExecState&

// ExecState.config includes all per-instruction configuration
struct PollConfig {
    uint32_t interruptEveryN;
    std::function<bool(VM&)> pollCallback;
    bool enableOpcodeCounts;  // Direct access for opcode counting
};
```

**VMContext for external APIs:** The `VMContext` wrapper is still used for external APIs (`stepOnce`, `fetchOpcode`,
`handleTrapDispatch`) to provide a stable interface, but it's not required on the per-instruction hot path.

### Execution Stack Pre-allocation

The execution stack (`execStack`) tracks active `ExecState` pointers for trap unwinding and debugging:

```cpp
// Pre-allocated to kExecStackInitialCapacity (64) in VM constructor
std::vector<ExecState*> execStack;

// Unified RAII guard for stack management
struct ExecStackGuard {
    VM& vm;
    ExecState* state;
    ExecStackGuard(VM& vmRef, ExecState& stRef) noexcept;
    ~ExecStackGuard() noexcept;
};
```

**Optimizations:**

- Pre-allocated capacity eliminates heap allocation for typical call depths
- Unified `ExecStackGuard` in VM.hpp removes code duplication
- `noexcept` specifiers enable compiler optimizations

### Inline String Literal Cache

Caches runtime handles for string literals:

```cpp
std::unordered_map<std::string_view, ZannaStringHandle, ...> inlineLiteralCache;
```

**Optimizations:**

- Pre-populated during VM construction by scanning all ConstStr operands in the module
- Fast path uses `find()` for pre-populated strings (common case)
- Fallback `try_emplace` only for edge cases (dynamically generated strings)
- Eliminates repeated allocation and map insertion for frequently used literals

### Switch Cache

Memoizes switch dispatch data:

```cpp
struct SwitchCache {
    std::unordered_map<int32_t, const BasicBlock*> caseMap;
    const BasicBlock* defaultTarget;
};
```

Amortizes switch table construction across iterations.

### Frame Buffer Pooling

The VM maintains pools for frequently allocated frame resources:

**Stack Buffer Pool:**
- Reuses operand stack buffers across function calls
- Pre-sized to `Frame::kDefaultStackSize` (64KB)
- Eliminates allocation overhead for recursive functions

**Register File Pool:**
- Reuses SSA register file vectors
- Sized by `clear()` and `resize()` rather than reallocation
- Reduces heap churn during deep call stacks

**Benefit:** Recursive functions like factorial(n) allocate only once per unique call depth,
then reuse pooled buffers for subsequent calls. This significantly reduces GC pressure and
improves cache locality.

### Host Polling

Configurable interrupt callback:

```cpp
vm.setPollConfig(everyN, [](VM& vm) {
    // Host logic (UI events, etc.)
    return true;  // Continue execution
});
```

Allows embedding applications to maintain responsiveness.

---

## Best Practices

### For VM Developers

1. **Opcode Handlers**: Keep handlers simple and delegate to helper functions
2. **Error Handling**: Use `RuntimeBridge::trap()` for runtime errors
3. **Caching**: Consider caching hot lookups (strings, functions, blocks)
4. **Debugging**: Add trace points for complex operations
5. **Testing**: Write unit tests for each handler

### For IL Generators

1. **SSA Form**: Ensure proper SSA (single assignment per register)
2. **Terminators**: Every block must end with a terminator
3. **Type Safety**: Match operand types to instruction signatures
4. **Exception Handlers**: Properly nest `eh.push`/`eh.pop` pairs
5. **Stack Usage**: Keep `alloca` sizes within frame limits (~64KB by default)

### For Embedders

1. **Configuration**: Choose appropriate dispatch strategy
2. **Polling**: Set reasonable interrupt frequency
3. **Externs**: Register custom functions before execution
4. **Tracing**: Enable tracing for debugging, disable for production
5. **Error Handling**: Catch and handle `TrapDispatchSignal` if needed

---

## Further Reading

**Zanna Documentation:**

- **[IL Guide](../il/il-guide.md)** — IL specification and semantics
- **[IL Reference](../il/il-guide.md#reference)** — Complete opcode catalog
- **[Getting Started](../getting-started.md)** — Build and run Zanna

**Developer Documentation:**

- [Architecture](architecture.md) — Overall system architecture
- [Error Handling Spec](../specs/errors.md) — Error handling specification
- [Threading and Globals](../specs/threading-and-globals.md) — VM concurrency model

**Source Code:**

- `src/vm/` — VM implementation
- `src/runtime/` — C runtime library
- `src/tests/vm/` — VM unit tests

---

## Appendix: Performance Tuning

This section summarizes runtime tuning knobs and benchmarking for the VM.

### Dispatch Modes

- Env `ZANNA_DISPATCH`:
    - `table`: function-table dispatch via `executeOpcode`
    - `switch`: inline switch dispatch with generated handlers
    - `threaded`: computed goto (if built with `ZANNA_VM_THREADED`)

- Env `ZANNA_ENABLE_OPCOUNTS` (default on): enable per-opcode execution counters. You can query counts via
  `Runner::opcodeCounts()` or the `--count` flag in `zanna -run`.

- Env `ZANNA_INTERRUPT_EVERY_N`: periodically invoke a host callback every N instructions (see
  `RunConfig::interruptEveryN`).

### Switch Backend Heuristics

Switch dispatch selects a backend per instruction. Heuristics can be tuned via env:

- `ZANNA_SWITCH_DENSE_MAX_RANGE` (default `4096`): maximum value range to consider a dense jump table.
- `ZANNA_SWITCH_DENSE_MIN_DENSITY` (default `0.60`): minimum case density for dense backend.
- `ZANNA_SWITCH_HASH_MIN_CASES` (default `64`): minimum number of cases before hashing is considered.
- `ZANNA_SWITCH_HASH_MAX_DENSITY` (default `0.15`): maximum density to prefer hashed backend.

If `ZANNA_SWITCH_MODE` is set to `dense|sorted|hashed|linear|auto`, it overrides the heuristic for all instructions.

### Benchmarking

Use the helper script to compare dispatch performance across modes:

- Script: `scripts/vm_benchmark.sh`

Environment variables:

- `IL_DIR` (default `examples/il/benchmarks`): directory of IL programs to benchmark (relative to repo root).
- `ILC_BIN`: optional path to `zanna`; otherwise auto-detected under `build/`.
- `RUNS_PER_CASE` (default `5`): number of runs per (mode, program) pair.

Each invocation writes a timestamped section header and a per-row timestamp, along with averages and min/max timings,
the actual dispatch kind, and instruction counts extracted from `--count` and `--time` summaries.

Example:

```bash
RUNS_PER_CASE=5 IL_DIR='src/tests/il/e2e' scripts/vm_benchmark.sh
```

The script sets `ZANNA_DEBUG_VM=1` so the VM prints the resolved dispatch kind, and `ZANNA_ENABLE_OPCOUNTS=1` to capture
counts.

---

## Appendix: Concurrency Model

Each `VM` instance is single‑threaded: only one host thread may execute within a given VM instance at a time. To
parallelize at the *embedder* level, create one VM per host thread (each VM has its own program state).

For *language-level* shared-memory threads (`Zanna.Threads`), the VM spawns a new host thread and runs a new VM instance
that shares a single `VM::ProgramState` (shared globals + shared `RtContext`) with its parent. This preserves the “one
host thread per VM instance” invariant while allowing a Zanna program to share memory across its threads.

The active VM is tracked via a thread‑local guard (see `ActiveVMGuard` in `src/vm/VMContext.*`), which binds the VM and
its runtime context for the duration of execution. In debug builds, attempting to activate a different VM while one is
already active on the same thread triggers an assertion.

---

---

## Appendix: Runtime ABI Reference

Extern symbols in IL map to C functions declared in `src/runtime/rt.hpp`. This section documents the core ABI surface
available to both the VM and native backends. For the complete list see the [Runtime Library Reference](../zannalib/README.md).

### Runtime symbol naming

- Canonical entry points use dotted `Zanna.*` names emitted by frontends (catalogued in `src/il/runtime/RuntimeSignatures.hpp`).
- Native backends rewrite these to C symbols via `il::runtime::mapCanonicalRuntimeName` and the alias table in `src/il/runtime/RuntimeNameMap.hpp`.
- When built with `-DZANNA_RUNTIME_NS_DUAL=ON`, legacy `@rt_*` externs are accepted as aliases of `@Zanna.*`.

### Math

| Symbol               | Signature         | Semantics                                   |
|----------------------|-------------------|---------------------------------------------|
| `@rt_sqrt`           | `f64 -> f64`      | square root                                 |
| `@rt_floor`          | `f64 -> f64`      | floor                                       |
| `@rt_ceil`           | `f64 -> f64`      | ceiling                                     |
| `@rt_sin`            | `f64 -> f64`      | sine                                        |
| `@rt_cos`            | `f64 -> f64`      | cosine                                      |
| `@rt_pow_f64_chkdom` | `f64, f64 -> f64` | power                                       |
| `@rt_abs_i64`        | `i64 -> i64`      | absolute value (integer, traps on overflow) |
| `@rt_abs_f64`        | `f64 -> f64`      | absolute value (float)                      |

### String operations

| Symbol              | Signature              | Semantics                                            |
|---------------------|------------------------|------------------------------------------------------|
| `@rt_str_len`       | `str -> i64`           | Return length of string in bytes                     |
| `@rt_str_concat`    | `str, str -> str`      | Concatenate two strings; consumes both operands      |
| `@rt_str_substr`    | `str, i64, i64 -> str` | Extract substring (0-based start, length)            |
| `@rt_str_left`      | `str, i64 -> str`      | Leftmost n characters                                |
| `@rt_str_right`     | `str, i64 -> str`      | Rightmost n characters                               |
| `@rt_str_mid`       | `str, i64 -> str`      | Substring from 1-based UTF-8 codepoint start to end  |
| `@rt_str_mid_len`   | `str, i64, i64 -> str` | Substring from 1-based UTF-8 codepoint start with length |
| `@rt_str_index_of`  | `str, str -> i64`      | Find needle; returns 1-based index or 0              |
| `@rt_str_trim`      | `str -> str`           | Remove leading and trailing whitespace               |
| `@rt_str_ucase`     | `str -> str`           | Convert ASCII to uppercase                           |
| `@rt_str_lcase`     | `str -> str`           | Convert ASCII to lowercase                           |
| `@rt_str_chr`       | `i64 -> str`           | Single-character string from ASCII code              |
| `@rt_str_asc`       | `str -> i64`           | ASCII code of first character                        |
| `@rt_str_eq`        | `str, str -> i1`       | Compare two strings for equality                     |

### Console I/O

| Symbol           | Signature     | Semantics                        |
|------------------|---------------|----------------------------------|
| `@rt_print_str`  | `str -> void` | Print string to stdout           |
| `@rt_print_i64`  | `i64 -> void` | Print 64-bit integer to stdout   |
| `@rt_print_f64`  | `f64 -> void` | Print float to stdout            |
| `@rt_input_line` | `void -> str` | Read a line from stdin           |

### Conversion

| Symbol           | Signature    | Semantics                           |
|------------------|--------------|-------------------------------------|
| `@rt_to_int`     | `str -> i64` | Parse decimal integer from string   |
| `@rt_to_double`  | `str -> f64` | Parse floating-point from string    |
| `@rt_int_to_str` | `i64 -> str` | Convert integer to decimal string   |
| `@rt_f64_to_str` | `f64 -> str` | Convert float to decimal string     |
| `@rt_val`        | `str -> f64` | Parse leading numeric prefix        |

### Terminal control

| Symbol                        | Signature          | Semantics                      |
|-------------------------------|--------------------|--------------------------------|
| `@rt_term_cls`                | `void -> void`     | Clear screen and home cursor   |
| `@rt_term_color_i32`          | `i32, i32 -> void` | Set foreground/background      |
| `@rt_term_locate_i32`         | `i32, i32 -> void` | Move cursor (1-based row, col) |
| `@rt_term_cursor_visible_i32` | `i32 -> void`      | Show/hide cursor               |

### Time & random

| Symbol              | Signature     | Semantics                           |
|---------------------|---------------|-------------------------------------|
| `@rt_timer_ms`      | `void -> i64` | Monotonic millisecond timestamp     |
| `@rt_rnd`           | `void -> f64` | Random f64 in [0,1)                 |
| `@rt_randomize_i64` | `i64 -> void` | Seed RNG                            |

### Environment

| Symbol              | Signature     | Semantics                          |
|---------------------|---------------|------------------------------------|
| `@rt_args_count`    | `void -> i64` | Number of program arguments        |
| `@rt_args_get`      | `i64 -> str`  | Program argument at zero-based index |
| `@rt_env_is_native` | `void -> i1`  | 1 for native binary, 0 for VM     |
