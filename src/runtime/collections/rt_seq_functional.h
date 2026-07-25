//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_seq_functional.h
// Purpose: IL-compatible wrapper functions for Seq functional operations (filter, reject, map,
// reduce, each, find) accepting function pointers as void* for IL calling convention.
//
// Key invariants:
//   - Function pointers are passed as void* and cast internally to the correct type.
//   - Predicate callbacks must be of type int8_t (*)(void *) for filter/reject/find.
//   - Map callbacks must be of type void *(*)(void *) for element transformation.
//   - These wrappers bridge the IL calling convention to the typed Seq API.
//
// Ownership/Lifetime:
//   - Returned Seq and Option objects are runtime/GC-managed.
//   - Source sequences are not modified or consumed.
//   - Borrowed element/accumulator results follow the delegated typed API.
//
// Links: src/runtime/collections/rt_seq_functional.c (implementation),
// src/runtime/collections/rt_seq.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares IL-callable trampolines for callback-driven Seq operations.
/// @details Each wrapper accepts a callback in the registry's opaque
///          representation, casts it to the operation-specific signature, and
///          delegates without adding state or changing ownership semantics.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Keep elements matching predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque `int8_t (*)(void *)`; `NULL` clones the source.
/// @return New runtime-managed Seq with matching elements.
void *rt_seq_keep_wrapper(void *seq, void *pred);

/// @brief Reject elements matching predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` clones the source.
/// @return New runtime-managed Seq containing nonmatching elements.
void *rt_seq_reject_wrapper(void *seq, void *pred);

/// @brief Apply transform to each element (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param fn Opaque `void *(*)(void *)`; `NULL` clones the source.
/// @return New runtime-managed owning Seq of transformed values.
void *rt_seq_apply_wrapper(void *seq, void *fn);

/// @brief Check if all elements match predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate, or `NULL`.
/// @return 1 for universal/vacuous truth, otherwise 0.
int8_t rt_seq_all_wrapper(void *seq, void *pred);

/// @brief Check if any element matches predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate, or `NULL`.
/// @return 1 on the first match, otherwise 0.
int8_t rt_seq_any_wrapper(void *seq, void *pred);

/// @brief Check if no elements match predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate, or `NULL`.
/// @return 1 when no element matches, otherwise 0.
int8_t rt_seq_none_wrapper(void *seq, void *pred);

/// @brief Count elements matching predicate (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` counts all elements.
/// @return Matching element count.
int64_t rt_seq_count_where_wrapper(void *seq, void *pred);

/// @brief Find first element matching predicate (wrapper for IL).
/// @details Returns a borrowed value and cannot distinguish absence from a
///          matching stored null.
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` selects the first element.
/// @return Borrowed match, or `NULL`.
void *rt_seq_find_where_wrapper(void *seq, void *pred);

/// @brief Find first element matching predicate as an Option (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` selects the first element.
/// @return New runtime-managed Option containing the first match, or `None`.
void *rt_seq_find_where_option_wrapper(void *seq, void *pred);

/// @brief Take elements while predicate is true (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` clones the source.
/// @return New runtime-managed leading-prefix Seq.
void *rt_seq_take_while_wrapper(void *seq, void *pred);

/// @brief Drop elements while predicate is true (wrapper for IL).
/// @param seq Source Seq, or `NULL`.
/// @param pred Opaque predicate; `NULL` produces an empty Seq.
/// @return New runtime-managed suffix Seq.
void *rt_seq_drop_while_wrapper(void *seq, void *pred);

/// @brief Fold/reduce sequence with accumulator (wrapper for IL).
/// @details The wrapper does not retain or release accumulator values.
/// @param seq Source Seq, or `NULL`.
/// @param init Initial accumulator.
/// @param fn Opaque `void *(*)(void *, void *)`, or `NULL`.
/// @return Final accumulator, or @p init when no fold runs.
void *rt_seq_fold_wrapper(void *seq, void *init, void *fn);

#ifdef __cplusplus
}
#endif
