---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Adding a New Runtime Class: Complete Guide

Step-by-step deep dive for adding a new class to the Zanna runtime, covering all 8 required touchpoints from C implementation through frontend consumption.

**Related documentation:**
- [How to Extend the Zanna Runtime](runtime-extend-howto.md) — broader overview covering both functions and classes
- [Architecture](architecture.md) — system-level design
- [IL Guide](../il/il-guide.md) — IL type system and instruction reference
- [Codemap](codemap.md) — project directory structure

---

## Table of Contents

1. [When to Use This Guide](#1-when-to-use-this-guide)
2. [Architecture — The 8-Touchpoint Pipeline](#2-architecture--the-8-touchpoint-pipeline)
3. [Pre-Flight Decisions](#3-pre-flight-decisions)
4. [Step 1 — RuntimeTypeId Enum](#4-step-1--runtimetypeid-enum)
5. [Step 2 — C Header](#5-step-2--c-header)
6. [Step 3 — C Implementation](#6-step-3--c-implementation)
7. [Step 4 — RT_FUNC Entries in runtime.def](#7-step-4--rt_func-entries-in-runtimedef)
8. [Step 5 — RT_CLASS_BEGIN/END Block](#8-step-5--rt_class_beginend-block)
9. [Step 6 — RuntimeSignatures.cpp Include](#9-step-6--runtimesignaturescpp-include)
10. [Step 7 — CMakeLists.txt Registration](#10-step-7--cmakeliststxt-registration)
11. [Step 8 — Testing](#11-step-8--testing)
12. [What Happens at Build Time](#12-what-happens-at-build-time)
13. [How Frontends Consume the New Class](#13-how-frontends-consume-the-new-class)
14. [Complete Worked Example — Zanna.Utils.Gauge](#14-complete-worked-example--zannautilsgauge)
15. [Common Patterns](#15-common-patterns)
16. [Troubleshooting](#16-troubleshooting)
17. [Validation Checklist](#17-validation-checklist)
- [Appendix A — Quick Reference Card](#appendix-a--quick-reference-card)
- [Appendix B — Type Mapping Reference](#appendix-b--type-mapping-reference)
- [Appendix C — File Path Reference](#appendix-c--file-path-reference)

---

## 1. When to Use This Guide

### Scope

This guide covers adding a **new runtime class** — a type with a constructor, instance methods, and properties that Zanna programs can instantiate and use. If you need to add a standalone function (e.g., `Zanna.Math.Square`), see [How to Extend the Zanna Runtime](runtime-extend-howto.md) instead.

### This Guide vs runtime-extend-howto.md

| | This Guide | runtime-extend-howto.md |
|---|---|---|
| **Scope** | Classes only (deep dive) | Functions + classes (survey) |
| **Step ordering** | 8 touchpoints in exact sequence | Organized by topic |
| **Generated output** | Shows concrete rtgen output per file | Describes rtgen conceptually |
| **Frontend details** | Explains BASIC lazy vs Zia eager registration | Summarizes in 1 page |
| **Worked example** | Gauge class (all 8 steps contiguous) | Counter class (steps interleaved with theory) |

### Class Categories

| Category | Constructor | Layout | Example |
|----------|-------------|--------|---------|
| **Instance class** | Has constructor (`ctor_id`) | `"obj"` | `Zanna.Collections.Stack` |
| **Static utility** | `none` | `"none"` | `Zanna.Math` |
| **Hybrid** | Has constructor + static factory methods | `"obj"` | `Zanna.Collections.Map` |

Instance classes that allocate heap objects must use stable, unique runtime class IDs. Shared families should centralize those constants in one header, such as `src/runtime/collections/rt_collection_ids.h`, and every public method that downcasts an object should validate the ID before casting. Do not reuse small positive IDs or a family-wide zero ID; they collide with other runtime objects and disable typed-handle validation.

### Prerequisites

- Familiarity with C programming
- Basic understanding of CMake build system
- Knowledge of Zanna IL type system (see [IL Guide](../il/il-guide.md))

---

## 2. Architecture — The 8-Touchpoint Pipeline

Adding a runtime class requires touching exactly 8 locations. Here is how data flows through the system:

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                          8-TOUCHPOINT PIPELINE                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ① RuntimeTypeId Enum    ② C Header (.h)      ③ C Implementation (.c)     │
│  ┌───────────────────┐   ┌──────────────────┐  ┌──────────────────────┐    │
│  │ RuntimeClasses.hpp│   │ rt_gauge.h       │  │ rt_gauge.c           │    │
│  │ RTCLS_Gauge       │   │ Function decls   │  │ Struct + functions   │    │
│  └───────────────────┘   └──────────────────┘  └──────────────────────┘    │
│           │                       │                      │                  │
│           └───────────────────────┼──────────────────────┘                  │
│                                   ▼                                         │
│  ④ RT_FUNC entries         ⑤ RT_CLASS_BEGIN/END                            │
│  ┌──────────────────────────────────────────────┐                          │
│  │              runtime.def                      │                          │
│  │  RT_FUNC(GaugeNew, rt_gauge_new, ...)        │                          │
│  │  RT_CLASS_BEGIN("Zanna.Utils.Gauge", ...)     │                          │
│  └───────────────────────┬──────────────────────┘                          │
│                           │                                                 │
│              ┌────────────┼─────────────────────┐                          │
│              ▼            ▼                      ▼                          │
│  ⑥ #include in Sigs   ⑦ CMakeLists.txt    ⑧ Tests                        │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐            │
│  │ RuntimeSigs.cpp │  │ RT_*_SOURCES    │  │ RTGaugeTests.cpp│            │
│  │ #include header │  │ RT_PUBLIC_HDRS  │  │ unit tests      │            │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘            │
│                                                                             │
│  BUILD TIME: cmake detects runtime.def change → rtgen regenerates:         │
│  ┌─────────────────────────────────────────────────────────────┐           │
│  │  RuntimeNameMap.inc  │  RuntimeClasses.inc  │  RuntimeSigs  │           │
│  │  RuntimeNames.hpp    │  ZiaRuntimeExterns.inc               │           │
│  └─────────────────────────────────────────────────────────────┘           │
│           │                        │                                        │
│           ▼                        ▼                                        │
│  BASIC Frontend              Zia Frontend                                  │
│  (lazy RuntimeRegistry)      (eager 3-phase Sema)                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### The rtgen Code Generation Detail

```text
runtime.def ──→ rtgen ──→ 5 generated files:
                           ├── RuntimeNameMap.inc      canonical → C symbol
                           ├── RuntimeClasses.inc      OOP class/method/property catalog
                           ├── RuntimeSignatures.inc   VM descriptor rows
                           ├── RuntimeNames.hpp        C++ name constants
                           └── ZiaRuntimeExterns.inc   Zia extern declarations
```

### 8 Touchpoints At a Glance

| Step | File | What You Add | Purpose |
|------|------|-------------|---------|
| 1 | `src/il/runtime/classes/RuntimeClasses.hpp` | `RTCLS_Gauge` enum entry | Stable type identifier |
| 2 | `src/runtime/<sub>/rt_gauge.h` | C function declarations | Public API |
| 3 | `src/runtime/<sub>/rt_gauge.c` | C function implementations | Runtime logic |
| 4 | `src/il/runtime/runtime.def` | `RT_FUNC(...)` entries | Function registry (source of truth) |
| 5 | `src/il/runtime/runtime.def` | `RT_CLASS_BEGIN/END` block | Class structure definition |
| 6 | `src/il/runtime/RuntimeSignatures.cpp` | `#include "rt_gauge.h"` | VM handler resolution |
| 7 | `src/runtime/CMakeLists.txt` | Source + header in build lists | Compilation |
| 8 | `src/tests/runtime/RTGaugeTests.cpp` | Unit tests | Correctness verification |

---

## 3. Pre-Flight Decisions

Before writing any code, make three decisions:

### Decision 1: Subdirectory

Choose the `src/runtime/` subdirectory that matches your class's domain:

| Subdirectory | Domain | Examples |
|-------------|--------|----------|
| `core/` | Math, time, strings, fundamental utilities | `rt_math.c`, `rt_datetime.c` |
| `collections/` | Data structures | `rt_stack.c`, `rt_ring.c`, `rt_map.c` |
| `text/` | Parsing, formatting, codecs, regex | `rt_json.c`, `rt_csv.c`, `rt_codec.c` |
| `io/` | Files, streams, compression | `rt_file.c`, `rt_stream.c`, `rt_compress.c` |
| `graphics/` | Canvas, sprites, input, physics, GUI | `rt_sprite.c`, `rt_camera.c` |
| `threads/` | Concurrency primitives | `rt_threads.c`, `rt_channel.c` |
| `network/` | TCP, HTTP, DNS, WebSocket | `rt_restclient.c`, `rt_ratelimit.c` |
| `audio/` | Sound playback | `rt_audio.c`, `rt_synth.c` |
| `system/` | OS-level operations | `rt_exec.c`, `rt_machine.c` |
| `oop/` | Object model internals | `rt_object.c`, `rt_box.c` |

### Decision 2: Naming Conventions

| Entity | Convention | Example |
|--------|-----------|---------|
| C source file | `rt_<module>.c` | `rt_gauge.c` |
| C header file | `rt_<module>.h` | `rt_gauge.h` |
| C function | `rt_<module>_<action>` | `rt_gauge_new` |
| Canonical name | `Zanna.<Namespace>.<Class>.<Method>` | `Zanna.Utils.Gauge.New` |
| Definition ID | PascalCase, globally unique | `GaugeNew` |
| Type ID | `RTCLS_` + PascalCase | `RTCLS_Gauge` |

### Decision 3: Class Design

| Question | Options |
|----------|---------|
| Instance or static? | Instance (has constructor) or static (`ctor = none`) |
| Needs GC finalizer? | Yes if it `malloc`s internal buffers (e.g., arrays, strings) |
| Layout type? | `"obj"` for instance classes, `"none"` for static-only |

---

## 4. Step 1 — RuntimeTypeId Enum

**File:** `src/il/runtime/classes/RuntimeClasses.hpp`

Every runtime class needs a stable type identifier in the `RuntimeTypeId` enum. This enum is consumed by the generated `RuntimeClasses.inc` catalog and must match the `type_id` argument you use later in `RT_CLASS_BEGIN`.

Add your entry in a logical location within the enum:

```cpp
enum class RuntimeTypeId : std::size_t
{
    // ... existing entries ...
    RTCLS_Easing,
    RTCLS_LruCache,
    RTCLS_Gauge,       // ← new entry
    RTCLS_Map,
    // ... more entries ...
};
```

**Rules:**
- Prefix: Always `RTCLS_`
- Naming: PascalCase matching your class name
- Placement: Alphabetical within category, or at the end of the relevant group
- The suffix (e.g., `Gauge`) must match the `type_id` argument in `RT_CLASS_BEGIN` exactly

---

## 5. Step 2 — C Header

**File:** `src/runtime/<sub>/rt_gauge.h`

The header declares your class's public C API. All runtime headers follow the same pattern.

### Type Mapping

| IL Type | C Type | `runtime.def` String |
|---------|--------|---------------------|
| void | `void` | `void` |
| Boolean | `int8_t` (0 or 1) | `i1` |
| 64-bit integer | `int64_t` | `i64` |
| 64-bit float | `double` | `f64` |
| String | `const char*` | `str` |
| Object reference | `void*` | `obj` |

### The Dual-Signature Convention

This is the trickiest part of runtime class development. The same C function is described two different ways:

| Context | Receiver Included? | Example for `Push(item)` |
|---------|-------------------|--------------------------|
| `RT_FUNC` signature | **Yes** — receiver is first `obj` param | `"void(obj,obj)"` |
| `RT_METHOD` signature | **No** — receiver is implicit | `"void(obj)"` |
| C function declaration | **Yes** — first param is `void *obj` | `void rt_gauge_push(void *obj, void *item)` |

The `RT_FUNC` describes the actual C ABI. The `RT_METHOD` describes the user-facing API (what the programmer sees). For instance methods, the receiver is implicit in `RT_METHOD` because it comes from the object the method is called on. Static/factory methods are receiverless: their `RT_FUNC` ABI has the same parameter count as the `RT_METHOD` signature, and frontend metadata records `hasReceiver=false`.

If a method returns a retained object or copied string, update `src/il/runtime/RuntimeOwnership.hpp` at the same time as the runtime implementation. The optimizer and verifier use this metadata to avoid inserting incorrect extra retains or releases around runtime calls.

### Complete Header Example

```c
//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_gauge.h
// Purpose: Bounded numeric gauge for Zanna.Utils.Gauge, providing clamped
//   integer values within a [min, max] range with percentage tracking.
//
// Key invariants:
//   - Value is always within [min, max] after any operation.
//   - Increment/Decrement clamp to bounds (no trap).
//   - Setting Value outside bounds traps with a descriptive message.
//   - Percentage returns f64 in [0.0, 1.0].
//
// Ownership/Lifetime:
//   - Gauge objects are GC-managed (rt_obj_new_i64).
//   - No internal heap allocations; no finalizer needed.
//
// Links: src/runtime/core/rt_gauge.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief Create a new gauge with specified bounds.
    /// @param min Minimum allowed value.
    /// @param max Maximum allowed value (must be >= min).
    /// @return Pointer to new Gauge object, or NULL on allocation failure.
    void *rt_gauge_new(int64_t min, int64_t max);

    /// @brief Create a new gauge with default bounds [0, 100].
    /// @return Pointer to new Gauge object.
    void *rt_gauge_new_default(void);

    /// @brief Get the current value.
    int64_t rt_gauge_get_value(void *obj);

    /// @brief Set the current value. Traps if outside [min, max].
    void rt_gauge_set_value(void *obj, int64_t value);

    /// @brief Get the minimum bound.
    int64_t rt_gauge_get_min(void *obj);

    /// @brief Get the maximum bound.
    int64_t rt_gauge_get_max(void *obj);

    /// @brief Get the current percentage as a float in [0.0, 1.0].
    double rt_gauge_get_percentage(void *obj);

    /// @brief Check if the value equals the minimum.
    int8_t rt_gauge_is_at_min(void *obj);

    /// @brief Check if the value equals the maximum.
    int8_t rt_gauge_is_at_max(void *obj);

    /// @brief Increment the value by amount, clamping to max.
    void rt_gauge_increment(void *obj, int64_t amount);

    /// @brief Decrement the value by amount, clamping to min.
    void rt_gauge_decrement(void *obj, int64_t amount);

    /// @brief Reset the value to min.
    void rt_gauge_reset(void *obj);

    /// @brief Clamp the value to [min, max] (no-op if already in range).
    void rt_gauge_clamp(void *obj);

#ifdef __cplusplus
}
#endif
```

---

## 6. Step 3 — C Implementation

**File:** `src/runtime/<sub>/rt_gauge.c`

### Internal Struct Pattern

Every instance class uses an opaque struct allocated through the GC:

```c
typedef struct
{
    int64_t value;
    int64_t min;
    int64_t max;
} ZannaGauge;
```

### GC Allocation

Use `rt_obj_new_i64()` to allocate GC-managed objects:

```c
void *rt_gauge_new(int64_t min, int64_t max)
{
    ZannaGauge *gauge = (ZannaGauge *)rt_obj_new_i64(0, (int64_t)sizeof(ZannaGauge));
    if (!gauge)
    {
        rt_trap("Gauge: memory allocation failed");
        return NULL;
    }
    gauge->min = min;
    gauge->max = max;
    gauge->value = min;
    return gauge;
}
```

Parameters:
- **First argument** (`0`): Class ID for type introspection. Use `0` for classes that do not use OOP dispatch.
- **Second argument**: Byte size of the struct.

### GC Finalizer (When Needed)

If your struct contains `malloc`-managed buffers (arrays, strings, etc.), register a finalizer:

```c
static void gauge_finalize(void *obj)
{
    if (!obj) return;
    ZannaGauge *g = (ZannaGauge *)obj;
    free(g->internal_buffer);  // Free heap-allocated members
    g->internal_buffer = NULL;
}

void *rt_gauge_new(...)
{
    ZannaGauge *gauge = (ZannaGauge *)rt_obj_new_i64(0, (int64_t)sizeof(ZannaGauge));
    if (!gauge) { ... }
    rt_obj_set_finalizer(gauge, gauge_finalize);  // Register cleanup
    // ... initialize fields ...
    return gauge;
}
```

**When to use a finalizer:**
- Your struct contains pointers to `malloc`/`calloc`-allocated memory
- Examples: `Ring` (items array), `Map` (bucket array), `StringBuilder` (character buffer)

**When NOT to use a finalizer:**
- Your struct contains only scalar fields (integers, floats, booleans)
- Example: A simple `Gauge` with just `value`, `min`, `max`

### Error Handling

Use `rt_trap()` for unrecoverable errors. It terminates execution with a descriptive message:

```c
void rt_gauge_set_value(void *obj, int64_t value)
{
    ZannaGauge *gauge = (ZannaGauge *)obj;
    if (value < gauge->min || value > gauge->max)
    {
        rt_trap("Gauge: value %lld out of bounds [%lld, %lld]",
                (long long)value, (long long)gauge->min, (long long)gauge->max);
        return;
    }
    gauge->value = value;
}
```

### Complete Implementation Example

```c
//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_gauge.c
// Purpose: Implements Zanna.Utils.Gauge, a bounded numeric gauge that keeps
//   an integer value clamped within a [min, max] range. Useful for health bars,
//   progress indicators, volume controls, and similar bounded-value scenarios.
//
// Key invariants:
//   - value is always in [min, max] after construction.
//   - Increment/Decrement clamp silently; set_value traps on out-of-bounds.
//   - Percentage is computed as (value - min) / (max - min) when max > min.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Gauge objects are GC-managed (rt_obj_new_i64). No finalizer needed
//     because the struct contains only scalar fields.
//
// Links: src/runtime/core/rt_gauge.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_gauge.h"

#include "rt_internal.h"
#include "rt_object.h"

/// @brief Internal gauge structure.
typedef struct
{
    int64_t value; ///< Current value, always in [min, max].
    int64_t min;   ///< Lower bound (inclusive).
    int64_t max;   ///< Upper bound (inclusive).
} ZannaGauge;

void *rt_gauge_new(int64_t min, int64_t max)
{
    if (max < min)
    {
        rt_trap("Gauge: max (%lld) must be >= min (%lld)",
                (long long)max, (long long)min);
        return NULL;
    }

    ZannaGauge *g = (ZannaGauge *)rt_obj_new_i64(0, (int64_t)sizeof(ZannaGauge));
    if (!g)
    {
        rt_trap("Gauge: memory allocation failed");
        return NULL;
    }

    g->min = min;
    g->max = max;
    g->value = min;
    return g;
}

void *rt_gauge_new_default(void)
{
    return rt_gauge_new(0, 100);
}

int64_t rt_gauge_get_value(void *obj)
{
    return ((ZannaGauge *)obj)->value;
}

void rt_gauge_set_value(void *obj, int64_t value)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    if (value < g->min || value > g->max)
    {
        rt_trap("Gauge: value %lld out of bounds [%lld, %lld]",
                (long long)value, (long long)g->min, (long long)g->max);
        return;
    }
    g->value = value;
}

int64_t rt_gauge_get_min(void *obj)
{
    return ((ZannaGauge *)obj)->min;
}

int64_t rt_gauge_get_max(void *obj)
{
    return ((ZannaGauge *)obj)->max;
}

double rt_gauge_get_percentage(void *obj)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    if (g->max == g->min)
        return 1.0; // Avoid division by zero
    return (double)(g->value - g->min) / (double)(g->max - g->min);
}

int8_t rt_gauge_is_at_min(void *obj)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    return g->value == g->min ? 1 : 0;
}

int8_t rt_gauge_is_at_max(void *obj)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    return g->value == g->max ? 1 : 0;
}

void rt_gauge_increment(void *obj, int64_t amount)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    int64_t newVal = g->value + amount;
    g->value = (newVal > g->max) ? g->max : newVal;
}

void rt_gauge_decrement(void *obj, int64_t amount)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    int64_t newVal = g->value - amount;
    g->value = (newVal < g->min) ? g->min : newVal;
}

void rt_gauge_reset(void *obj)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    g->value = g->min;
}

void rt_gauge_clamp(void *obj)
{
    ZannaGauge *g = (ZannaGauge *)obj;
    if (g->value < g->min)
        g->value = g->min;
    else if (g->value > g->max)
        g->value = g->max;
}
```

---

## 7. Step 4 — RT_FUNC Entries in runtime.def

**File:** `src/il/runtime/runtime.def`

Every public function — constructors, property getters, property setters, and methods — must have an `RT_FUNC` entry. This is the single source of truth for all runtime metadata.

### Where to Add

Find the alphabetically appropriate section, or create a new one with the standard banner:

```c
//=============================================================================
// GAUGE (Bounded Numeric Value)
//=============================================================================
```

### RT_FUNC Parameters

```c
RT_FUNC(id, c_symbol, "canonical", "signature")
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `id` | Unique PascalCase C++ identifier | `GaugeNew` |
| `c_symbol` | Exact C function name from your `.h` | `rt_gauge_new` |
| `canonical` | Zanna namespace path | `"Zanna.Utils.Gauge.New"` |
| `signature` | IL type signature (receiver included) | `"obj(i64,i64)"` |

### Property Canonical Naming

Properties use `get_` and `set_` prefixes in the canonical name:

```c
// Getter: "Zanna.Utils.Gauge.get_Value"
RT_FUNC(GaugeGetValue, rt_gauge_get_value, "Zanna.Utils.Gauge.get_Value", "i64(obj)")

// Setter: "Zanna.Utils.Gauge.set_Value"
RT_FUNC(GaugeSetValue, rt_gauge_set_value, "Zanna.Utils.Gauge.set_Value", "void(obj,i64)")
```

### Dual-Signature Convention (Detailed)

The signature in `RT_FUNC` includes the receiver. The signature in `RT_METHOD` does not. Here is a side-by-side comparison for every function type:

| Function Type | C Signature | RT_FUNC Signature | RT_METHOD Signature |
|---------------|-------------|-------------------|---------------------|
| Constructor (no args) | `void* rt_gauge_new_default(void)` | `"obj()"` | n/a (ctor, not a method) |
| Constructor (with args) | `void* rt_gauge_new(int64_t, int64_t)` | `"obj(i64,i64)"` | n/a |
| Property getter | `int64_t rt_gauge_get_value(void*)` | `"i64(obj)"` | n/a (use RT_PROP) |
| Property setter | `void rt_gauge_set_value(void*, int64_t)` | `"void(obj,i64)"` | n/a (use RT_PROP) |
| Method (no args) | `void rt_gauge_reset(void*)` | `"void(obj)"` | `"void()"` |
| Method (with args) | `void rt_gauge_increment(void*, int64_t)` | `"void(obj,i64)"` | `"void(i64)"` |

### Completeness Rule

Every handler ID referenced in `RT_METHOD` or `RT_PROP` **must** have a corresponding `RT_FUNC` entry. This is enforced by `scripts/check_runtime_completeness.sh`.

### Complete RT_FUNC Block

```c
//=============================================================================
// GAUGE (Bounded Numeric Value)
//=============================================================================

RT_FUNC(GaugeNew,           rt_gauge_new,            "Zanna.Utils.Gauge.New",            "obj(i64,i64)")
RT_FUNC(GaugeNewDefault,    rt_gauge_new_default,    "Zanna.Utils.Gauge.NewDefault",     "obj()")
RT_FUNC(GaugeGetValue,      rt_gauge_get_value,      "Zanna.Utils.Gauge.get_Value",      "i64(obj)")
RT_FUNC(GaugeSetValue,      rt_gauge_set_value,      "Zanna.Utils.Gauge.set_Value",      "void(obj,i64)")
RT_FUNC(GaugeGetMin,        rt_gauge_get_min,        "Zanna.Utils.Gauge.get_Min",        "i64(obj)")
RT_FUNC(GaugeGetMax,        rt_gauge_get_max,        "Zanna.Utils.Gauge.get_Max",        "i64(obj)")
RT_FUNC(GaugeGetPercentage, rt_gauge_get_percentage, "Zanna.Utils.Gauge.get_Percentage", "f64(obj)")
RT_FUNC(GaugeIsAtMin,       rt_gauge_is_at_min,      "Zanna.Utils.Gauge.get_IsAtMin",    "i1(obj)")
RT_FUNC(GaugeIsAtMax,       rt_gauge_is_at_max,      "Zanna.Utils.Gauge.get_IsAtMax",    "i1(obj)")
RT_FUNC(GaugeIncrement,     rt_gauge_increment,      "Zanna.Utils.Gauge.Increment",      "void(obj,i64)")
RT_FUNC(GaugeDecrement,     rt_gauge_decrement,      "Zanna.Utils.Gauge.Decrement",      "void(obj,i64)")
RT_FUNC(GaugeReset,         rt_gauge_reset,          "Zanna.Utils.Gauge.Reset",          "void(obj)")
RT_FUNC(GaugeClamp,         rt_gauge_clamp,          "Zanna.Utils.Gauge.Clamp",          "void(obj)")
```

---

## 8. Step 5 — RT_CLASS_BEGIN/END Block

**File:** `src/il/runtime/runtime.def` (in the RUNTIME CLASSES section, after all RT_FUNC entries)

The class block defines the OOP structure — which properties and methods belong to the class. This is consumed by `rtgen` to generate the `RuntimeClasses.inc` catalog.

### RT_CLASS_BEGIN Parameters

```c
RT_CLASS_BEGIN("name", type_id, "layout", ctor_id)
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `"name"` | Fully qualified class name | `"Zanna.Utils.Gauge"` |
| `type_id` | Must match the `RTCLS_` suffix in RuntimeTypeId | `Gauge` |
| `"layout"` | Memory layout: `"obj"` for instance, `"none"` for static | `"obj"` |
| `ctor_id` | Default constructor function ID, or `none` | `GaugeNew` |

### RT_PROP Parameters

```c
RT_PROP("name", "type", getter_id, setter_id_or_none)
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `"name"` | Property name (PascalCase) | `"Value"` |
| `"type"` | IL type string | `"i64"` |
| `getter_id` | RT_FUNC ID for the getter | `GaugeGetValue` |
| `setter_id_or_none` | RT_FUNC ID for setter, or `none` if read-only | `GaugeSetValue` |

### RT_METHOD Parameters

```c
RT_METHOD("name", "signature", target_id)
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `"name"` | Method name (PascalCase) | `"Increment"` |
| `"signature"` | Signature **without receiver** | `"void(i64)"` |
| `target_id` | RT_FUNC ID for the implementation | `GaugeIncrement` |

### Static Class Pattern

For classes with no instances (all static methods), use `none` for both constructor and layout:

```c
RT_CLASS_BEGIN("Zanna.Math", Math, "none", none)
    RT_METHOD("Sin", "f64(f64)", MathSin)
    RT_METHOD("Cos", "f64(f64)", MathCos)
RT_CLASS_END()
```

Static methods do not receive an implicit object. Their `RT_FUNC` signatures should contain only the user-visible parameters shown in the `RT_METHOD` signature.

### Complete Class Block

```c
// Zanna.Utils.Gauge - bounded numeric value with clamping
RT_CLASS_BEGIN("Zanna.Utils.Gauge", Gauge, "obj", GaugeNew)
    RT_PROP("Value", "i64", GaugeGetValue, GaugeSetValue)
    RT_PROP("Min", "i64", GaugeGetMin, none)
    RT_PROP("Max", "i64", GaugeGetMax, none)
    RT_PROP("Percentage", "f64", GaugeGetPercentage, none)
    RT_PROP("IsAtMin", "i1", GaugeIsAtMin, none)
    RT_PROP("IsAtMax", "i1", GaugeIsAtMax, none)
    RT_METHOD("Increment", "void(i64)", GaugeIncrement)
    RT_METHOD("Decrement", "void(i64)", GaugeDecrement)
    RT_METHOD("Reset", "void()", GaugeReset)
    RT_METHOD("Clamp", "void()", GaugeClamp)
RT_CLASS_END()
```

**Note:** The `RT_METHOD` signature for `Increment` is `"void(i64)"` — only the `amount` parameter. The receiver `obj` is implicit. Compare with the `RT_FUNC` signature `"void(obj,i64)"` which includes both.

---

## 9. Step 6 — RuntimeSignatures.cpp Include

**File:** `src/il/runtime/RuntimeSignatures.cpp`

This file contains a large alphabetically-sorted block of `#include` directives for every runtime header. The VM's `DirectHandler` template system uses these includes to resolve C function pointers at compile time.

Add your header in alphabetical order:

```cpp
// ... existing includes ...
#include "rt_future.h"
#include "rt_gauge.h"       // ← add here
#include "rt_grid2d.h"
// ... more includes ...
```

**Why this is required:** Without this include, the `DirectHandler<&rt_gauge_new, void*, int64_t, int64_t>` template instantiation in the generated `RuntimeSignatures.inc` cannot resolve the function pointer, causing a compilation error.

**Note:** The include uses a flat name (no subdirectory prefix) because all `src/runtime/` subdirectories are added to the include path by CMake.

---

## 10. Step 7 — CMakeLists.txt Registration

**File:** `src/runtime/CMakeLists.txt`

### Add Source File

Add your `.c` file to the appropriate `RT_*_SOURCES` variable:

| Variable | Subdirectory |
|----------|-------------|
| `RT_BASE_SOURCES` | `core/` |
| `RT_ARRAY_SOURCES` | `arrays/` |
| `RT_OOP_SOURCES` | `oop/` |
| `RT_COLLECTIONS_SOURCES` | `collections/` |
| `RT_TEXT_SOURCES` | `text/` |
| `RT_IO_FS_SOURCES` | `io/` |
| `RT_EXEC_SOURCES` | `system/` |
| `RT_GRAPHICS_SOURCES` | `graphics/` |
| `RT_AUDIO_SOURCES` | `audio/` |
| `RT_THREADS_SOURCES` | `threads/` |

For a class in `core/`:

```cmake
set(RT_BASE_SOURCES
        # ... existing sources ...
        core/rt_gauge.c
)
```

### Add Public Header

Add your `.h` file to `RT_PUBLIC_HEADERS`:

```cmake
set(RT_PUBLIC_HEADERS
        # ... existing headers ...
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_gauge.h
)
```

---

## 11. Step 8 — Testing

### Test File

**File:** `src/tests/runtime/RTGaugeTests.cpp`

Runtime tests are standalone executables (not GTest). They use `assert()` for verification and a `setjmp`/`longjmp`-based `EXPECT_TRAP` macro for testing error paths.

```cpp
//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGaugeTests.cpp
// Purpose: Unit tests for Zanna.Utils.Gauge bounded numeric gauge.
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_gauge.h"

#include <cassert>
#include <csetjmp>

namespace
{
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
} // namespace

extern "C" void vm_trap(const char *msg)
{
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                      \
    do                                                                         \
    {                                                                          \
        g_trap_expected = true;                                                \
        g_last_trap = nullptr;                                                 \
        if (setjmp(g_trap_jmp) == 0)                                           \
        {                                                                      \
            expr;                                                              \
            assert(false && "Expected trap did not occur");                    \
        }                                                                      \
        g_trap_expected = false;                                               \
    } while (0)

static void test_new_and_defaults()
{
    void *g = rt_gauge_new(0, 100);
    assert(g != nullptr);
    assert(rt_gauge_get_value(g) == 0);
    assert(rt_gauge_get_min(g) == 0);
    assert(rt_gauge_get_max(g) == 100);
    assert(rt_gauge_is_at_min(g) == 1);
    assert(rt_gauge_is_at_max(g) == 0);
}

static void test_new_default()
{
    void *g = rt_gauge_new_default();
    assert(g != nullptr);
    assert(rt_gauge_get_min(g) == 0);
    assert(rt_gauge_get_max(g) == 100);
}

static void test_set_value()
{
    void *g = rt_gauge_new(0, 100);
    rt_gauge_set_value(g, 50);
    assert(rt_gauge_get_value(g) == 50);
}

static void test_set_value_out_of_bounds_traps()
{
    void *g = rt_gauge_new(0, 100);
    EXPECT_TRAP(rt_gauge_set_value(g, 101));
    EXPECT_TRAP(rt_gauge_set_value(g, -1));
}

static void test_increment_clamps()
{
    void *g = rt_gauge_new(0, 10);
    rt_gauge_increment(g, 7);
    assert(rt_gauge_get_value(g) == 7);
    rt_gauge_increment(g, 5); // Would be 12, clamps to 10
    assert(rt_gauge_get_value(g) == 10);
    assert(rt_gauge_is_at_max(g) == 1);
}

static void test_decrement_clamps()
{
    void *g = rt_gauge_new(0, 10);
    rt_gauge_set_value(g, 5);
    rt_gauge_decrement(g, 3);
    assert(rt_gauge_get_value(g) == 2);
    rt_gauge_decrement(g, 10); // Would be -8, clamps to 0
    assert(rt_gauge_get_value(g) == 0);
    assert(rt_gauge_is_at_min(g) == 1);
}

static void test_percentage()
{
    void *g = rt_gauge_new(0, 100);
    rt_gauge_set_value(g, 50);
    double pct = rt_gauge_get_percentage(g);
    assert(pct > 0.49 && pct < 0.51);
}

static void test_reset()
{
    void *g = rt_gauge_new(10, 20);
    rt_gauge_set_value(g, 15);
    rt_gauge_reset(g);
    assert(rt_gauge_get_value(g) == 10); // Resets to min
}

static void test_invalid_bounds_traps()
{
    EXPECT_TRAP(rt_gauge_new(100, 0)); // max < min
}

int main()
{
    test_new_and_defaults();
    test_new_default();
    test_set_value();
    test_set_value_out_of_bounds_traps();
    test_increment_clamps();
    test_decrement_clamps();
    test_percentage();
    test_reset();
    test_invalid_bounds_traps();

    return 0;
}
```

### CMake Registration

**File:** `src/tests/unit/CMakeLists.txt`

Add at the end of the runtime test section:

```cmake
if (NOT TARGET test_rt_gauge)
    zanna_add_test(test_rt_gauge ${ZANNA_TESTS_DIR}/runtime/RTGaugeTests.cpp)
    target_link_libraries(test_rt_gauge PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gauge test_rt_gauge)
endif ()
```

### Running Tests

```bash
# Build and run all tests
./scripts/build_zanna.sh

# Run only the gauge tests
ctest --test-dir build -R gauge --output-on-failure
```

---

## 12. What Happens at Build Time

After editing `runtime.def`, the build system automatically regenerates all generated files:

```text
┌─────────────────┐     ┌──────────┐     ┌──────────────────────────────────┐
│ runtime.def     │────▶│  rtgen   │────▶│ build/generated/il/runtime/      │
│ (you edited)    │     │ (tool)   │     │  ├── RuntimeNameMap.inc          │
└─────────────────┘     └──────────┘     │  ├── RuntimeClasses.inc         │
                                          │  ├── RuntimeSignatures.inc      │
                                          │  ├── RuntimeNames.hpp           │
                                          │  └── ZiaRuntimeExterns.inc      │
                                          └──────────────────────────────────┘
```

### What rtgen Generates for the Gauge Class

**1. RuntimeNameMap.inc** — Maps canonical names to C symbols for native codegen:

```c
RUNTIME_NAME_ALIAS("Zanna.Utils.Gauge.New", "rt_gauge_new")
RUNTIME_NAME_ALIAS("Zanna.Utils.Gauge.NewDefault", "rt_gauge_new_default")
RUNTIME_NAME_ALIAS("Zanna.Utils.Gauge.get_Value", "rt_gauge_get_value")
RUNTIME_NAME_ALIAS("Zanna.Utils.Gauge.set_Value", "rt_gauge_set_value")
// ... one entry per RT_FUNC
```

**2. RuntimeClasses.inc** — OOP catalog (class structure with properties and methods):

```cpp
RUNTIME_CLASS("Zanna.Utils.Gauge",
    RTCLS_Gauge, "obj", "Zanna.Utils.Gauge.New",
    RUNTIME_PROPS(
        RUNTIME_PROP("Value", "i64", "Zanna.Utils.Gauge.get_Value", "Zanna.Utils.Gauge.set_Value"),
        RUNTIME_PROP("Min", "i64", "Zanna.Utils.Gauge.get_Min", nullptr),
        // ... more properties
    ),
    RUNTIME_METHODS(
        RUNTIME_METHOD("Increment", "void(i64)", "Zanna.Utils.Gauge.Increment"),
        RUNTIME_METHOD("Decrement", "void(i64)", "Zanna.Utils.Gauge.Decrement"),
        // ... more methods
    )
)
```

**3. RuntimeSignatures.inc** — VM descriptor rows with function pointers:

```cpp
{ "Zanna.Utils.Gauge.New", SigId::ObjI64I64, &DirectHandler<&rt_gauge_new, ...>::invoke, ... },
{ "Zanna.Utils.Gauge.get_Value", SigId::I64Obj, &DirectHandler<&rt_gauge_get_value, ...>::invoke, ... },
// ... one row per RT_FUNC
```

**4. RuntimeNames.hpp** — C++ constants for frontends:

```cpp
inline constexpr const char *kUtilsGaugeNew = "Zanna.Utils.Gauge.New";
inline constexpr const char *kUtilsGaugeGetValue = "Zanna.Utils.Gauge.get_Value";
// ... one constant per RT_FUNC
```

**5. ZiaRuntimeExterns.inc** — Zia extern declarations:

```cpp
typeRegistry_["Zanna.Utils.Gauge"] = types::runtimeClass("Zanna.Utils.Gauge");
defineExternFunction("Zanna.Utils.Gauge.New", types::runtimeClass("Zanna.Utils.Gauge"));
defineExternFunction("Zanna.Utils.Gauge.get_Value", types::integer());
// ... one entry per RT_FUNC
```

### Generated Files Reference

| File | Purpose | Consumers |
|------|---------|-----------|
| `RuntimeNameMap.inc` | Canonical name → C symbol mapping | Native codegen |
| `RuntimeClasses.inc` | OOP class/method/property catalog | `RuntimeClasses.cpp` → both frontends |
| `RuntimeSignatures.inc` | VM descriptor table with handlers | `RuntimeSignatures.cpp` → VM |
| `RuntimeNames.hpp` | C++ string constants | Frontend source code |
| `ZiaRuntimeExterns.inc` | Zia-specific extern declarations | `Sema_Runtime.cpp` (Zia frontend) |

---

## 13. How Frontends Consume the New Class

Both frontends automatically discover new classes from the generated files. **No frontend code changes are needed** when adding a new runtime class.

### BASIC Frontend: Lazy Resolution

The BASIC frontend resolves runtime calls on demand via the `RuntimeRegistry` singleton:

```text
BASIC code: gauge.Increment(5)
     │
     ▼
RuntimeMethodIndex.find("Zanna.Utils.Gauge", "Increment", 1)
     │  delegates to
     ▼
RuntimeRegistry::instance().findMethod(...)
     │  O(1) hash lookup in methodIndex_
     ▼
ParsedMethod { name="Increment", target="Zanna.Utils.Gauge.Increment",
               signature={ ret=Void, params=[I64] } }
     │
     ▼
Lowerer emits: b.addExtern("Zanna.Utils.Gauge.Increment", Type::Void, {Type::I64})
```

Key files:
- `src/frontends/basic/sem/RuntimeMethodIndex.cpp` — Delegates to `RuntimeRegistry`
- `src/frontends/basic/LowerRuntime.cpp` — Emits extern declarations on demand

### Zia Frontend: Eager 3-Phase Registration

The Zia frontend registers all runtime functions upfront during semantic analysis:

```text
Sema::initRuntimeFunctions() {
    Phase 1: Register class types
    ┌─────────────────────────────────────────────────────────────────┐
    │ for (cls : catalog)                                             │
    │     typeRegistry_["Zanna.Utils.Gauge"] = types::runtimeClass() │
    └─────────────────────────────────────────────────────────────────┘

    Phase 2: Bulk extern declarations (coarse, return type only)
    ┌─────────────────────────────────────────────────────────────────┐
    │ #include "il/runtime/ZiaRuntimeExterns.inc"                     │
    │ defineExternFunction("Zanna.Utils.Gauge.Increment", void_type) │
    └─────────────────────────────────────────────────────────────────┘

    Phase 3: Full method/property registration (overrides Phase 2)
    ┌─────────────────────────────────────────────────────────────────┐
    │ for (cls : catalog)                                             │
    │     for (method : cls.methods)                                  │
    │         defineExternFunction(target, returnType, paramTypes)    │
    │     for (prop : cls.properties)                                 │
    │         defineExternFunction(getter, propType, {})              │
    └─────────────────────────────────────────────────────────────────┘
}
```

Key files:
- `src/frontends/zia/Sema_Runtime.cpp` — 3-phase registration
- `src/frontends/zia/RuntimeAdapter.hpp` — `toZiaType()`, `toZiaReturnType()`

### Why the Difference?

**BASIC** uses a simpler, more permissive type system. It can resolve `Zanna.Utils.Gauge.Increment` lazily via `RuntimeRegistry` at the point of use, only emitting extern declarations for functions the program actually calls.

**Zia** is statically typed and needs all symbols registered in its symbol table before analysis begins. The `ZiaRuntimeExterns.inc` file gives it coarse type info for every `RT_FUNC` in one pass; Phase 3 then overrides with precise signatures from the `RuntimeClasses.inc` catalog.

### Type Mapping Across Frontends

| IL Type | BASIC Type | Zia Type |
|---------|-----------|----------|
| `i64` | `BasicType::Int` | `types::integer()` |
| `f64` | `BasicType::Float` | `types::float64()` |
| `i1` | `BasicType::Bool` | `types::boolean()` |
| `str` | `BasicType::String` | `types::string()` |
| `obj` | `BasicType::Object` | `types::runtimeClass(qname)` |
| `void` | `BasicType::Void` | `types::voidType()` |

---

## 14. Complete Worked Example — Zanna.Utils.Gauge

This section shows all 8 steps contiguously for the `Zanna.Utils.Gauge` class — a bounded numeric gauge that keeps an integer value within `[min, max]`.

### API Surface

| Member | Type | Description |
|--------|------|-------------|
| `Gauge.New(min, max)` | Constructor | Create gauge with bounds |
| `Gauge.NewDefault()` | Factory | Create gauge [0, 100] |
| `.Value` | Property (rw) | Current value; traps if set out of bounds |
| `.Min` | Property (ro) | Lower bound |
| `.Max` | Property (ro) | Upper bound |
| `.Percentage` | Property (ro) | Value as f64 in [0.0, 1.0] |
| `.IsAtMin` | Property (ro) | True if value == min |
| `.IsAtMax` | Property (ro) | True if value == max |
| `.Increment(amount)` | Method | Add amount, clamp to max |
| `.Decrement(amount)` | Method | Subtract amount, clamp to min |
| `.Reset()` | Method | Set value to min |
| `.Clamp()` | Method | Ensure value is in [min, max] |

### Step 1: RuntimeTypeId

In `src/il/runtime/classes/RuntimeClasses.hpp`:

```cpp
    RTCLS_Gauge,
```

### Step 2: C Header

Create `src/runtime/core/rt_gauge.h` — see [Section 5](#5-step-2--c-header) for the complete file.

### Step 3: C Implementation

Create `src/runtime/core/rt_gauge.c` — see [Section 6](#6-step-3--c-implementation) for the complete file.

### Step 4: RT_FUNC Entries

In `src/il/runtime/runtime.def` — see [Section 7](#7-step-4--rt_func-entries-in-runtimedef) for the complete block.

### Step 5: RT_CLASS_BEGIN/END

In `src/il/runtime/runtime.def` — see [Section 8](#8-step-5--rt_class_beginend-block) for the complete block.

### Step 6: Include

In `src/il/runtime/RuntimeSignatures.cpp`:

```cpp
#include "rt_gauge.h"
```

### Step 7: CMakeLists.txt

In `src/runtime/CMakeLists.txt`:

```cmake
# In RT_BASE_SOURCES:
        core/rt_gauge.c

# In RT_PUBLIC_HEADERS:
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_gauge.h
```

### Step 8: Tests

Create `src/tests/runtime/RTGaugeTests.cpp` — see [Section 11](#11-step-8--testing) for the complete file.

In `src/tests/unit/CMakeLists.txt`:

```cmake
if (NOT TARGET test_rt_gauge)
    zanna_add_test(test_rt_gauge ${ZANNA_TESTS_DIR}/runtime/RTGaugeTests.cpp)
    target_link_libraries(test_rt_gauge PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gauge test_rt_gauge)
endif ()
```

### Build and Verify

```bash
# Build (rtgen runs automatically)
./scripts/build_zanna.sh

# Verify generated output
grep "Gauge" build/generated/il/runtime/RuntimeClasses.inc
grep "Gauge" build/generated/il/runtime/RuntimeNameMap.inc

# Run tests
ctest --test-dir build -R gauge --output-on-failure

# Check completeness
./scripts/check_runtime_completeness.sh
```

### Usage in BASIC

```basic
DIM g AS Gauge
g = Gauge.New(0, 100)

g.Increment(30)
PRINT "Value: "; g.Value       ' Output: Value: 30
PRINT "Pct: "; g.Percentage    ' Output: Pct: 0.3

g.Value = 75
PRINT "At max? "; g.IsAtMax    ' Output: At max? 0

g.Reset()
PRINT "After reset: "; g.Value ' Output: After reset: 0
```

### Usage in Zia

```zia
var g = Gauge.New(0, 100)

g.Increment(30)
print("Value: " + g.Value.ToString())         // Output: Value: 30
print("Pct: " + g.Percentage.ToString())       // Output: Pct: 0.3

g.Value = 75
print("At max? " + g.IsAtMax.ToString())       // Output: At max? false

g.Reset()
print("After reset: " + g.Value.ToString())    // Output: After reset: 0
```

---

## 15. Common Patterns

### Pattern 1: Static Utility Class

No instances, all static methods. Constructor is `none`, layout is `"none"`:

```c
// runtime.def
RT_CLASS_BEGIN("Zanna.Math", Math, "none", none)
    RT_METHOD("Sin", "f64(f64)", MathSin)
    RT_METHOD("Cos", "f64(f64)", MathCos)
    RT_METHOD("Sqrt", "f64(f64)", MathSqrt)
RT_CLASS_END()
```

### Pattern 2: Multiple Constructors (Factory Methods)

The primary constructor goes in `ctor_id`. Additional factories are regular methods or separate RT_FUNC entries:

```c
// runtime.def
RT_FUNC(GaugeNew,        rt_gauge_new,         "Zanna.Utils.Gauge.New",        "obj(i64,i64)")
RT_FUNC(GaugeNewDefault, rt_gauge_new_default,  "Zanna.Utils.Gauge.NewDefault", "obj()")

RT_CLASS_BEGIN("Zanna.Utils.Gauge", Gauge, "obj", GaugeNew)
    // GaugeNewDefault is accessible as Gauge.NewDefault() via RT_FUNC canonical name
    // ... properties and methods ...
RT_CLASS_END()
```

### Pattern 3: Read-Only Properties

Use `none` for the setter:

```c
RT_PROP("Length", "i64", StackLen, none)
RT_PROP("IsEmpty", "i1", StackIsEmpty, none)
```

### Pattern 4: Read-Write Properties

Provide both getter and setter IDs:

```c
RT_PROP("Value", "i64", GaugeGetValue, GaugeSetValue)
```

### Pattern 5: Fluent API (Return Self)

Return the receiver from methods to enable chaining:

```c
// C implementation
void *rt_builder_append(void *obj, const char *text)
{
    // ... append logic ...
    return obj;  // Return self
}

// runtime.def
RT_METHOD("Append", "obj(str)", BuilderAppend)
```

Usage: `builder.Append("Hello").Append(" World")`

### Pattern 6: Overloaded Methods (Different Arities)

Create multiple RT_FUNC entries with different canonical names. Methods with the same user-facing name but different arities are resolved by the frontend:

```c
RT_FUNC(SubstrFrom,  rt_substr_from,  "Zanna.String.Substring", "str(obj,i64)")
RT_FUNC(SubstrRange, rt_substr_range, "Zanna.String.Substring", "str(obj,i64,i64)")
```

### Pattern 7: Error Handling with rt_trap

Use `rt_trap()` for precondition violations. Always include a descriptive message:

```c
void *rt_list_get(void *obj, int64_t index)
{
    ZannaList *list = (ZannaList *)obj;
    if (index < 0 || index >= list->count)
    {
        rt_trap("List index out of bounds: %lld (size: %lld)",
                (long long)index, (long long)list->count);
        return NULL;  // Unreachable after trap
    }
    return list->items[index];
}
```

### Pattern 8: GC Finalizer

For classes with `malloc`-managed internal buffers (see `src/runtime/collections/rt_ring.c` for a real-world example):

```c
static void ring_finalize(void *obj)
{
    if (!obj) return;
    rt_ring_impl *ring = (rt_ring_impl *)obj;
    free(ring->items);
    ring->items = NULL;
}

void *rt_ring_new(int64_t capacity)
{
    rt_ring_impl *ring = (rt_ring_impl *)rt_obj_new_i64(0, (int64_t)sizeof(rt_ring_impl));
    if (!ring) return NULL;
    ring->items = (void **)calloc((size_t)capacity, sizeof(void *));
    rt_obj_set_finalizer(ring, ring_finalize);
    // ... initialize ...
    return ring;
}
```

### Pattern 9: Returning Collections

Methods like `ToList()` or `ToSeq()` that convert to standard collection types:

```c
// C implementation calls the collection's constructor
void *rt_stack_to_list(void *obj)
{
    rt_stack_impl *s = (rt_stack_impl *)obj;
    void *list = rt_list_new();
    for (int64_t i = 0; i < s->len; i++)
        rt_list_add(list, s->items[i]);
    return list;
}

// runtime.def
RT_METHOD("ToList", "obj()", StackToList)
RT_METHOD("ToSeq", "obj()", StackToSeq)
```

### Pattern 10: Cross-Class References

Methods that accept or return objects of a different runtime class:

```c
// C implementation
void *rt_physics_add_body(void *world, void *body)
{
    // world is Physics2DWorld, body is Physics2DBody
    // ... register body in world ...
    return body;
}

// runtime.def — both params are "obj" regardless of specific class
RT_FUNC(PhysAddBody, rt_physics_add_body, "Zanna.Physics2D.World.AddBody", "obj(obj,obj)")
```

---

## 16. Troubleshooting

### 1. "Unknown runtime function" at compile time

**Symptom:** Compiler reports unknown function when calling `Zanna.Utils.Gauge.New`

**Causes:**
1. Function not added to `runtime.def`
2. Typo in canonical name
3. Build not regenerated after editing `runtime.def`

**Solution:**
```bash
# Force full regeneration
rm -rf build/generated/il/runtime/
cmake --build build -j
```

---

### 2. Linker error: "undefined reference to rt_gauge_xxx"

**Symptom:** Build succeeds but link fails with undefined symbol

**Causes:**
1. Source file not added to `CMakeLists.txt`
2. Missing `extern "C"` wrapper in header (name mangling)
3. Function declared in header but not implemented in `.c` file

**Solution:** Check all three causes. The `extern "C"` guard is required because `RuntimeSignatures.cpp` is compiled as C++.

---

### 3. "Signature mismatch" type error

**Symptom:** Frontend reports type error when calling runtime method

**Cause:** The IL signature in `runtime.def` does not match the actual C function signature.

**Solution:** Verify parameter types match exactly:
- `i64` = `int64_t` (NOT `int` or `long`)
- `f64` = `double` (NOT `float`)
- `i1` = `int8_t` (0 or 1)
- `str` = `const char*`
- `obj` = `void*`

---

### 4. "Class methods not found on instance"

**Symptom:** Constructor works but methods are not accessible

**Cause:** Missing `RT_CLASS_BEGIN`/`RT_CLASS_END` block in `runtime.def`, or methods not listed inside the block.

**Solution:** Ensure the class block exists and contains all methods/properties. The `RT_FUNC` entries alone are not enough for OOP dispatch — the class block tells frontends which functions are methods of the class.

---

### 5. "Property getter/setter not working"

**Symptom:** Property access compiles but returns wrong type or doesn't work

**Cause:** Canonical name does not follow the `get_`/`set_` convention.

**Solution:** Property getters must use `"Zanna.Class.get_PropName"` and setters must use `"Zanna.Class.set_PropName"` as their canonical names in the `RT_FUNC` entry.

---

### 6. check_runtime_completeness.sh reports missing handlers

**Symptom:** Script outputs "ERROR: N handler(s) referenced in RT_METHOD/RT_PROP have no RT_FUNC entry"

**Cause:** An `RT_METHOD` or `RT_PROP` references a handler ID that has no corresponding `RT_FUNC` entry.

**Solution:** Add the missing `RT_FUNC` entry. Every handler ID used in `RT_METHOD("Name", "sig", HandlerId)` and `RT_PROP("Name", "type", GetterId, SetterId)` must appear as the first argument to an `RT_FUNC(HandlerId, ...)` line.

---

### 7. DirectHandler compilation error in RuntimeSignatures.cpp

**Symptom:** Build error in `RuntimeSignatures.cpp` about unresolved function pointer

**Cause:** Missing `#include "rt_gauge.h"` in `RuntimeSignatures.cpp`

**Solution:** Add the include in alphabetical order. The `DirectHandler` template needs the C function declarations to resolve at compile time.

---

### 8. RuntimeTypeId mismatch assertion

**Symptom:** Assertion failure mentioning type ID during catalog construction

**Cause:** The `type_id` in `RT_CLASS_BEGIN("...", TypeId, ...)` does not match any `RTCLS_TypeId` entry in the `RuntimeTypeId` enum.

**Solution:** Ensure the second argument to `RT_CLASS_BEGIN` (e.g., `Gauge`) matches the suffix after `RTCLS_` in the enum (e.g., `RTCLS_Gauge`).

---

### 9. Build succeeds but class is inaccessible from frontend

**Symptom:** No errors, but `Gauge.New()` is not recognized in BASIC or Zia

**Cause:** Stale generated files. The build system may not have detected the `runtime.def` change.

**Solution:**
```bash
# Clean generated files and rebuild
rm -rf build/generated/il/runtime/
./scripts/build_zanna.sh
```

---

### 10. Segfault when calling constructor

**Symptom:** Program crashes immediately when creating an instance

**Causes:**
1. `rt_obj_new_i64` returned NULL and you did not check
2. Struct field accessed before initialization
3. Finalizer registered but accesses uninitialized fields

**Solution:** Always NULL-check after `rt_obj_new_i64`. Initialize all struct fields before returning. If registering a finalizer, ensure all fields the finalizer accesses are initialized (even if to NULL/0).

---

## 17. Validation Checklist

After completing all 8 steps, run these commands to verify everything is wired up:

```bash
# 1. Build (rtgen runs automatically on runtime.def change)
./scripts/build_zanna.sh

# 2. Run completeness check (all RT_METHOD/RT_PROP handlers have RT_FUNC)
./scripts/check_runtime_completeness.sh

# 3. Run all tests
ctest --test-dir build --output-on-failure

# 4. Verify generated output contains your class
grep "Gauge" build/generated/il/runtime/RuntimeClasses.inc
grep "Gauge" build/generated/il/runtime/RuntimeNameMap.inc
grep "Gauge" build/generated/il/runtime/ZiaRuntimeExterns.inc
grep "Gauge" build/generated/il/runtime/RuntimeSignatures.inc

# 5. Run your specific tests
ctest --test-dir build -R gauge --output-on-failure

# 6. Cross-platform: verify builds on both macOS and Linux
```

All commands should complete with zero errors and zero warnings.

---

## Appendix A — Quick Reference Card

### 8-Step Checklist

```text
□ 1. RTCLS_ enum     → src/il/runtime/classes/RuntimeClasses.hpp
□ 2. C header (.h)   → src/runtime/<sub>/rt_myclass.h
□ 3. C source (.c)   → src/runtime/<sub>/rt_myclass.c
□ 4. RT_FUNC entries  → src/il/runtime/runtime.def
□ 5. RT_CLASS block   → src/il/runtime/runtime.def
□ 6. #include header  → src/il/runtime/RuntimeSignatures.cpp
□ 7. CMakeLists.txt   → src/runtime/CMakeLists.txt
□ 8. Tests            → src/tests/runtime/RTMyClassTests.cpp
                        + src/tests/unit/CMakeLists.txt
```

### Minimal Skeleton

**RuntimeClasses.hpp:**
```cpp
RTCLS_MyClass,
```

**rt_myclass.h:**
```c
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void    *rt_myclass_new(void);
int64_t  rt_myclass_get_value(void *obj);
void     rt_myclass_do_thing(void *obj);
#ifdef __cplusplus
}
#endif
```

**runtime.def (RT_FUNC):**
```c
RT_FUNC(MyClassNew,      rt_myclass_new,       "Zanna.NS.MyClass.New",       "obj()")
RT_FUNC(MyClassGetValue, rt_myclass_get_value,  "Zanna.NS.MyClass.get_Value", "i64(obj)")
RT_FUNC(MyClassDoThing,  rt_myclass_do_thing,   "Zanna.NS.MyClass.DoThing",   "void(obj)")
```

**runtime.def (RT_CLASS):**
```c
RT_CLASS_BEGIN("Zanna.NS.MyClass", MyClass, "obj", MyClassNew)
    RT_PROP("Value", "i64", MyClassGetValue, none)
    RT_METHOD("DoThing", "void()", MyClassDoThing)
RT_CLASS_END()
```

**RuntimeSignatures.cpp:**
```cpp
#include "rt_myclass.h"
```

**CMakeLists.txt:**
```cmake
core/rt_myclass.c              # in RT_BASE_SOURCES (or appropriate list)
${CMAKE_CURRENT_SOURCE_DIR}/core/rt_myclass.h  # in RT_PUBLIC_HEADERS
```

**Test CMakeLists.txt:**
```cmake
zanna_add_test(test_rt_myclass ${ZANNA_TESTS_DIR}/runtime/RTMyClassTests.cpp)
target_link_libraries(test_rt_myclass PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
zanna_add_ctest(test_rt_myclass test_rt_myclass)
```

---

## Appendix B — Type Mapping Reference

| IL Type | C Type | `runtime.def` | BASIC Type | Zia Type |
|---------|--------|---------------|-----------|----------|
| `void` | `void` | `void` | `BasicType::Void` | `types::voidType()` |
| `i1` | `int8_t` | `i1` | `BasicType::Bool` | `types::boolean()` |
| `i8` | `int8_t` | `i8` | — | — |
| `i16` | `int16_t` | `i16` | — | — |
| `i32` | `int32_t` | `i32` | — | — |
| `i64` | `int64_t` | `i64` | `BasicType::Int` | `types::integer()` |
| `f32` | `float` | `f32` | — | — |
| `f64` | `double` | `f64` | `BasicType::Float` | `types::float64()` |
| `str` | `const char*` | `str` | `BasicType::String` | `types::string()` |
| `obj` | `void*` | `obj` | `BasicType::Object` | `types::runtimeClass(qname)` |
| `ptr` | `void*` | `ptr` | `BasicType::Object` | `types::ptr()` |
| `seq<T>` | `void*` | `seq<str>` | — | `types::seqOf(T)` |

---

## Appendix C — File Path Reference

| Purpose | File Path | What to Add |
|---------|-----------|-------------|
| Type ID enum | `src/il/runtime/classes/RuntimeClasses.hpp` | `RTCLS_MyClass` entry |
| C header | `src/runtime/<sub>/rt_myclass.h` | Function declarations |
| C source | `src/runtime/<sub>/rt_myclass.c` | Function implementations |
| Function registry | `src/il/runtime/runtime.def` | `RT_FUNC(...)` entries |
| Class definition | `src/il/runtime/runtime.def` | `RT_CLASS_BEGIN`/`END` block |
| VM handler include | `src/il/runtime/RuntimeSignatures.cpp` | `#include "rt_myclass.h"` |
| Build: source | `src/runtime/CMakeLists.txt` | `.c` in `RT_*_SOURCES` |
| Build: header | `src/runtime/CMakeLists.txt` | `.h` in `RT_PUBLIC_HEADERS` |
| Test source | `src/tests/runtime/RTMyClassTests.cpp` | Test functions + main() |
| Test registration | `src/tests/unit/CMakeLists.txt` | `zanna_add_test(...)` block |
| Generated: name map | `build/generated/il/runtime/RuntimeNameMap.inc` | (auto) |
| Generated: classes | `build/generated/il/runtime/RuntimeClasses.inc` | (auto) |
| Generated: signatures | `build/generated/il/runtime/RuntimeSignatures.inc` | (auto) |
| Generated: names | `build/generated/il/runtime/RuntimeNames.hpp` | (auto) |
| Generated: Zia externs | `build/generated/il/runtime/ZiaRuntimeExterns.inc` | (auto) |
| Validation script | `scripts/check_runtime_completeness.sh` | (run, don't edit) |
