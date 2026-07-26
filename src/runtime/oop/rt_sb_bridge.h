//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/oop/rt_sb_bridge.h
// Purpose: Prototypes for the StringBuilder bridge that creates a heap-managed,
// reference-counted StringBuilder object for Zanna.Text.StringBuilder.
//
// Key invariants:
//   - Constructors return heap-managed, reference-counted object pointers.
//   - The vptr field is always at offset 0 in every returned object.
//   - All returned objects start with refcount 1.
//   - Construction leaves the reserved vptr slot zero-initialized; the
//     StringBuilder runtime methods access embedded state directly.
//
// Ownership/Lifetime:
//   - Objects returned are managed by the runtime object heap.
//   - Callers must release objects according to refcounting rules.
//
// Links: src/runtime/oop/rt_sb_bridge.c (implementation), src/runtime/core/rt_heap.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sb_bridge.h
 * @brief Declares construction of managed native StringBuilder wrappers.
 * @details Each returned object begins with an OOP-compatible reserved vtable
 *          slot, embeds initialized builder state, starts with one caller-owned
 *          reference, and finalizes any dynamically grown buffer through the
 *          normal managed-object lifecycle.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate an opaque object instance for Zanna.Text.StringBuilder.
/// @details Bridges OOP allocation to the C runtime for use by the VM.
///          Creates a generic-class heap object with a zero-initialized leading
///          vptr slot, an initialized embedded `rt_string_builder`, and a
///          finalizer that releases grown buffer storage. No vtable is installed
///          by this constructor.
/// @return Caller-owned StringBuilder wrapper, or @c NULL after an allocation
///         failure trap.
void *rt_sb_new(void);

#ifdef __cplusplus
}
#endif
