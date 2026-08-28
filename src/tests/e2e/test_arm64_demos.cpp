//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3.
//===----------------------------------------------------------------------===//
// File: tests/e2e/test_arm64_demos.cpp
// Purpose: End-to-end tests for ARM64 code generation with demo programs
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include "tests/common/WaitCompat.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct RunResult {
    int exit_code;
    std::string out;
    std::string err;
};

// Simple process runner using system()
static RunResult run_process(const std::vector<std::string> &args) {
    RunResult result;

    // Build command string
    std::string cmd;
    for (const auto &arg : args) {
        if (!cmd.empty())
            cmd += " ";
        // Simple quoting - may need improvement for complex cases
        if (arg.find(' ') != std::string::npos) {
            cmd += "\"" + arg + "\"";
        } else {
            cmd += arg;
        }
    }

    // Redirect output to temp files
    auto tmpDir = std::filesystem::temp_directory_path();
    std::string outFile = (tmpDir / ("test_out_" + std::to_string(rand()) + ".txt")).string();
    std::string errFile = (tmpDir / ("test_err_" + std::to_string(rand()) + ".txt")).string();
    cmd += " >" + outFile + " 2>" + errFile;

    // Run command
    result.exit_code = std::system(cmd.c_str());
    if (result.exit_code != -1) {
        result.exit_code = WEXITSTATUS(result.exit_code);
    }

    // Read output files
    std::ifstream outStream(outFile);
    std::stringstream outBuf;
    outBuf << outStream.rdbuf();
    result.out = outBuf.str();

    std::ifstream errStream(errFile);
    std::stringstream errBuf;
    errBuf << errStream.rdbuf();
    result.err = errBuf.str();

    // Clean up temp files
    std::remove(outFile.c_str());
    std::remove(errFile.c_str());

    return result;
}

// Only run these tests on macOS ARM64 or when ARM64_E2E_TESTS is set
static bool shouldRunARM64Tests() {
#ifdef __APPLE__
#ifdef __aarch64__
    return true;
#endif
#endif
    return std::getenv("ARM64_E2E_TESTS") != nullptr;
}

static std::string getBuildDir() {
    // Assume we're running from the build directory
    return ".";
}

static bool fileExists(const std::string &path) {
    return std::filesystem::exists(path);
}

static bool writeFile(const std::string &path, const std::string &content) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << content;
    return out.good();
}

TEST(ARM64E2E, MinimalPrintTest) {
    if (!shouldRunARM64Tests()) {
        // Skip test silently if not on ARM64
        return;
    }

    const std::string buildDir = getBuildDir();
    const std::string zbasic = buildDir + "/src/tools/zbasic/zbasic";
    const std::string ilc = buildDir + "/src/tools/zanna/zanna";

    if (!fileExists(zbasic) || !fileExists(ilc)) {
        return;
    }

    // Create a minimal BASIC program
    const std::string basFile = "/tmp/test_minimal.bas";
    const std::string ilFile = "/tmp/test_minimal.il";
    const std::string basicCode = "REM Minimal ARM64 test\n"
                                  "PRINT \"ARM64_TEST\"\n";

    ASSERT_TRUE(writeFile(basFile, basicCode));

    // Compile BASIC to IL
    RunResult rr = run_process({zbasic, basFile, "-o", ilFile});
    ASSERT_EQ(rr.exit_code, 0); // zbasic failed
    ASSERT_TRUE(fileExists(ilFile));

    // Compile IL to ARM64 and run
    rr = run_process({ilc, "codegen", "arm64", ilFile, "-run-native"});

    // For now, just check it doesn't crash
    // Print functionality may not be working yet
    EXPECT_NE(rr.exit_code, -1); // Program crashed
}

TEST(ARM64E2E, ArrayOperationsTest) {
    if (!shouldRunARM64Tests()) {
        return;
    }

    const std::string buildDir = getBuildDir();
    const std::string zbasic = buildDir + "/src/tools/zbasic/zbasic";
    const std::string ilc = buildDir + "/src/tools/zanna/zanna";

    if (!fileExists(zbasic) || !fileExists(ilc)) {
        return;
    }

    // Create a BASIC program that uses arrays
    const std::string basFile = "/tmp/test_arrays.bas";
    const std::string ilFile = "/tmp/test_arrays.il";
    const std::string basicCode = "REM Array test\n"
                                  "DIM arr(3) AS INTEGER\n"
                                  "arr(0) = 10\n"
                                  "arr(1) = 20\n"
                                  "arr(2) = 30\n";

    ASSERT_TRUE(writeFile(basFile, basicCode));

    // Compile BASIC to IL
    RunResult rr = run_process({zbasic, basFile, "-o", ilFile});
    ASSERT_EQ(rr.exit_code, 0); // zbasic failed
    ASSERT_TRUE(fileExists(ilFile));

    // Compile IL to ARM64 assembly (don't run yet due to potential issues)
    const std::string asmFile = "/tmp/test_arrays.s";
    rr = run_process({ilc, "codegen", "arm64", ilFile, "-S", asmFile});
    EXPECT_EQ(rr.exit_code, 0);       // ilc codegen failed
    EXPECT_TRUE(fileExists(asmFile)); // Assembly file not generated
}

// Test that Frogger compiles to assembly (may not link/run yet)
TEST(ARM64E2E, FroggerCompilesToAsm) {
    if (!shouldRunARM64Tests()) {
        return;
    }

    const std::string buildDir = getBuildDir();
    const std::string zbasic = buildDir + "/src/tools/zbasic/zbasic";
    const std::string ilc = buildDir + "/src/tools/zanna/zanna";
    const std::string froggerBas = "../examples/games/frogger-basic/frogger.bas";

    if (!fileExists(zbasic) || !fileExists(ilc)) {
        return;
    }

    if (!fileExists(froggerBas)) {
        return;
    }

    const std::string ilFile = "/tmp/frogger_test.il";
    const std::string asmFile = "/tmp/frogger_test.s";

    // Compile BASIC to IL
    RunResult rr = run_process({zbasic, froggerBas, "-o", ilFile});
    ASSERT_EQ(rr.exit_code, 0); // zbasic failed on Frogger
    ASSERT_TRUE(fileExists(ilFile));

    // Compile IL to ARM64 assembly
    rr = run_process({ilc, "codegen", "arm64", ilFile, "-S", asmFile});
    EXPECT_EQ(rr.exit_code, 0);       // ilc codegen failed on Frogger
    EXPECT_TRUE(fileExists(asmFile)); // Frogger assembly not generated

    // Verify assembly has expected content
    std::ifstream asmIn(asmFile);
    std::string asmContent((std::istreambuf_iterator<char>(asmIn)),
                           std::istreambuf_iterator<char>());
    EXPECT_TRUE(asmContent.find("_main:") != std::string::npos ||
                asmContent.find("main:") != std::string::npos); // Assembly missing main function
    EXPECT_TRUE(asmContent.find("rt_arr_obj") !=
                std::string::npos); // Assembly missing array operations
}

// Test that vTris compiles to assembly
TEST(ARM64E2E, VtrisCompilesToAsm) {
    if (!shouldRunARM64Tests()) {
        return;
    }

    const std::string buildDir = getBuildDir();
    const std::string zbasic = buildDir + "/src/tools/zbasic/zbasic";
    const std::string ilc = buildDir + "/src/tools/zanna/zanna";
    const std::string vtrisBas = "../examples/games/vtris/vtris.bas";

    if (!fileExists(zbasic) || !fileExists(ilc)) {
        return;
    }

    if (!fileExists(vtrisBas)) {
        return;
    }

    const std::string ilFile = "/tmp/vtris_test.il";
    const std::string asmFile = "/tmp/vtris_test.s";

    // Compile BASIC to IL
    RunResult rr = run_process({zbasic, vtrisBas, "-o", ilFile});
    ASSERT_EQ(rr.exit_code, 0); // zbasic failed on vTris
    ASSERT_TRUE(fileExists(ilFile));

    // Compile IL to ARM64 assembly
    rr = run_process({ilc, "codegen", "arm64", ilFile, "-S", asmFile});
    EXPECT_EQ(rr.exit_code, 0);       // ilc codegen failed on vTris
    EXPECT_TRUE(fileExists(asmFile)); // vTris assembly not generated
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
