//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_lazyseq.c
// Purpose: Implements the lazy sequence (LazySeq) type for the Zanna collections
//          runtime. A LazySeq wraps a source sequence and a pipeline of
//          functional transformations (Map, Filter, TakeWhile, SkipWhile, etc.)
//          that are applied only when elements are materialized.
//
// Key invariants:
//   - Transformations are chained by wrapping a LazySeq in another LazySeq.
//   - Materialization (ToList, Count, ForEach) applies the full pipeline once.
//   - Composite nodes retain their source sequences until finalization.
//   - Map and Filter callback function pointers are not retained; callers
//     must ensure their lifetimes exceed the LazySeq's use.
//   - Wrapper functions (rt_lazyseq_w_*) adapt Zia-style fn pointers to C
//     function pointer types expected by the LazySeq API.
//
// Ownership/Lifetime:
//   - LazySeq objects are heap-allocated and managed by the runtime GC.
//   - The source sequence is retained by the LazySeq for its lifetime.
//
// Links: src/runtime/oop/rt_lazyseq.h (public API),
//        src/runtime/rt_seq.h (eager sequence used as source and output)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_lazyseq.c
 * @brief Implements single-pass lazy sequence sources and transformations.
 * @details Generator, range, repeat, and iteration sources feed composable map,
 *          filter, take, drop, predicate, concatenation, and zip stages.
 *          Elements are produced on demand, with retained source ownership and
 *          explicit exhaustion state allowing NULL to remain a valid value.
 */

#include "rt_lazyseq.h"
#include "rt_collection_ids.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_trap.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// LazySeq Types
//=============================================================================

typedef enum {
    LAZYSEQ_GENERATOR,  // User-provided generator function
    LAZYSEQ_RANGE,      // Integer range
    LAZYSEQ_REPEAT,     // Repeated value
    LAZYSEQ_ITERATE,    // Iterative function application
    LAZYSEQ_MAP,        // Transformed sequence
    LAZYSEQ_FILTER,     // Filtered sequence
    LAZYSEQ_TAKE,       // Bounded sequence
    LAZYSEQ_DROP,       // Skipping sequence
    LAZYSEQ_TAKE_WHILE, // Take while predicate
    LAZYSEQ_DROP_WHILE, // Drop while predicate
    LAZYSEQ_CONCAT,     // Concatenated sequences
    LAZYSEQ_ZIP,        // Zipped sequences
} lazyseq_type;

/// @brief Add a LazySeq range step without signed overflow.
/// @details Range iteration advances after yielding the current value. When the
///          next value would overflow `int64_t`, the helper reports failure so
///          the range can mark itself exhausted after the current element rather
///          than wrapping and yielding values outside the requested sequence.
/// @param cur Current range value.
/// @param step Positive or negative step.
/// @param out Receives the next value when the addition is representable.
/// @return 1 when `cur + step` fits in `int64_t`; 0 when it would overflow.
static int lazyseq_checked_step(int64_t cur, int64_t step, int64_t *out) {
    if (!out)
        return 0;
    if ((step > 0 && cur > INT64_MAX - step) || (step < 0 && cur < INT64_MIN - step))
        return 0;
    *out = cur + step;
    return 1;
}

/// Internal structure for LazySeq.
struct rt_lazyseq_impl {
    lazyseq_type type;
    int64_t index;    // Current position
    int8_t exhausted; // 1 if sequence ended
    int8_t peeked;    // 1 if we have a peeked value
    void *peek_value; // Cached peeked value

    union {
        // Generator
        struct {
            rt_lazyseq_gen_fn gen;
            void *state;
        } generator;

        // Range
        struct {
            int64_t current;
            int64_t end;
            int64_t step;
            int64_t value_storage; // per-seq storage for returned value
        } range;

        // Repeat
        struct {
            void *value;
            int64_t remaining; // -1 for infinite
        } repeat;

        // Iterate
        struct {
            void *current;
            /// @brief Produce the next iterate value from the current value.
            /// @param value Borrowed current runtime value.
            /// @return Next runtime value in the sequence.
            void *(*fn)(void *);
            int8_t started;
        } iterate;

        // Map/Filter
        struct {
            rt_lazyseq source;

            union {
                /// @brief Transform one borrowed source element.
                /// @param value Borrowed source value.
                /// @return Runtime value emitted by the mapped sequence.
                void *(*map_fn)(void *);
                /// @brief Test whether one borrowed source element should be emitted.
                /// @param value Borrowed source value.
                /// @return Nonzero to retain @p value; otherwise zero.
                int8_t (*filter_fn)(void *);
            };
        } transform;

        // Take/Drop
        struct {
            rt_lazyseq source;
            int64_t limit;
            int64_t consumed;
        } bounded;

        // TakeWhile/DropWhile
        struct {
            rt_lazyseq source;
            /// @brief Test the boundary condition for a take/drop-while sequence.
            /// @param value Borrowed source value.
            /// @return Nonzero while the predicate condition remains satisfied.
            int8_t (*pred)(void *);
            int8_t done;
        } predicated;

        // Concat
        struct {
            rt_lazyseq first;
            rt_lazyseq second;
            int8_t on_second;
        } concat;

        // Zip
        struct {
            rt_lazyseq seq1;
            rt_lazyseq seq2;
            /// @brief Combine one element from each zipped source sequence.
            /// @param left Borrowed element from the first sequence.
            /// @param right Borrowed element from the second sequence.
            /// @return Runtime value emitted for the pair.
            void *(*combine)(void *, void *);
        } zip;
    } data;
};

//=============================================================================
// Internal helpers
//=============================================================================

/// @brief Release a source LazySeq reference via the object release path.
/// @details Decrements the refcount and, if it reaches zero, runs the
///          source's finalizer (which recursively releases its own children)
///          before freeing the memory.
/// @param[in] source Retained source reference to release, or NULL.
static void release_source(rt_lazyseq source) {
    if (!source)
        return;
    if (rt_obj_release_check0(source))
        rt_obj_free(source);
}

/// @brief Finalizer for LazySeq objects.
/// @details Releases retained source sequences for composite types.
///          Installed on every LazySeq via alloc_lazyseq().
/// @param[in,out] obj LazySeq payload being finalized.
static void lazyseq_finalizer(void *obj) {
    struct rt_lazyseq_impl *seq = (struct rt_lazyseq_impl *)obj;
    if (!seq)
        return;

    switch (seq->type) {
        case LAZYSEQ_MAP:
        case LAZYSEQ_FILTER:
            release_source(seq->data.transform.source);
            seq->data.transform.source = NULL;
            break;
        case LAZYSEQ_TAKE:
        case LAZYSEQ_DROP:
            release_source(seq->data.bounded.source);
            seq->data.bounded.source = NULL;
            break;
        case LAZYSEQ_TAKE_WHILE:
        case LAZYSEQ_DROP_WHILE:
            release_source(seq->data.predicated.source);
            seq->data.predicated.source = NULL;
            break;
        case LAZYSEQ_CONCAT:
            release_source(seq->data.concat.first);
            release_source(seq->data.concat.second);
            seq->data.concat.first = NULL;
            seq->data.concat.second = NULL;
            break;
        case LAZYSEQ_ZIP:
            release_source(seq->data.zip.seq1);
            release_source(seq->data.zip.seq2);
            seq->data.zip.seq1 = NULL;
            seq->data.zip.seq2 = NULL;
            break;
        case LAZYSEQ_REPEAT:
            release_source(seq->data.repeat.value);
            seq->data.repeat.value = NULL;
            break;
        default:
            break;
    }
}

/// @brief Allocate a fresh LazySeq node with the given variant tag, zero-initialized union, and
/// the GC finalizer wired in. Caller fills the appropriate `seq->data.<variant>` fields.
/// @param[in] type Variant tag selecting the active union member.
/// @return Caller-owned managed LazySeq, or NULL after allocation failure and a trap.
static rt_lazyseq alloc_lazyseq(lazyseq_type type) {
    struct rt_lazyseq_impl *seq = (struct rt_lazyseq_impl *)rt_obj_new_i64(
        RT_LAZYSEQ_CLASS_ID, (int64_t)sizeof(struct rt_lazyseq_impl));
    if (!seq) {
        rt_trap("LazySeq: memory allocation failed");
        return NULL;
    }
    memset(seq, 0, sizeof(struct rt_lazyseq_impl));
    seq->type = type;
    rt_obj_set_finalizer(seq, lazyseq_finalizer);
    return seq;
}

//=============================================================================
// Creation
//=============================================================================

/// @brief Construct a generator-driven LazySeq. `gen(state, index, has_more)` is invoked on each
/// `_next` call; setting `*has_more = 0` ends the sequence. Returns NULL if `gen` is NULL.
/// Useful for arbitrary computed sources (file readers, network streams, infinite series).
/// @param[in] gen Borrowed generator callback.
/// @param[in,out] state Borrowed opaque state forwarded to @p gen.
/// @return Caller-owned generator LazySeq, or NULL for a null callback or allocation failure.
rt_lazyseq rt_lazyseq_new(rt_lazyseq_gen_fn gen, void *state) {
    if (!gen)
        return NULL;

    rt_lazyseq seq = alloc_lazyseq(LAZYSEQ_GENERATOR);
    if (!seq)
        return NULL;

    seq->data.generator.gen = gen;
    seq->data.generator.state = state;
    return seq;
}

/// @brief Construct an integer range LazySeq from `[start, end)` with the given `step` (positive
/// or negative; zero is rejected with NULL). Each `_next` returns a pointer to internal storage
/// holding the current value, so callers must consume it before the next call.
/// Overflow after the last representable yielded value exhausts the range without wrapping.
/// @param[in] start Inclusive first integer.
/// @param[in] end Exclusive directional bound.
/// @param[in] step Nonzero signed increment.
/// @return Caller-owned range LazySeq, or NULL after invalid step or allocation failure.
rt_lazyseq rt_lazyseq_range(int64_t start, int64_t end, int64_t step) {
    if (step == 0) {
        rt_trap("LazySeq.Range: step must be non-zero");
        return NULL;
    }

    rt_lazyseq seq = alloc_lazyseq(LAZYSEQ_RANGE);
    if (!seq)
        return NULL;

    seq->data.range.current = start;
    seq->data.range.end = end;
    seq->data.range.step = step;
    return seq;
}

/// @brief Construct a LazySeq that yields `value` exactly `count` times. Pass `count = -1` for an
/// infinite repeat (the canonical way to build infinite seeds for combinator pipelines).
/// @param[in] value Managed object retained for the sequence lifetime, or NULL.
/// @param[in] count Nonnegative yield count, or -1 for infinity.
/// @return Caller-owned repeat LazySeq, or NULL after invalid count or allocation failure.
rt_lazyseq rt_lazyseq_repeat(void *value, int64_t count) {
    // Only -1 is the documented infinite sentinel; other negatives are
    // mistakes, not requests for infinity (VDOC-092).
    if (count < -1) {
        rt_trap("LazySeq.Repeat: count must be >= 0 (or -1 for infinite)");
        return NULL;
    }
    rt_lazyseq seq = alloc_lazyseq(LAZYSEQ_REPEAT);
    if (!seq)
        return NULL;

    // Retain the repeated value for the node's lifetime (VDOC-091); the
    // finalizer releases it.
    rt_obj_retain_maybe(value);
    seq->data.repeat.value = value;
    seq->data.repeat.remaining = count;
    return seq;
}

/// @brief Construct an iterative LazySeq: yields `seed`, `fn(seed)`, `fn(fn(seed))`, ...
/// infinitely. Useful for sequences defined by recurrence (Fibonacci, geometric series, walk
/// states). Combine with `rt_lazyseq_take(n)` to get a finite prefix.
/// @param[in] seed Initial borrowed element returned first.
/// @param[in] fn Borrowed recurrence callback.
/// @return Caller-owned infinite LazySeq, or NULL for a null callback or allocation failure.
rt_lazyseq rt_lazyseq_iterate(void *seed, void *(*fn)(void *)) {
    if (!fn)
        return NULL;

    rt_lazyseq seq = alloc_lazyseq(LAZYSEQ_ITERATE);
    if (!seq)
        return NULL;

    seq->data.iterate.current = seed;
    seq->data.iterate.fn = fn;
    seq->data.iterate.started = 0;
    return seq;
}

/// @brief Release one caller-owned LazySeq reference.
/// @details When this is the final reference, the registered finalizer
///          recursively releases retained sources and frees the object. The
///          pointer must not be reused after its final release.
/// @param[in] seq Caller-owned LazySeq reference to release, or NULL.
void rt_lazyseq_destroy(rt_lazyseq seq) {
    if (!seq)
        return;

    // Release one reference. If this is the last reference, the finalizer
    // (lazyseq_finalizer) recursively releases child sources and frees the
    // object. This replaces the old manual recursive cleanup.
    if (rt_obj_release_check0(seq))
        rt_obj_free(seq);
}

//=============================================================================
// Element Access
//=============================================================================

/// @brief Pull the next element from the LazySeq, applying the full transformation pipeline.
/// Sets `*has_more = 0` and returns NULL when the underlying source is exhausted (after which
/// further calls also return NULL). Returns a previously-cached `peek_value` if `_peek` was
/// called without a subsequent `_next`. Composite types (MAP/FILTER/TAKE_WHILE/...) recursively
/// pull from `seq->data.<variant>.source`; FILTER loops until a passing element is found.
/// @param[in,out] seq LazySeq cursor to advance.
/// @param[out] has_more Optional destination set to one for a yielded element or zero at end.
/// @return Borrowed next element, which may itself be NULL; inspect @p has_more
///         to distinguish a null element from exhaustion.
void *rt_lazyseq_next(rt_lazyseq seq, int8_t *has_more) {
    if (!seq || seq->exhausted) {
        if (has_more)
            *has_more = 0;
        return NULL;
    }

    // Return peeked value if available
    if (seq->peeked) {
        seq->peeked = 0;
        seq->index++;
        if (has_more)
            *has_more = 1;
        return seq->peek_value;
    }

    void *result = NULL;
    int8_t more = 1;

    switch (seq->type) {
        case LAZYSEQ_GENERATOR: {
            result = seq->data.generator.gen(seq->data.generator.state, seq->index, &more);
            break;
        }

        case LAZYSEQ_RANGE: {
            int64_t cur = seq->data.range.current;
            int64_t end = seq->data.range.end;
            int64_t step = seq->data.range.step;

            if ((step > 0 && cur >= end) || (step < 0 && cur <= end)) {
                more = 0;
            } else {
                seq->data.range.value_storage = cur;
                result = &seq->data.range.value_storage;
                int64_t next = 0;
                if (lazyseq_checked_step(cur, step, &next)) {
                    seq->data.range.current = next;
                } else {
                    seq->exhausted = 1;
                }
            }
            break;
        }

        case LAZYSEQ_REPEAT: {
            if (seq->data.repeat.remaining == 0) {
                more = 0;
            } else {
                result = seq->data.repeat.value;
                if (seq->data.repeat.remaining > 0) {
                    seq->data.repeat.remaining--;
                }
            }
            break;
        }

        case LAZYSEQ_ITERATE: {
            if (!seq->data.iterate.started) {
                seq->data.iterate.started = 1;
                result = seq->data.iterate.current;
            } else {
                seq->data.iterate.current = seq->data.iterate.fn(seq->data.iterate.current);
                result = seq->data.iterate.current;
            }
            break;
        }

        case LAZYSEQ_MAP: {
            int8_t src_more;
            void *elem = rt_lazyseq_next(seq->data.transform.source, &src_more);
            if (src_more) {
                result = seq->data.transform.map_fn(elem);
            } else {
                more = 0;
            }
            break;
        }

        case LAZYSEQ_FILTER: {
            while (1) {
                int8_t src_more;
                void *elem = rt_lazyseq_next(seq->data.transform.source, &src_more);
                if (!src_more) {
                    more = 0;
                    break;
                }
                if (seq->data.transform.filter_fn(elem)) {
                    result = elem;
                    break;
                }
            }
            break;
        }

        case LAZYSEQ_TAKE: {
            if (seq->data.bounded.consumed >= seq->data.bounded.limit) {
                more = 0;
            } else {
                int8_t src_more;
                result = rt_lazyseq_next(seq->data.bounded.source, &src_more);
                if (src_more) {
                    seq->data.bounded.consumed++;
                } else {
                    more = 0;
                }
            }
            break;
        }

        case LAZYSEQ_DROP: {
            // Skip elements on first access
            while (seq->data.bounded.consumed < seq->data.bounded.limit) {
                int8_t src_more;
                rt_lazyseq_next(seq->data.bounded.source, &src_more);
                if (!src_more) {
                    more = 0;
                    break;
                }
                seq->data.bounded.consumed++;
            }
            if (more) {
                int8_t src_more;
                result = rt_lazyseq_next(seq->data.bounded.source, &src_more);
                more = src_more;
            }
            break;
        }

        case LAZYSEQ_TAKE_WHILE: {
            if (seq->data.predicated.done) {
                more = 0;
            } else {
                int8_t src_more;
                void *elem = rt_lazyseq_next(seq->data.predicated.source, &src_more);
                if (!src_more || !seq->data.predicated.pred(elem)) {
                    seq->data.predicated.done = 1;
                    more = 0;
                } else {
                    result = elem;
                }
            }
            break;
        }

        case LAZYSEQ_DROP_WHILE: {
            if (!seq->data.predicated.done) {
                // Skip elements while predicate is true
                while (1) {
                    int8_t src_more;
                    void *elem = rt_lazyseq_next(seq->data.predicated.source, &src_more);
                    if (!src_more) {
                        more = 0;
                        break;
                    }
                    if (!seq->data.predicated.pred(elem)) {
                        seq->data.predicated.done = 1;
                        result = elem;
                        break;
                    }
                }
            } else {
                int8_t src_more;
                result = rt_lazyseq_next(seq->data.predicated.source, &src_more);
                more = src_more;
            }
            break;
        }

        case LAZYSEQ_CONCAT: {
            if (!seq->data.concat.on_second) {
                int8_t src_more;
                result = rt_lazyseq_next(seq->data.concat.first, &src_more);
                if (src_more) {
                    break;
                }
                seq->data.concat.on_second = 1;
            }
            int8_t src_more;
            result = rt_lazyseq_next(seq->data.concat.second, &src_more);
            more = src_more;
            break;
        }

        case LAZYSEQ_ZIP: {
            int8_t more1, more2;
            void *elem1 = rt_lazyseq_next(seq->data.zip.seq1, &more1);
            void *elem2 = rt_lazyseq_next(seq->data.zip.seq2, &more2);
            if (more1 && more2) {
                result = seq->data.zip.combine(elem1, elem2);
            } else {
                more = 0;
            }
            break;
        }
    }

    if (!more) {
        seq->exhausted = 1;
    } else {
        seq->index++;
    }

    if (has_more)
        *has_more = more;
    return result;
}

/// @brief Look at the next element without consuming it. Internally calls `_next` and caches the
/// result in `peek_value` (rolling `index` back by 1 to preserve "advance per `_next` call"
/// semantics). Subsequent `_next` returns the cached value and re-increments. NULL/exhausted
/// sequences return NULL with `*has_more = 0`.
/// @param[in,out] seq LazySeq cursor to inspect.
/// @param[out] has_more Optional destination set to one when an element is available.
/// @return Borrowed next element, which may be NULL; use @p has_more to distinguish exhaustion.
void *rt_lazyseq_peek(rt_lazyseq seq, int8_t *has_more) {
    if (!seq) {
        if (has_more)
            *has_more = 0;
        return NULL;
    }

    if (seq->peeked) {
        if (has_more)
            *has_more = 1;
        return seq->peek_value;
    }

    int8_t more;
    void *val = rt_lazyseq_next(seq, &more);

    if (more) {
        seq->peeked = 1;
        seq->peek_value = val;
        seq->index--; // Undo the increment from next
    }

    if (has_more)
        *has_more = more;
    return val;
}

/// @brief Restart traversal from the beginning where possible. Recursively resets composite
/// sources (MAP/FILTER/TAKE/...). **Limitations:** RANGE and REPEAT cannot be reset (they don't
/// preserve their original `start`/`count` separately from the running state); GENERATOR resets
/// only the index, not the user-state. Use cases: re-running a pipeline over a stable source.
/// @param[in,out] seq LazySeq whose cursor and resettable source state are reset.
void rt_lazyseq_reset(rt_lazyseq seq) {
    if (!seq)
        return;

    seq->index = 0;
    seq->exhausted = 0;
    seq->peeked = 0;
    seq->peek_value = NULL;

    switch (seq->type) {
        case LAZYSEQ_RANGE:
            // Cannot reset range without original start value
            // This is a limitation
            break;
        case LAZYSEQ_REPEAT:
            // Cannot reset count
            break;
        case LAZYSEQ_ITERATE:
            seq->data.iterate.started = 0;
            break;
        case LAZYSEQ_MAP:
        case LAZYSEQ_FILTER:
            rt_lazyseq_reset(seq->data.transform.source);
            break;
        case LAZYSEQ_TAKE:
        case LAZYSEQ_DROP:
            seq->data.bounded.consumed = 0;
            rt_lazyseq_reset(seq->data.bounded.source);
            break;
        case LAZYSEQ_TAKE_WHILE:
        case LAZYSEQ_DROP_WHILE:
            seq->data.predicated.done = 0;
            rt_lazyseq_reset(seq->data.predicated.source);
            break;
        case LAZYSEQ_CONCAT:
            seq->data.concat.on_second = 0;
            rt_lazyseq_reset(seq->data.concat.first);
            rt_lazyseq_reset(seq->data.concat.second);
            break;
        case LAZYSEQ_ZIP:
            rt_lazyseq_reset(seq->data.zip.seq1);
            rt_lazyseq_reset(seq->data.zip.seq2);
            break;
        default:
            break;
    }
}

/// @brief Return the count of elements yielded so far (advances on every successful `_next`).
/// @param[in] seq LazySeq cursor, or NULL.
/// @return Number of consumed elements, or zero for NULL.
int64_t rt_lazyseq_index(rt_lazyseq seq) {
    return seq ? seq->index : 0;
}

/// @brief Returns 1 once the underlying source has signaled end-of-stream. NULL handle → 1
/// (treats absence as exhaustion so loop guards `while (!is_exhausted)` are tolerant).
/// @param[in] seq LazySeq cursor, or NULL.
/// @return 1 when exhausted or NULL; otherwise 0.
int8_t rt_lazyseq_is_exhausted(rt_lazyseq seq) {
    return seq ? seq->exhausted : 1;
}

//=============================================================================
// Transformations
//=============================================================================

// =============================================================================
// Transformations — each combinator wraps the source in a new LazySeq node,
// retaining the source so it stays alive for the lifetime of the wrapper.
// All combinators are O(1) construction; work happens lazily during traversal.
// Returns NULL on bad input (NULL source, NULL function, negative limits).
// =============================================================================

/// @brief Wrap a source LazySeq so each `_next` returns `fn(source.next())`. The function pointer
/// is borrowed (caller must keep it alive); the source is retained.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] fn Borrowed element transform callback.
/// @return Caller-owned mapped LazySeq, or NULL for invalid input or allocation failure.
rt_lazyseq rt_lazyseq_map(rt_lazyseq seq, void *(*fn)(void *)) {
    if (!seq || !fn)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_MAP);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.transform.source = seq;
    result->data.transform.map_fn = fn;
    return result;
}

/// @brief Wrap a source so `_next` skips elements where `pred(elem) == 0`. May call the source's
/// `_next` arbitrarily many times per call (depending on selectivity); inner loop has no upper
/// bound, so combine with `_take` for infinite filtered sources.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] pred Borrowed inclusion predicate.
/// @return Caller-owned filtered LazySeq, or NULL for invalid input or allocation failure.
rt_lazyseq rt_lazyseq_filter(rt_lazyseq seq, int8_t (*pred)(void *)) {
    if (!seq || !pred)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_FILTER);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.transform.source = seq;
    result->data.transform.filter_fn = pred;
    return result;
}

/// @brief Wrap a source so the resulting LazySeq exhausts after `n` elements (or sooner if the
/// source ends first). Cheap way to truncate infinite sequences to a known prefix.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] n Nonnegative maximum yield count.
/// @return Caller-owned bounded LazySeq, or NULL after invalid input or allocation failure.
rt_lazyseq rt_lazyseq_take(rt_lazyseq seq, int64_t n) {
    if (!seq)
        return NULL;
    if (n < 0) {
        rt_trap("LazySeq.Take: count must be >= 0");
        return NULL;
    }

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_TAKE);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.bounded.source = seq;
    result->data.bounded.limit = n;
    result->data.bounded.consumed = 0;
    return result;
}

/// @brief Wrap a source so the first `n` elements are skipped on first traversal. The skip
/// happens lazily — `n` source `_next` calls are issued the first time a value is requested.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] n Nonnegative number of leading elements to consume.
/// @return Caller-owned drop LazySeq, or NULL after invalid input or allocation failure.
rt_lazyseq rt_lazyseq_drop(rt_lazyseq seq, int64_t n) {
    if (!seq)
        return NULL;
    if (n < 0) {
        rt_trap("LazySeq.Drop: count must be >= 0");
        return NULL;
    }

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_DROP);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.bounded.source = seq;
    result->data.bounded.limit = n;
    result->data.bounded.consumed = 0;
    return result;
}

/// @brief Wrap a source so traversal stops at the first element where `pred(elem) == 0`. Useful
/// for "consume until X" patterns over a streaming source.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] pred Borrowed continuation predicate.
/// @return Caller-owned take-while LazySeq, or NULL for invalid input or allocation failure.
rt_lazyseq rt_lazyseq_take_while(rt_lazyseq seq, int8_t (*pred)(void *)) {
    if (!seq || !pred)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_TAKE_WHILE);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.predicated.source = seq;
    result->data.predicated.pred = pred;
    result->data.predicated.done = 0;
    return result;
}

/// @brief Wrap a source so leading elements satisfying `pred` are skipped, then the rest is
/// emitted unchanged (including elements that would have failed the predicate). Mirror of
/// `take_while` for trailing data; e.g. skip-leading-whitespace patterns.
/// @param[in] seq Source LazySeq retained by the wrapper.
/// @param[in] pred Borrowed leading-element predicate.
/// @return Caller-owned drop-while LazySeq, or NULL for invalid input or allocation failure.
rt_lazyseq rt_lazyseq_drop_while(rt_lazyseq seq, int8_t (*pred)(void *)) {
    if (!seq || !pred)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_DROP_WHILE);
    if (!result)
        return NULL;

    rt_heap_retain(seq);
    result->data.predicated.source = seq;
    result->data.predicated.pred = pred;
    result->data.predicated.done = 0;
    return result;
}

/// @brief Wrap two sources so the result yields all of `first`'s elements, then all of `second`'s.
/// Both sources are retained until the wrapper is released.
/// @param[in] first First source LazySeq.
/// @param[in] second Second source LazySeq.
/// @return Caller-owned concatenated LazySeq, or NULL for a null source or allocation failure.
rt_lazyseq rt_lazyseq_concat(rt_lazyseq first, rt_lazyseq second) {
    if (!first || !second)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_CONCAT);
    if (!result)
        return NULL;

    rt_heap_retain(first);
    rt_heap_retain(second);
    result->data.concat.first = first;
    result->data.concat.second = second;
    result->data.concat.on_second = 0;
    return result;
}

/// @brief Pairwise-zip two sources via `combine(a, b)`. Stops as soon as either source is
/// exhausted (length = min). Both sources are retained.
/// @param[in] seq1 First source LazySeq.
/// @param[in] seq2 Second source LazySeq.
/// @param[in] combine Borrowed pair-combining callback.
/// @return Caller-owned zipped LazySeq, or NULL for invalid input or allocation failure.
rt_lazyseq rt_lazyseq_zip(rt_lazyseq seq1, rt_lazyseq seq2, void *(*combine)(void *, void *)) {
    if (!seq1 || !seq2 || !combine)
        return NULL;

    rt_lazyseq result = alloc_lazyseq(LAZYSEQ_ZIP);
    if (!result)
        return NULL;

    rt_heap_retain(seq1);
    rt_heap_retain(seq2);
    result->data.zip.seq1 = seq1;
    result->data.zip.seq2 = seq2;
    result->data.zip.combine = combine;
    return result;
}

//=============================================================================
// Collectors
//=============================================================================

// =============================================================================
// Collectors — terminal operations that traverse the LazySeq pipeline once and
// produce an eager result. Each call exhausts its input (or the take-prefix);
// re-traversal requires a `_reset` first.
// =============================================================================

/// @brief Materialize the entire LazySeq into a Seq[Box]. Drives `_next` until exhaustion,
/// pushing each element. Beware of infinite sources — pair with `_take` first.
/// @param[in,out] seq LazySeq to exhaust, or NULL.
/// @return Caller-owned managed Seq configured to own its collected entries.
void *rt_lazyseq_to_seq(rt_lazyseq seq) {
    if (!seq) {
        void *empty = rt_seq_new();
        rt_seq_set_owns_elements(empty, 1);
        return empty;
    }

    // Collected Seqs own their entries, like publicly constructed Seqs
    // (VDOC-091).
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);
    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        rt_seq_push(result, elem);
    }

    return result;
}

/// @brief Materialize at most `n` elements into a pre-sized Seq[Box]. Safe over infinite sources;
/// the underlying source state advances by the actual number of items consumed.
/// @param[in,out] seq LazySeq to consume, or NULL.
/// @param[in] n Maximum number of elements; nonpositive values produce an empty Seq.
/// @return Caller-owned managed Seq configured to own its collected entries.
void *rt_lazyseq_to_seq_n(rt_lazyseq seq, int64_t n) {
    if (!seq || n <= 0) {
        void *empty = rt_seq_new();
        rt_seq_set_owns_elements(empty, 1);
        return empty;
    }

    void *result = rt_seq_with_capacity(n);
    rt_seq_set_owns_elements(result, 1);
    int8_t has_more;
    int64_t count = 0;

    while (count < n) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        rt_seq_push(result, elem);
        count++;
    }

    return result;
}

/// @brief Left-fold (a.k.a. reduce). Applies `acc = fn(acc, elem)` for each element, starting
/// from `init`, returning the final accumulator. Returns `init` unchanged if `seq` or `fn` is NULL.
/// @param[in,out] seq LazySeq to exhaust.
/// @param[in] init Initial accumulator pointer.
/// @param[in] fn Borrowed reducer callback.
/// @return Final callback-produced accumulator, or @p init for invalid input.
void *rt_lazyseq_fold(rt_lazyseq seq, void *init, void *(*fn)(void *, void *)) {
    if (!seq || !fn)
        return init;

    void *acc = init;
    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        acc = fn(acc, elem);
    }

    return acc;
}

/// @brief Count elements by exhausting the LazySeq. Discards the actual elements (no allocation
/// for materialization). Same caveat as `to_seq`: do not call on infinite sources.
/// @param[in,out] seq LazySeq to exhaust, or NULL.
/// @return Number of elements consumed, or zero for NULL.
int64_t rt_lazyseq_count(rt_lazyseq seq) {
    if (!seq)
        return 0;

    int64_t count = 0;
    int8_t has_more;

    while (1) {
        rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        count++;
    }

    return count;
}

/// @brief Apply a side-effecting function to every element. The element pointer is borrowed —
/// `fn` must not retain it past its call (no lifetime guarantee after `_next`).
/// @param[in,out] seq LazySeq to exhaust.
/// @param[in] fn Borrowed callback invoked once per element.
void rt_lazyseq_foreach(rt_lazyseq seq, void (*fn)(void *)) {
    if (!seq || !fn)
        return;

    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        fn(elem);
    }
}

/// @brief Short-circuiting search: returns the first element where `pred(elem) == 1` and sets
/// `*found = 1`; returns NULL with `*found = 0` otherwise. Stops traversal at the first hit
/// (safe for infinite sources if a match exists).
/// @param[in,out] seq LazySeq to search.
/// @param[in] pred Borrowed match predicate.
/// @param[out] found Optional destination set to one on a match or zero on failure/end.
/// @return Borrowed matching element, which may be NULL; use @p found to distinguish absence.
void *rt_lazyseq_find(rt_lazyseq seq, int8_t (*pred)(void *), int8_t *found) {
    if (!seq || !pred) {
        if (found)
            *found = 0;
        return NULL;
    }

    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        if (pred(elem)) {
            if (found)
                *found = 1;
            return elem;
        }
    }

    if (found)
        *found = 0;
    return NULL;
}

/// @brief Short-circuiting "exists": returns 1 as soon as any element satisfies `pred`. Equivalent
/// to `find != NULL` but doesn't return the matching element.
/// @param[in,out] seq LazySeq to search.
/// @param[in] pred Borrowed match predicate.
/// @return 1 on the first match; otherwise 0, including invalid input.
int8_t rt_lazyseq_any(rt_lazyseq seq, int8_t (*pred)(void *)) {
    if (!seq || !pred)
        return 0;

    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        if (pred(elem))
            return 1;
    }

    return 0;
}

/// @brief Short-circuiting "forall": returns 0 on the first element that fails `pred`, otherwise
/// 1. Empty sequence vacuously returns 1; NULL `pred` also returns 1 by convention.
/// @param[in,out] seq LazySeq to inspect.
/// @param[in] pred Borrowed predicate, or NULL for the vacuous true convention.
/// @return 0 on the first failure; otherwise 1.
int8_t rt_lazyseq_all(rt_lazyseq seq, int8_t (*pred)(void *)) {
    if (!seq || !pred)
        return 1;

    int8_t has_more;

    while (1) {
        void *elem = rt_lazyseq_next(seq, &has_more);
        if (!has_more)
            break;
        if (!pred(elem))
            return 0;
    }

    return 1;
}

//=============================================================================
// IL ABI wrappers (void* interface for runtime signature handlers)
//
// Each `rt_lazyseq_w_*` is a thin trampoline that re-types pointers between the
// IL's `void *` ABI and the typed C functions above. The IL signature dispatcher
// only knows pointer-sized arguments; these wrappers cast in/out of `rt_lazyseq`
// (themselves typedef'd to a struct pointer) and `void *(*)()` callbacks. They
// add no logic beyond the cast — see the typed function for actual semantics.
//=============================================================================

/// @brief IL trampoline: dispatch to `rt_lazyseq_range` returning the result as `void *`.
/// @param[in] start Inclusive first integer.
/// @param[in] end Exclusive directional bound.
/// @param[in] step Nonzero signed increment.
/// @return Result from rt_lazyseq_range().
void *rt_lazyseq_w_range(int64_t start, int64_t end, int64_t step) {
    return (void *)rt_lazyseq_range(start, end, step);
}

/// @brief IL trampoline for `rt_lazyseq_repeat`.
/// @param[in] value Managed value to repeat.
/// @param[in] count Yield count, or -1 for infinity.
/// @return Result from rt_lazyseq_repeat().
void *rt_lazyseq_w_repeat(void *value, int64_t count) {
    return (void *)rt_lazyseq_repeat(value, count);
}

/// @brief Checked receiver cast for the IL boundary (VDOC-090).
/// @details LazySeq nodes carry RT_LAZYSEQ_CLASS_ID; a wrong explicit receiver
///          (e.g. a Map) traps instead of being reinterpreted as cursor state.
/// @param[in] obj Candidate managed receiver.
/// @return Validated LazySeq payload, or NULL after trapping.
static rt_lazyseq as_lazyseq(void *obj) {
    if (!rt_obj_is_instance(obj, RT_LAZYSEQ_CLASS_ID, sizeof(struct rt_lazyseq_impl))) {
        rt_trap("LazySeq: invalid LazySeq object");
        return NULL;
    }
    return (rt_lazyseq)obj;
}

/// @brief IL trampoline for `rt_lazyseq_next`. Discards `has_more` (caller uses `_is_exhausted`).
/// @param[in,out] seq Managed LazySeq receiver.
/// @return Borrowed next element or NULL for both a null element and exhaustion.
void *rt_lazyseq_w_next(void *seq) {
    int8_t has_more;
    return rt_lazyseq_next(as_lazyseq(seq), &has_more);
}

/// @brief IL trampoline for `rt_lazyseq_peek`. Discards `has_more`.
/// @param[in,out] seq Managed LazySeq receiver.
/// @return Borrowed next element or NULL for both a null element and exhaustion.
void *rt_lazyseq_w_peek(void *seq) {
    int8_t has_more;
    return rt_lazyseq_peek(as_lazyseq(seq), &has_more);
}

/// @brief IL trampoline: `Next` that preserves null elements as Option.Some.
/// @details The plain wrapper discards `has_more` and cannot distinguish a
///          yielded null element from exhaustion (VDOC-095); this variant
///          returns Some(value) while the stream is live and None at the end.
/// @param[in,out] seq Managed LazySeq receiver.
/// @return Caller-owned Some containing the next element, or caller-owned None at exhaustion.
void *rt_lazyseq_w_next_option(void *seq) {
    int8_t has_more = 0;
    void *value = rt_lazyseq_next(as_lazyseq(seq), &has_more);
    return has_more ? rt_option_some(value) : rt_option_none();
}

/// @brief IL trampoline: `Peek` returning Option (see rt_lazyseq_w_next_option).
/// @param[in,out] seq Managed LazySeq receiver.
/// @return Caller-owned Some containing the next element, or caller-owned None at exhaustion.
void *rt_lazyseq_w_peek_option(void *seq) {
    int8_t has_more = 0;
    void *value = rt_lazyseq_peek(as_lazyseq(seq), &has_more);
    return has_more ? rt_option_some(value) : rt_option_none();
}

/// @brief IL trampoline for `rt_lazyseq_reset`.
/// @param[in,out] seq Managed LazySeq receiver.
void rt_lazyseq_w_reset(void *seq) {
    rt_lazyseq_reset(as_lazyseq(seq));
}

/// @brief IL trampoline for `rt_lazyseq_index`.
/// @param[in] seq Managed LazySeq receiver.
/// @return Current consumed-element index.
int64_t rt_lazyseq_w_index(void *seq) {
    return rt_lazyseq_index(as_lazyseq(seq));
}

/// @brief IL trampoline for `rt_lazyseq_is_exhausted`.
/// @param[in] seq Managed LazySeq receiver.
/// @return 1 when exhausted; otherwise 0.
int8_t rt_lazyseq_w_is_exhausted(void *seq) {
    return rt_lazyseq_is_exhausted(as_lazyseq(seq));
}

/// @brief IL trampoline for `rt_lazyseq_take`.
/// @param[in] seq Managed source LazySeq.
/// @param[in] n Nonnegative maximum yield count.
/// @return Result from rt_lazyseq_take().
void *rt_lazyseq_w_take(void *seq, int64_t n) {
    return (void *)rt_lazyseq_take(as_lazyseq(seq), n);
}

/// @brief IL trampoline for `rt_lazyseq_drop`.
/// @param[in] seq Managed source LazySeq.
/// @param[in] n Nonnegative leading-element count.
/// @return Result from rt_lazyseq_drop().
void *rt_lazyseq_w_drop(void *seq, int64_t n) {
    return (void *)rt_lazyseq_drop(as_lazyseq(seq), n);
}

/// @brief IL trampoline for `rt_lazyseq_concat`.
/// @param[in] first Managed first source LazySeq.
/// @param[in] second Managed second source LazySeq.
/// @return Result from rt_lazyseq_concat().
void *rt_lazyseq_w_concat(void *first, void *second) {
    return (void *)rt_lazyseq_concat(as_lazyseq(first), as_lazyseq(second));
}

/// @brief IL trampoline for `rt_lazyseq_to_seq`.
/// @param[in,out] seq Managed LazySeq to exhaust.
/// @return Caller-owned collected Seq.
void *rt_lazyseq_w_to_seq(void *seq) {
    return rt_lazyseq_to_seq(as_lazyseq(seq));
}

/// @brief IL trampoline for `rt_lazyseq_to_seq_n`.
/// @param[in,out] seq Managed LazySeq to consume.
/// @param[in] n Maximum collection count.
/// @return Caller-owned collected Seq.
void *rt_lazyseq_w_to_seq_n(void *seq, int64_t n) {
    return rt_lazyseq_to_seq_n(as_lazyseq(seq), n);
}

/// @brief IL trampoline for `rt_lazyseq_count`.
/// @param[in,out] seq Managed LazySeq to exhaust.
/// @return Number of consumed elements.
int64_t rt_lazyseq_w_count(void *seq) {
    return rt_lazyseq_count(as_lazyseq(seq));
}

/// @brief IL trampoline for `rt_lazyseq_map`. Re-types the user fn pointer for the typed call.
/// @param[in] seq Managed source LazySeq.
/// @param[in] fn Opaque transform function pointer.
/// @return Result from rt_lazyseq_map().
void *rt_lazyseq_w_map(void *seq, void *fn) {
    return (void *)rt_lazyseq_map(as_lazyseq(seq), RT_FN_PTR_CAST((void *(*)(void *))fn));
}

/// @brief IL trampoline for `rt_lazyseq_filter`.
/// @param[in] seq Managed source LazySeq.
/// @param[in] pred Opaque predicate function pointer.
/// @return Result from rt_lazyseq_filter().
void *rt_lazyseq_w_filter(void *seq, void *pred) {
    return (void *)rt_lazyseq_filter(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}

/// @brief IL trampoline for `rt_lazyseq_take_while`.
/// @param[in] seq Managed source LazySeq.
/// @param[in] pred Opaque predicate function pointer.
/// @return Result from rt_lazyseq_take_while().
void *rt_lazyseq_w_take_while(void *seq, void *pred) {
    return (void *)rt_lazyseq_take_while(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}

/// @brief IL trampoline for `rt_lazyseq_drop_while`.
/// @param[in] seq Managed source LazySeq.
/// @param[in] pred Opaque predicate function pointer.
/// @return Result from rt_lazyseq_drop_while().
void *rt_lazyseq_w_drop_while(void *seq, void *pred) {
    return (void *)rt_lazyseq_drop_while(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}

/// @brief IL trampoline for `rt_lazyseq_find`. Discards the `found` flag (caller checks NULL).
/// @param[in,out] seq Managed LazySeq to search.
/// @param[in] pred Opaque predicate function pointer.
/// @return Borrowed matching element, or NULL for no match and matching null elements.
void *rt_lazyseq_w_find(void *seq, void *pred) {
    int8_t found;
    return rt_lazyseq_find(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred), &found);
}

/// @brief IL trampoline for `rt_lazyseq_find` that preserves absence as Option.None.
/// @details The legacy wrapper discards the found flag and therefore cannot
///          distinguish a found NULL element from no match. This wrapper uses
///          the found flag to return `Some(value)` or `None`.
/// @param seq LazySeq object.
/// @param pred Opaque predicate function pointer.
/// @return Opaque Zanna.Option containing the first matching element, or None.
void *rt_lazyseq_w_find_option(void *seq, void *pred) {
    int8_t found = 0;
    void *value =
        rt_lazyseq_find(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred), &found);
    return found ? rt_option_some(value) : rt_option_none();
}

/// @brief IL trampoline for `rt_lazyseq_any`.
/// @param[in,out] seq Managed LazySeq to search.
/// @param[in] pred Opaque predicate function pointer.
/// @return 1 on the first match; otherwise 0.
int8_t rt_lazyseq_w_any(void *seq, void *pred) {
    return rt_lazyseq_any(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}

/// @brief IL trampoline for `rt_lazyseq_all`.
/// @param[in,out] seq Managed LazySeq to inspect.
/// @param[in] pred Opaque predicate function pointer.
/// @return 0 on the first failure; otherwise 1.
int8_t rt_lazyseq_w_all(void *seq, void *pred) {
    return rt_lazyseq_all(as_lazyseq(seq), RT_FN_PTR_CAST((int8_t (*)(void *))pred));
}
