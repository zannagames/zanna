//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_diff_vm_native_property.cpp
// Purpose: Property-based differential testing between VM and native backends.
// Key invariants: VM and native execution must produce identical results.
// Ownership/Lifetime: Test generates ephemeral IL modules for each iteration.
// Links: docs/internals/testing.md
//
// Backend Selection:
//   - On ARM64 hosts (Apple Silicon): Uses AArch64 backend automatically
//   - On x86-64 hosts: Uses x86-64 backend (if available)
//   - Force AArch64: Define ZANNA_FORCE_ARM64_DIFF_TEST at compile time
//   - Environment var: Set ZANNA_DIFF_BACKEND=arm64 to force ARM64 at runtime
//
// Example usage:
//   # Run tests normally (auto-detect backend):
//   ./build/src/tests/unit/codegen/test_diff_vm_native_property
//
//   # Force ARM64 backend via environment:
//   ZANNA_DIFF_BACKEND=arm64 ./build/src/tests/unit/codegen/test_diff_vm_native_property
//
//===----------------------------------------------------------------------===//

#include "common/ILGenerator.hpp"
#include "common/VmFixture.hpp"
#include "il/io/Parser.hpp"
#include "il/verify/Verifier.hpp"
#include "support/diag_expected.hpp"
#include "tests/TestHarness.hpp"
#include "tools/zanna/cmd_codegen_arm64.hpp"

#include "tests/common/PosixCompat.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace zanna::tests;
using namespace zanna::tools::ilc;

namespace {

/// @brief Number of property test iterations.
/// @details Keep low for CI stability; can be increased for local fuzzing.
constexpr std::size_t kDefaultIterations = 10;

std::size_t iterationsForPropertyTest() {
    if (const char *env = std::getenv("ZANNA_DIFF_ITERATIONS")) {
        char *end = nullptr;
        const unsigned long v = std::strtoul(env, &end, 10);
        if (end && *end == '\0' && v > 0)
            return static_cast<std::size_t>(v);
    }
    return kDefaultIterations;
}

/// @brief Get a stable base seed for reproducible tests.
/// @details Uses a combination of PID and a counter to ensure unique but
///          reproducible seeds across test runs in the same process.
std::uint64_t getStableBaseSeed() {
    static std::atomic<std::uint64_t> counter{0};
    // Use PID to differentiate between parallel test processes
    // Use counter to differentiate between test cases within a process
    return static_cast<std::uint64_t>(getpid()) * 1000000ULL + counter.fetch_add(1);
}

/// @brief Backend type for differential testing.
enum class Backend {
    None,
    AArch64,
    // X86_64, // Future: add x86-64 backend support
};

/// @brief Get string name for backend.
const char *backendName(Backend b) {
    switch (b) {
        case Backend::AArch64:
            return "AArch64";
        case Backend::None:
        default:
            return "None";
    }
}

/// @brief Check if ARM64 native execution is available on this host.
bool isArm64HostAvailable() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return true;
#else
    return false;
#endif
}

/// @brief Check if ARM64 should be forced via compile-time or runtime config.
bool isArm64Forced() {
#ifdef ZANNA_FORCE_ARM64_DIFF_TEST
    return true;
#else
    const char *env = std::getenv("ZANNA_DIFF_BACKEND");
    return env && std::string(env) == "arm64";
#endif
}

/// @brief Select the native backend to use for differential testing.
/// @details Selection priority:
///   1. Compile-time ZANNA_FORCE_ARM64_DIFF_TEST -> AArch64
///   2. Runtime ZANNA_DIFF_BACKEND=arm64 -> AArch64
///   3. Host is ARM64 -> AArch64
///   4. Otherwise -> None (tests will be skipped)
Backend selectBackend() {
    if (isArm64Forced()) {
        return Backend::AArch64;
    }
    if (isArm64HostAvailable()) {
        return Backend::AArch64;
    }
    return Backend::None;
}

/// @brief Global backend selection (computed once at startup).
Backend g_selectedBackend = Backend::None;
bool g_backendLogged = false;

/// @brief Log backend selection (once per test run).
void logBackendSelection() {
    if (!g_backendLogged) {
        g_selectedBackend = selectBackend();
        std::cerr << "\n"
                  << "=== VM vs Native Differential Test ===" << "\n"
                  << "  Selected backend: " << backendName(g_selectedBackend) << "\n"
                  << "  Host ARM64: " << (isArm64HostAvailable() ? "yes" : "no") << "\n"
                  << "  Force ARM64: " << (isArm64Forced() ? "yes" : "no") << "\n"
                  << "=======================================" << "\n\n";
        g_backendLogged = true;
    }
}

/// @brief Create output directory for test artifacts.
std::filesystem::path ensureOutputDir() {
    namespace fs = std::filesystem;
    // Use PID to avoid conflicts when tests run in parallel
    const fs::path dir{"build/test-out/diff-property-" + std::to_string(getpid())};
    fs::create_directories(dir);
    return dir;
}

/// @brief Write IL source to a file.
void writeILFile(const std::filesystem::path &path, const std::string &source) {
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::runtime_error("Failed to write IL file: " + path.string());
    }
    ofs << source;
}

/// @brief Run IL module on VM and return result.
/// @return Exit code from VM execution.
std::int64_t runOnVm(il::core::Module &module) {
    VmFixture fixture;
    return fixture.run(module);
}

/// @brief Run IL via ARM64 native backend and return exit code.
/// @param ilPath Path to IL source file.
/// @return Native execution exit code, or -1 on failure.
int runOnArm64Native(const std::filesystem::path &ilPath) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    const char *argv[] = {ilPath.c_str(), "-run-native", "--verify-mir"};
    return cmd_codegen_arm64(3, const_cast<char **>(argv));
#else
    (void)ilPath;
    return -1; // Not supported on this platform
#endif
}

/// @brief Check if native backend execution is available.
bool isNativeAvailable() {
    logBackendSelection();
    return g_selectedBackend != Backend::None;
}

constexpr const char kMaterialAnisotropyRoundTripIl[] = R"(il 0.3.0
extern @Zanna.Graphics3D.Material3D.New() -> ptr
extern @Zanna.Graphics3D.Material3D.set_Anisotropy(ptr, i64) -> void
extern @Zanna.Graphics3D.Material3D.get_Anisotropy(ptr) -> i64

func @main() -> i64 {
entry:
  %mat = call @Zanna.Graphics3D.Material3D.New()
  %default = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 0)
  %low = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 64)
  %high = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 8)
  %round = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  %a = iadd.ovf %default, %low
  %b = iadd.ovf %a, %high
  %sum = iadd.ovf %b, %round
  ret %sum
}
)";

il::core::Module parseModuleFromSource(const std::string &source) {
    std::istringstream iss{source};
    il::core::Module module;
    auto parseResult = il::io::Parser::parse(iss, module);
    ASSERT_TRUE(parseResult);
    auto verifyResult = il::verify::Verifier::verify(module);
    if (!verifyResult) {
        std::ostringstream errStream;
        il::support::printDiag(verifyResult.error(), errStream);
        std::cerr << errStream.str() << "\n";
    }
    ASSERT_TRUE(verifyResult);
    return module;
}

/// @brief Result of a differential test run.
struct DiffTestResult {
    bool passed = false;
    std::uint64_t seed = 0;
    std::int64_t vmResult = 0;
    int nativeResult = 0;
    std::string ilSource;
    std::string errorMessage;
};

/// @brief Run a single differential test iteration.
DiffTestResult runDifferentialTest(ILGenerator &generator,
                                   const ILGeneratorConfig &config,
                                   const std::filesystem::path &outputDir,
                                   std::size_t iteration) {
    DiffTestResult result;
    result.seed = generator.seed();

    // Generate IL module
    ILGeneratorResult genResult = generator.generate(config);
    result.ilSource = genResult.ilSource;

    // Verify generated module
    auto verifyResult = il::verify::Verifier::verify(genResult.module);
    if (!verifyResult) {
        std::ostringstream errStream;
        il::support::printDiag(verifyResult.error(), errStream);
        result.errorMessage = "Generated IL failed verification: " + errStream.str() +
                              "\nIL source:\n" + genResult.ilSource;
        return result;
    }

    // Run on VM
    try {
        il::core::Module moduleCopy = genResult.module;
        result.vmResult = runOnVm(moduleCopy);
    } catch (const std::exception &e) {
        result.errorMessage = "VM execution failed: " + std::string(e.what());
        return result;
    }

    // Run on native backend (if available)
    if (g_selectedBackend == Backend::AArch64) {
        // Write IL to temp file
        const std::filesystem::path ilPath =
            outputDir / ("iter_" + std::to_string(iteration) + "_seed_" +
                         std::to_string(genResult.seed) + ".il");
        try {
            writeILFile(ilPath, genResult.ilSource);
        } catch (const std::exception &e) {
            result.errorMessage = "Failed to write IL file: " + std::string(e.what());
            return result;
        }

        result.nativeResult = runOnArm64Native(ilPath);

        // Compare results
        // Note: VM returns i64, native returns int (truncated to 8 bits on exit)
        const int vmExitCode = static_cast<int>(result.vmResult) & 0xFF;
        const int nativeExitCode = result.nativeResult & 0xFF;

        if (vmExitCode != nativeExitCode) {
            std::ostringstream oss;
            oss << "Result mismatch!\n"
                << "  Seed: " << genResult.seed << "\n"
                << "  VM result: " << result.vmResult << " (exit code: " << vmExitCode << ")\n"
                << "  Native result: " << result.nativeResult << " (exit code: " << nativeExitCode
                << ")\n"
                << "  IL source:\n"
                << genResult.ilSource;
            result.errorMessage = oss.str();
            return result;
        }

        // Clean up temp file on success
        std::error_code ec;
        std::filesystem::remove(ilPath, ec);
    }

    result.passed = true;
    return result;
}

} // namespace

TEST(DiffVmNativeProperty, RuntimeMaterial3DAnisotropyRoundTrip) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    il::core::Module vmModule = parseModuleFromSource(kMaterialAnisotropyRoundTripIl);
    VmFixture fixture;
    const std::int64_t vmResult = fixture.run(vmModule);
    ASSERT_EQ(vmResult, 26);

    const std::filesystem::path outputDir = ensureOutputDir();
    const std::filesystem::path ilPath = outputDir / "runtime_material3d_anisotropy.il";
    writeILFile(ilPath, kMaterialAnisotropyRoundTripIl);

    const int nativeResult = runOnArm64Native(ilPath);
    const int vmExitCode = static_cast<int>(vmResult) & 0xFF;
    const int nativeExitCode = nativeResult & 0xFF;
    ASSERT_EQ(vmExitCode, nativeExitCode);

    std::error_code ec;
    std::filesystem::remove(ilPath, ec);
}

TEST(DiffVmNativeProperty, ArithmeticOnly) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    const std::filesystem::path outputDir = ensureOutputDir();

    ILGeneratorConfig config;
    config.includeControlFlow = false;
    config.includeComparisons = false;
    config.includeBitwise = false;
    config.includeShifts = false;
    config.minInstructions = 3;
    config.maxInstructions = 10;
    config.minBlocks = 1;
    config.maxBlocks = 1;

    const std::uint64_t baseSeed = getStableBaseSeed();
    const std::size_t iterations = iterationsForPropertyTest();

    for (std::size_t i = 0; i < iterations; ++i) {
        ILGenerator generator(baseSeed + i);
        DiffTestResult result = runDifferentialTest(generator, config, outputDir, i);

        if (!result.passed)
            std::cerr << "Iteration " << i << " failed:\n" << result.errorMessage << "\n";
        ASSERT_TRUE(result.passed);
    }
}

TEST(DiffVmNativeProperty, ArithmeticWithComparisons) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    const std::filesystem::path outputDir = ensureOutputDir();

    ILGeneratorConfig config;
    config.includeControlFlow = false;
    config.includeComparisons = true;
    config.includeBitwise = false;
    config.includeShifts = false;
    config.minInstructions = 5;
    config.maxInstructions = 15;
    config.minBlocks = 1;
    config.maxBlocks = 1;

    const std::uint64_t baseSeed = getStableBaseSeed();
    const std::size_t iterations = iterationsForPropertyTest();

    for (std::size_t i = 0; i < iterations; ++i) {
        ILGenerator generator(baseSeed + i);
        DiffTestResult result = runDifferentialTest(generator, config, outputDir, i);

        if (!result.passed)
            std::cerr << "Iteration " << i << " failed:\n" << result.errorMessage << "\n";
        ASSERT_TRUE(result.passed);
    }
}

TEST(DiffVmNativeProperty, BitwiseAndShifts) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    const std::filesystem::path outputDir = ensureOutputDir();

    ILGeneratorConfig config;
    config.includeControlFlow = false;
    config.includeComparisons = false;
    config.includeBitwise = true;
    config.includeShifts = true;
    config.minInstructions = 5;
    config.maxInstructions = 12;
    config.minBlocks = 1;
    config.maxBlocks = 1;

    const std::uint64_t baseSeed = getStableBaseSeed();
    const std::size_t iterations = iterationsForPropertyTest();

    for (std::size_t i = 0; i < iterations; ++i) {
        ILGenerator generator(baseSeed + i);
        DiffTestResult result = runDifferentialTest(generator, config, outputDir, i);

        if (!result.passed)
            std::cerr << "Iteration " << i << " failed:\n" << result.errorMessage << "\n";
        ASSERT_TRUE(result.passed);
    }
}

TEST(DiffVmNativeProperty, MixedOperations) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    const std::filesystem::path outputDir = ensureOutputDir();

    ILGeneratorConfig config;
    config.includeControlFlow = false;
    config.includeComparisons = true;
    config.includeBitwise = true;
    config.includeShifts = true;
    config.minInstructions = 8;
    config.maxInstructions = 20;
    config.minBlocks = 1;
    config.maxBlocks = 1;

    const std::uint64_t baseSeed = getStableBaseSeed();
    const std::size_t iterations = iterationsForPropertyTest();

    for (std::size_t i = 0; i < iterations; ++i) {
        ILGenerator generator(baseSeed + i);
        DiffTestResult result = runDifferentialTest(generator, config, outputDir, i);

        if (!result.passed)
            std::cerr << "Iteration " << i << " failed:\n" << result.errorMessage << "\n";
        ASSERT_TRUE(result.passed);
    }
}

TEST(DiffVmNativeProperty, ControlFlow) {
    if (!isNativeAvailable()) {
        ZANNA_TEST_SKIP(std::string("Native execution not available (backend: ") +
                        backendName(g_selectedBackend) + ")");
    }

    const std::filesystem::path outputDir = ensureOutputDir();

    ILGeneratorConfig config;
    config.includeControlFlow = true;
    config.includeComparisons = true;
    config.includeBitwise = true;
    config.includeShifts = true;
    config.minInstructions = 5;
    config.maxInstructions = 12;
    config.minBlocks = 2;
    config.maxBlocks = 4;

    const std::uint64_t baseSeed = getStableBaseSeed();
    const std::size_t iterations = iterationsForPropertyTest();

    for (std::size_t i = 0; i < iterations; ++i) {
        ILGenerator generator(baseSeed + i);
        DiffTestResult result = runDifferentialTest(generator, config, outputDir, i);

        if (!result.passed)
            std::cerr << "Iteration " << i << " failed:\n" << result.errorMessage << "\n";
        ASSERT_TRUE(result.passed);
    }
}

TEST(DiffVmNativeProperty, ReproducibilityWithSeed) {
    constexpr std::uint64_t kTestSeed = 12345678;

    ILGeneratorConfig config;
    config.includeControlFlow = false;
    config.minInstructions = 5;
    config.maxInstructions = 5;

    ILGenerator gen1(kTestSeed);
    ILGenerator gen2(kTestSeed);

    ILGeneratorResult result1 = gen1.generate(config);
    ILGeneratorResult result2 = gen2.generate(config);

    if (result1.ilSource != result2.ilSource)
        std::cerr << "Same seed should produce identical IL source\n";
    ASSERT_EQ(result1.ilSource, result2.ilSource);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
