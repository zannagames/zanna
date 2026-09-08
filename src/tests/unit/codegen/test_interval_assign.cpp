//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_interval_assign.cpp
// Purpose: Pins the backend-independent assignment core both function-wide
//          allocators share (common/ra/IntervalAssign.hpp): range-list
//          algebra, hint preference, callee-saved preference across calls,
//          fixed physical ranges, weight-based eviction, memory-homing across
//          a setjmp-like call, first-fit slot sharing, and the private slot
//          of a value that a longjmp handler reads.
// Key invariants:
//   - Registers are ordinals here; the tests describe a four-register file
//     (0,1 caller-saved; 2,3 callee-saved) of one class.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/common/ra/IntervalAssign.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/ra/IntervalAssign.hpp"

#include <vector>

using zanna::codegen::ra::IntervalAssigner;
using zanna::codegen::ra::IntervalInfo;
using zanna::codegen::ra::kAnyClass;
using zanna::codegen::ra::kNoPos;
using zanna::codegen::ra::kNoReg;
using zanna::codegen::ra::RangeList;
using zanna::codegen::ra::RegisterFile;

namespace {

/// Four registers of class 0: 0 and 1 caller-saved, 2 and 3 callee-saved.
RegisterFile fourRegisters() {
    RegisterFile regs;
    regs.orderByClass = {{0, 1, 2, 3}};
    regs.allocatable = {1, 1, 1, 1};
    regs.calleeSaved = {0, 0, 1, 1};
    regs.classOf = {0, 0, 0, 0};
    return regs;
}

/// The same file with every register withheld from the pool: everything spills.
RegisterFile noRegisters() {
    RegisterFile regs = fourRegisters();
    regs.allocatable = {0, 0, 0, 0};
    return regs;
}

IntervalInfo interval(uint32_t id,
                      std::vector<std::pair<unsigned, unsigned>> ranges,
                      double weight = 1.0) {
    IntervalInfo info;
    info.id = id;
    info.cls = 0;
    for (const auto &[s, e] : ranges)
        info.live.add(s, e);
    info.weight = weight;
    return info;
}

std::vector<RangeList> noFixed() {
    return std::vector<RangeList>(4);
}

} // namespace

TEST(IntervalAssign, RangeListMergesAndQueries) {
    RangeList list;
    list.add(10, 12);
    list.add(2, 4);
    list.add(5, 6); // adjacent to [2,4]: merges
    EXPECT_EQ(list.toString(), "[2,6] [10,12]");
    EXPECT_TRUE(list.contains(6));
    EXPECT_FALSE(list.contains(7));
    EXPECT_EQ(list.start(), 2u);
    EXPECT_EQ(list.end(), 12u);

    RangeList other;
    other.add(7, 9);
    EXPECT_FALSE(list.intersects(other));
    other.add(9, 10);
    EXPECT_TRUE(list.intersects(other));
    EXPECT_EQ(list.firstIntersection(other), 10u);

    RangeList empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.start(), kNoPos);
    EXPECT_FALSE(empty.intersects(list));
}

TEST(IntervalAssign, DisjointIntervalsShareTheFirstRegister) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 4}}), interval(2, {{6, 9}})};
    IntervalAssigner a(ivs, fourRegisters(), noFixed());
    a.run();
    EXPECT_EQ(a.assigned(0), 0u);
    EXPECT_EQ(a.assigned(1), 0u);
    EXPECT_EQ(a.spilledCount(), 0u);
    EXPECT_TRUE(a.occupied(0).contains(2));
    EXPECT_TRUE(a.occupied(0).contains(8));
    EXPECT_FALSE(a.occupied(1).contains(2));
}

TEST(IntervalAssign, HintsAreHonouredWhenFree) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 4}}), interval(2, {{0, 4}})};
    ivs[1].hintPhys = 3;
    ivs[0].hintIds = {2};
    IntervalAssigner a(ivs, fourRegisters(), noFixed());
    a.run();
    // Interval 1 has no free hinted register yet (2 is unassigned when 1 is
    // placed); it takes register 0. Interval 2 takes its physical hint.
    EXPECT_EQ(a.assigned(0), 0u);
    EXPECT_EQ(a.assigned(1), 3u);
}

TEST(IntervalAssign, CallCrossingIntervalPrefersCalleeSaved) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 10}})};
    ivs[0].crossesCall = true;
    IntervalAssigner a(ivs, fourRegisters(), noFixed());
    a.run();
    EXPECT_EQ(a.assigned(0), 2u);
}

TEST(IntervalAssign, FixedRangesKeepARegisterOutOfReach) {
    std::vector<RangeList> fixed = noFixed();
    fixed[0].add(2, 3); // e.g. an argument marshalled into register 0
    std::vector<IntervalInfo> ivs{interval(1, {{0, 4}})};
    IntervalAssigner a(ivs, fourRegisters(), std::move(fixed));
    a.run();
    EXPECT_EQ(a.assigned(0), 1u);
}

TEST(IntervalAssign, PressureEvictsTheLightestAndSpillsWhenHeavier) {
    // Four registers, five overlapping intervals: the lightest ends in memory.
    std::vector<IntervalInfo> ivs;
    for (uint32_t id = 1; id <= 4; ++id)
        ivs.push_back(interval(id, {{0, 10}}, 5.0));
    ivs.push_back(interval(5, {{1, 9}}, 100.0)); // heavy latecomer evicts one
    ivs.push_back(interval(6, {{2, 8}}, 1.0));   // light: spilled
    IntervalAssigner a(ivs, fourRegisters(), noFixed());
    a.run();
    EXPECT_NE(a.assigned(4), kNoReg);
    EXPECT_EQ(a.assigned(5), kNoReg);
    EXPECT_EQ(a.spilledCount(), 2u); // one evicted, one spilled outright
}

TEST(IntervalAssign, EhPushCrossingIntervalIsMemoryHomed) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 10}})};
    ivs[0].crossesEhPush = true;
    IntervalAssigner a(ivs, fourRegisters(), noFixed());
    a.run();
    EXPECT_EQ(a.assigned(0), kNoReg);
    EXPECT_EQ(a.spilledCount(), 1u);
}

TEST(IntervalAssign, WithheldRegistersAreNeverAssignedEvenUnderEviction) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 4}}, 50.0)};
    IntervalAssigner a(ivs, noRegisters(), noFixed());
    a.run();
    EXPECT_EQ(a.assigned(0), kNoReg);
    EXPECT_EQ(a.spilledCount(), 1u);
}

TEST(IntervalAssign, SpillSlotsAreSharedByNonIntersectingIntervalsHottestFirst) {
    std::vector<IntervalInfo> ivs{
        interval(1, {{0, 4}}, 1.0), interval(2, {{6, 9}}, 5.0), interval(3, {{2, 7}}, 3.0)};
    IntervalAssigner a(ivs, noRegisters(), noFixed());
    a.run();
    const auto groups = a.shareSlots(kAnyClass);
    ASSERT_EQ(groups.size(), 2u);
    // Hottest first: interval 2 (weight 5) opens slot 0 and interval 1 joins
    // it (disjoint); interval 3 intersects both and gets slot 1.
    EXPECT_EQ(groups[0].size(), 2u);
    EXPECT_EQ(groups[0][0], 1u);
    EXPECT_EQ(groups[0][1], 0u);
    EXPECT_EQ(groups[1].size(), 1u);
    EXPECT_EQ(groups[1][0], 2u);
}

TEST(IntervalAssign, ValueReadByALongjmpHandlerKeepsAPrivateSlot) {
    // Interval 1 is defined before the setjmp-like call and read only in the
    // handler, so its range has a hole over the protected body where the
    // body-local interval 2 lives. They must not share a slot: the longjmp
    // edge that carries interval 1 into the handler is invisible to the CFG.
    std::vector<IntervalInfo> ivs{interval(1, {{0, 3}, {20, 24}}, 1.0),
                                  interval(2, {{5, 15}}, 1.0)};
    ivs[0].crossesEhPush = true;
    IntervalAssigner a(ivs, noRegisters(), noFixed());
    a.run();
    const auto groups = a.shareSlots(kAnyClass);
    ASSERT_EQ(groups.size(), 2u);
    for (const auto &g : groups)
        EXPECT_EQ(g.size(), 1u);
}

TEST(IntervalAssign, SlotSharingRespectsTheClassFilter) {
    std::vector<IntervalInfo> ivs{interval(1, {{0, 4}}), interval(2, {{6, 9}})};
    ivs[1].cls = 1;
    IntervalAssigner a(ivs, noRegisters(), noFixed());
    a.run();
    EXPECT_EQ(a.shareSlots(0).size(), 1u);
    EXPECT_EQ(a.shareSlots(1).size(), 1u);
    EXPECT_EQ(a.shareSlots(0)[0][0], 0u);
    EXPECT_EQ(a.shareSlots(1)[0][0], 1u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
