//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTLazySeqTests.cpp
// Purpose: Validate LazySeq type.
//
//===----------------------------------------------------------------------===//

#include "rt_lazyseq.h"
#include "rt_box.h"
#include "rt_heap.h"
#include "rt_option.h"
#include "rt_seq.h"

#include "rt_trap.h"

#include <cassert>
#include <csetjmp>
#include <climits>
#include <cstdio>

/// @brief Helper to print test result.
static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

//=============================================================================
// Range Tests
//=============================================================================

static void test_lazyseq_range() {
    printf("Testing LazySeq Range:\n");

    // Test 1: Basic range
    {
        rt_lazyseq seq = rt_lazyseq_range(0, 5, 1);
        test_result("Range created", seq != NULL);

        int8_t has_more;
        int64_t sum = 0;
        int64_t count = 0;

        while (1) {
            void *val = rt_lazyseq_next(seq, &has_more);
            if (!has_more)
                break;
            sum += *(int64_t *)val;
            count++;
        }

        test_result("Range produced 5 elements", count == 5);
        test_result("Sum is correct (0+1+2+3+4=10)", sum == 10);
        test_result("Sequence exhausted", rt_lazyseq_is_exhausted(seq) == 1);

        rt_lazyseq_destroy(seq);
    }

    // Test 2: Range with step
    {
        rt_lazyseq seq = rt_lazyseq_range(0, 10, 2);
        int8_t has_more;
        int64_t count = 0;

        while (1) {
            rt_lazyseq_next(seq, &has_more);
            if (!has_more)
                break;
            count++;
        }

        test_result("Range with step 2: 5 elements", count == 5);
        rt_lazyseq_destroy(seq);
    }

    // Test 3: Negative step
    {
        rt_lazyseq seq = rt_lazyseq_range(5, 0, -1);
        int8_t has_more;
        int64_t first;

        void *val = rt_lazyseq_next(seq, &has_more);
        first = *(int64_t *)val;
        test_result("Descending range starts at 5", first == 5);
        rt_lazyseq_destroy(seq);
    }

    // Test 4: Step overflow exhausts instead of wrapping.
    {
        rt_lazyseq seq = rt_lazyseq_range(INT64_MAX - 1, INT64_MAX, 2);
        int8_t has_more;
        void *val = rt_lazyseq_next(seq, &has_more);
        test_result("Overflow range yields first boundary value",
                    has_more && *(int64_t *)val == INT64_MAX - 1);
        val = rt_lazyseq_next(seq, &has_more);
        test_result("Overflow range exhausts after checked step", !has_more && val == NULL);
        rt_lazyseq_destroy(seq);
    }

    // Test 5: Negative step underflow exhausts instead of wrapping.
    {
        rt_lazyseq seq = rt_lazyseq_range(INT64_MIN + 1, INT64_MIN, -2);
        int8_t has_more;
        void *val = rt_lazyseq_next(seq, &has_more);
        test_result("Underflow range yields first boundary value",
                    has_more && *(int64_t *)val == INT64_MIN + 1);
        val = rt_lazyseq_next(seq, &has_more);
        test_result("Underflow range exhausts after checked step", !has_more && val == NULL);
        rt_lazyseq_destroy(seq);
    }

    printf("\n");
}

//=============================================================================
// Repeat Tests
//=============================================================================

static void test_lazyseq_repeat() {
    printf("Testing LazySeq Repeat:\n");

    // Test: Finite repeat
    {
        static int value = 42;
        rt_lazyseq seq = rt_lazyseq_repeat(&value, 3);

        int8_t has_more;
        int64_t count = 0;

        while (1) {
            void *val = rt_lazyseq_next(seq, &has_more);
            if (!has_more)
                break;
            test_result("Repeat returns same value", *(int *)val == 42);
            count++;
        }

        test_result("Repeat produced 3 elements", count == 3);
        rt_lazyseq_destroy(seq);
    }

    printf("\n");
}

//=============================================================================
// Iterate Tests
//=============================================================================

static void *double_fn(void *x) {
    static int64_t result;
    result = *(int64_t *)x * 2;
    return &result;
}

static void test_lazyseq_iterate() {
    printf("Testing LazySeq Iterate:\n");

    // Test: Powers of 2
    {
        static int64_t seed = 1;
        rt_lazyseq seq = rt_lazyseq_iterate(&seed, double_fn);

        int8_t has_more;
        int64_t vals[5];

        for (int i = 0; i < 5; i++) {
            void *val = rt_lazyseq_next(seq, &has_more);
            vals[i] = *(int64_t *)val;
        }

        test_result("Iterate: first value is 1", vals[0] == 1);
        test_result("Iterate: second value is 2", vals[1] == 2);
        test_result("Iterate: third value is 4", vals[2] == 4);
        test_result("Iterate: fourth value is 8", vals[3] == 8);
        test_result("Iterate: fifth value is 16", vals[4] == 16);

        rt_lazyseq_destroy(seq);
    }

    printf("\n");
}

//=============================================================================
// Transformation Tests
//=============================================================================

static void *triple_fn(void *x) {
    static int64_t result;
    result = *(int64_t *)x * 3;
    return &result;
}

static int8_t is_even(void *x) {
    return (*(int64_t *)x % 2) == 0 ? 1 : 0;
}

static void test_lazyseq_transform() {
    printf("Testing LazySeq Transformations:\n");

    // Test 1: Map
    {
        rt_lazyseq base = rt_lazyseq_range(1, 4, 1);
        rt_lazyseq mapped = rt_lazyseq_map(base, triple_fn);

        int8_t has_more;
        void *val;

        val = rt_lazyseq_next(mapped, &has_more);
        test_result("Map: 1*3 = 3", *(int64_t *)val == 3);

        val = rt_lazyseq_next(mapped, &has_more);
        test_result("Map: 2*3 = 6", *(int64_t *)val == 6);

        val = rt_lazyseq_next(mapped, &has_more);
        test_result("Map: 3*3 = 9", *(int64_t *)val == 9);

        rt_lazyseq_destroy(mapped);
    }

    // Test 2: Filter
    {
        rt_lazyseq base = rt_lazyseq_range(1, 7, 1);
        rt_lazyseq filtered = rt_lazyseq_filter(base, is_even);

        int8_t has_more;
        int64_t count = 0;

        while (1) {
            void *val = rt_lazyseq_next(filtered, &has_more);
            if (!has_more)
                break;
            test_result("Filter: value is even", (*(int64_t *)val % 2) == 0);
            count++;
        }

        test_result("Filter: 3 even numbers in 1-6", count == 3);
        rt_lazyseq_destroy(filtered);
    }

    // Test 3: Take
    {
        rt_lazyseq base = rt_lazyseq_range(0, 100, 1);
        rt_lazyseq taken = rt_lazyseq_take(base, 3);

        int64_t count = rt_lazyseq_count(taken);
        test_result("Take: limited to 3 elements", count == 3);

        rt_lazyseq_destroy(taken);
    }

    // Test 4: Drop
    {
        rt_lazyseq base = rt_lazyseq_range(0, 5, 1);
        rt_lazyseq dropped = rt_lazyseq_drop(base, 2);

        int8_t has_more;
        void *val = rt_lazyseq_next(dropped, &has_more);

        test_result("Drop: first after drop is 2", *(int64_t *)val == 2);
        rt_lazyseq_destroy(dropped);
    }

    printf("\n");
}

//=============================================================================
// Collector Tests
//=============================================================================

static void test_lazyseq_collectors() {
    printf("Testing LazySeq Collectors:\n");

    // Test 1: ToSeq
    {
        rt_lazyseq seq = rt_lazyseq_range(0, 5, 1);
        void *result = rt_lazyseq_to_seq(seq);

        test_result("ToSeq: 5 elements", rt_seq_len(result) == 5);
        rt_lazyseq_destroy(seq);
    }

    // Test 2: ToSeqN
    {
        rt_lazyseq seq = rt_lazyseq_range(0, 100, 1);
        void *result = rt_lazyseq_to_seq_n(seq, 3);

        test_result("ToSeqN: limited to 3", rt_seq_len(result) == 3);
        rt_lazyseq_destroy(seq);
    }

    // Test 3: Any
    {
        rt_lazyseq seq = rt_lazyseq_range(1, 10, 1);
        int8_t found = rt_lazyseq_any(seq, is_even);

        test_result("Any: found even in 1-9", found == 1);
        rt_lazyseq_destroy(seq);
    }

    // Test 4: All (will consume all)
    {
        rt_lazyseq base = rt_lazyseq_range(2, 8, 2); // 2, 4, 6
        int8_t all_even = rt_lazyseq_all(base, is_even);

        test_result("All: 2,4,6 are all even", all_even == 1);
        rt_lazyseq_destroy(base);
    }

    printf("\n");
}

//=============================================================================
// Peek and Index Tests
//=============================================================================

static void test_lazyseq_peek() {
    printf("Testing LazySeq Peek/Index:\n");

    // Test: Peek doesn't consume
    {
        rt_lazyseq seq = rt_lazyseq_range(10, 15, 1);
        int8_t has_more;

        void *peek1 = rt_lazyseq_peek(seq, &has_more);
        void *peek2 = rt_lazyseq_peek(seq, &has_more);
        void *next = rt_lazyseq_next(seq, &has_more);

        test_result("Peek is idempotent", *(int64_t *)peek1 == *(int64_t *)peek2);
        test_result("Next returns peeked", *(int64_t *)next == 10);
        test_result("Index after one next is 1", rt_lazyseq_index(seq) == 1);

        rt_lazyseq_destroy(seq);
    }

    printf("\n");
}

//=============================================================================
// Concat Tests
//=============================================================================

static void test_lazyseq_concat() {
    printf("Testing LazySeq Concat:\n");

    {
        rt_lazyseq seq1 = rt_lazyseq_range(1, 3, 1); // 1, 2
        rt_lazyseq seq2 = rt_lazyseq_range(3, 5, 1); // 3, 4
        rt_lazyseq combined = rt_lazyseq_concat(seq1, seq2);

        int64_t count = rt_lazyseq_count(combined);
        test_result("Concat: 4 total elements", count == 4);

        rt_lazyseq_destroy(combined);
    }

    printf("\n");
}

//=============================================================================
// NULL Handling Tests
//=============================================================================

static void test_lazyseq_null_handling() {
    printf("Testing LazySeq NULL handling:\n");

    int8_t has_more;
    test_result("Next NULL returns NULL", rt_lazyseq_next(NULL, &has_more) == NULL);
    test_result("IsExhausted NULL returns 1", rt_lazyseq_is_exhausted(NULL) == 1);
    test_result("Index NULL returns 0", rt_lazyseq_index(NULL) == 0);

    printf("\n");
}

//=============================================================================
// IL Wrapper Tests
//=============================================================================

static void test_lazyseq_il_wrappers() {
    printf("Testing LazySeq IL wrappers:\n");

    // Test: Range wrapper returns valid sequence
    {
        void *seq = rt_lazyseq_w_range(1, 10, 1);
        test_result("w_range: non-null", seq != NULL);

        // Test w_index
        int64_t idx = rt_lazyseq_w_index(seq);
        test_result("w_index: starts at 0", idx == 0);

        // Test w_is_exhausted
        int8_t exh = rt_lazyseq_w_is_exhausted(seq);
        test_result("w_is_exhausted: false at start", exh == 0);

        // Test w_count
        int64_t count = rt_lazyseq_w_count(seq);
        test_result("w_count: 9 elements in range(1,10,1)", count == 9);

        // Test w_reset
        rt_lazyseq_w_reset(seq);
        idx = rt_lazyseq_w_index(seq);
        test_result("w_reset: index back to 0", idx == 0);

        rt_lazyseq_destroy((rt_lazyseq)seq);
    }

    // Test: Next/Peek wrappers
    {
        void *seq = rt_lazyseq_w_range(10, 13, 1);
        void *peeked = rt_lazyseq_w_peek(seq);
        test_result("w_peek: first is 10", *(int64_t *)peeked == 10);

        void *next = rt_lazyseq_w_next(seq);
        test_result("w_next: returns 10", *(int64_t *)next == 10);

        next = rt_lazyseq_w_next(seq);
        test_result("w_next: returns 11", *(int64_t *)next == 11);

        rt_lazyseq_destroy((rt_lazyseq)seq);
    }

    printf("\n");
}

//=============================================================================
// Entry Point
//=============================================================================

static void test_wrapper_receiver_validation() {
    // VDOC-090: an IL-boundary call with a non-LazySeq receiver traps instead
    // of reinterpreting foreign object fields as cursor state.
    extern void rt_lazyseq_w_reset(void *);
    void *not_a_lazyseq = rt_seq_new();
    jmp_buf env;
    rt_trap_set_recovery(&env);
    bool trapped = true;
    if (setjmp(env) == 0) {
        rt_lazyseq_w_reset(not_a_lazyseq);
        trapped = false;
    }
    rt_trap_clear_recovery();
    assert(trapped && "wrong receiver must trap");
}

static void test_collectors_own_elements() {
    // VDOC-091: Repeat retains its value and collected Seqs own their
    // entries, so materialized elements stay alive independently.
    void *boxed = rt_box_i64(7);
    size_t base = rt_heap_hdr(boxed)->refcnt;

    rt_lazyseq rep = rt_lazyseq_repeat(boxed, 3);
    assert(rt_heap_hdr(boxed)->refcnt == base + 1); // node retains the value

    void *seq = rt_lazyseq_to_seq(rep);
    assert(rt_seq_len(seq) == 3);
    // Owning collection: three additional strong references.
    assert(rt_heap_hdr(boxed)->refcnt == base + 4);
}

static void test_invalid_limits_trap() {
    // VDOC-092: invalid bounds trap consistently instead of silently
    // producing NULL pipelines or unintended infinite sources.
    {
        jmp_buf env;
        rt_trap_set_recovery(&env);
        bool trapped = true;
        if (setjmp(env) == 0) {
            (void)rt_lazyseq_repeat(rt_box_i64(1), -2);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "Repeat(-2) must trap");
    }
    {
        rt_lazyseq src = rt_lazyseq_range(0, 10, 1);
        jmp_buf env;
        rt_trap_set_recovery(&env);
        volatile bool trapped = true;
        if (setjmp(env) == 0) {
            (void)rt_lazyseq_take(src, -1);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "Take(-1) must trap");
    }
    {
        jmp_buf env;
        rt_trap_set_recovery(&env);
        volatile bool trapped = true;
        if (setjmp(env) == 0) {
            (void)rt_lazyseq_range(0, 10, 0);
            trapped = false;
        }
        rt_trap_clear_recovery();
        assert(trapped && "Range step 0 must trap");
    }
    // -1 remains the infinite sentinel.
    rt_lazyseq inf = rt_lazyseq_repeat(rt_box_i64(9), -1);
    assert(inf != nullptr);
}

static void test_next_option_null_elements() {
    // VDOC-095: NextOption distinguishes a yielded null element from end.
    extern void *rt_lazyseq_w_next_option(void *);
    rt_lazyseq rep = rt_lazyseq_repeat(nullptr, 2);
    void *first = rt_lazyseq_w_next_option(rep);
    assert(rt_option_is_some(first) == 1);   // a real (null) element
    void *second = rt_lazyseq_w_next_option(rep);
    assert(rt_option_is_some(second) == 1);
    void *done = rt_lazyseq_w_next_option(rep);
    assert(rt_option_is_none(done) == 1);    // exhaustion, not an element
}

int main() {
    test_wrapper_receiver_validation();
    test_next_option_null_elements();
    test_collectors_own_elements();
    test_invalid_limits_trap();
    printf("=== RT LazySeq Tests ===\n\n");

    test_lazyseq_range();
    test_lazyseq_repeat();
    test_lazyseq_iterate();
    test_lazyseq_transform();
    test_lazyseq_collectors();
    test_lazyseq_peek();
    test_lazyseq_concat();
    test_lazyseq_null_handling();
    test_lazyseq_il_wrappers();

    printf("All LazySeq tests passed!\n");
    return 0;
}
