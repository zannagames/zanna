//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTArrayStrTests.cpp
// Purpose: Verify basic behavior of the string runtime array helpers.
// Key invariants:
//   - String elements are properly reference-counted on get/put/release.
//   - An element that was never written reads as the empty string.
// Ownership/Lifetime:
//   - Tests own allocated arrays and release them via rt_arr_str_release().
// Links: src/runtime/arrays/rt_array_str.c
//
//===----------------------------------------------------------------------===//

#include "zanna/runtime/rt.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    // Test 1: Allocate empty array
    rt_string *arr = rt_arr_str_alloc(0);
    assert(arr != nullptr);
    assert(rt_arr_str_len(arr) == 0);
    rt_arr_str_release(arr, 0);

    // Test 2: Allocate array with 3 elements
    arr = rt_arr_str_alloc(3);
    assert(arr != nullptr);
    assert(rt_arr_str_len(arr) == 3);

    // Slots that were never written read as the empty string
    for (size_t i = 0; i < 3; ++i) {
        rt_string s = rt_arr_str_get(arr, i);
        assert(s != nullptr);
        assert(rt_str_len(s) == 0);
        assert(rt_str_eq(s, rt_str_empty()));
        // rt_arr_str_get hands the caller a reference to release
        rt_str_release_maybe(s);
    }

    // Test 3: Put strings into array
    rt_string str1 = rt_string_from_bytes("Hello", 5);
    rt_string str2 = rt_string_from_bytes("World", 5);
    rt_string str3 = rt_string_from_bytes("Test", 4);

    rt_arr_str_put(arr, 0, str1);
    rt_arr_str_put(arr, 1, str2);
    rt_arr_str_put(arr, 2, str3);

    // rt_arr_str_put retains the values, so we can release our references
    rt_str_release_maybe(str1);
    rt_str_release_maybe(str2);
    rt_str_release_maybe(str3);

    // Test 4: Get strings from array (get returns retained handles)
    rt_string retrieved1 = rt_arr_str_get(arr, 0);
    rt_string retrieved2 = rt_arr_str_get(arr, 1);
    rt_string retrieved3 = rt_arr_str_get(arr, 2);

    assert(retrieved1 != nullptr);
    assert(retrieved2 != nullptr);
    assert(retrieved3 != nullptr);

    assert(rt_str_len(retrieved1) == 5);
    assert(rt_str_len(retrieved2) == 5);
    assert(rt_str_len(retrieved3) == 4);

    // Release retrieved handles (since get retains)
    rt_str_release_maybe(retrieved1);
    rt_str_release_maybe(retrieved2);
    rt_str_release_maybe(retrieved3);

    // Test 5: Overwrite a slot
    rt_string new_str = rt_string_from_bytes("Updated", 7);
    rt_arr_str_put(arr, 1, new_str);
    rt_str_release_maybe(new_str);

    rt_string check = rt_arr_str_get(arr, 1);
    assert(rt_str_len(check) == 7);
    rt_str_release_maybe(check);

    // Test 6: Put NULL into a slot; it reads back as the empty string
    rt_arr_str_put(arr, 2, nullptr);
    rt_string null_check = rt_arr_str_get(arr, 2);
    assert(null_check != nullptr);
    assert(rt_str_len(null_check) == 0);
    rt_str_release_maybe(null_check);

    // Test 7: Release array (should release all remaining strings)
    rt_arr_str_release(arr, 3);

    // Test 8: Releasing one shared array reference must not tear down elements
    // still owned by the surviving alias.
    arr = rt_arr_str_alloc(1);
    assert(arr != nullptr);
    rt_string shared_value = rt_string_from_bytes("Shared", 6);
    rt_arr_str_put(arr, 0, shared_value);
    rt_str_release_maybe(shared_value);

    rt_heap_retain(arr);
    rt_string *alias = arr;
    rt_arr_str_release(arr, 1);

    assert(rt_arr_str_len(alias) == 1);
    rt_string shared_check = rt_arr_str_get(alias, 0);
    assert(shared_check != nullptr);
    assert(rt_str_len(shared_check) == 6);
    assert(std::memcmp(rt_string_cstr(shared_check), "Shared", 6) == 0);
    rt_str_release_maybe(shared_check);
    rt_arr_str_release(alias, 1);

    std::fprintf(stderr, "All string array tests passed!\n");
    return 0;
}
