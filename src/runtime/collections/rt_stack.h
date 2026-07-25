//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_stack.h
// Purpose: Runtime-backed LIFO stack for Zanna.Collections.Stack, providing push/pop/peek with
// automatic growth and O(1) operations.
//
// Key invariants:
//   - LIFO ordering: push and pop both operate on the top.
//   - Pop and peek on an empty stack trap immediately.
//   - Internal array doubles capacity on overflow.
//   - By default elements are borrowed; runtime conversion helpers can enable
//     retained-element ownership before inserting values.
//
// Ownership/Lifetime:
//   - Stack objects are runtime/GC-managed opaque pointers.
//   - Owned-element stacks retain on push and release on pop/clear/finalize.
//   - Borrowing stacks never extend element lifetime.
//
// Error conventions:
//   - Allocation failure → rt_trap()
//   - Pop/Peek on empty → rt_trap()
//   - TryPop on empty → returns NULL
//
// Links: src/runtime/collections/rt_stack.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the growable LIFO Stack collection.
/// @details Stacks start in borrowing mode and can switch to retained-element
///          ownership only while empty. The choice controls retain/release and
///          GC traversal; storage and traversal order remain bottom to top.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty stack with default capacity.
/// @return New runtime-managed borrowing Stack with sixteen reserved slots.
void *rt_stack_new(void);

/// @brief Enable or disable retained-element ownership for an empty stack.
/// @details Traps on a mode change while non-empty. A null Stack is a no-op.
/// @param obj Stack handle, or `NULL`.
/// @param owns Nonzero to retain/trace/release elements; zero to borrow them.
void rt_stack_set_owns_elements(void *obj, int8_t owns);

/// @brief Return whether the stack retains its elements.
/// @param obj Stack handle, or `NULL`.
/// @return 1 in owning mode, otherwise 0.
int8_t rt_stack_owns_elements(void *obj);

/// @brief Get the number of elements on the stack.
/// @param obj Stack handle, or `NULL`.
/// @return Live element count, or 0 for `NULL`.
int64_t rt_stack_len(void *obj);

/// @brief Check if the stack is empty.
/// @param obj Stack handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_stack_is_empty(void *obj);

/// @brief Push an element onto the top of the stack.
/// @details Runs in amortized O(1). Owning Stacks retain the value; borrowing
///          Stacks store it raw. A null Stack traps.
/// @param obj Non-null Stack.
/// @param elem Element to push; may be `NULL`.
void rt_stack_push(void *obj, void *elem);

/// @brief Pop and return the top element from the stack.
/// @details Owning Stacks return a caller-retained value; borrowing Stacks
///          return the raw pointer. Null/empty Stacks trap.
/// @param obj Non-null Stack.
/// @return Removed top value, which may be a stored `NULL`.
void *rt_stack_pop(void *obj);

/// @brief Return the top element without removing it.
/// @details Creates no retain; the result is borrowed. Null/empty Stacks trap.
/// @param obj Non-null Stack.
/// @return Borrowed top value, which may be `NULL`.
void *rt_stack_peek(void *obj);

/// @brief Remove all elements from the stack.
/// @details Clears active slots, releases values in owning mode, and preserves
///          capacity/ownership mode. A null Stack is a no-op.
/// @param obj Stack handle, or `NULL`.
void rt_stack_clear(void *obj);

/// @brief Check if the stack contains an equal element.
/// @details Uses boxed-value equality for boxed numeric/string values and
///          identity for ordinary objects.
/// @param obj Stack handle, or `NULL`.
/// @param elem Element to search for; may be `NULL`.
/// @return 1 if found, 0 otherwise.
int8_t rt_stack_has(void *obj, void *elem);

/// @brief Convert the stack to a List (bottom-to-top order).
/// @details Implemented by the conversion layer without mutating the source;
///          the result independently retains its values.
/// @param obj Stack handle, or `NULL`.
/// @return New runtime-managed List, empty for `NULL`.
void *rt_stack_to_list(void *obj);

/// @brief Pop the top element, or return NULL if empty (no trap).
/// @details Ownership follows @ref rt_stack_pop. A stored null is ambiguous
///          with absence.
/// @param obj Stack handle, or `NULL`.
/// @return Removed value, or `NULL` if unavailable/null-valued.
void *rt_stack_try_pop(void *obj);

/// @brief Pop the top element as an Option.
/// @details Returns `None` when the stack is empty and `Some(value)` when an
///          element is removed. A stored NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_stack_try_pop.
/// @param obj Stack handle, or `NULL`.
/// @return New runtime-managed Zanna.Option object.
void *rt_stack_try_pop_option(void *obj);

/// @brief Create a shallow copy of the stack.
/// @details Preserves bottom-to-top order and ownership mode. Owning clones
///          independently retain values.
/// @param obj Source Stack, or `NULL`.
/// @return New runtime-managed clone, empty borrowing Stack for `NULL`.
void *rt_stack_clone(void *obj);

#ifdef __cplusplus
}
#endif
