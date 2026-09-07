//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/fuzz/fuzz_il_native_diff.cpp
// Purpose: libFuzzer harness that derives a seed from the input, generates an
//          IL kernel from it, and requires the VM, the -O0 native build, and
//          the -O2 native build (MIR verifier on) to agree on the exit code.
//          A disagreement aborts with the seed and the kept IL path so the
//          program can be replayed through `zanna -run` and
//          `zanna codegen <arch> -run-native -O<n>`.
// Key invariants:
//   - The harness never rejects an input: every byte string maps to a seed.
//   - Generator defects (unparseable or unverifiable text) abort too; they
//     are bugs in the generator, not in the backend.
// Ownership/Lifetime: Scratch IL lives under build/test-out/il-kernels-fuzz.
// Links: src/tests/common/ILKernelGenerator.hpp, src/tests/common/ILKernelDiff.hpp,
//        src/tests/unit/codegen/test_differential_il_kernels.cpp
//
//===----------------------------------------------------------------------===//

#include "common/ILKernelDiff.hpp"
#include "common/ILKernelGenerator.hpp"

#if defined(ZANNA_KERNEL_DIFF_ARM64)
#include "tools/zanna/cmd_codegen_arm64.hpp"
#elif defined(ZANNA_KERNEL_DIFF_X64)
#include "tools/zanna/cmd_codegen_x64.hpp"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace {

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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (size_t k = 0; k < size; ++k)
        seed = (seed ^ data[k]) * 0x100000001B3ULL;

    const zanna::tests::KernelProgram program = zanna::tests::generateKernelProgram(seed);
    const zanna::tests::KernelDiffOutcome outcome = zanna::tests::runKernelDiff(
        program, std::filesystem::path{"build/test-out/il-kernels-fuzz"}, runNativeOnHost);
    if (outcome.kind != zanna::tests::KernelDiffOutcome::Kind::Match) {
        std::fprintf(stderr, "il-kernel differential failure: %s\n", outcome.detail.c_str());
        std::abort();
    }
    return 0;
}
