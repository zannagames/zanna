---
status: active
audience: public
last-verified: 2026-09-01
---

# Object Layout and Call ABI

This page documents the current runtime object payload layout and method-call
lowering behavior used by the BASIC and Zia front ends.

## Heap Header Versus Payload

Runtime objects are allocated through `rt_obj_new_i64(class_id, byte_size)`.
That helper delegates to the shared heap allocator and returns a pointer to the
payload, not to the heap metadata header.

The hidden heap header lives immediately before the payload pointer returned to
compiled code. `rt_heap_hdr_t` stores validation metadata, heap kind, element
kind, flags, refcount, length, capacity, allocation size, `class_id`, and an
optional finalizer. Runtime helpers recover the header with `rt_heap_hdr()` or
`rt_heap_try_get_header()`.

The `class_id` passed to `rt_obj_new_i64()` is stored in the heap header and is
read by helpers such as `rt_obj_class_id()` and `rt_obj_is_instance()`.

## Runtime OOP Metadata

The OOP runtime ABI is declared in `src/runtime/oop/rt_oop.h`.

- `rt_class_info` records type id, qualified name, base class metadata, canonical
  vtable pointer, and vtable length.
- `rt_object` is the user-visible OOP payload header for vtable-based class
  instances. It contains `void **vptr` at payload offset 0.
- `rt_register_class*` functions bind class ids and canonical vtables into the
  registry.
- `rt_get_class_vtable(type_id)` returns the canonical vtable pointer for a
  registered type.
- `rt_get_vfunc(obj, slot)` reads `obj->vptr[slot]` after validating the vtable
  against registered class metadata.
- `rt_register_interface*`, `rt_register_interface_impl()`,
  `rt_get_interface_impl()`, and `rt_itable_lookup()` support interface tables.

The process-wide type registry is populated during module initialization before
objects that depend on the metadata are used.

## BASIC Class Payload Layout

BASIC class payloads reserve payload offset 0 for the vtable pointer. Instance
fields start after the pointer-sized vptr slot and are aligned to 8 bytes.
Inherited fields are laid out before derived fields.

Allocation and construction are lowered as follows:

1. The BASIC lowerer computes the class layout and stable class id.
2. `NEW C(...)` emits `rt_obj_new_i64(class_id, object_size)`.
3. The constructor receives the object pointer as the leading `ME` argument.
4. Constructor lowering retrieves the registered vtable with
   `rt_get_class_vtable(class_id)` and stores it at payload offset 0.
5. The constructor body initializes fields and user parameters.

Constructors and destructors use the shared OOP name mangling helpers:

- constructor: `ClassName.__ctor`
- destructor: `ClassName.__dtor`
- method: `ClassName.MethodName`
- OOP module initializer: `__mod_init$oop`

## BASIC Method Dispatch

The BASIC method-call lowerer resolves calls in this order:

1. static user-class calls
2. runtime-catalog methods
3. `Zanna.Core.Object` fallback methods
4. explicit interface dispatch through an `AS Interface` receiver
5. virtual dispatch through the object's vtable slot
6. direct calls to the selected mangled method

`BASE.Method(...)` forces direct dispatch to the immediate base implementation
and passes `ME` as the leading receiver argument.

For virtual calls, BASIC loads the table pointer from payload offset 0, computes
the slot address, loads the function pointer, and emits `call.indirect` with the
receiver as the first argument.

For interface calls, BASIC looks up the receiver's interface table with
`rt_itable_lookup(receiver, iface_id)`, loads `itable[slot]`, and emits
`call.indirect`.

## Zia Class And Interface Lowering

Zia uses the same runtime object allocation helper and runtime class/interface
registry, but its current class dispatch lowering is not identical to BASIC's
per-object vptr dispatch.

Zia layout constants in `src/frontends/zia/RuntimeNames.hpp` define:

- `kMachineWordSize == 8`
- `kObjectHeaderSize == 8`
- `kVtablePtrOffset == 8`
- `kVtablePtrSize == 8`
- `kClassFieldsOffset == 16`

`ClassTypeInfo` field offsets start at `kClassFieldsOffset`, include inherited
fields, and maintain a compile-time vtable slot list.

Zia virtual method lowering in `Lowerer_Dispatch.cpp` uses runtime class-id
dispatch:

1. It calls `rt_obj_class_id(receiver)`.
2. If there is one implementation for the slot, it emits a direct call.
3. If multiple implementations exist, it emits a `switch.i32` over class ids and
   direct calls in each case block.

Zia interface method lowering uses runtime itable dispatch:

1. It calls `rt_obj_class_id(receiver)`.
2. It calls `rt_get_interface_impl(class_id, iface_id)`.
3. It loads the function pointer from `itable[slot]`.
4. It emits `call.indirect` with the receiver as the first argument.

Zia also emits interface initialization (`__zia_iface_init`) that registers
classes, structs, interfaces, and interface implementation tables in the runtime
registry.

## ABI Boundaries

The stable shared boundary is the runtime heap and type registry ABI:

- payload pointers returned by `rt_obj_new_i64()`
- hidden `rt_heap_hdr_t` metadata immediately before each payload
- class ids stored in the heap header
- runtime class/interface registration helpers
- direct and indirect IL calls using receiver-as-first-argument convention

Do not assume all frontends use the same per-object payload header for every
dispatch path. BASIC currently uses payload offset 0 as the vptr for virtual
dispatch. Zia currently keeps class virtual dispatch class-id based and uses
itable lookup for interface dispatch.
