---
status: active
audience: contributors
last-verified: 2026-08-17
---

# CODEMAP: IL Build

IR construction utilities (`src/il/build/`) for programmatic IL generation.

## Overview

- **Total source files**: 2 (.hpp/.cpp)

## IR Builder

| File            | Purpose                                                  |
|-----------------|----------------------------------------------------------|
| `IRBuilder.cpp` | Stateful API implementation                              |
| `IRBuilder.hpp` | Stateful API for constructing modules, functions, blocks |

### `IRBuilder` (`il/build/IRBuilder.hpp`, namespace `il::build`)

Stateful helper for constructing IL modules programmatically. Maintains an insertion point (current basic block) and provides helpers to emit instructions, manage control flow, and track SSA temporaries. Enforces structural invariants (one terminator per block).

**Ownership:** Does not own the `Module` it operates on. The caller must ensure the `Module` outlives all builder operations.

#### Constructor

- `IRBuilder(Module &m)` — create a builder operating on module `m`

#### Module-level operations

- `addExtern(name, ret, params)` — `Extern &`: add an external function declaration
- `addGlobal(name, type, init = "")` — `Global &`: add a global variable (empty `init` = zero-initialized)
- `addGlobalStr(name, value)` — `Global &`: add a global string constant
- `startFunction(name, ret, params)` — `Function &`: begin definition of a new function

#### Block management

- `addBlock(fn, label)` — `BasicBlock &`: backward-compatible helper to add a parameter-less block
- `blockParam(bb, idx)` — `Value`: access parameter `idx` of block `bb` as a value
- `createBlock(fn, label, params = {})` — `BasicBlock &`: create a basic block with optional parameters
- `insertBlock(fn, idx, label)` — `BasicBlock &`: insert a parameter-less block at index `idx` in `fn`
- `setInsertPoint(bb)` — set the current insertion point to block `bb`

#### Control flow emission

- `br(dst, args = {})` — emit unconditional branch to `dst` with arguments `args`
- `cbr(cond, t, targs, f, fargs)` — emit conditional branch on `cond` to true block `t` or false block `f`
- `emitRet(v, loc)` — emit return from current function with optional value `v`
- `emitResumeLabel(token, target, loc)` — emit resume to a specific handler block label
- `emitResumeNext(token, loc)` — emit resume propagating to the next enclosing handler
- `emitResumeSame(token, loc)` — emit resume that rethrows within the same handler

#### Instruction emission

- `emitCall(callee, args, dst, loc)` — emit a call to `callee` with arguments `args`; stores result in `dst` if provided
- `emitConstStr(globalName, loc)` — `Value`: emit reference to global string `globalName`

#### SSA management

- `reserveTempId()` — `unsigned`: reserve the next SSA temporary identifier for the active function

#### Private members (implementation detail)

- `append(instr)` — `Instr &`: append instruction to the current block
- `isTerminator(op) const` — `bool`: check if opcode `op` terminates a block
- `calleeReturnTypes` — `std::unordered_map<std::string, Type>`: cached return types by callee name
- `curBlockIdx` — `std::optional<size_t>`: index of the current insertion block
- `curFunc` — `Function *`: pointer to the current function being built
- `mod` — `Module &`: module being constructed
- `nextTemp` — `unsigned`: next temporary id counter

#### Usage pattern

```cpp
Module m;
IRBuilder builder(m);

// Declare external function
builder.addExtern("rt_print_str", Type(Type::Kind::Void), {Type(Type::Kind::Str)});

// Define a function
auto &fn = builder.startFunction("main", Type(Type::Kind::I64), {});

// Create blocks first (push_back invalidates references)
auto &entry = builder.createBlock(fn, "entry");

// Set insertion point and emit instructions
builder.setInsertPoint(entry);
builder.emitRet(Value::constInt(0), {});
```

> **Note:** `Function::blocks` is a `std::vector<BasicBlock>` — `push_back` invalidates all existing
> references. When creating multiple blocks, create them all first, then access via
> `fn.blocks[0]`, `fn.blocks[1]`, etc. Use `builder.blockParam(bb, idx)` to reference block
> parameters, not `Value::temp(idx)`.
