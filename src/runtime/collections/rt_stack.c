//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_stack.c
// Purpose: Implements Zanna.Collections.Stack, a LIFO (last-in-first-out)
//   dynamic collection backed by a contiguous dynamic array. Push and pop
//   operate on the top (highest index), providing O(1) amortized push and O(1)
//   pop with cache-friendly sequential memory layout.
//
// Key invariants:
//   - Initial capacity is STACK_DEFAULT_CAP (16); grows by STACK_GROWTH_FACTOR (2).
//   - The "top" of the stack is items[len-1]; push writes to items[len] and
//     increments len; pop reads items[len-1] and decrements len.
//   - Pop on an empty stack traps with a descriptive error message.
//   - Peek returns items[len-1] without removing it; Peek and Pop trap on an
//     empty stack (TryPop returns None instead).
//   - Stacks may own elements when created through the owning constructor; in
//     that mode push retains and pop returns a retained transfer to the caller.
//     Borrowing stacks store raw pointers without retain/release.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Stack objects are GC-managed (rt_obj_new_i64). The items array is
//     malloc-managed and freed by the GC finalizer (stack_finalizer).
//
// Links: src/runtime/collections/rt_stack.h (public API),
//        src/runtime/collections/rt_deque.h (double-ended queue, superset)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime growable LIFO Stack collection.
/// @details Stack uses a contiguous pointer array and selectable borrowing or
///          retained-element ownership. Ownership mode controls GC traversal
///          and result lifetimes but never changes bottom-to-top storage or
///          constant-time top access.

#include "rt_collection_ids.h"

#include "rt_box.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_stack_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Install a non-local recovery target for the current thread.
/// @param buf Jump buffer that receives control after a runtime trap.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's active legacy recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the most recent runtime trap diagnostic.
/// @return Null-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

/// @brief Pointer slots reserved by a newly constructed Stack.
#define STACK_DEFAULT_CAP 16
/// @brief Multiplicative capacity increase used when Push fills the array.
#define STACK_GROWTH_FACTOR 2

/// @brief Internal stack implementation structure.
///
/// The Stack is implemented as a dynamic array that grows as needed.
/// Elements are stored contiguously, with the "top" of the stack being
/// the element at index (len - 1). This provides O(1) push/pop operations
/// and cache-friendly memory access patterns.
///
/// Memory layout:
/// ```
/// Stack object (GC-managed):
///   +-----+-----+-------+
///   | len | cap | items |
///   |  3  | 16  | ----->|
///   +-----+-----+---|---+
///                   |
///                   v
/// items array (malloc'd):
///   +---+---+---+---+---+---+...+----+
///   | A | B | C | ? | ? | ? |   | ?  |
///   +---+---+---+---+---+---+...+----+
///   [0]  [1] [2]              [cap-1]
///         ^
///         | top = items[len-1] = C
/// ```
/// @brief Checked cast of an opaque handle to the Stack implementation;
///        traps with @p what if @p obj is NULL or not a Stack.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated Stack implementation, or `NULL` after trapping.
static rt_stack_impl *as_stack(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_STACK_CLASS_ID, sizeof(rt_stack_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_stack_impl *)obj;
}

/// @brief Drop one GC reference to a stored element and free it at zero.
/// @param value Runtime object reference, or `NULL` for a no-op.
static void stack_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void stack_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Finalizer callback invoked when a Stack is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// Stack object becomes unreachable. It frees the internal items array to
/// prevent memory leaks.
///
/// @param obj Pointer to the Stack object being finalized. May be NULL (no-op).
///
/// @note Owning stacks release each stored element during finalization. Borrowing
///       stacks skip element release so the same object can be shared elsewhere.
/// @note This function is idempotent - safe to call on already-finalized stacks.
///
/// @see rt_stack_clear For removing elements without finalization
static void rt_stack_finalize(void *obj) {
    if (!obj)
        return;
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (stack->owns_elements && stack->items) {
        int64_t len = stack->len;
        stack->len = 0;
        for (int64_t i = 0; i < len; i++) {
            void *value = stack->items[i];
            stack->items[i] = NULL;
            stack_release_value(value);
        }
    }
    free(stack->items);
    stack->items = NULL;
    stack->len = 0;
    stack->cap = 0;
}

/// @brief GC traversal: visit every live element (only when the stack owns
///        its elements).
/// @param obj Stack whose owned slots are to be traced.
/// @param visitor Collector callback invoked from bottom to top.
/// @param ctx Opaque collector context forwarded unchanged.
static void rt_stack_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack->owns_elements || !stack->items)
        return;
    for (int64_t i = 0; i < stack->len; i++)
        visitor(stack->items[i], ctx);
}

/// @brief Ensures the stack has capacity for at least `needed` elements.
///
/// If the current capacity is insufficient, the items array is reallocated
/// to a larger size. Growth is exponential (doubling) to amortize allocation
/// costs over many push operations, giving O(1) amortized push complexity.
///
/// **Growth strategy:**
/// - Capacity doubles each time growth is needed
/// - Starting capacity is 16 (STACK_DEFAULT_CAP)
/// - Growth sequence: 16 → 32 → 64 → 128 → 256 → ...
///
/// @param stack Pointer to the stack implementation. Must not be NULL.
/// @param needed Minimum required capacity after this call.
/// @return 1 when capacity is sufficient, or 0 after an overflow/allocation
///         trap.
///
/// @note Traps on memory allocation failure with "Stack: memory allocation failed".
/// @note Never shrinks the capacity - only grows when needed.
///
/// @see rt_stack_push For the primary user of this function
static int stack_ensure_capacity(rt_stack_impl *stack, int64_t needed) {
    if (needed <= stack->cap)
        return 1;

    int64_t new_cap = stack->cap;
    while (new_cap < needed) {
        if (new_cap > INT64_MAX / STACK_GROWTH_FACTOR) {
            rt_trap("Stack: capacity overflow");
            return 0;
        }
        new_cap *= STACK_GROWTH_FACTOR;
    }

    if ((uint64_t)new_cap > SIZE_MAX / sizeof(void *)) {
        rt_trap("Stack: allocation size overflow");
        return 0;
    }
    void **new_items = realloc(stack->items, (size_t)new_cap * sizeof(void *));
    if (!new_items) {
        rt_trap("Stack: memory allocation failed");
        return 0;
    }

    stack->items = new_items;
    stack->cap = new_cap;
    return 1;
}

/// @brief Creates a new empty Stack with default capacity.
///
/// Allocates and initializes a Stack data structure for LIFO (Last-In-First-Out)
/// operations. The Stack starts with a default capacity of 16 slots and grows
/// automatically as elements are pushed.
///
/// The Stack is allocated through Zanna's garbage-collected object system,
/// meaning it will be automatically freed when no longer referenced. A finalizer
/// is registered to clean up the internal items array.
///
/// **Usage example:**
/// ```
/// Dim stack = Stack.New()
/// stack.Push("first")
/// stack.Push("second")
/// stack.Push("third")
/// Print stack.Pop()   ' Outputs: third
/// Print stack.Pop()   ' Outputs: second
/// Print stack.Pop()   ' Outputs: first
/// ```
///
/// @return A pointer to the newly created Stack object. Traps and does not
///         return if memory allocation fails.
///
/// @note Initial capacity is 16 elements (STACK_DEFAULT_CAP).
/// @note A new Stack starts in borrowing mode. Retained ownership may be
///       enabled with @ref rt_stack_set_owns_elements while empty.
/// @note Thread safety: Not thread-safe. External synchronization required
///       for concurrent access.
///
/// @see rt_stack_push For adding elements
/// @see rt_stack_pop For removing elements
/// @see rt_stack_finalize For cleanup behavior
void *rt_stack_new(void) {
    rt_stack_impl *stack =
        (rt_stack_impl *)rt_obj_new_i64(RT_STACK_CLASS_ID, (int64_t)sizeof(rt_stack_impl));
    if (!stack) {
        rt_trap("Stack: memory allocation failed");
        return NULL;
    }

    stack->len = 0;
    stack->cap = STACK_DEFAULT_CAP;
    stack->owns_elements = 0;
    stack->items = malloc((size_t)STACK_DEFAULT_CAP * sizeof(void *));
    rt_obj_set_finalizer(stack, rt_stack_finalize);
    rt_gc_track(stack, rt_stack_traverse);

    if (!stack->items) {
        if (rt_obj_release_check0(stack))
            rt_obj_free(stack);
        rt_trap("Stack: memory allocation failed");
        return NULL;
    }

    return stack;
}

/// @brief Select borrowing or retained-element ownership for an empty Stack.
/// @details A mode change while non-empty traps because existing slots were
///          inserted under the current lifetime contract. A null Stack is a
///          no-op.
/// @param obj Stack to configure, or `NULL`.
/// @param owns Nonzero to retain/trace/release elements; zero to borrow them.
void rt_stack_set_owns_elements(void *obj, int8_t owns) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack) {
        rt_gc_mutator_exit();
        return;
    }
    owns = owns ? 1 : 0;
    if (stack->len != 0 && stack->owns_elements != owns) {
        rt_gc_mutator_exit();
        rt_trap("Stack.SetOwnsElements: cannot change ownership mode on non-empty stack");
        return;
    }
    stack->owns_elements = owns;
    rt_gc_mutator_exit();
}

/// @brief Report whether a Stack retains and traces its elements.
/// @param obj Stack handle, or `NULL`.
/// @return 1 in owning mode, otherwise 0; `NULL` reports 0.
int8_t rt_stack_owns_elements(void *obj) {
    if (!obj)
        return 0;
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    return stack && stack->owns_elements ? 1 : 0;
}

/// @brief Returns the number of elements currently on the Stack.
///
/// This function returns how many elements have been pushed and not yet popped.
/// The count is maintained internally and returned in O(1) time.
///
/// @param obj Pointer to a Stack object. If NULL, returns 0.
///
/// @return The number of elements on the Stack (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_stack_is_empty For a boolean check
/// @see rt_stack_push For operations that increase the count
/// @see rt_stack_pop For operations that decrease the count
int64_t rt_stack_len(void *obj) {
    if (!obj)
        return 0;
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    return stack ? stack->len : 0;
}

/// @brief Checks whether the Stack contains no elements.
///
/// A Stack is considered empty when its length is 0, which occurs:
/// - Immediately after creation
/// - After all elements have been popped
/// - After calling rt_stack_clear
///
/// Calling Pop or Peek on an empty Stack will trap with an error.
///
/// @param obj Pointer to a Stack object. If NULL, returns true (1).
///
/// @return 1 (true) if the Stack is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_stack_len For the exact count
/// @see rt_stack_pop For removing elements (traps if empty)
/// @see rt_stack_peek For viewing top element (traps if empty)
int8_t rt_stack_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    return !stack || stack->len == 0 ? 1 : 0;
}

/// @brief Pushes an element onto the top of the Stack.
///
/// Adds a new element to the top of the Stack. This is the primary insertion
/// operation for LIFO behavior - the most recently pushed element will be
/// the first one returned by Pop.
///
/// If the Stack's capacity is exceeded, it automatically grows to accommodate
/// the new element. Growth is exponential (doubling) for O(1) amortized time.
///
/// **Visual example:**
/// ```
/// Before Push(D):  [A, B, C]  (top = C)
/// After Push(D):   [A, B, C, D]  (top = D)
/// ```
///
/// @param obj Pointer to a Stack object. Must not be NULL.
/// @param elem The element to push. May be NULL (NULL is a valid element).
///
/// @note O(1) amortized time complexity. Occasional O(n) when resizing occurs.
/// @note Owning Stacks retain @p elem; borrowing Stacks store it raw.
/// @note Traps with "Stack.Push: null stack" if obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_stack_pop For the inverse operation
/// @see rt_stack_peek For viewing without removing
void rt_stack_push(void *obj, void *elem) {
    if (!obj) {
        rt_trap("Stack.Push: null stack");
        return;
    }

    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack) {
        rt_gc_mutator_exit();
        return;
    }

    if (stack->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Stack: maximum length reached");
        return;
    }
    if (!stack_ensure_capacity(stack, stack->len + 1)) {
        rt_gc_mutator_exit();
        return;
    }
    if (stack->owns_elements &&
        !rt_collection_retain_checked(elem, "Stack.Push: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    stack->items[stack->len] = elem;
    stack->len++;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the top element from the Stack.
///
/// Removes the most recently pushed element (the "top" of the Stack) and
/// returns it. This is the primary retrieval operation for LIFO behavior.
///
/// **Visual example:**
/// ```
/// Before Pop():    [A, B, C, D]  (top = D)
/// After Pop():     [A, B, C]     (top = C)
/// Returns: D
/// ```
///
/// **Error handling:**
/// Calling Pop on an empty Stack is a programming error and traps with
/// "Stack.Pop: stack is empty". Always check rt_stack_is_empty before
/// popping, or use a try-catch pattern if available.
///
/// @param obj Pointer to a Stack object. Must not be NULL.
///
/// @return The element that was on top of the Stack.
///
/// @note O(1) time complexity.
/// @note Owning Stacks return a caller-retained value; borrowing Stacks return
///       the raw pushed pointer without extending its lifetime.
/// @note Traps if the Stack is empty or obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_stack_push For the inverse operation
/// @see rt_stack_peek For viewing without removing
/// @see rt_stack_is_empty For checking before pop
void *rt_stack_pop(void *obj) {
    if (!obj) {
        rt_trap("Stack.Pop: null stack");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (stack->len == 0) {
        rt_gc_mutator_exit();
        rt_trap("Stack.Pop: stack is empty");
        return NULL;
    }

    void *value = stack->items[stack->len - 1];
    stack->len--;
    stack->items[stack->len] = NULL;
    // Owning stacks move the stored reference to the caller. Borrowing stacks
    // continue to return their raw stored pointer.
    rt_gc_mutator_exit();
    return value;
}

/// @brief Returns the top element without removing it from the Stack.
///
/// Peeks at the most recently pushed element without modifying the Stack.
/// This is useful for:
/// - Inspecting the next element to be popped
/// - Implementing conditional pop logic
/// - Debugging or logging
///
/// **Example:**
/// ```
/// stack.Push("A")
/// stack.Push("B")
/// Print stack.Peek()  ' Outputs: B
/// Print stack.Peek()  ' Outputs: B (still there)
/// Print stack.Pop()   ' Outputs: B (now removed)
/// Print stack.Peek()  ' Outputs: A
/// ```
///
/// @param obj Pointer to a Stack object. Must not be NULL.
///
/// @return The element on top of the Stack (not removed).
///
/// @note O(1) time complexity.
/// @note No retain is created. In owning mode the result is borrowed from the
///       Stack; in borrowing mode the original producer controls lifetime.
/// @note Traps if the Stack is empty or obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_stack_pop For removing while retrieving
/// @see rt_stack_is_empty For checking before peek
void *rt_stack_peek(void *obj) {
    if (!obj) {
        rt_trap("Stack.Peek: null stack");
        return NULL;
    }

    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack)
        return NULL;

    if (stack->len == 0) {
        rt_trap("Stack.Peek: stack is empty");
        return NULL;
    }

    return stack->items[stack->len - 1];
}

/// @brief Removes all elements from the Stack.
///
/// Clears the Stack by resetting its length to 0. The capacity remains
/// unchanged (no memory is freed), allowing the Stack to be efficiently
/// reused for new elements.
///
/// **After clear:**
/// - Length becomes 0
/// - is_empty returns true
/// - Capacity unchanged (no reallocation)
/// - Owning values are released; borrowing pointers are forgotten
///
/// @param obj Pointer to a Stack object. If NULL, this is a no-op.
///
/// @note O(n) time complexity because all active slots are cleared; owning
///       Stacks additionally release every value.
/// @note Active slots are cleared so released handles do not remain in
///       reusable storage.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_stack_finalize For complete cleanup (including the array)
/// @see rt_stack_is_empty For checking if empty
void rt_stack_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack) {
        rt_gc_mutator_exit();
        return;
    }
    int64_t len = stack->len;
    stack->len = 0;
    if (stack->owns_elements) {
        for (int64_t i = 0; i < len; i++) {
            void *value = stack->items[i];
            stack->items[i] = NULL;
            stack_release_value(value);
        }
    } else {
        for (int64_t i = 0; i < len; i++)
            stack->items[i] = NULL;
    }
    rt_gc_mutator_exit();
}

/// @brief Check whether the Stack contains an equal element.
/// @details Scans bottom to top with `rt_box_equal`: boxed numeric/string
///          values compare by content and ordinary objects by identity.
/// @param obj Stack handle, or `NULL`.
/// @param elem Element to compare; may be `NULL`.
/// @return 1 if found, 0 otherwise.
int8_t rt_stack_has(void *obj, void *elem) {
    if (!obj)
        return 0;

    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack)
        return 0;
    for (int64_t i = 0; i < stack->len; i++) {
        if (rt_box_equal(stack->items[i], elem))
            return 1;
    }
    return 0;
}

/// @brief Pop the top element, or return NULL if empty (no trap).
/// @details Ownership matches @ref rt_stack_pop. A stored null is ambiguous
///          with an empty/null Stack; use the Option form when that matters.
/// @param obj Stack handle, or `NULL`.
/// @return Removed top value, or `NULL` if unavailable/null-valued.
void *rt_stack_try_pop(void *obj) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack || stack->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void *value = stack->items[stack->len - 1];
    stack->len--;
    stack->items[stack->len] = NULL;
    // Transfer the owning stack's stored reference; borrowing mode remains a
    // raw-pointer removal.
    rt_gc_mutator_exit();
    return value;
}

/// @brief Pop the top element as an Option, preserving NULL as a present value.
/// @details Returns `None` only when the stack is empty. If the top element is a
///          literal NULL pointer, this returns `Some(NULL)`. The Option is
///          constructed before the stack is mutated, so a failed retain leaves
///          the top element in place.
/// @param obj Stack handle, or `NULL`.
/// @return New runtime-managed `Some(value)` when removed, otherwise `None`.
void *rt_stack_try_pop_option(void *obj) {
    if (!obj)
        return rt_option_none();

    rt_gc_mutator_enter();
    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (stack->len == 0) {
        rt_gc_mutator_exit();
        return rt_option_none();
    }

    void *value = stack->items[stack->len - 1];
    void *option = rt_option_some(value);
    if (!option) {
        rt_gc_mutator_exit();
        return NULL;
    }

    stack->len--;
    stack->items[stack->len] = NULL;
    if (stack->owns_elements)
        stack_release_value(value);
    rt_gc_mutator_exit();
    return option;
}

/// @brief Create a shallow copy of the stack.
///
/// Allocates a new Stack and pushes all elements from the source in
/// bottom-to-top order, preserving the original stack ordering.
/// The ownership mode is preserved: an owning clone independently retains
/// values, while a borrowing clone copies raw pointers.
///
/// @param obj Source Stack handle, or `NULL`.
/// @return New runtime-managed clone, or an empty borrowing Stack for `NULL`.
void *rt_stack_clone(void *obj) {
    if (!obj)
        return rt_stack_new();

    rt_stack_impl *stack = as_stack(obj, "Stack: invalid Stack object");
    if (!stack)
        return NULL;

    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        stack_save_trap(saved_error, sizeof(saved_error), "Stack.Clone: copy failed");
        rt_trap_clear_recovery();
        stack_release_value((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    result = rt_stack_new();
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (stack->owns_elements)
        rt_stack_set_owns_elements((void *)result, 1);
    for (int64_t i = 0; i < stack->len; i++) {
        rt_stack_push((void *)result, stack->items[i]);
    }
    rt_trap_clear_recovery();
    return (void *)result;
}
