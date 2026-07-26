//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_seq_functional.c
// Purpose: Provides C-callable wrapper functions for Seq's higher-order
//   operations (Keep/filter, Reject, Apply/map, All, Any, Fold/reduce) that
//   accept function pointers as void* and internally cast them to the correct
//   typed function pointer before delegating to the corresponding rt_seq_*
//   implementation in rt_seq_ops.c. This indirection is required because the IL
//   runtime signature system passes all callables as untyped void* pointers.
//
// Key invariants:
//   - Each wrapper has a 1:1 correspondence with an rt_seq_* function in
//     rt_seq_ops.c.
//   - The cast from void* to typed function pointer is safe because the IL
//     frontend and runtime.def ensure the actual function passed always has
//     the matching signature (predicate_fn, transform_fn, or reducer_fn).
//   - No state is held in this file; all wrappers are pure forwarding functions.
//   - Adding a new functional Seq operation requires: an rt_seq_* implementation
//     in rt_seq_ops.c, a new typedef here if the callback type is new, and a new
//     rt_seq_*_wrapper function here.
//
// Ownership/Lifetime:
//   - No objects are allocated or freed here. Ownership follows the semantics
//     of the underlying rt_seq_* functions (returned Seqs are GC-managed).
//
// Links: src/runtime/collections/rt_seq.h (functional API declarations),
//        src/runtime/collections/rt_seq_ops.c (actual implementations)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Adapts opaque IL callable handles to typed Seq callback APIs.
/// @details The runtime registry exposes callbacks through `void *`. Each
///          trampoline performs exactly one ABI-governed cast and forwards all
///          null handling, traversal, allocation, ownership, and short-circuit
///          behavior to the corresponding typed operation.

#include "rt_seq.h"

#include <stdint.h>

/// @brief Typed predicate signature guaranteed by the IL runtime registry.
/// @param value Borrowed sequence element to test.
/// @return Nonzero when @p value satisfies the predicate; otherwise zero.
typedef int8_t (*predicate_fn)(void *);
/// @brief Typed element-transform signature guaranteed by the IL registry.
/// @param value Borrowed sequence element to transform.
/// @return Runtime value produced for @p value.
typedef void *(*transform_fn)(void *);
/// @brief Typed left-fold signature guaranteed by the IL runtime registry.
/// @param accumulator Current accumulated runtime value.
/// @param value Borrowed sequence element to incorporate.
/// @return Updated accumulator passed to the next reduction step.
typedef void *(*reducer_fn)(void *, void *);

//=============================================================================
// Wrapper Functions
//=============================================================================

/// @brief Filter a Seq through an opaque IL predicate.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` clones the source.
/// @return New runtime-managed Seq preserving source ownership mode and
///         containing matching values.
void *rt_seq_keep_wrapper(void *seq, void *pred) {
    return rt_seq_keep(seq, (predicate_fn)pred);
}

/// @brief Exclude values selected by an opaque IL predicate.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` clones the source.
/// @return New runtime-managed Seq preserving source ownership mode and
///         containing nonmatching values.
void *rt_seq_reject_wrapper(void *seq, void *pred) {
    return rt_seq_reject(seq, (predicate_fn)pred);
}

/// @brief Transform every Seq element through an opaque IL callable.
/// @param seq Source Seq, or `NULL`.
/// @param fn Opaque `transform_fn`; `NULL` clones the source.
/// @return New runtime-managed owning Seq of transformed results.
void *rt_seq_apply_wrapper(void *seq, void *fn) {
    return rt_seq_apply(seq, (transform_fn)fn);
}

/// @brief Check if all elements satisfy a predicate (opaque function pointer wrapper).
/// @details Short-circuits at the first false result. Null sequences and null
///          predicates produce vacuous truth.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`, or `NULL`.
/// @return 1 if all elements match, otherwise 0.
int8_t rt_seq_all_wrapper(void *seq, void *pred) {
    return rt_seq_all(seq, (predicate_fn)pred);
}

/// @brief Check if any element satisfies a predicate (opaque function pointer wrapper).
/// @details Short-circuits at the first true result. Null sequences or
///          predicates produce false.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`, or `NULL`.
/// @return 1 if at least one element matches, 0 if none do.
int8_t rt_seq_any_wrapper(void *seq, void *pred) {
    return rt_seq_any(seq, (predicate_fn)pred);
}

/// @brief Check if no elements satisfy a predicate (opaque function pointer wrapper).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`, or `NULL`.
/// @return 1 if no elements match, 0 if any do.
int8_t rt_seq_none_wrapper(void *seq, void *pred) {
    return rt_seq_none(seq, (predicate_fn)pred);
}

/// @brief Count elements satisfying a predicate (opaque function pointer wrapper).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` counts every element.
/// @return Number of matching elements, or 0 for a null source.
int64_t rt_seq_count_where_wrapper(void *seq, void *pred) {
    return rt_seq_count_where(seq, (predicate_fn)pred);
}

/// @brief Borrow the first element selected by an opaque predicate.
/// @details A null predicate selects the first element. A null return is
///          ambiguous with a matching stored null; use the Option wrapper when
///          that distinction matters.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`, or `NULL`.
/// @return Borrowed matching value, or `NULL` when absent/null-valued.
void *rt_seq_find_where_wrapper(void *seq, void *pred) {
    return rt_seq_find_where(seq, (predicate_fn)pred);
}

/// @brief IL trampoline for sentinel-free `Seq.FindWhereOption`.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` selects the first element.
/// @return New runtime-managed Option containing the first match, or `None`.
void *rt_seq_find_where_option_wrapper(void *seq, void *pred) {
    return rt_seq_find_where_option(seq, (predicate_fn)pred);
}

/// @brief Copy the longest leading prefix accepted by an opaque predicate.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` clones the source.
/// @return New runtime-managed Seq preserving source ownership mode.
void *rt_seq_take_while_wrapper(void *seq, void *pred) {
    return rt_seq_take_while(seq, (predicate_fn)pred);
}

/// @brief Copy the suffix after an opaque predicate stops matching.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `predicate_fn`; `NULL` produces an empty like-mode Seq.
/// @return New runtime-managed Seq preserving source ownership mode.
void *rt_seq_drop_while_wrapper(void *seq, void *pred) {
    return rt_seq_drop_while(seq, (predicate_fn)pred);
}

/// @brief Fold a Seq left through an opaque reducer.
/// @details The wrapper does not retain or release accumulator values; the
///          callback contract determines their ownership.
/// @param seq Source Seq, or `NULL`.
/// @param init Initial accumulator returned unchanged for null inputs.
/// @param fn Opaque `reducer_fn`, or `NULL`.
/// @return Final callback-produced accumulator, or @p init when no fold runs.
void *rt_seq_fold_wrapper(void *seq, void *init, void *fn) {
    return rt_seq_fold(seq, init, (reducer_fn)fn);
}
