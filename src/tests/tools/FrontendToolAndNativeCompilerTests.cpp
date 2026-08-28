//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/tools/FrontendToolAndNativeCompilerTests.cpp
// Purpose: Regress CLI/frontend parsing and native compiler temp-path behavior.
// Key invariants:
//   - Command-line parsing keeps compiler and forwarded program arguments distinct.
//   - Project and temporary path strings round-trip as UTF-8 on every host.
// Ownership/Lifetime: Tests own and remove every temporary project tree they create.
// Links: src/tools/common/frontend_tool.hpp, src/tools/common/project_loader.cpp
//
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "tools/common/frontend_tool.hpp"
#include "tools/common/native_compiler.hpp"
#include "tools/common/project_loader.hpp"
#include "tools/zanna/cmd_codegen_x64.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void noopUsage() {}

void noopVersion() {}

int noopFrontend(int, char **) {
    return 0;
}

std::filesystem::path takeTempProjectDirectoryPath() {
    const std::filesystem::path reserved =
        zanna::filesystem::pathFromUtf8(zanna::tools::generateTempIlPath());
    std::error_code ec;
    std::filesystem::remove(reserved, ec);
    assert(!ec);
    std::filesystem::path directory = reserved;
    directory.replace_extension("");
    return directory;
}

void testFrontendDoubleDashSeparatesProgramArgs() {
    using namespace zanna::tools;

    FrontendToolCallbacks callbacks{
        ".bas",
        "BASIC",
        noopUsage,
        noopVersion,
        noopFrontend,
    };

    char exe[] = "zbasic";
    char src[] = "demo.bas";
    char sep[] = "--";
    char flag[] = "--trace";
    char value[] = "runtime-arg";
    char *argv[] = {exe, src, sep, flag, value};

    const FrontendToolConfig config = parseArgs(5, argv, callbacks);
    assert(config.sourcePath == "demo.bas");
    assert(config.run);
    assert(config.forwardedArgs.empty());
    assert(config.programArgs.size() == 2);
    assert(config.programArgs[0] == "--trace");
    assert(config.programArgs[1] == "runtime-arg");
}

void testNativeCompilerTempPathsAreUnique() {
    using namespace zanna::tools;

    const std::string ilA = generateTempIlPath();
    const std::string ilB = generateTempIlPath();
    const std::string assetA = generateTempAssetPath();
    const std::string assetB = generateTempAssetPath();

    assert(ilA != ilB);
    assert(assetA != assetB);
    assert(zanna::filesystem::pathFromUtf8(ilA).extension() == ".il");
    assert(zanna::filesystem::pathFromUtf8(assetA).extension() == ".zpak");
    std::error_code ec;
    assert(std::filesystem::remove(zanna::filesystem::pathFromUtf8(ilA), ec) && !ec);
    ec.clear();
    assert(std::filesystem::remove(zanna::filesystem::pathFromUtf8(ilB), ec) && !ec);
    ec.clear();
    assert(std::filesystem::remove(zanna::filesystem::pathFromUtf8(assetA), ec) && !ec);
    ec.clear();
    assert(std::filesystem::remove(zanna::filesystem::pathFromUtf8(assetB), ec) && !ec);
}

/// @brief Verify IL output detection is case-insensitive.
/// @details Filesystems and editor save dialogs often preserve uppercase
///          extensions. The frontends should treat `-o OUT.IL` the same as
///          `-o out.il` and avoid accidentally invoking native code generation.
void testNativeCompilerIlExtensionIsCaseInsensitive() {
    using namespace zanna::tools;

    assert(!isNativeOutputPath("out.il"));
    assert(!isNativeOutputPath("OUT.IL"));
    assert(!isNativeOutputPath("module.Il"));
    assert(isNativeOutputPath("app"));
    assert(isNativeOutputPath("app.exe"));
}

void testX64CodegenAcceptsAssetBlobAndExtraObjectFlags() {
    char input[] = "missing.il";
    char assetFlag[] = "--asset-blob";
    char assetPath[] = "assets.zpak";
    char objFlag[] = "--extra-obj";
    char objPath[] = "assets.o";
    char *argv[] = {input, assetFlag, assetPath, objFlag, objPath};

    std::ostringstream errCapture;
    auto *oldErr = std::cerr.rdbuf(errCapture.rdbuf());
    const int rc = zanna::tools::ilc::cmd_codegen_x64(5, argv);
    std::cerr.rdbuf(oldErr);

    const std::string diagnostics = errCapture.str();
    assert(rc != 0);
    assert(diagnostics.find("unknown flag") == std::string::npos);
    assert(diagnostics.find("--asset-blob requires") == std::string::npos);
    assert(diagnostics.find("--extra-obj requires") == std::string::npos);
}

void testX64CodegenAcceptsDebugLineFlags() {
    char input[] = "missing.il";
    char debugLines[] = "--debug-lines";
    char noDebugLines[] = "--no-debug-lines";
    char *argv[] = {input, debugLines, noDebugLines};

    std::ostringstream errCapture;
    auto *oldErr = std::cerr.rdbuf(errCapture.rdbuf());
    const int rc = zanna::tools::ilc::cmd_codegen_x64(3, argv);
    std::cerr.rdbuf(oldErr);

    const std::string diagnostics = errCapture.str();
    assert(rc != 0);
    assert(diagnostics.find("unknown flag") == std::string::npos);
}

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path);
    assert(out.is_open());
    out << text;
}

void testProjectDefaultsUseBalancedOptimization() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    std::filesystem::create_directories(dir);
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");

    auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(resolved.value().buildProfile == "balanced");
    assert(resolved.value().optimizeLevel == "O1");
    assert(!resolved.value().buildProfileExplicit);
    assert(!resolved.value().optimizeLevelExplicit);

    std::filesystem::remove_all(dir);
}

void testProjectProfilesMapToOptimizationLevels() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    std::filesystem::create_directories(dir);
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");
    writeText(dir / "zanna.project",
              "project perf\n"
              "version 0.1.0\n"
              "lang zia\n"
              "entry main.zia\n"
              "profile release\n");

    auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(resolved.value().buildProfile == "release");
    assert(resolved.value().optimizeLevel == "O2");
    assert(resolved.value().buildProfileExplicit);
    assert(!resolved.value().optimizeLevelExplicit);

    writeText(dir / "zanna.project",
              "project perf\n"
              "version 0.1.0\n"
              "lang zia\n"
              "entry main.zia\n"
              "profile release\n"
              "optimize O1\n");
    auto overridden = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(overridden);
    assert(overridden.value().buildProfile == "release");
    assert(overridden.value().optimizeLevel == "O1");
    assert(overridden.value().buildProfileExplicit);
    assert(overridden.value().optimizeLevelExplicit);

    std::filesystem::remove_all(dir);
}

/// @brief Verify manifest booleans accept numeric forms.
/// @details CLI on/off parsing accepts `1` and `0`; project manifests should
///          accept the same values for boolean build and packaging directives so
///          users do not hit inconsistent configuration grammars.
void testProjectManifestBooleanNumericForms() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    std::filesystem::create_directories(dir);
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");
    writeText(dir / "zanna.project",
              "project boolproj\n"
              "version 0.1.0\n"
              "lang zia\n"
              "entry main.zia\n"
              "bounds-checks 0\n"
              "overflow-checks 1\n"
              "null-checks 0\n"
              "shortcut-menu 1\n"
              "shortcut-desktop 0\n");

    auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(!resolved.value().boundsChecks);
    assert(resolved.value().overflowChecks);
    assert(!resolved.value().nullChecks);
    assert(resolved.value().packageConfig.shortcutMenu);
    assert(!resolved.value().packageConfig.shortcutDesktop);

    std::filesystem::remove_all(dir);
}

void testProjectManifestAcceptsUtf8Bom() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    std::filesystem::create_directories(dir);
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");
    writeText(dir / "zanna.project",
              "\xEF\xBB\xBF"
              "project bomproj\n"
              "version 0.1.0\n"
              "lang zia\n"
              "entry main.zia\n");

    auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(resolved.value().name == "bomproj");

    std::filesystem::remove_all(dir);
}

void testConventionDiscoverySkipsGeneratedAndVendorTrees() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    std::filesystem::create_directories(dir / "build");
    std::filesystem::create_directories(dir / "vendor");
    std::filesystem::create_directories(dir / "node_modules" / "pkg");
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");
    writeText(dir / "build" / "ignored.zia", "module ignored;\nfunc helper() {}\n");
    writeText(dir / "vendor" / "ignored.zia", "module ignored_vendor;\nfunc helper() {}\n");
    writeText(dir / "node_modules" / "pkg" / "ignored.zia",
              "module ignored_node;\nfunc helper() {}\n");

    auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(resolved.value().sourceFiles.size() == 1);
    assert(zanna::filesystem::pathFromUtf8(resolved.value().sourceFiles.front()).filename() ==
           "main.zia");
    assert(std::find_if(resolved.value().sourceFiles.begin(),
                        resolved.value().sourceFiles.end(),
                        [](const std::string &path) {
                            return path.find("ignored.zia") != std::string::npos;
                        }) == resolved.value().sourceFiles.end());

    std::filesystem::remove_all(dir);
}

void testProjectLoaderPreservesUtf8NativePaths() {
    using namespace il::tools::common;
    using namespace zanna::tools;

    std::filesystem::path dir = takeTempProjectDirectoryPath();
    dir += zanna::filesystem::pathFromUtf8("_\xE6\x9D\xB1\xE4\xBA\xAC").native();
    const std::string sourceName = "\xE5\x85\xA5\xE5\x8F\xA3.zia";
    const std::filesystem::path source = dir / zanna::filesystem::pathFromUtf8(sourceName);
    std::filesystem::create_directories(dir);
    writeText(source, "module unicode_project;\nfunc start() {}\n");
    writeText(dir / "zanna.project",
              "project unicode_project\n"
              "version 0.1.0\n"
              "lang zia\n"
              "entry " +
                  sourceName + "\n");

    const auto resolved = resolveProject(zanna::filesystem::pathToUtf8(dir));
    assert(resolved);
    assert(resolved.value().rootDir ==
           zanna::filesystem::pathToUtf8(std::filesystem::canonical(dir)));
    assert(resolved.value().entryFile ==
           zanna::filesystem::pathToUtf8(std::filesystem::canonical(source)));
    assert(resolved.value().sourceFiles.size() == 1);
    assert(resolved.value().sourceFiles.front() == resolved.value().entryFile);

    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    testFrontendDoubleDashSeparatesProgramArgs();
    testNativeCompilerTempPathsAreUnique();
    testNativeCompilerIlExtensionIsCaseInsensitive();
    testX64CodegenAcceptsAssetBlobAndExtraObjectFlags();
    testX64CodegenAcceptsDebugLineFlags();
    testProjectDefaultsUseBalancedOptimization();
    testProjectProfilesMapToOptimizationLevels();
    testProjectManifestBooleanNumericForms();
    testProjectManifestAcceptsUtf8Bom();
    testConventionDiscoverySkipsGeneratedAndVendorTrees();
    testProjectLoaderPreservesUtf8NativePaths();
    return 0;
}
