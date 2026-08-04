//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "tests/TestHarness.hpp"
#include "tools/common/asset/AssetCompiler.hpp"
#include "tools/common/asset/ZpakWriter.hpp"
#include "tools/common/project_loader.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

using il::tools::common::ProjectConfig;

namespace {

fs::path makeTempRoot(const fs::path &name) {
    fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

void writeText(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST(AssetCompiler, RejectsSourcePathTraversal) {
    fs::path root = makeTempRoot("zanna_asset_compiler_escape_project");
    fs::path outside = root.parent_path() / "zanna_asset_compiler_outside.txt";
    writeText(outside, "outside");

    ProjectConfig config;
    config.name = "escape";
    config.rootDir = zanna::filesystem::pathToUtf8(root);
    config.embedAssets.push_back({"../zanna_asset_compiler_outside.txt"});

    std::string err;
    auto bundle = zanna::asset::compileAssets(config, zanna::filesystem::pathToUtf8(root), err);
    EXPECT_TRUE(!bundle.has_value());
    EXPECT_TRUE(err.find("must not contain") != std::string::npos ||
                err.find("escapes") != std::string::npos);

    std::error_code ec;
    fs::remove(outside, ec);
    fs::remove_all(root, ec);
}

TEST(AssetCompiler, SanitizesPackOutputName) {
    fs::path root = makeTempRoot("zanna_asset_compiler_pack_project");
    fs::path output = root / "out";
    writeText(root / "assets" / "file.txt", "payload");
    fs::create_directories(output);

    ProjectConfig config;
    config.name = "My App";
    config.rootDir = zanna::filesystem::pathToUtf8(root);
    config.packGroups.push_back({"Data Pack", {"assets"}, false});

    std::string err;
    auto bundle = zanna::asset::compileAssets(config, zanna::filesystem::pathToUtf8(output), err);
    ASSERT_TRUE(bundle.has_value());
    ASSERT_EQ(bundle->packFilePaths.size(), static_cast<size_t>(1));
    const fs::path packPath = zanna::filesystem::pathFromUtf8(bundle->packFilePaths[0]);
    EXPECT_EQ(zanna::filesystem::pathToUtf8(packPath.filename()), "my_app-data_pack.zpak");
    EXPECT_TRUE(fs::exists(packPath));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(AssetCompiler, CrackmanRequiredAssetsArePresent) {
#if defined(ZANNA_SOURCE_DIR)
    fs::path output = makeTempRoot("zanna_crackman_asset_compiler");
    auto project = il::tools::common::resolveProject(zanna::filesystem::pathToUtf8(
        fs::path(ZANNA_SOURCE_DIR) / "examples" / "games" / "crackman"));
    ASSERT_TRUE(project);

    std::string err;
    auto bundle =
        zanna::asset::compileAssets(project.value(), zanna::filesystem::pathToUtf8(output), err);
    EXPECT_TRUE(bundle.has_value());
    EXPECT_EQ(err, std::string());

    std::error_code ec;
    fs::remove_all(output, ec);
#endif
}

#if ZANNA_HOST_WINDOWS
TEST(AssetCompiler, SupportsUnicodeProjectAssetsAndOutput) {
    const fs::path root = makeTempRoot(L"zanna_asset_\u6771\u4eac_\u03b1");
    const fs::path output = root / L"\u51fa\u529b";
    const fs::path asset = root / L"\u7d20\u6750" / L"\u732b.txt";
    writeText(asset, "unicode asset payload");
    fs::create_directories(output);

    ProjectConfig config;
    config.name = "Unicode App";
    config.rootDir = zanna::filesystem::pathToUtf8(root);
    config.packGroups.push_back(
        {"Data", {zanna::filesystem::genericPathToUtf8(asset.lexically_relative(root))}, false});

    std::string err;
    const auto bundle =
        zanna::asset::compileAssets(config, zanna::filesystem::pathToUtf8(output), err);
    ASSERT_TRUE(bundle.has_value());
    ASSERT_EQ(bundle->packFilePaths.size(), static_cast<size_t>(1));
    EXPECT_TRUE(fs::exists(zanna::filesystem::pathFromUtf8(bundle->packFilePaths.front())));
    EXPECT_EQ(err, std::string());

    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

TEST(ZpakWriter, RejectsDuplicateEntries) {
    const uint8_t data[] = {'x'};
    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/a.txt", data, sizeof(data), false);
    EXPECT_THROWS(writer.addEntry("assets/a.txt", data, sizeof(data), false),
                  std::invalid_argument);
}

TEST(ZpakWriter, RejectsUnsafeNames) {
    const uint8_t data[] = {'x'};
    zanna::asset::ZpakWriter writer;
    EXPECT_THROWS(writer.addEntry("../escape.txt", data, sizeof(data), false),
                  std::invalid_argument);
    EXPECT_THROWS(writer.addEntry("assets\\a.txt", data, sizeof(data), false),
                  std::invalid_argument);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
