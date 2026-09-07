//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_parallel_copy.cpp
// Purpose: Pins the shared parallel-copy sequentializer: dependency-safe
//          ordering, identity elision, cycle breaking through a scratch,
//          memory-to-memory routing through a temporary, scratch release, and
//          deterministic emission order.
// Key invariants:
//   - Each case simulates the copy on a value map and checks that every
//     destination ends up holding its source's original value.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/common/ra/ParallelCopy.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/ra/ParallelCopy.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using zanna::codegen::ra::CopyLoc;
using zanna::codegen::ra::ParallelCopyTask;
using zanna::codegen::ra::sequentializeParallelCopy;

namespace {

constexpr unsigned kGPR = 0;
constexpr unsigned kFPR = 1;
constexpr unsigned kScratchBase = 100; ///< Scratch registers are numbered from here.

/// @brief Recording emitter that also simulates the copy on a value map.
struct SimEmitter {
    std::map<std::string, std::string> values; ///< location name -> held value
    std::vector<std::string> trace;            ///< textual move log
    std::vector<CopyLoc> outstanding;          ///< scratches handed out, not released
    unsigned nextScratch{kScratchBase};
    std::size_t maxOutstanding{0};

    static std::string name(const CopyLoc &loc) {
        return (loc.isReg() ? "r" : "m") + std::to_string(loc.isReg() ? loc.reg : loc.slot) +
               (loc.cls == kFPR ? "f" : "");
    }

    void move(const CopyLoc &dst, const CopyLoc &src) {
        if (!dst.isReg() && !src.isReg())
            throw std::logic_error("emitter saw a mem<-mem move");
        if (dst.cls != src.cls)
            throw std::logic_error("emitter saw a class mismatch");
        auto it = values.find(name(src));
        values[name(dst)] = it == values.end() ? "?" : it->second;
        trace.push_back(name(dst) + "<-" + name(src));
    }

    CopyLoc cycleScratch(unsigned cls) {
        return take(cls);
    }

    CopyLoc memTemp(unsigned cls) {
        return take(cls);
    }

    void releaseScratch(const CopyLoc &loc) {
        for (std::size_t i = 0; i < outstanding.size(); ++i) {
            if (outstanding[i] == loc) {
                outstanding.erase(outstanding.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
        throw std::logic_error("released a scratch that was not outstanding");
    }

  private:
    CopyLoc take(unsigned cls) {
        const CopyLoc loc = CopyLoc::regLoc(cls, nextScratch++);
        outstanding.push_back(loc);
        if (outstanding.size() > maxOutstanding)
            maxOutstanding = outstanding.size();
        return loc;
    }
};

CopyLoc R(unsigned reg, unsigned cls = kGPR) {
    return CopyLoc::regLoc(cls, reg);
}

CopyLoc M(int slot, unsigned cls = kGPR) {
    return CopyLoc::memLoc(cls, slot);
}

/// @brief Seed every source with a value named after itself and run the copy.
SimEmitter run(const std::vector<ParallelCopyTask> &tasks) {
    SimEmitter emit;
    for (const auto &t : tasks)
        emit.values[SimEmitter::name(t.src)] = "v_" + SimEmitter::name(t.src);
    (void)sequentializeParallelCopy(tasks, emit);
    return emit;
}

/// @brief Every destination holds the value its source held before the copy.
void expectSemantics(const SimEmitter &emit, const std::vector<ParallelCopyTask> &tasks) {
    for (const auto &t : tasks) {
        const auto it = emit.values.find(SimEmitter::name(t.dst));
        ASSERT_TRUE(it != emit.values.end());
        EXPECT_EQ(it->second, "v_" + SimEmitter::name(t.src));
    }
    EXPECT_EQ(emit.outstanding.size(), 0u);
}

} // namespace

TEST(ParallelCopy, IdentityEmitsNothing) {
    const std::vector<ParallelCopyTask> tasks{{R(1), R(1)}, {M(8), M(8)}};
    SimEmitter emit;
    EXPECT_EQ(sequentializeParallelCopy(tasks, emit), 0u);
    EXPECT_EQ(emit.trace.size(), 0u);
}

TEST(ParallelCopy, ChainIsEmittedInDependencyOrder) {
    // r1 <- r2, r2 <- r3: r1 must be written before r2 is overwritten.
    const std::vector<ParallelCopyTask> tasks{{R(2), R(3)}, {R(1), R(2)}};
    const SimEmitter emit = run(tasks);
    ASSERT_EQ(emit.trace.size(), 2u);
    EXPECT_EQ(emit.trace[0], "r1<-r2");
    EXPECT_EQ(emit.trace[1], "r2<-r3");
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, TwoCycleUsesOneScratch) {
    const std::vector<ParallelCopyTask> tasks{{R(1), R(2)}, {R(2), R(1)}};
    const SimEmitter emit = run(tasks);
    ASSERT_EQ(emit.trace.size(), 3u);
    EXPECT_EQ(emit.trace[0], "r100<-r2");
    EXPECT_EQ(emit.trace[1], "r2<-r1");
    EXPECT_EQ(emit.trace[2], "r1<-r100");
    EXPECT_EQ(emit.maxOutstanding, 1u);
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, ThreeCycleUsesOneScratch) {
    const std::vector<ParallelCopyTask> tasks{{R(1), R(2)}, {R(2), R(3)}, {R(3), R(1)}};
    const SimEmitter emit = run(tasks);
    EXPECT_EQ(emit.trace.size(), 4u);
    EXPECT_EQ(emit.maxOutstanding, 1u);
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, CycleThroughMemoryPrefersRegisterSeed) {
    // m8 <- r1, r1 <- m8: the register-to-register seed is absent, so the
    // scratch load comes from memory; semantics still hold.
    const std::vector<ParallelCopyTask> tasks{{M(8), R(1)}, {R(1), M(8)}};
    const SimEmitter emit = run(tasks);
    EXPECT_EQ(emit.trace.size(), 3u);
    expectSemantics(emit, tasks);

    // With a register pair in the cycle the seed is that pair.
    const std::vector<ParallelCopyTask> mixed{{M(8), R(1)}, {R(1), R(2)}, {R(2), M(8)}};
    const SimEmitter emit2 = run(mixed);
    ASSERT_TRUE(!emit2.trace.empty());
    EXPECT_EQ(emit2.trace[0], "r100<-r2");
    expectSemantics(emit2, mixed);
}

TEST(ParallelCopy, MemoryToMemoryGoesThroughTemporary) {
    const std::vector<ParallelCopyTask> tasks{{M(16), M(8)}};
    const SimEmitter emit = run(tasks);
    ASSERT_EQ(emit.trace.size(), 2u);
    EXPECT_EQ(emit.trace[0], "r100<-m8");
    EXPECT_EQ(emit.trace[1], "m16<-r100");
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, RegisterAndMemoryDirectionsAreDirect) {
    const std::vector<ParallelCopyTask> tasks{{R(1), M(8)}, {M(16), R(2)}};
    const SimEmitter emit = run(tasks);
    ASSERT_EQ(emit.trace.size(), 2u);
    EXPECT_EQ(emit.maxOutstanding, 0u);
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, IndependentClassesDoNotInterfere) {
    const std::vector<ParallelCopyTask> tasks{
        {R(1), R(2)}, {R(2), R(1)}, {R(1, kFPR), R(2, kFPR)}, {R(2, kFPR), R(1, kFPR)}};
    const SimEmitter emit = run(tasks);
    EXPECT_EQ(emit.trace.size(), 6u);
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, FanOutReadsSourceBeforeItIsOverwritten) {
    // r2 <- r1, r3 <- r1, r1 <- r4
    const std::vector<ParallelCopyTask> tasks{{R(1), R(4)}, {R(2), R(1)}, {R(3), R(1)}};
    const SimEmitter emit = run(tasks);
    ASSERT_EQ(emit.trace.size(), 3u);
    EXPECT_EQ(emit.trace[2], "r1<-r4");
    expectSemantics(emit, tasks);
}

TEST(ParallelCopy, EmissionIsDeterministic) {
    const std::vector<ParallelCopyTask> tasks{
        {R(1), R(2)}, {R(2), R(3)}, {R(3), R(1)}, {M(8), R(1)}, {R(5), M(24)}};
    const SimEmitter a = run(tasks);
    const SimEmitter b = run(tasks);
    EXPECT_EQ(a.trace.size(), b.trace.size());
    for (std::size_t i = 0; i < a.trace.size() && i < b.trace.size(); ++i)
        EXPECT_EQ(a.trace[i], b.trace[i]);
    expectSemantics(a, tasks);
}

TEST(ParallelCopy, RejectsClassMismatchAndDuplicateDestination) {
    SimEmitter emit;
    const std::vector<ParallelCopyTask> mismatch{{R(1, kGPR), R(2, kFPR)}};
    EXPECT_THROWS(sequentializeParallelCopy(mismatch, emit), std::runtime_error);
    const std::vector<ParallelCopyTask> duplicate{{R(1), R(2)}, {R(1), R(3)}};
    EXPECT_THROWS(sequentializeParallelCopy(duplicate, emit), std::runtime_error);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
