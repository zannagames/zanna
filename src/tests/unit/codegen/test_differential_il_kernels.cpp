//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_differential_il_kernels.cpp
// Purpose: Fixed-seed differential smoke over generated IL kernels: every
//          seed in the range is run on the VM and natively at -O0 and -O2
//          (with the MIR verifier on) and the three exit codes must agree.
//          Runs in the ordinary gate; the libFuzzer harness in
//          src/tests/fuzz/fuzz_il_native_diff.cpp explores seeds beyond it.
// Key invariants:
//   - The native backend is the host's (AArch64 on Apple arm64, x86-64 on
//     x86-64 Linux/Windows); the build registers the test only where one is
//     available and selects the entry point with a compile definition.
//   - The seed range is fixed so a failure names a reproducible program;
//     ZANNA_KERNEL_SEEDS=<count> widens it locally.
// Ownership/Lifetime: Standalone test binary; scratch IL lives under build/test-out.
// Links: src/tests/common/ILKernelGenerator.hpp, src/tests/common/ILKernelDiff.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.5)
//
//===----------------------------------------------------------------------===//

#include "common/ILKernelDiff.hpp"
#include "common/ILKernelGenerator.hpp"
#include "tests/TestHarness.hpp"

#include "tests/common/PosixCompat.h"

#if defined(ZANNA_KERNEL_DIFF_ARM64)
#include "tools/zanna/cmd_codegen_arm64.hpp"
#elif defined(ZANNA_KERNEL_DIFF_X64)
#include "tools/zanna/cmd_codegen_x64.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

using namespace zanna::tests;

namespace {

/// @brief Seeds covered by the gate run.
constexpr std::uint64_t kDefaultSeedCount = 48;

std::uint64_t seedCount() {
    if (const char *env = std::getenv("ZANNA_KERNEL_SEEDS")) {
        char *end = nullptr;
        const unsigned long v = std::strtoul(env, &end, 10);
        if (end && *end == '\0' && v > 0)
            return v;
    }
    return kDefaultSeedCount;
}

/// @brief Run @p ilPath natively on the host backend and return the exit status.
/// @details The command prints the program's stdout to std::cout; redirect it
///          so the test log stays readable.
int runNativeOnHost(const std::filesystem::path &ilPath, const char *optFlag) {
#if defined(ZANNA_KERNEL_DIFF_ARM64) || defined(ZANNA_KERNEL_DIFF_X64)
    const std::string path = ilPath.string();
    const char *argv[] = {path.c_str(), "-run-native", "--verify-mir", optFlag};
    std::ostringstream sink;
    std::streambuf *saved = std::cout.rdbuf(sink.rdbuf());
#if defined(ZANNA_KERNEL_DIFF_ARM64)
    const int rc = zanna::tools::ilc::cmd_codegen_arm64(4, const_cast<char **>(argv));
#else
    const int rc = zanna::tools::ilc::cmd_codegen_x64(4, const_cast<char **>(argv));
#endif
    std::cout.rdbuf(saved);
    return rc;
#else
    (void)ilPath;
    (void)optFlag;
    return -1;
#endif
}

std::filesystem::path scratchDir() {
    return std::filesystem::path{"build/test-out/il-kernels-" + std::to_string(getpid())};
}

} // namespace

TEST(DifferentialIlKernels, EveryShapeIsGeneratedAndVerifies) {
    // The first few hundred seeds must cover every shape and all of them
    // must parse and verify; this guards the generator itself.
    bool seen[static_cast<unsigned>(KernelShape::Count)] = {};
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        const KernelProgram program = generateKernelProgram(seed);
        for (KernelShape shape : program.shapes)
            seen[static_cast<unsigned>(shape)] = true;
        il::core::Module module;
        std::istringstream iss{program.il};
        ASSERT_TRUE(il::io::Parser::parse(iss, module));
        auto verified = il::verify::Verifier::verify(module);
        if (!verified) {
            std::ostringstream err;
            il::support::printDiag(verified.error(), err);
            std::cerr << "seed " << seed << ": " << err.str() << "\n" << program.il;
        }
        ASSERT_TRUE(verified);
    }
    for (unsigned k = 0; k < static_cast<unsigned>(KernelShape::Count); ++k)
        EXPECT_TRUE(seen[k]);

    // Determinism: the same seed yields the same text.
    EXPECT_EQ(generateKernelProgram(7).il, generateKernelProgram(7).il);
}

TEST(DifferentialIlKernels, VmAndNativeAgreeAcrossOptLevels) {
#if !defined(ZANNA_KERNEL_DIFF_ARM64) && !defined(ZANNA_KERNEL_DIFF_X64)
    ZANNA_TEST_SKIP("no native backend for this host");
#else
    const std::filesystem::path scratch = scratchDir();
    const std::uint64_t count = seedCount();
    std::size_t mismatches = 0;
    for (std::uint64_t seed = 0; seed < count; ++seed) {
        const KernelProgram program = generateKernelProgram(seed);
        const KernelDiffOutcome outcome = runKernelDiff(program, scratch, runNativeOnHost);
        if (outcome.kind != KernelDiffOutcome::Kind::Match) {
            std::cerr << "il-kernel differential: " << outcome.detail << "\n";
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
#endif
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
