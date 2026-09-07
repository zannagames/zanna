//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/common/ILKernelDiff.hpp
// Purpose: One routine, shared by the ctest smoke and the libFuzzer harness,
//          that takes a generated IL kernel through parse, verify, the VM,
//          and the native backend at -O0 and -O2, and reports the first
//          disagreement.
// Key invariants:
//   - The three executions are compared on the 8-bit exit code the kernel
//     returns (the VM result masked to 8 bits, the native process exit
//     status); every kernel folds its whole computation into that value.
//   - A generator defect (text that does not parse or verify) is reported
//     as a failure of its own kind, never mistaken for a backend bug.
//   - The native runner is injected so each host links only the backend
//     command it can execute.
// Ownership/Lifetime:
//   - Header-only; the caller owns the scratch directory.
// Links: src/tests/common/ILKernelGenerator.hpp,
//        src/tests/unit/codegen/test_differential_il_kernels.cpp,
//        src/tests/fuzz/fuzz_il_native_diff.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ILKernelGenerator.hpp"
#include "common/VmFixture.hpp"
#include "il/core/Module.hpp"
#include "il/io/Parser.hpp"
#include "il/verify/Verifier.hpp"
#include "support/diag_expected.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

/// @file
/// @brief Declares the VM / native -O0 / native -O2 kernel comparison.

namespace zanna::tests {

/// @brief Outcome of one kernel comparison.
struct KernelDiffOutcome {
    /// @brief What went wrong, if anything.
    enum class Kind {
        Match,          ///< All three executions agree.
        GeneratorError, ///< The text did not parse or verify (generator bug).
        VmError,        ///< The VM threw while running the kernel.
        Mismatch        ///< The executions disagree (backend bug).
    };

    Kind kind{Kind::Match};
    std::uint64_t seed{0};
    int vmExit{0};      ///< VM result masked to 8 bits.
    int nativeO0{0};    ///< Native exit status at -O0.
    int nativeO2{0};    ///< Native exit status at -O2.
    std::string detail; ///< Human-readable explanation for a non-match.
};

/// @brief Runs the IL at @p ilPath natively with @p optFlag (e.g. "-O2") and
///        returns the process exit status.
using NativeKernelRunner =
    std::function<int(const std::filesystem::path &ilPath, const char *optFlag)>;

/// @brief Take @p program through parse, verify, VM, native -O0, native -O2.
/// @param program  Generated kernel.
/// @param scratch  Directory receiving the IL file (kept on mismatch).
/// @param runNative Native runner for this host.
/// @return The outcome; on a match the IL file is deleted.
inline KernelDiffOutcome runKernelDiff(const KernelProgram &program,
                                       const std::filesystem::path &scratch,
                                       const NativeKernelRunner &runNative) {
    KernelDiffOutcome outcome;
    outcome.seed = program.seed;

    il::core::Module module;
    {
        std::istringstream iss{program.il};
        if (!il::io::Parser::parse(iss, module)) {
            outcome.kind = KernelDiffOutcome::Kind::GeneratorError;
            outcome.detail = "generated IL does not parse";
            return outcome;
        }
        auto verified = il::verify::Verifier::verify(module);
        if (!verified) {
            std::ostringstream err;
            il::support::printDiag(verified.error(), err);
            outcome.kind = KernelDiffOutcome::Kind::GeneratorError;
            outcome.detail = "generated IL does not verify: " + err.str();
            return outcome;
        }
    }

    try {
        VmFixture vm;
        outcome.vmExit = static_cast<int>(vm.run(module)) & 0xFF;
    } catch (const std::exception &e) {
        outcome.kind = KernelDiffOutcome::Kind::VmError;
        outcome.detail = std::string("VM execution failed: ") + e.what();
        return outcome;
    }

    std::filesystem::create_directories(scratch);
    const std::filesystem::path ilPath =
        scratch / ("kernel_seed_" + std::to_string(program.seed) + ".il");
    {
        std::ofstream ofs(ilPath);
        ofs << program.il;
    }

    outcome.nativeO0 = runNative(ilPath, "-O0") & 0xFF;
    outcome.nativeO2 = runNative(ilPath, "-O2") & 0xFF;

    if (outcome.nativeO0 != outcome.vmExit || outcome.nativeO2 != outcome.vmExit) {
        std::ostringstream what;
        what << "seed " << program.seed << ": VM=" << outcome.vmExit
             << " native-O0=" << outcome.nativeO0 << " native-O2=" << outcome.nativeO2
             << " (shapes:";
        for (KernelShape shape : program.shapes)
            what << ' ' << kernelShapeName(shape);
        what << "; IL kept at " << ilPath.string() << ")";
        outcome.kind = KernelDiffOutcome::Kind::Mismatch;
        outcome.detail = what.str();
        return outcome;
    }

    std::error_code ec;
    std::filesystem::remove(ilPath, ec);
    return outcome;
}

} // namespace zanna::tests
