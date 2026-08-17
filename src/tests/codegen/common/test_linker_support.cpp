//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/common/test_linker_support.cpp
// Purpose: Verify linker archive discovery, dependency closure, and frontend
//          bridge selection across build-tree and installed layouts.
// Key invariants:
//   - Runtime component closure includes every archive dependency.
//   - Language-service symbols select their strong frontend bridge archives.
// Ownership/Lifetime: Tests own all temporary directories and environment edits.
// Links: codegen/common/LinkerSupport.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/LinkerSupport.hpp"
#include "tests/TestHarness.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace zanna::codegen::common;

namespace {

void setEnvVar(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unsetEnvVar(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct ScopedEnvVar {
    explicit ScopedEnvVar(const char *envName) : name(envName) {
        if (const char *existing = std::getenv(name)) {
            hadOldValue = true;
            oldValue = existing;
        }
    }

    ~ScopedEnvVar() {
        if (hadOldValue)
            setEnvVar(name, oldValue);
        else
            unsetEnvVar(name);
    }

    const char *name;
    bool hadOldValue = false;
    std::string oldValue;
};

#if defined(_WIN32)
std::string archiveFileName(std::string_view base) {
    return std::string(base) + ".lib";
}
#else
std::string archiveFileName(std::string_view base) {
    return "lib" + std::string(base) + ".a";
}
#endif

void writeArchive(const std::filesystem::path &path, const char *contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

bool containsComponent(const LinkContext &ctx, zanna::codegen::RtComponent component) {
    return std::find(ctx.requiredComponents.begin(), ctx.requiredComponents.end(), component) !=
           ctx.requiredComponents.end();
}

} // namespace

TEST(LinkerSupport, InstalledLibDirViaEnvVar) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_linker_support_env";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    {
        std::ofstream out(tmpRoot / archiveFileName("zanna_rt_base"), std::ios::binary);
        out << "ar";
    }
    {
        std::ofstream out(tmpRoot / archiveFileName("zannagfx"), std::ios::binary);
        out << "gfx";
    }

    const char *var = "ZANNA_LIB_PATH";
    ScopedEnvVar scopedEnv(var);
    setEnvVar(var, tmpRoot.string());

    const auto found = findInstalledLibDir();
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->lexically_normal(), tmpRoot.lexically_normal());
    EXPECT_EQ(runtimeArchivePath({}, "zanna_rt_base").lexically_normal(),
              (tmpRoot / archiveFileName("zanna_rt_base")).lexically_normal());
    EXPECT_EQ(supportLibraryPath({}, "zannagfx").lexically_normal(),
              (tmpRoot / archiveFileName("zannagfx")).lexically_normal());

    fs::remove_all(tmpRoot);
}

TEST(LinkerSupport, InstalledLayoutPreferredOverBuildTree) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_linker_support_prefer_installed";
    const fs::path installedDir = tmpRoot / "installed";
    const fs::path buildDir = tmpRoot / "build";

    fs::remove_all(tmpRoot);
    fs::create_directories(installedDir);
    fs::create_directories(buildDir);

    writeArchive(installedDir / archiveFileName("zanna_rt_base"), "installed-runtime");
    writeArchive(installedDir / archiveFileName("zannagfx"), "installed-gfx");

#if defined(_WIN32)
    writeArchive(buildDir / "src/runtime/Debug" / archiveFileName("zanna_rt_base"),
                 "build-runtime");
    writeArchive(buildDir / "lib/Debug" / archiveFileName("zannagfx"), "build-gfx");
#else
    writeArchive(buildDir / "src/runtime" / archiveFileName("zanna_rt_base"), "build-runtime");
    writeArchive(buildDir / "lib" / archiveFileName("zannagfx"), "build-gfx");
#endif

    const char *var = "ZANNA_LIB_PATH";
    ScopedEnvVar scopedEnv(var);
    setEnvVar(var, installedDir.string());

    EXPECT_EQ(runtimeArchivePath(buildDir, "zanna_rt_base").lexically_normal(),
              (installedDir / archiveFileName("zanna_rt_base")).lexically_normal());
    EXPECT_EQ(supportLibraryPath(buildDir, "zannagfx").lexically_normal(),
              (installedDir / archiveFileName("zannagfx")).lexically_normal());

    fs::remove_all(tmpRoot);
}

TEST(LinkerSupport, BuildDirViaEnvVar) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_linker_support_build_dir";

    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::ofstream out(tmpRoot / "CMakeCache.txt", std::ios::binary);
        out << "# test cache\n";
    }

    const char *var = "ZANNA_BUILD_DIR";
    ScopedEnvVar scopedEnv(var);
    setEnvVar(var, tmpRoot.string());

    const auto found = findBuildDir();
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->lexically_normal(), tmpRoot.lexically_normal());

    fs::remove_all(tmpRoot);
}

TEST(LinkerSupport, SharedRegexEngineUsesCommonTextBuildDirectory) {
    namespace fs = std::filesystem;
    const fs::path buildDir = fs::path("build") / "linker-support-layout";
    const fs::path expected =
        buildDir / "src" / "common" / "text" / archiveFileName("zanna_regex_engine");

    EXPECT_EQ(supportLibraryPath(buildDir, "zanna_regex_engine").lexically_normal(),
              expected.lexically_normal());
}

TEST(LinkerSupport, RuntimeAndGuiComponentsRequireSharedRegexEngine) {
    LinkContext ctx;
    EXPECT_FALSE(requiresRegexEngineArchive(ctx));

    ctx.requiredComponents.push_back(zanna::codegen::RtComponent::Base);
    EXPECT_TRUE(requiresRegexEngineArchive(ctx));
    ctx.requiredComponents.clear();

    ctx.requiredComponents.push_back(zanna::codegen::RtComponent::Text);
    EXPECT_TRUE(requiresRegexEngineArchive(ctx));
    ctx.requiredComponents.clear();

    ctx.requiredComponents.push_back(zanna::codegen::RtComponent::Graphics);
    EXPECT_TRUE(requiresRegexEngineArchive(ctx));
}

TEST(LinkerSupport, ArchiveClosureAddsTextForBaseStringIntern) {
    LinkContext ctx;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(0, prepareLinkContextFromSymbols({"rt_string_intern"}, ctx, out, err));
    ASSERT_TRUE(err.str().empty());

    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Base));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Text));
}

TEST(LinkerSupport, ArchiveClosureAddsIoFsForBaseChannelHelpers) {
    LinkContext ctx;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(0, prepareLinkContextFromSymbols({"rt_eof_ch"}, ctx, out, err));
    ASSERT_TRUE(err.str().empty());

    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Base));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::IoFs));
}

TEST(LinkerSupport, ArchiveClosureAddsTextAndIoFsForGraphicsRuntimeDeps) {
    LinkContext ctx;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(
        0, prepareLinkContextFromSymbols({"rt_action_load", "rt_pixels_load_png"}, ctx, out, err));
    ASSERT_TRUE(err.str().empty());

    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Graphics));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Text));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::IoFs));
}

TEST(LinkerSupport, ArchiveClosureFollowsInternalCanvas3DGameUiAdapter) {
    LinkContext ctx;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(0, prepareLinkContextFromSymbols({"rt_canvas3d_new"}, ctx, out, err));
    ASSERT_TRUE(err.str().empty());

    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Base));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Graphics));
    EXPECT_TRUE(containsComponent(ctx, zanna::codegen::RtComponent::Game));
}

TEST(LinkerSupport, BasicLanguageServiceSelectsStrongFrontendBridge) {
    LinkContext ctx;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(
        0, prepareLinkContextFromSymbols({"rt_basic_completion_symbols_for_file"}, ctx, out, err));
    ASSERT_TRUE(err.str().empty());

    EXPECT_TRUE(ctx.needsBasicFrontend);
    EXPECT_FALSE(ctx.needsZiaFrontend);
    EXPECT_TRUE(fileExists(supportLibraryPath(ctx.buildDir, "fe_basic")));
    EXPECT_FALSE(basicFrontendClosureLibs().empty());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
