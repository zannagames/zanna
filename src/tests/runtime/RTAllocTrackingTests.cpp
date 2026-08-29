//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTAllocTrackingTests.cpp
// Purpose: ZB-28 — raw rt_alloc blocks are classifiable as program-owned
//          memory while tracking is enabled (the reference IL VM turns it on),
//          and invisible otherwise.
// Key invariants:
//   - Tracking is off by default and costs nothing when off.
//   - rt_free forgets a block; interior ranges that cross a block end are rejected.
// Ownership/Lifetime:
//   - The test owns every block it allocates and frees it before exit.
// Links: src/runtime/core/rt_memory.c, src/vm/OpHandlerAccess.hpp
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_heap.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    assert(rt_alloc_tracking_enabled() == 0);
    void *before = rt_alloc(32);
    assert(rt_alloc_contains_range(before, 8) == 0); // off: nothing is tracked

    rt_alloc_set_tracking(1);
    assert(rt_alloc_tracking_enabled() == 1);
    assert(rt_alloc_contains_range(before, 8) == 0); // allocated before enable

    std::vector<void *> blocks;
    for (int i = 0; i < 5000; ++i)
        blocks.push_back(rt_alloc(24));
    for (void *b : blocks) {
        assert(rt_alloc_contains_range(b, 24) == 1);
        assert(rt_alloc_contains_range(static_cast<char *>(b) + 16, 8) == 1);
        assert(rt_alloc_contains_range(static_cast<char *>(b) + 17, 8) == 0);
        assert(rt_alloc_contains_range(b, 0) == 0);
    }
    int64_t stackSlot = 0;
    assert(rt_alloc_contains_range(&stackSlot, 8) == 0);
    assert(rt_alloc_contains_range(nullptr, 8) == 0);

    void *gone = blocks.back();
    blocks.pop_back();
    rt_free(gone);
    assert(rt_alloc_contains_range(gone, 8) == 0);

    void *zero = rt_alloc(0); // one-byte block
    assert(rt_alloc_contains_range(zero, 1) == 1);
    assert(rt_alloc_contains_range(zero, 2) == 0);
    rt_free(zero);

    for (void *b : blocks)
        rt_free(b);
    rt_alloc_set_tracking(0);
    assert(rt_alloc_tracking_enabled() == 0);
    rt_free(before);
    return 0;
}
