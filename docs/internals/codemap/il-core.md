---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: IL Core

Core IL data structures (`src/il/core/`) representing modules, functions, blocks, and instructions.

## Overview

- **Total source files**: 27 (.hpp/.cpp/.def)

## Module Structure

| File            | Purpose                                                       |
|-----------------|---------------------------------------------------------------|
| `Module.cpp`    | Module container implementation                               |
| `Module.hpp`    | Module container: externs, globals, functions, version        |
| `Function.cpp`  | Function definition implementation                            |
| `Function.hpp`  | Function definition: signature, params, blocks, SSA names     |
| `BasicBlock.cpp`| Block implementation                                          |
| `BasicBlock.hpp`| Block: label, parameters, instructions, terminator flag       |
| `Instr.cpp`     | Instruction implementation                                    |
| `Instr.hpp`     | Instruction: opcode, result, operands, successors, source loc |
| `EffectAttrs.hpp`| Shared semantic effect attributes (`nothrow`/`readonly`/`pure`) for callable IL decls; `FunctionAttrs`/`CallAttrs` alias it |

### `Module` (`Module.hpp`)

Top-level container for a compilation unit. Owns all externs, globals, and functions by value.

**Fields:**
- `externs` — `std::vector<Extern>`: declared external functions
- `functions` — `std::vector<Function>`: function definitions
- `globals` — `std::vector<Global>`: global variable declarations
- `target` — `std::optional<std::string>`: optional target triple directive
- `version` — `std::string`: IL format version string (defaults to `ZANNA_IL_VERSION_STR`)

**Invariants:** Function, extern, and global names must be unique within the module.

### `Function` (`Function.hpp`)

An IL function definition with parameters, basic blocks, SSA value names, and semantic attributes.

**Fields:**
- `Attrs` — `FunctionAttrs`: mutable semantic attribute bundle
- `blocks` — `std::vector<BasicBlock>`: basic blocks comprising the function body
- `linkage` — `Linkage`: visibility for cross-module linking (default: `Internal`)
- `name` — `std::string`: human-readable identifier, unique within its module
- `params` — `std::vector<Param>`: ordered list of parameters
- `retType` — `Type`: declared return type
- `valueNames` — `std::vector<std::string>`: mapping from SSA value IDs to diagnostic names

**Methods:**
- `attrs()` — `FunctionAttrs &`: access mutable attribute bundle
- `attrs() const` — `const FunctionAttrs &`: access read-only attribute bundle

### `FunctionAttrs` (`Function.hpp`)

Semantic hints for a function used by optimisation passes.

**Fields:**
- `nothrow` — `bool`: function is guaranteed not to throw (default: `false`)
- `pure` — `bool`: function is free of observable side effects (default: `false`)
- `readonly` — `bool`: function may read memory but performs no writes (default: `false`)

### `BasicBlock` (`BasicBlock.hpp`)

A maximal sequence of IL instructions with a single entry point and single exit terminator.

**Fields:**
- `instructions` — `std::vector<Instr>`: ordered list of IL instructions
- `label` — `std::string`: human-readable identifier, unique within the parent function
- `params` — `std::vector<Param>`: block parameters (phi-node equivalents)
- `terminated` — `bool`: true when the last instruction is a terminator opcode

### `Instr` (`Instr.hpp`)

A single IL instruction within a basic block.

**Fields:**
- `brArgs` — `std::vector<std::vector<Value>>`: branch arguments per target (outer size matches `labels`)
- `CallAttr` — `CallAttrs`: semantic attributes for call-like instructions
- `callee` — `std::string`: callee name for call instructions
- `labels` — `std::vector<std::string>`: branch target labels
- `loc` — `il::support::SourceLoc`: source location (`{0,0}` denotes unknown)
- `op` — `Opcode`: operation code selecting semantics
- `operands` — `std::vector<Value>`: general operands (size depends on opcode)
- `result` — `std::optional<unsigned>`: destination temporary id; disengaged if no result
- `type` — `Type`: result type or void

**Switch helpers (free functions in `Instr.hpp`):**
- `switchCaseArgs(instr, index)` — `const std::vector<Value> &`: branch arguments for case arm `index`
- `switchCaseCount(instr)` — `size_t`: number of explicit case arms
- `switchCaseLabel(instr, index)` — `const std::string &`: branch label for case arm `index`
- `switchCaseValue(instr, index)` — `const Value &`: value guarding case arm `index`
- `switchDefaultArgs(instr)` — `const std::vector<Value> &`: default branch arguments
- `switchDefaultLabel(instr)` — `const std::string &`: default branch label
- `switchScrutinee(instr)` — `const Value &`: scrutinee operand of a switch instruction

### `CallAttrs` (`Instr.hpp`)

Semantic attributes for call-like instructions.

**Fields:**
- `nothrow` — `bool`: call cannot throw (default: `false`)
- `pure` — `bool`: call has no observable side effects or memory access (default: `false`)
- `readonly` — `bool`: call may read but not write memory (default: `false`)

## Linkage

| File          | Purpose                                                   |
|---------------|-----------------------------------------------------------|
| `Linkage.hpp` | Linkage enum for cross-module function/global visibility  |

### `Linkage` (`Linkage.hpp`)

Enum class controlling whether a function or global is visible across module boundaries during linking.

**Enum values:**
- `Internal` — Module-private (default). Not visible to other modules. Name may be prefixed during linking to avoid collisions.
- `Export` — Defined in this module and visible to other modules. The function must have a body.
- `Import` — Declared in this module but defined elsewhere. The function has no body; the linker resolves it to a matching `Export` function.

**Used by:** `Function::linkage`, `Global::linkage`

**Related:** [Cross-Language Interop Guide](../../languages/interop.md), [IL Guide: Function Linkage](../../il/il-guide.md)

---

## Types and Values

| File        | Purpose                                                      |
|-------------|--------------------------------------------------------------|
| `Type.cpp`  | Type wrapper implementation                                  |
| `Type.hpp`  | Type wrapper and Kind enumeration (void, i1, i16, i32, i64, f64, ptr, str, error, resume_tok) |
| `Value.cpp` | SSA value implementation                                     |
| `Value.hpp` | SSA value tagged union: temps, constants, globals, null      |
| `Param.hpp` | Function/block parameter: type, name, id, and optional attributes |
| `FPCast.hpp` | Checked floating-point cast helpers: `CheckedFPCastFailure`, `CheckedFPCastResult` |

### `Type` (`Type.hpp`)

Lightweight value-type wrapper around a `Type::Kind` enum representing the 10 primitive IL types.

**`Type::Kind` enum values:**
- `Error`
- `F64`
- `I1`
- `I16`
- `I32`
- `I64`
- `Ptr`
- `ResumeTok`
- `Str`
- `Void`

**Fields:**
- `kind` — `Type::Kind`: discriminator specifying the active kind

**Methods:**
- `toString() const` — `std::string`: convert type to lowercase mnemonic string

**Free functions:**
- `kindToString(k)` — `std::string`: convert `Type::Kind` to mnemonic string

### `Value` (`Value.hpp`)

Tagged union representing operands and constants in IL instructions.

**`Value::Kind` enum values:**
- `ConstFloat`
- `ConstInt`
- `ConstStr`
- `GlobalAddr`
- `NullPtr`
- `Temp`

**Fields:**
- `f64` — `double` (union member): floating-point payload when `kind == ConstFloat`
- `i64` — `long long` (union member): integer payload when `kind == ConstInt`
- `id` — `unsigned` (union member): temporary identifier when `kind == Temp`
- `isBool` — `bool`: set when the integer literal represents an i1 boolean (only meaningful for `ConstInt`)
- `kind` — `Value::Kind`: discriminant
- `str` — `std::string`: string payload for `ConstStr` and `GlobalAddr`

**Factory methods (static):**
- `Value::constBool(v)` — boolean constant (sets `isBool = true`)
- `Value::constFloat(v)` — floating-point constant
- `Value::constInt(v)` — integer constant
- `Value::constStr(s)` — string constant
- `Value::global(s)` — global address value
- `Value::null()` — null pointer value
- `Value::temp(t)` — temporary value

**Free functions:**
- `toString(v)` — `std::string`: convert value to string representation
- `valueEquals(a, b)` — `bool`: semantic equality comparison
- `valueHash(v)` — `size_t`: hash for use in unordered containers

**Hash constants:**
- `kHashBoolFlag` — sentinel hash bit for boolean flag discrimination
- `kHashKindMix` — Murmur-like mixing constant
- `kHashNullSentinel` — sentinel hash value for null pointers
- `kHashPhiMix` — golden ratio fractional constant for hash mixing

### `Param` (`Param.hpp`)

Describes a function or basic block parameter (the IL's phi-node equivalent).

**Fields:**
- `Attrs` — `ParamAttrs`: attribute bundle for aliasing and lifetime hints
- `id` — `unsigned`: ordinal identifier assigned during IR construction
- `name` — `std::string`: name used for diagnostics; may be empty
- `type` — `Type`: static type of the parameter

**Methods:**
- `isNoAlias() const` — `bool`: query `noalias` attribute
- `isNoCapture() const` — `bool`: query `nocapture` attribute
- `isNonNull() const` — `bool`: query `nonnull` attribute
- `setNoAlias(value)` — set `noalias` attribute
- `setNoCapture(value)` — set `nocapture` attribute
- `setNonNull(value)` — set `nonnull` attribute

### `ParamAttrs` (`Param.hpp`)

Aliasing and lifetime hints for a parameter.

**Fields:**
- `noalias` — `bool`: parameter does not alias any other pointer argument (default: `false`)
- `nocapture` — `bool`: parameter value is not captured beyond the callee (default: `false`)
- `nonnull` — `bool`: parameter is guaranteed never to be null (default: `false`)

## Declarations

| File         | Purpose                                           |
|--------------|---------------------------------------------------|
| `Extern.cpp` | Extern declaration implementation                 |
| `Extern.hpp` | Extern declaration: name, return type, parameters |
| `Global.cpp` | Global variable/constant implementation           |
| `Global.hpp` | Global variable/constant: name, type, initializer |

### `Extern` (`Extern.hpp`)

External function declaration providing a type-checked interface to foreign functions.

**Fields:**
- `name` — `std::string`: identifier of the external function; unique among externs in a module
- `params` — `std::vector<Type>`: ordered parameter types forming the extern's signature
- `retType` — `Type`: declared return type

### `Global` (`Global.hpp`)

Module-scope variable or constant accessible to all functions within a module.

**Fields:**
- `init` — `std::string`: serialized initializer data; non-empty only for constant-initialized globals
- `name` — `std::string`: identifier, unique among all globals in the module
- `type` — `Type`: declared IL type

## Opcodes

| File             | Purpose                                                              |
|------------------|----------------------------------------------------------------------|
| `Opcode.def`     | X-macro table defining all IL opcodes (name, arity, types, dispatch) |
| `Opcode.hpp`     | Central `Opcode` enumeration for all IL operations                   |
| `OpcodeInfo.cpp` | Opcode metadata implementation                                       |
| `OpcodeInfo.hpp` | Opcode metadata: operand counts, result arity, effects, dispatch     |
| `OpcodeNames.cpp`| Opcode to/from mnemonic string conversion                            |

### `Opcode` (`Opcode.hpp`)

Enum class enumerating all IL operation codes, generated from `Opcode.def` via X-macros.

**Key constant:**
- `kNumOpcodes` — `constexpr size_t`: total number of opcodes (`static_cast<size_t>(Opcode::Count)`)

**Free function:**
- `toString(op)` — `const char *`: convert opcode to its lowercase mnemonic string

**Selected opcode list** (from `Opcode.def`):

Arithmetic: `Add`, `Sub`, `Mul`, `SDiv`, `UDiv`, `SRem`, `URem`, `IAddOvf`, `ISubOvf`, `IMulOvf`, `SDivChk0`, `UDivChk0`, `SRemChk0`, `URemChk0`, `IdxChk`

Bitwise: `And`, `AShr`, `LShr`, `Or`, `Shl`, `Xor`

Floating-point: `FAdd`, `FDiv`, `FMul`, `FSub`

Integer comparisons: `ICmpEq`, `ICmpNe`, `SCmpGE`, `SCmpGT`, `SCmpLE`, `SCmpLT`, `UCmpGE`, `UCmpGT`, `UCmpLE`, `UCmpLT`

Floating-point comparisons: `FCmpEQ`, `FCmpGE`, `FCmpGT`, `FCmpLE`, `FCmpLT`, `FCmpNE`, `FCmpOrd`, `FCmpUno`

Casts: `CastFpToSiRteChk`, `CastFpToUiRteChk`, `CastSiNarrowChk`, `CastSiToFp`, `CastUiNarrowChk`, `CastUiToFp`, `Fptosi`, `Sitofp`, `Trunc1`, `Zext1`

Memory: `Alloca`, `GEP`, `Load`, `Store`, `AddrOf`

Constants: `ConstF64`, `ConstNull`, `ConstStr`, `GAddr`

Control flow: `Br`, `CBr`, `Ret`, `SwitchI32`, `Trap`, `TrapFromErr`

Calls: `Call`, `CallIndirect`

Error/exception: `EhEntry`, `EhPop`, `EhPush`, `ErrGetCode`, `ErrGetIp`, `ErrGetKind`, `ErrGetLine`, `ResumeLabel`, `ResumeNext`, `ResumeSame`, `TrapErr`, `TrapKind`

### `OpcodeInfo` (`OpcodeInfo.hpp`)

Static description of an opcode's signature and behaviour.

**`ResultArity` enum:**
- `None` — instruction never produces a result
- `One` — instruction must produce exactly one result
- `Optional` — instruction may omit or provide a result

**`TypeCategory` enum:**
- `Any`, `Dynamic`, `Error`, `F64`, `I1`, `I16`, `I32`, `I64`, `InstrType`, `None`, `Ptr`, `ResumeTok`, `Str`, `Void`

**`MemoryEffects` enum:**
- `None`, `Read`, `ReadWrite`, `Unknown`, `Write`

**`VMDispatch` enum:**
- Identifier selecting the VM interpreter dispatch handler for each opcode.

**`OperandParseKind` enum:**
- `BranchTarget`, `Call`, `None`, `Switch`, `TypeImmediate`, `Value`

**`OpcodeInfo` struct fields:**
- `hasSideEffects` — `bool`
- `isTerminator` — `bool`
- `name` — `const char *`: canonical mnemonic
- `numOperandsMax` — `uint8_t`: maximum operand count or `kVariadicOperandCount`
- `numOperandsMin` — `uint8_t`: minimum operand count
- `numSuccessors` — `uint8_t`
- `operandTypes` — `std::array<TypeCategory, kMaxOperandCategories>`: operand constraints
- `parse` — `std::array<OperandParseSpec, kMaxOperandParseEntries>`: textual parsing recipe
- `resultArity` — `ResultArity`
- `resultType` — `TypeCategory`
- `vmDispatch` — `VMDispatch`

**Constants:**
- `kMaxOperandCategories` — `constexpr size_t = 3`
- `kMaxOperandParseEntries` — `constexpr size_t = 4`
- `kVariadicOperandCount` — `constexpr uint8_t = UINT8_MAX`: sentinel for variadic operand arity

**Free functions:**
- `all_opcodes()` — `const std::vector<Opcode> &`: enumerate all opcodes in declaration order
- `getOpcodeInfo(op)` — `const OpcodeInfo &`: access metadata for a specific opcode
- `hasMemoryRead(op)` — `bool`: true if opcode may read memory (inline)
- `hasMemoryWrite(op)` — `bool`: true if opcode may write memory (inline)
- `isVariadicOperandCount(value)` — `bool`
- `isVariadicSuccessorCount(value)` — `bool`
- `memoryEffects(op)` — `MemoryEffects`: conservative memory interaction classification
- `opcode_mnemonic(op)` — `std::string`: canonical mnemonic string for an opcode

**Global:**
- `kOpcodeTable` — `const std::array<OpcodeInfo, kNumOpcodes>`: metadata table indexed by `Opcode` enumerators

## Forward Declarations

| File      | Purpose                                          |
|-----------|--------------------------------------------------|
| `fwd.hpp` | Forward declarations to minimize header coupling |

Declares (incomplete types only): `Module`, `Function`, `BasicBlock`, `Instr`, `Value` — all in namespace `il::core`.
