---
status: active
audience: contributors
last-verified: 2026-07-26
---

# How to Extend the Zanna Runtime

Complete implementation guide for adding new classes, methods, and static functions to the Zanna runtime library. This document walks through every step required to expose new functionality to Zanna programs.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture Overview](#2-architecture-overview)
3. [Quick Start: Adding a Simple Function](#3-quick-start-adding-a-simple-function)
4. [Adding a New Runtime Class](#4-adding-a-new-runtime-class)
5. [Definition File Syntax](#5-definition-file-syntax)
6. [Code Generation with rtgen](#6-code-generation-with-rtgen)
7. [Frontend Integration](#7-frontend-integration)
8. [CMake Build Integration](#8-cmake-build-integration)
9. [Testing Your Extension](#9-testing-your-extension)
10. [Complete Example: Counter Class](#10-complete-example-counter-class)
11. [Common Patterns](#11-common-patterns)
12. [Troubleshooting](#12-troubleshooting)
13. [Reference Materials](#13-reference-materials)

---

## 1. Introduction

### What You'll Learn

This guide teaches you how to extend the Zanna runtime with:

- **Static functions**: Standalone utility functions (like `Zanna.Math.Sin`)
- **Classes with methods**: Object-oriented types (like `Zanna.Collections.Map`)
- **Properties**: Getter/setter pairs on class instances
- **Constructor functions**: Factory functions for creating class instances

### When to Extend the Runtime

Extend the runtime when you need to:

- Expose platform-specific functionality (file I/O, networking, graphics)
- Provide high-performance operations implemented in C
- Add new data structures or algorithms
- Wrap platform APIs or in-tree C implementations. Zanna is zero-dependency, so do not add product dependencies on external libraries.

### Prerequisites

- Familiarity with C programming
- Basic understanding of the Zanna build system (CMake)
- Knowledge of Zanna IL type system (see [IL Guide](../il/il-guide.md))

---

## 2. Architecture Overview

### The Runtime Extension Pipeline

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                        RUNTIME EXTENSION PIPELINE                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  1. C Implementation        2. Definition File       3. Code Generation     │
│  ┌─────────────────┐        ┌─────────────────┐      ┌─────────────────┐    │
│  │ core/rt_counter.c│       │ runtime.def     │      │ rtgen tool      │    │
│  │ core/rt_counter.h│──────▶│ RT_FUNC(...)    │─────▶│ (build time)    │    │
│  │                 │        │ RT_CLASS_BEGIN  │      │                 │    │
│  └─────────────────┘        └─────────────────┘      └────────┬────────┘    │
│                                                                │             │
│                                                                ▼             │
│  4. Generated Headers       5. Frontend Integration  6. Usage in Zanna      │
│  ┌─────────────────┐        ┌─────────────────┐      ┌─────────────────┐    │
│  │ RuntimeNameMap  │        │ RuntimeRegistry │      │ Dim c = Counter │    │
│  │ RuntimeClasses  │───────▶│ + frontend      │─────▶│ c.Increment()   │    │
│  │ RuntimeSigs     │        │ adapters        │      │ Print c.Value   │    │
│  └─────────────────┘        └─────────────────┘      └─────────────────┘    │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **Runtime Library** | `src/runtime/` | C implementation of runtime functions |
| **Definition File** | `src/il/runtime/runtime.def` | Single source of truth for all runtime metadata |
| **rtgen Tool** | `src/tools/rtgen/` | Build-time code generator |
| **Generated Headers** | `build/generated/il/runtime/` | Auto-generated `.inc` and `.hpp` files |
| **RuntimeClasses** | `src/il/runtime/classes/` | C++ wrappers for OOP integration |

### Naming Conventions

| Entity | Convention | Example |
|--------|------------|---------|
| C source file | `src/runtime/<component>/rt_<module>.c` | `src/runtime/core/rt_counter.c` |
| C header file | `src/runtime/<component>/rt_<module>.h` | `src/runtime/core/rt_counter.h` |
| C function | `rt_<module>_<action>` | `rt_counter_new` |
| Canonical name | `Zanna.<Namespace>.<Class>.<Method>` | `Zanna.Utils.Counter.New` |
| Definition ID | PascalCase unique identifier | `CounterNew` |

---

## 3. Quick Start: Adding a Simple Function

Let's add a simple static function: `Zanna.Utils.Square` that squares an integer.

### Step 1: Add the C Implementation

Create or edit `src/runtime/core/rt_math.c` (or a new file in the appropriate `src/runtime/<component>/` subdirectory):

```c
/// @brief Squares an integer value.
/// @param n The integer to square.
/// @return n * n
int64_t rt_math_square(int64_t n)
{
    return n * n;
}
```

Add the declaration to `src/runtime/core/rt_math.h`:

```c
/// @brief Squares an integer value.
int64_t rt_math_square(int64_t n);
```

### Step 2: Add the Definition

Edit `src/il/runtime/runtime.def` and add in the appropriate section:

```c
//=============================================================================
// MATH FUNCTIONS
//=============================================================================

// ... existing functions ...

RT_FUNC(MathSquare, rt_math_square, "Zanna.Math.Square", "i64(i64)")
```

The `RT_FUNC` macro parameters are:
1. **id**: Unique C++ identifier (`MathSquare`)
2. **c_symbol**: C function name (`rt_math_square`)
3. **canonical**: Zanna namespace path (`Zanna.Math.Square`)
4. **signature**: IL type signature (`i64(i64)`)

### Step 3: Regenerate Code

```bash
ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh
```

Use the platform build script (`build_zanna_linux.sh`, `build_zanna_mac.sh`, or
`build_zanna_win.ps1`). The build system automatically runs `rtgen` when
`runtime.def` changes.

### Step 4: Use in Zanna

**BASIC:**
```basic
DIM result AS INTEGER
result = Zanna.Math.Square(5)
PRINT result  ' Outputs: 25
```

**Zia:**
```zia
module Main;
bind Zanna.Terminal;

func start() {
    var result = Zanna.Math.Square(5);
    SayInt(result);  // Outputs: 25
}
```

---

## 4. Adding a New Runtime Class

Classes are more complex than standalone functions. They require:
- A constructor function (or `none` for static utility classes)
- Instance methods (operate on `self`)
- Properties (getters and optional setters)

### Class Categories

| Category | Constructor | Example |
|----------|-------------|---------|
| **Instance Class** | Has constructor | `Zanna.Collections.Map` |
| **Static Utility** | `none` | `Zanna.DateTime` |

### Internal Structure Pattern

Runtime classes typically use an opaque struct:

```c
// rt_counter.c

typedef struct
{
    int64_t value;      // Current counter value
    int64_t step;       // Increment step size
} ZannaCounter;
```

The struct is allocated via `rt_obj_new_i64()`, which returns zeroed
runtime-managed heap storage with reference-count bookkeeping.

---

## 5. Definition File Syntax

### Location

`src/il/runtime/runtime.def`

### RT_FUNC - Standalone Functions

```c
RT_FUNC(id, c_symbol, "canonical", "signature")
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `id` | Unique C++ identifier | `MathSin` |
| `c_symbol` | C function name | `rt_math_sin` |
| `canonical` | Zanna namespace path | `"Zanna.Math.Sin"` |
| `signature` | IL type signature | `"f64(f64)"` |

Aliases are intentionally unsupported. When a runtime API is renamed, update
callers to the canonical `RT_FUNC` name instead of adding a compatibility entry.

### RT_CLASS_BEGIN/END - Class Definition

```c
RT_CLASS_BEGIN("name", type_id, "layout", ctor_id)
    // properties and methods go here
RT_CLASS_END()
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `name` | Fully qualified class name | `"Zanna.Utils.Counter"` |
| `type_id` | Type identifier suffix | `Counter` |
| `layout` | Memory layout type | `"obj"` |
| `ctor_id` | Constructor function ID or `none` | `CounterNew` |

### RT_PROP - Properties

```c
RT_PROP("name", "type", getter_id, setter_id_or_none)
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `name` | Property name (PascalCase) | `"Value"` |
| `type` | IL type | `"i64"` |
| `getter_id` | Getter function ID | `CounterGetValue` |
| `setter_id_or_none` | Setter function ID or `none` | `CounterSetValue` |

**Getter convention:** `"Zanna.Class.get_PropName"` canonical name
**Setter convention:** `"Zanna.Class.set_PropName"` canonical name

### RT_METHOD - Methods

```c
RT_METHOD("name", "signature", target_id)
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `name` | Method name (PascalCase) | `"Increment"` |
| `signature` | Signature WITHOUT receiver | `"void()"` |
| `target_id` | Implementation function ID | `CounterIncrement` |

**Important:** Instance-method signatures omit the implicit `self` (receiver) parameter. The C function for an instance method must still accept that receiver as its first argument. Static/factory methods have no receiver; their `RT_FUNC` parameter count matches the `RT_METHOD` signature, and the frontend runtime-method index records them as receiverless.

### Type Abbreviations

| Abbreviation | IL Type | C Type |
|--------------|---------|--------|
| `void` | No return value | `void` |
| `i1` | Boolean | `int8_t` (0 or 1) |
| `i16` | 16-bit signed | `int16_t` |
| `i32` | 32-bit signed | `int32_t` |
| `i64` | 64-bit signed | `int64_t` |
| `f64` | 64-bit float | `double` |
| `str` | String | `rt_string` |
| `obj` | Object reference | `void*` |
| `ptr` | Raw pointer | `void*` |
| `bool` | Boolean alias | `int8_t` (0 or 1) |
| `string` | String alias | `rt_string` |
| `resume` / `resume_tok` | Resume token | VM resume token |

`i16` and `i32` are accepted by the runtime signature parser, but new frontend-visible APIs should normally use `i64` for integer values and `f64` for numbers. `i8` and `f32` are legacy catalog-comment tokens, not accepted by the current `RT_FUNC` signature parser.

Use parameterized signatures whenever the runtime object type is known:

| Signature | Meaning |
|-----------|---------|
| `obj<Zanna.Collections.Bytes>` | A typed runtime object |
| `obj<Zanna.Option>` | An Option object |
| `seq<str>` | A sequence of strings |
| `seq<obj>` | A sequence of runtime objects |

### Raw Pointer Policy

`ptr` is reserved for native implementation details. Do not expose a new `ptr`
return or parameter to frontend languages; add a typed wrapper instead.

Preferred replacements:

- Return `obj<Runtime.Class>` for managed handles instead of `ptr`.
- Return `seq<T>` for arrays/lists of values instead of raw buffers.
- Return `obj<Zanna.Option>` or `obj<Zanna.Result>` instead of using out parameters.
- Accept typed runtime classes such as `obj<Zanna.Graphics.Path2D>` instead of raw coordinate buffers.
- For callbacks, add a managed bridge that takes an explicit `&function` and a typed/object payload; keep native callback pointers inside the runtime implementation.

Frontend builds reject raw pointer APIs. If a legacy C ABI must remain, add a safe wrapper beside it and document the wrapper in the diagnostic alternative map and tests.

---

## 6. Code Generation with rtgen

### What rtgen Does

The `rtgen` tool reads `runtime.def` and generates five output files:

| File | Purpose |
|------|---------|
| `RuntimeNameMap.inc` | Maps canonical names to C symbols for native codegen |
| `RuntimeClasses.inc` | Class/method/property catalog for OOP dispatch |
| `RuntimeSignatures.inc` | Function signatures for type checking |
| `RuntimeNames.hpp` | Runtime name constants |
| `ZiaRuntimeExterns.inc` | Extern declarations for the Zia frontend |

### Generated Output Example

For a function defined as:
```c
RT_FUNC(CounterNew, rt_counter_new, "Zanna.Utils.Counter.New", "obj()")
```

`RuntimeNameMap.inc` generates:
```c
RUNTIME_NAME_ALIAS("Zanna.Utils.Counter.New", "rt_counter_new")
```

### When rtgen Runs

The build system detects changes to `runtime.def` and reruns rtgen automatically:

```cmake
# In src/CMakeLists.txt
set(RTGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/il/runtime")

add_custom_command(
    OUTPUT ${RTGEN_OUTPUT_DIR}/RuntimeNameMap.inc
           ${RTGEN_OUTPUT_DIR}/RuntimeClasses.inc
           ${RTGEN_OUTPUT_DIR}/RuntimeSignatures.inc
           ${RTGEN_OUTPUT_DIR}/RuntimeNames.hpp
           ${RTGEN_OUTPUT_DIR}/ZiaRuntimeExterns.inc
    COMMAND rtgen ${RTGEN_INPUT} ${RTGEN_OUTPUT_DIR}
    DEPENDS rtgen ${RTGEN_INPUT}
)
```

### Manual Regeneration

```bash
./build/src/rtgen src/il/runtime/runtime.def build/generated/il/runtime/
```

---

## 7. Frontend Integration

### How Frontends Discover Runtime Functions

Both BASIC and Zia frontends use the generated runtime metadata to:

1. **Resolve canonical names** to C symbols
2. **Type-check** method calls against signatures
3. **Emit correct IL** for runtime calls

### BASIC Frontend: RuntimeMethodIndex

The BASIC frontend uses `RuntimeMethodIndex` (`src/frontends/basic/sem/RuntimeMethodIndex.cpp`),
which delegates through the shared `RuntimeMethodResolver` to the IL-layer `RuntimeRegistry`:

```cpp
std::optional<RuntimeMethodInfo> RuntimeMethodIndex::find(
    std::string_view classQName, std::string_view method, std::size_t arity) const
{
    auto resolved =
        il::frontends::common::RuntimeMethodResolver::instance().find(classQName, method, arity);
    // Convert IL types to BASIC types and return RuntimeMethodInfo.
}
```

### Zia Frontend: Direct Lowering

Zia imports runtime metadata during semantic analysis (`src/frontends/zia/Sema_Runtime.cpp`)
and emits extern declarations for used runtime calls during lowering (`src/frontends/zia/Lowerer.cpp`):

```cpp
// Emit extern declarations for used runtime functions
const auto *desc = il::runtime::findRuntimeDescriptor(externName);
if (desc)
    builder_->addExtern(std::string(desc->name),
                        desc->signature.retType,
                        desc->signature.paramTypes);
```

### Adding Support for New Functions

After adding a function to `runtime.def`:

1. **No additional frontend changes needed** for static functions called by canonical name
2. **For new classes**, ensure frontends can resolve the class type (usually automatic)
3. **For special syntax**, may need parser/lowerer changes (rare)

---

## 8. CMake Build Integration

### Runtime Library Structure

The runtime is organized into component libraries in `src/runtime/CMakeLists.txt`:

```cmake
set(RT_BASE_SOURCES
    core/rt_context.c
    core/rt_error.c
    # ... core functions
)

set(RT_COLLECTIONS_SOURCES
    collections/rt_map.c
    collections/rt_list.c
    # ... collection types
)

# Object libraries (for compilation)
add_library(zanna_rt_base_obj OBJECT ${RT_BASE_SOURCES})
add_library(zanna_rt_collections_obj OBJECT ${RT_COLLECTIONS_SOURCES})

# Static libraries (for linking)
add_library(zanna_rt_base STATIC $<TARGET_OBJECTS:zanna_rt_base_obj>)
add_library(zanna_rt_collections STATIC $<TARGET_OBJECTS:zanna_rt_collections_obj>)

# Combined runtime library
add_library(zanna_runtime STATIC
    $<TARGET_OBJECTS:zanna_rt_base_obj>
    $<TARGET_OBJECTS:zanna_rt_collections_obj>
    # ... all component object libraries
)
```

### Adding a New Source File

1. Identify the appropriate component (base, collections, text, io, etc.)
2. Add the source file to the corresponding `RT_*_SOURCES` list:

```cmake
set(RT_BASE_SOURCES
    # ... existing sources ...
    core/rt_counter.c    # Add your new file
)
```

3. If creating a new component, add new `OBJECT` and `STATIC` library definitions

### Adding a Public Header

Add to `RT_PUBLIC_HEADERS`:

```cmake
set(RT_PUBLIC_HEADERS
    # ... existing headers ...
    ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_counter.h
)
```

---

## 9. Testing Your Extension

### Unit Tests

Create a unit test in `src/tests/unit/runtime/` for C runtime behavior, or
`src/tests/unit/runtime_classes/` for catalog/registry behavior:

```cpp
// TestRuntimeCounter.cpp
#include <gtest/gtest.h>

extern "C" {
#include "core/rt_counter.h"
}

TEST(RuntimeCounter, NewCreatesZeroCounter)
{
    void *counter = rt_counter_new();
    EXPECT_EQ(rt_counter_get_value(counter), 0);
}

TEST(RuntimeCounter, IncrementAddsOne)
{
    void *counter = rt_counter_new();
    rt_counter_increment(counter);
    EXPECT_EQ(rt_counter_get_value(counter), 1);
}
```

### Integration Tests (Golden Tests)

Create BASIC runtime golden tests under `src/tests/golden/<suite>/` and register
them in `src/tests/golden/CMakeLists.txt`. Runtime API programs written in Zia
live under `src/tests/fixtures/runtime/` and are wired from `src/tests/CMakeLists.txt`.

**`counter_test.bas`:**
```basic
DIM c AS Counter
c = Counter.New()
PRINT c.Value      ' Expected: 0
c.Increment()
PRINT c.Value      ' Expected: 1
```

**`counter_test.expected`:**
```text
0
1
```

### Running Tests

```bash
# Build and run all tests
ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh

# Run specific test
ctest --test-dir build -R counter
```

---

## 10. Complete Example: Counter Class

Let's implement a complete `Zanna.Utils.Counter` class with:
- Constructor: `Counter.New()` and `Counter.NewWithStep(step)`
- Properties: `Value` (read-only), `Step` (read/write)
- Methods: `Increment()`, `Decrement()`, `Reset()`

### Step 1: Create rt_counter.h

```c
//===----------------------------------------------------------------------===//
// File: src/runtime/core/rt_counter.h
// Purpose: Simple counter class for demonstration.
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief Create a new counter with default step of 1.
    /// @return Pointer to new counter object.
    void *rt_counter_new(void);

    /// @brief Create a new counter with specified step.
    /// @param step The increment/decrement step size.
    /// @return Pointer to new counter object.
    void *rt_counter_new_with_step(int64_t step);

    /// @brief Get the current counter value.
    /// @param obj Counter pointer.
    /// @return Current value.
    int64_t rt_counter_get_value(void *obj);

    /// @brief Get the step size.
    /// @param obj Counter pointer.
    /// @return Step size.
    int64_t rt_counter_get_step(void *obj);

    /// @brief Set the step size.
    /// @param obj Counter pointer.
    /// @param step New step size.
    void rt_counter_set_step(void *obj, int64_t step);

    /// @brief Increment the counter by step.
    /// @param obj Counter pointer.
    void rt_counter_increment(void *obj);

    /// @brief Decrement the counter by step.
    /// @param obj Counter pointer.
    void rt_counter_decrement(void *obj);

    /// @brief Reset the counter to zero.
    /// @param obj Counter pointer.
    void rt_counter_reset(void *obj);

#ifdef __cplusplus
}
#endif
```

### Step 2: Create rt_counter.c

```c
//===----------------------------------------------------------------------===//
// File: src/runtime/core/rt_counter.c
// Purpose: Simple counter class implementation.
//===----------------------------------------------------------------------===//

#include "rt_counter.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <stdint.h>

/// @brief Internal counter structure.
typedef struct
{
    int64_t value; ///< Current counter value.
    int64_t step;  ///< Increment/decrement step size.
} ZannaCounter;

void *rt_counter_new(void)
{
    ZannaCounter *counter = (ZannaCounter *)rt_obj_new_i64(0, (int64_t)sizeof(ZannaCounter));
    if (!counter)
    {
        rt_trap("Counter: memory allocation failed");
        return NULL;
    }

    counter->value = 0;
    counter->step = 1;

    return counter;
}

void *rt_counter_new_with_step(int64_t step)
{
    ZannaCounter *counter = (ZannaCounter *)rt_obj_new_i64(0, (int64_t)sizeof(ZannaCounter));
    if (!counter)
    {
        rt_trap("Counter: memory allocation failed");
        return NULL;
    }

    counter->value = 0;
    counter->step = step;

    return counter;
}

int64_t rt_counter_get_value(void *obj)
{
    return ((ZannaCounter *)obj)->value;
}

int64_t rt_counter_get_step(void *obj)
{
    return ((ZannaCounter *)obj)->step;
}

void rt_counter_set_step(void *obj, int64_t step)
{
    ((ZannaCounter *)obj)->step = step;
}

void rt_counter_increment(void *obj)
{
    ZannaCounter *counter = (ZannaCounter *)obj;
    counter->value += counter->step;
}

void rt_counter_decrement(void *obj)
{
    ZannaCounter *counter = (ZannaCounter *)obj;
    counter->value -= counter->step;
}

void rt_counter_reset(void *obj)
{
    ((ZannaCounter *)obj)->value = 0;
}
```

### Step 3: Add to runtime.def

Add to `src/il/runtime/runtime.def` in an appropriate section:

```c
//=============================================================================
// UTILS - COUNTER
//=============================================================================

RT_FUNC(CounterNew,         rt_counter_new,           "Zanna.Utils.Counter.New",           "obj()")
RT_FUNC(CounterNewWithStep, rt_counter_new_with_step, "Zanna.Utils.Counter.NewWithStep",   "obj(i64)")
RT_FUNC(CounterGetValue,    rt_counter_get_value,     "Zanna.Utils.Counter.get_Value",     "i64(obj)")
RT_FUNC(CounterGetStep,     rt_counter_get_step,      "Zanna.Utils.Counter.get_Step",      "i64(obj)")
RT_FUNC(CounterSetStep,     rt_counter_set_step,      "Zanna.Utils.Counter.set_Step",      "void(obj,i64)")
RT_FUNC(CounterIncrement,   rt_counter_increment,     "Zanna.Utils.Counter.Increment",     "void(obj)")
RT_FUNC(CounterDecrement,   rt_counter_decrement,     "Zanna.Utils.Counter.Decrement",     "void(obj)")
RT_FUNC(CounterReset,       rt_counter_reset,         "Zanna.Utils.Counter.Reset",         "void(obj)")

// Class definition for OOP dispatch
RT_CLASS_BEGIN("Zanna.Utils.Counter", Counter, "obj", CounterNew)
    RT_PROP("Value", "i64", CounterGetValue, none)
    RT_PROP("Step", "i64", CounterGetStep, CounterSetStep)
    RT_METHOD("Increment", "void()", CounterIncrement)
    RT_METHOD("Decrement", "void()", CounterDecrement)
    RT_METHOD("Reset", "void()", CounterReset)
RT_CLASS_END()
```

### Step 4: Update CMakeLists.txt

Edit `src/runtime/CMakeLists.txt`:

```cmake
set(RT_BASE_SOURCES
    # ... existing sources ...
    core/rt_counter.c
)

set(RT_PUBLIC_HEADERS
    # ... existing headers ...
    ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_counter.h
)
```

### Step 5: Build and Test

```bash
# Build
ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh

# Verify the function is registered
rg "Counter" build/generated/il/runtime/

# Run tests
ctest --test-dir build --output-on-failure
```

### Step 6: Use in Zanna Programs

**BASIC:**
```basic
' Create a counter
DIM c AS Counter
c = Counter.New()

' Use methods
c.Increment()
c.Increment()
PRINT "Value: "; c.Value   ' Output: Value: 2

' Change step
c.Step = 5
c.Increment()
PRINT "Value: "; c.Value   ' Output: Value: 7

' Reset
c.Reset()
PRINT "Value: "; c.Value   ' Output: Value: 0
```

**Zia:**
```zia
module Main;
bind Zanna.Terminal;

func start() {
    // Create a counter
    var c = Zanna.Utils.Counter.New();

    // Use methods
    c.Increment();
    c.Increment();
    SayInt(c.Value);  // Output: 2

    // Change step
    c.Step = 5;
    c.Increment();
    SayInt(c.Value);  // Output: 7

    // Reset
    c.Reset();
    SayInt(c.Value);  // Output: 0
}
```

---

## 11. Common Patterns

### Pattern 1: Static Utility Class

For classes with no instances (all static methods):

```c
// runtime.def
RT_CLASS_BEGIN("Zanna.Math", Math, "obj", none)  // note: ctor is 'none'
    RT_METHOD("Sin", "f64(f64)", MathSin)
    RT_METHOD("Cos", "f64(f64)", MathCos)
    RT_METHOD("Sqrt", "f64(f64)", MathSqrt)
RT_CLASS_END()
```

### Pattern 2: Factory Methods

For classes with multiple constructors:

```c
// runtime.def
RT_FUNC(F64BufNew,     rt_f64buf_new,      "Zanna.Collections.F64Buffer.New",     "obj(i64)")
RT_FUNC(F64BufFromSeq, rt_f64buf_from_seq, "Zanna.Collections.F64Buffer.FromSeq", "obj<Zanna.Collections.F64Buffer>(obj)")

RT_CLASS_BEGIN("Zanna.Collections.F64Buffer", F64Buffer, "obj(i64)", F64BufNew)
    // F64BufNew is the default constructor.
    // F64BufFromSeq is a static factory method.
    RT_METHOD("FromSeq", "obj<Zanna.Collections.F64Buffer>(obj)", F64BufFromSeq)
    // ... other methods
RT_CLASS_END()
```

### Pattern 3: Read-Only Properties

```c
RT_PROP("Length", "i64", ArrayGetLength, none)  // no setter
```

### Pattern 4: Read-Write Properties

```c
RT_PROP("Capacity", "i64", ArrayGetCapacity, ArraySetCapacity)
```

### Pattern 5: Methods Returning Self (Fluent API)

```c
// C implementation
void *rt_builder_append(void *obj, const char *text)
{
    // ... append logic ...
    return obj;  // Return self for chaining
}

// runtime.def
RT_METHOD("Append", "obj(str)", BuilderAppend)
```

Usage:
```basic
builder = Zanna.Text.StringBuilder.Append(builder, "Hello")
builder = Zanna.Text.StringBuilder.Append(builder, " ")
builder = Zanna.Text.StringBuilder.Append(builder, "World")
```

### Pattern 6: Optional Behavior Without Overloads

Runtime canonical names must be unique. For optional behavior, expose distinct
canonical names or implement frontend syntax/lowering that chooses one runtime
helper:

```c
RT_FUNC(BytesNew,      rt_bytes_new,      "Zanna.Collections.Bytes.New",      "obj(i64)")
RT_FUNC(BytesFromStr,  rt_bytes_from_str, "Zanna.Collections.Bytes.FromStr",  "obj(str)")
```

### Pattern 7: Error Handling with Traps

```c
void *rt_list_get(void *obj, int64_t index)
{
    ZannaList *list = (ZannaList *)obj;
    if (index < 0 || index >= list->count)
    {
        char message[128];
        snprintf(message,
                 sizeof(message),
                 "List index out of bounds: %lld (size: %lld)",
                 (long long)index,
                 (long long)list->count);
        rt_trap(message);
        return NULL;  // Unreachable after trap
    }
    return list->items[index];
}
```

---

## 12. Troubleshooting

### "Unknown runtime function" Error

**Symptom:** Compiler reports unknown function when calling `Zanna.X.Y`

**Causes:**
1. Function not added to `runtime.def`
2. Typo in canonical name
3. Build not regenerated after editing `runtime.def`

**Solution:**
```bash
# Force regeneration
rm -rf build/generated/il/runtime/
ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh
```

### "Signature mismatch" Error

**Symptom:** Type error when calling runtime function

**Cause:** Signature in `runtime.def` doesn't match C function

**Solution:** Verify parameter types match exactly:
- `i64` = `int64_t`
- `f64` = `double`
- `str` = `rt_string`
- `obj` = `void*`

### Linker Error: "undefined reference to rt_xxx"

**Symptom:** Link fails with undefined symbol

**Causes:**
1. Source file not added to CMakeLists.txt
2. Function declared but not implemented
3. Missing `extern "C"` in header (for C++ callers)

**Solution:**
1. Verify source file is in appropriate `RT_*_SOURCES` list
2. Check function implementation exists
3. Ensure header has `extern "C"` wrapper

### Class Methods Not Found

**Symptom:** Method calls work for static functions but not on class instances

**Cause:** Missing `RT_CLASS_BEGIN/END` block or method not listed inside

**Solution:** Verify class definition includes all methods:
```c
RT_CLASS_BEGIN("Zanna.Utils.Counter", Counter, "obj", CounterNew)
    RT_METHOD("Increment", "void()", CounterIncrement)  // Must be inside the block
RT_CLASS_END()
```

### Property Getter/Setter Not Working

**Symptom:** Property access fails or returns wrong type

**Cause:** Canonical name convention not followed

**Solution:** Use exact conventions:
- Getter: `"Zanna.Class.get_PropertyName"`
- Setter: `"Zanna.Class.set_PropertyName"`

---

## 13. Reference Materials

### Related Documentation

- **[Architecture](architecture.md)** - System architecture overview
- **[Codemap](codemap.md)** - Codebase navigation
- **[Frontend How-To](frontend-howto.md)** - Building language frontends
- **[IL Guide](../il/il-guide.md)** - IL type system and instruction reference

### Key Source Files

| File | Purpose |
|------|---------|
| `src/il/runtime/classes/RuntimeClasses.hpp` | C++ wrapper for class metadata |
| `src/il/runtime/runtime.def` | Single source of truth for runtime metadata |
| `src/runtime/CMakeLists.txt` | Runtime build configuration |
| `src/runtime/rt_internal.h` | Internal runtime utilities |
| `src/runtime/oop/rt_object.h` | Object allocation and reference-counted lifetime |
| `src/tools/rtgen/rtgen.cpp` | Code generator implementation |

### Example Implementations

Study these existing implementations as references:

| Class | Location | Complexity |
|-------|----------|------------|
| `Zanna.Collections.Map` | `src/runtime/collections/rt_map.c` | Complex (data structure) |
| `Zanna.IO.File` | `src/runtime/io/rt_file.c` | Complex (OS integration) |
| `Zanna.Text.Uuid` | `src/runtime/text/rt_guid.c` | Simple (static utility) |
| `Zanna.Time.Stopwatch` | `src/runtime/core/rt_stopwatch.c` | Medium (instance class) |

---

## Appendix: Quick Reference Card

### Adding a Static Function

```c
// 1. Implement in src/runtime/<component>/rt_module.c
int64_t rt_module_func(int64_t arg) { ... }

// 2. Declare in src/runtime/<component>/rt_module.h
int64_t rt_module_func(int64_t arg);

// 3. Add to runtime.def
RT_FUNC(ModuleFunc, rt_module_func, "Zanna.Module.Func", "i64(i64)")

// 4. Add source to CMakeLists.txt (if new file)
// 5. Build: ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh
```

### Adding a New Class

```c
// 1. Implement constructor and methods
void *rt_myclass_new(void) { ... }
void rt_myclass_do_thing(void *obj) { ... }
int64_t rt_myclass_get_value(void *obj) { ... }

// 2. Add RT_FUNCs for all functions
RT_FUNC(MyClassNew, rt_myclass_new, "Zanna.MyClass.New", "obj()")
RT_FUNC(MyClassDoThing, rt_myclass_do_thing, "Zanna.MyClass.DoThing", "void(obj)")
RT_FUNC(MyClassGetValue, rt_myclass_get_value, "Zanna.MyClass.get_Value", "i64(obj)")

// 3. Add RT_CLASS_BEGIN/END block
RT_CLASS_BEGIN("Zanna.MyClass", MyClass, "obj", MyClassNew)
    RT_PROP("Value", "i64", MyClassGetValue, none)
    RT_METHOD("DoThing", "void()", MyClassDoThing)
RT_CLASS_END()
```

### Type Mapping Quick Reference

| IL | C | BASIC | Zia |
|----|---|-------|-----|
| `i64` | `int64_t` | `Integer` / `Long` | `Integer` |
| `f64` | `double` | `Double` | `Number` |
| `i1` | `int8_t` (0/1) | `Boolean` | `Boolean` |
| `str` | `rt_string` | `String` | `String` |
| `obj` | `void*` | Object type | class instance |
| `void` | `void` | (no return) | (no return) |
