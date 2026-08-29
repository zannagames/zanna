//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTModvarTests.cpp
// Purpose: Stress-test rt_modvar_* helpers (table growth + stable addresses).
//
//===----------------------------------------------------------------------===//

#include "rt_context.h"
#include "rt_modvar.h"
#include "rt_string.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    RtContext ctx{};
    rt_context_init(&ctx);
    rt_set_current_context(&ctx);

    constexpr int kVars = 100;
    std::vector<int64_t *> addrs;
    addrs.reserve(kVars);

    for (int i = 0; i < kVars; ++i) {
        std::string name = "X" + std::to_string(i);
        rt_string s = rt_string_from_bytes(name.c_str(), name.size());
        assert(s != nullptr);
        int64_t *addr = (int64_t *)rt_modvar_addr_i64(s);
        assert(addr != nullptr);
        *addr = (int64_t)(i * 10);
        addrs.push_back(addr);
        rt_string_unref(s);
    }

    for (int i = 0; i < kVars; i += 7) {
        std::string name = "X" + std::to_string(i);
        rt_string s = rt_string_from_bytes(name.c_str(), name.size());
        assert(s != nullptr);
        int64_t *addr = (int64_t *)rt_modvar_addr_i64(s);
        assert(addr == addrs[static_cast<size_t>(i)]);
        assert(*addr == (int64_t)(i * 10));
        rt_string_unref(s);
    }

    // ZB-28: the IL VM classifies module-variable storage through this query.
    {
        int64_t *first = addrs[0];
        assert(rt_modvar_contains_range(first, 8) == 1);
        assert(rt_modvar_contains_range(first, 0) == 0);
        assert(rt_modvar_contains_range(reinterpret_cast<char *>(first) + 4, 8) == 0);
        assert(rt_modvar_contains_range(nullptr, 8) == 0);
        int64_t stackSlot = 0;
        assert(rt_modvar_contains_range(&stackSlot, 8) == 0);
        rt_string block = rt_string_from_bytes("BLK", 3);
        char *blk = (char *)rt_modvar_addr_block(block, 24);
        assert(blk != nullptr);
        assert(rt_modvar_contains_range(blk + 16, 8) == 1);
        assert(rt_modvar_contains_range(blk + 17, 8) == 0);
        rt_string_unref(block);
    }

    rt_set_current_context(nullptr);
    rt_context_cleanup(&ctx);
    return 0;
}
