//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_modvar.h
// Purpose: Runtime support for module-level (global) BASIC variables, providing stable address
// lookup by name and type for i64, f64, i1, pointer, and string variables.
//
// Key invariants:
//   - The same (name, type) pair always yields the same stable address.
//   - Addresses are allocated once per name+type combination and never moved.
//   - Variable names may be empty; identity uses NUL-terminated text plus kind,
//     so bytes after an embedded NUL do not distinguish variables.
//   - String variables store rt_string pointers; the slot is initialized to NULL.
//
// Ownership/Lifetime:
//   - Storage is allocated once per RtContext and owned by that context.
//   - Freed when the owning context is cleaned up; callers must not free the
//     returned pointers.
//
// Links: src/runtime/core/rt_modvar.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares stable per-context storage for module-level variables.
/// @details Each accessor borrows the name, creates zero-initialized storage
///   on first use, and returns a context-owned address that remains stable until
///   that context is destroyed.

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Get the address of a 64-bit integer module variable.
/// @details Provides stable storage for BASIC global variables. Looks up or
///          creates an eight-byte slot keyed by name and integer kind. Reusing
///          the key validates the same storage size.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @return Context-owned stable storage address, or NULL if an error trap returns.
void *rt_modvar_addr_i64(rt_string name);

/// @brief Get the address of a 64-bit floating-point module variable.
/// @details Provides stable storage for BASIC global variables. Looks up or
///          creates a zero-initialized eight-byte slot keyed by name and
///          floating-point kind.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @return Context-owned stable storage address, or NULL if an error trap returns.
void *rt_modvar_addr_f64(rt_string name);

/// @brief Get the address of a boolean (i1) module variable.
/// @details Provides stable storage for BASIC global variables. Looks up or
///          creates a zero-initialized one-byte slot keyed by name and boolean kind.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @return Context-owned stable storage address, or NULL if an error trap returns.
void *rt_modvar_addr_i1(rt_string name);

/// @brief Get the address of a pointer module variable.
/// @details Provides stable storage for BASIC global variables. Looks up or
///          creates a zero-initialized eight-byte slot keyed by name and
///          pointer kind, matching the module ABI storage width.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @return Context-owned stable storage address, or NULL if an error trap returns.
void *rt_modvar_addr_ptr(rt_string name);

/// @brief Get the address of a string module variable.
/// @details Provides stable storage for BASIC global variables. Looks up or
///          creates a pointer-sized, NULL-initialized slot keyed by name and
///          string kind. The slot stores an `rt_string` handle; callers remain
///          responsible for retain/release operations when replacing its value.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @return Context-owned stable storage address, or NULL if an error trap returns.
void *rt_modvar_addr_str(rt_string name);

/// @brief Get the address of a module variable block with arbitrary size.
/// @details Supports arrays and records as module-level variables. Looks up
///          or creates a slot keyed by name and returns its address. The
///          allocated block is zero-initialized on first creation. Reusing the
///          same name with a different requested size traps.
/// @param name Borrowed valid runtime string; NULL or invalid handles trap.
/// @param size Positive size of the variable block in bytes.
/// @return Context-owned stable storage address, or NULL if validation,
///   allocation, or context acquisition traps and returns.
void *rt_modvar_addr_block(rt_string name, int64_t size);

/// @brief Test whether a byte range lies wholly inside one module-variable block.
/// @details The IL VM's memory validator consults this beside the runtime heap
///          registry: module-variable storage comes from rt_alloc, so it is
///          otherwise invisible to `rt_heap_contains_range` (ZB-28).
/// @param ptr First byte of the requested range.
/// @param bytes Number of bytes requested; zero-byte ranges are never owned.
/// @return 1 when the range lies inside one live module-variable block of the
///   current context, otherwise 0.
int8_t rt_modvar_contains_range(const void *ptr, size_t bytes);

#ifdef __cplusplus
}
#endif
