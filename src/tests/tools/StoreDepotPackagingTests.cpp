//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/tools/StoreDepotPackagingTests.cpp
// Purpose: Unit tests for store depot packaging (ADR 0354): native binary
//          inspection, macOS entitlement merging, Steam directive rules and
//          manifest parsing, depot planning diagnostics, and depot staging
//          with manifests and SteamPipe build scripts.
// Key invariants:
//   - Binaries are synthesized headers plus NUL-terminated export names, so
//     every test runs on every host without Valve files.
//   - Each test works in its own temporary tree and removes it.
// Ownership/Lifetime:
//   - Self-contained test binary.
// Links: src/tools/common/packaging/StoreDepotBuilder.hpp,
//        src/tools/common/packaging/NativeBinaryInspector.hpp,
//        src/tools/common/packaging/MacOSEntitlements.hpp,
//        docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "tests/TestHarness.hpp"
#include "tools/common/packaging/MacOSEntitlements.hpp"
#include "tools/common/packaging/NativeBinaryInspector.hpp"
#include "tools/common/packaging/PkgHash.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "tools/common/packaging/StoreDepotBuilder.hpp"
#include "tools/common/project_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace zanna::pkg;

namespace {

/// @brief ELF machine number of x86-64.
constexpr uint16_t kElfX64 = 62;
/// @brief ELF machine number of AArch64.
constexpr uint16_t kElfArm64 = 183;
/// @brief Mach-O CPU type of x86-64.
constexpr uint32_t kMachX64 = 0x01000007u;
/// @brief Mach-O CPU type of arm64.
constexpr uint32_t kMachArm64 = 0x0100000Cu;

/// @brief Export names every supported Steamworks redistributable carries.
const std::vector<std::string> &steamCoreExports() {
    return storeProfile(StoreKind::Steam).requiredExports;
}

/// @brief Store a little-endian 16-bit value.
void put16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
    data[offset] = static_cast<uint8_t>(value & 0xFF);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

/// @brief Store a little-endian 32-bit value.
void put32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
}

/// @brief Store a big-endian 32-bit value.
void putBE32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * (3 - i)));
}

/// @brief Append NUL-terminated names to a binary.
void appendNames(std::vector<uint8_t> &data, const std::vector<std::string> &names) {
    for (const auto &name : names) {
        data.insert(data.end(), name.begin(), name.end());
        data.push_back(0);
    }
}

/// @brief Synthesize a 64-bit little-endian ELF header.
/// @param machine e_machine value.
/// @param type e_type value (2 executable, 3 shared object).
/// @param names Strings appended after the header.
std::vector<uint8_t> makeElf(uint16_t machine,
                             uint16_t type,
                             const std::vector<std::string> &names) {
    std::vector<uint8_t> data(64, 0);
    data[0] = 0x7F;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 2;
    data[5] = 1;
    data[6] = 1;
    put16(data, 16, type);
    put16(data, 18, machine);
    appendNames(data, names);
    return data;
}

/// @brief Synthesize a PE32+ image without imports.
/// @param machine COFF machine (0x8664 or 0xAA64).
/// @param dll Whether IMAGE_FILE_DLL is set.
/// @param names Strings appended after the headers.
std::vector<uint8_t> makePe(uint16_t machine, bool dll, const std::vector<std::string> &names) {
    constexpr size_t peOffset = 0x80;
    std::vector<uint8_t> data(peOffset + 4 + 20 + 240, 0);
    data[0] = 'M';
    data[1] = 'Z';
    put32(data, 0x3C, static_cast<uint32_t>(peOffset));
    data[peOffset] = 'P';
    data[peOffset + 1] = 'E';
    put16(data, peOffset + 4, machine);
    put16(data, peOffset + 4 + 16, 240);
    put16(data, peOffset + 4 + 18, static_cast<uint16_t>(dll ? 0x2022 : 0x0022));
    put16(data, peOffset + 24, 0x20B);
    appendNames(data, names);
    return data;
}

/// @brief Synthesize a thin 64-bit Mach-O header.
/// @param cputype CPU type.
/// @param filetype 2 for an executable, 6 for a dylib.
/// @param names Strings appended after the header.
std::vector<uint8_t> makeMachO(uint32_t cputype,
                               uint32_t filetype,
                               const std::vector<std::string> &names) {
    std::vector<uint8_t> data(32, 0);
    put32(data, 0, 0xFEEDFACFu);
    put32(data, 4, cputype);
    put32(data, 12, filetype);
    appendNames(data, names);
    return data;
}

/// @brief Synthesize a universal Mach-O from thin slices.
/// @param slices Thin Mach-O files.
/// @param names Strings appended after the last slice.
std::vector<uint8_t> makeFatMachO(const std::vector<std::vector<uint8_t>> &slices,
                                  const std::vector<std::string> &names) {
    std::vector<uint8_t> data(8 + 20 * slices.size(), 0);
    putBE32(data, 0, 0xCAFEBABEu);
    putBE32(data, 4, static_cast<uint32_t>(slices.size()));
    for (size_t i = 0; i < slices.size(); ++i) {
        while (data.size() % 16 != 0)
            data.push_back(0);
        const size_t entry = 8 + 20 * i;
        const uint32_t cputype = static_cast<uint32_t>(slices[i][4]) |
                                 (static_cast<uint32_t>(slices[i][5]) << 8) |
                                 (static_cast<uint32_t>(slices[i][6]) << 16) |
                                 (static_cast<uint32_t>(slices[i][7]) << 24);
        putBE32(data, entry, cputype);
        putBE32(data, entry + 8, static_cast<uint32_t>(data.size()));
        putBE32(data, entry + 12, static_cast<uint32_t>(slices[i].size()));
        data.insert(data.end(), slices[i].begin(), slices[i].end());
    }
    appendNames(data, names);
    return data;
}

/// @brief Write bytes, creating parent directories.
void writeBytes(const fs::path &path, const std::vector<uint8_t> &data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

/// @brief Write text, creating parent directories.
void writeText(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// @brief Read a whole file as text.
std::string readText(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// @brief UTF-8 spelling of a path.
std::string utf8(const fs::path &path) {
    return zanna::filesystem::pathToUtf8(path);
}

/// @brief Run @p fn and return the message it threw, or empty.
std::string errorOf(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &ex) {
        return ex.what();
    }
    return {};
}

/// @brief A temporary directory tree removed on destruction.
struct TempTree {
    fs::path root; ///< Tree root.

    /// @brief Create a fresh tree.
    explicit TempTree(const std::string &name)
        : root(fs::temp_directory_path() / ("zanna_store_depot_" + name)) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "project");
    }

    /// @brief Remove the tree.
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

/// @brief Linux x64 depot parameters over a tree with an executable and redistributable.
/// @param tree Temporary tree.
/// @param usesProvider Whether the executable carries the provider marker.
StoreDepotParams linuxDepot(const TempTree &tree, bool usesProvider = true) {
    std::vector<std::string> exeNames = {"zanna_rt_main"};
    if (usesProvider)
        exeNames.push_back("SteamAPI_InitFlat");
    writeBytes(tree.root / "bin" / "depotapp", makeElf(kElfX64, 2, exeNames));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "linux64" / "libsteam_api.so",
               makeElf(kElfX64, 3, steamCoreExports()));
    StoreDepotParams params;
    params.store = StoreKind::Steam;
    params.os = DepotOs::Linux;
    params.arch = "x64";
    params.projectName = "DepotApp";
    params.version = "1.2.3";
    params.projectRoot = utf8(tree.root / "project");
    params.pkgConfig.steamAppId = "480";
    params.pkgConfig.steamDepots = {{"linux", "482"}};
    params.outputRoot = utf8(tree.root / "out");
    params.executablePath = utf8(tree.root / "bin" / "depotapp");
    params.redistSource = "sdk";
    params.redistOrigin = "steam-redist";
    params.redistBaseDir = utf8(tree.root);
    return params;
}

/// @brief Report whether a depot result lists a content path.
bool hasFile(const StoreDepotResult &result, const std::string &path) {
    return std::any_of(result.files.begin(), result.files.end(), [&](const StoreDepotFile &file) {
        return file.path == path;
    });
}

} // namespace

//===----------------------------------------------------------------------===//
// Native binary inspection
//===----------------------------------------------------------------------===//

TEST(NativeBinaryInspector, IdentifiesElfPeAndThinMachO) {
    const auto elfLib = inspectNativeBinary(makeElf(kElfX64, 3, {}));
    ASSERT_TRUE(elfLib.has_value());
    EXPECT_TRUE(elfLib->format == NativeBinaryFormat::ELF);
    EXPECT_TRUE(elfLib->kind == NativeBinaryKind::SharedLibrary);
    ASSERT_EQ(elfLib->architectures.size(), static_cast<size_t>(1));
    EXPECT_EQ(elfLib->architectures[0], "x64");

    const auto elfExe = inspectNativeBinary(makeElf(kElfArm64, 2, {}));
    ASSERT_TRUE(elfExe.has_value());
    EXPECT_TRUE(elfExe->kind == NativeBinaryKind::Executable);
    EXPECT_EQ(elfExe->architectures[0], "arm64");

    const auto peDll = inspectNativeBinary(makePe(0x8664, true, {}));
    ASSERT_TRUE(peDll.has_value());
    EXPECT_TRUE(peDll->format == NativeBinaryFormat::PE);
    EXPECT_TRUE(peDll->kind == NativeBinaryKind::SharedLibrary);
    EXPECT_TRUE(peDll->pe32Plus);
    EXPECT_EQ(peDll->architectures[0], "x64");

    const auto peExe = inspectNativeBinary(makePe(0xAA64, false, {}));
    ASSERT_TRUE(peExe.has_value());
    EXPECT_TRUE(peExe->kind == NativeBinaryKind::Executable);
    EXPECT_EQ(peExe->architectures[0], "arm64");

    const auto dylib = inspectNativeBinary(makeMachO(kMachArm64, 6, {}));
    ASSERT_TRUE(dylib.has_value());
    EXPECT_TRUE(dylib->format == NativeBinaryFormat::MachO);
    EXPECT_TRUE(dylib->kind == NativeBinaryKind::SharedLibrary);
    EXPECT_FALSE(dylib->universal);
    EXPECT_EQ(dylib->architectures[0], "arm64");

    const auto machExe = inspectNativeBinary(makeMachO(kMachX64, 2, {}));
    ASSERT_TRUE(machExe.has_value());
    EXPECT_TRUE(machExe->kind == NativeBinaryKind::Executable);
    EXPECT_EQ(machExe->architectures[0], "x64");
}

TEST(NativeBinaryInspector, IdentifiesUniversalMachO) {
    const auto fat = inspectNativeBinary(
        makeFatMachO({makeMachO(kMachX64, 6, {}), makeMachO(kMachArm64, 6, {})}, {}));
    ASSERT_TRUE(fat.has_value());
    EXPECT_TRUE(fat->format == NativeBinaryFormat::MachO);
    EXPECT_TRUE(fat->universal);
    EXPECT_TRUE(fat->kind == NativeBinaryKind::SharedLibrary);
    ASSERT_EQ(fat->architectures.size(), static_cast<size_t>(2));
    EXPECT_EQ(fat->architectures[0], "arm64");
    EXPECT_EQ(fat->architectures[1], "x64");

    const auto mixed = inspectNativeBinary(
        makeFatMachO({makeMachO(kMachX64, 2, {}), makeMachO(kMachArm64, 6, {})}, {}));
    ASSERT_TRUE(mixed.has_value());
    EXPECT_TRUE(mixed->kind == NativeBinaryKind::Unknown);
}

TEST(NativeBinaryInspector, RejectsMalformedHeaders) {
    std::vector<uint8_t> shortElf = makeElf(kElfX64, 3, {});
    shortElf.resize(40);
    EXPECT_FALSE(inspectNativeBinary(shortElf).has_value());

    std::vector<uint8_t> badPe = makePe(0x8664, true, {});
    put32(badPe, 0x3C, 0x7FFFFFF0u);
    EXPECT_FALSE(inspectNativeBinary(badPe).has_value());

    std::vector<uint8_t> fat =
        makeFatMachO({makeMachO(kMachX64, 6, {}), makeMachO(kMachArm64, 6, {})}, {});
    putBE32(fat, 8 + 20 + 8, 0x00FFFFF0u);
    EXPECT_FALSE(inspectNativeBinary(fat).has_value());

    std::vector<uint8_t> emptyFat(8, 0);
    putBE32(emptyFat, 0, 0xCAFEBABEu);
    EXPECT_FALSE(inspectNativeBinary(emptyFat).has_value());

    const std::string text = "#!/bin/sh\necho not a binary\n";
    EXPECT_FALSE(inspectNativeBinary(std::vector<uint8_t>(text.begin(), text.end())).has_value());
}

TEST(NativeBinaryInspector, FindsNulTerminatedSymbolNames) {
    const std::vector<uint8_t> machO =
        makeMachO(kMachArm64, 6, {"_SteamAPI_Shutdown", "SteamAPI_InitFlatExtra"});
    EXPECT_TRUE(binaryContainsSymbolName(machO, "SteamAPI_Shutdown"));
    EXPECT_FALSE(binaryContainsSymbolName(machO, "SteamAPI_InitFlat"));
    EXPECT_TRUE(binaryContainsSymbolName(machO, "SteamAPI_InitFlatExtra"));
    EXPECT_FALSE(binaryContainsSymbolName(machO, ""));
}

//===----------------------------------------------------------------------===//
// macOS entitlements
//===----------------------------------------------------------------------===//

namespace {

/// @brief Steam's required entitlement keys.
const std::vector<std::string> kRequired = {
    "com.apple.security.cs.disable-library-validation",
    "com.apple.security.cs.allow-dyld-environment-variables"};
/// @brief Steam's forbidden entitlement keys.
const std::vector<std::string> kForbidden = {"com.apple.security.app-sandbox"};

/// @brief Count occurrences of @p needle in @p haystack.
size_t countOf(const std::string &haystack, const std::string &needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

} // namespace

TEST(MacOSEntitlements, AddsRequiredKeysToEmptyInput) {
    const EntitlementsMergeResult merged =
        mergeMacOSEntitlements("", "(generated)", kRequired, kForbidden, "Steam");
    ASSERT_EQ(merged.addedKeys.size(), static_cast<size_t>(2));
    EXPECT_CONTAINS(merged.xml, "<plist version=\"1.0\">");
    EXPECT_CONTAINS(merged.xml,
                    "\t<key>com.apple.security.cs.disable-library-validation</key>\n\t<true/>\n");
    EXPECT_CONTAINS(merged.xml,
                    "\t<key>com.apple.security.cs.allow-dyld-environment-variables</key>\n"
                    "\t<true/>\n");

    const EntitlementsMergeResult again =
        mergeMacOSEntitlements(merged.xml, "merged.plist", kRequired, kForbidden, "Steam");
    EXPECT_TRUE(again.addedKeys.empty());
    EXPECT_EQ(again.xml, merged.xml);
}

TEST(MacOSEntitlements, PreservesExistingEntries) {
    const std::string source =
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<!-- project entitlements -->\n"
        "<dict>\n"
        "  <key>com.apple.security.network.client</key><true/>\n"
        "  <key>com.apple.security.cs.disable-library-validation</key>\n"
        "  <true></true>\n"
        "  <key>com.example.note</key><string>R&amp;D &#x263A;</string>\n"
        "  <key>com.example.groups</key><array><string>a</string><integer>7</integer></array>\n"
        "  <key>com.apple.security.app-sandbox</key><false/>\n"
        "</dict>\n"
        "</plist>\n";
    const EntitlementsMergeResult merged =
        mergeMacOSEntitlements(source, "game.entitlements", kRequired, kForbidden, "Steam");
    ASSERT_EQ(merged.addedKeys.size(), static_cast<size_t>(1));
    EXPECT_EQ(merged.addedKeys[0], "com.apple.security.cs.allow-dyld-environment-variables");
    EXPECT_EQ(countOf(merged.xml, "com.apple.security.cs.disable-library-validation"),
              static_cast<size_t>(1));
    EXPECT_CONTAINS(merged.xml, "<key>com.apple.security.network.client</key>");
    EXPECT_CONTAINS(merged.xml, "<string>R&amp;D \xE2\x98\xBA</string>");
    EXPECT_CONTAINS(merged.xml, "\t\t<integer>7</integer>\n");
    EXPECT_CONTAINS(merged.xml, "<key>com.apple.security.app-sandbox</key>\n\t<false/>");
}

TEST(MacOSEntitlements, RejectsConflictsAndSandbox) {
    const std::string falseValue =
        "<plist><dict><key>com.apple.security.cs.disable-library-validation</key><false/>"
        "</dict></plist>";
    EXPECT_EQ(errorOf([&] {
                  mergeMacOSEntitlements(falseValue, "game.plist", kRequired, kForbidden, "Steam");
              }),
              "macOS entitlements 'game.plist' set "
              "com.apple.security.cs.disable-library-validation to false, but Steam requires true");

    const std::string stringValue =
        "<plist><dict><key>com.apple.security.cs.allow-dyld-environment-variables</key>"
        "<string>yes</string></dict></plist>";
    EXPECT_EQ(errorOf([&] {
                  mergeMacOSEntitlements(stringValue, "game.plist", kRequired, kForbidden, "Steam");
              }),
              "macOS entitlements 'game.plist' set "
              "com.apple.security.cs.allow-dyld-environment-variables to a string, but Steam "
              "requires true");

    const std::string sandbox =
        "<plist><dict><key>com.apple.security.app-sandbox</key><true/></dict></plist>";
    EXPECT_EQ(errorOf([&] {
                  mergeMacOSEntitlements(sandbox, "game.plist", kRequired, kForbidden, "Steam");
              }),
              "macOS entitlements 'game.plist' enable com.apple.security.app-sandbox, which Steam "
              "does not support");
}

TEST(MacOSEntitlements, RejectsBinaryAndMalformedPlists) {
    EXPECT_EQ(errorOf([&] {
                  mergeMacOSEntitlements("bplist00\x01\x02", "bin.plist", kRequired, {}, "Steam");
              }),
              "macOS entitlements 'bin.plist' is a binary property list; convert it with "
              "'plutil -convert xml1'");

    const std::string missingValue = errorOf([&] {
        mergeMacOSEntitlements(
            "<plist><dict><key>a</key></dict></plist>", "bad.plist", kRequired, {}, "Steam");
    });
    EXPECT_CONTAINS(missingValue,
                    "macOS entitlements 'bad.plist' is not a valid XML property list: ");

    const std::string duplicate = errorOf([&] {
        mergeMacOSEntitlements(
            "<plist><dict><key>a</key><true/><key>a</key><false/></dict></plist>",
            "dup.plist",
            kRequired,
            {},
            "Steam");
    });
    EXPECT_CONTAINS(duplicate, "duplicate key 'a'");

    EXPECT_EQ(errorOf([&] {
                  mergeMacOSEntitlements(
                      "<plist><array/></plist>", "array.plist", kRequired, {}, "Steam");
              }),
              "macOS entitlements 'array.plist' is not a valid XML property list: the top-level "
              "value must be a dictionary");
}

//===----------------------------------------------------------------------===//
// Steam directive rules
//===----------------------------------------------------------------------===//

TEST(SteamDepotRules, CanonicalizesIds) {
    EXPECT_EQ(canonicalSteamId("480").value_or("?"), "480");
    EXPECT_EQ(canonicalSteamId("000480").value_or("?"), "480");
    EXPECT_EQ(canonicalSteamId("4294967295").value_or("?"), "4294967295");
    EXPECT_EQ(canonicalSteamId("0000000004294967295").value_or("?"), "4294967295");
    EXPECT_FALSE(canonicalSteamId("4294967296").has_value());
    EXPECT_FALSE(canonicalSteamId("99999999999").has_value());
    EXPECT_FALSE(canonicalSteamId("0").has_value());
    EXPECT_FALSE(canonicalSteamId("").has_value());
    EXPECT_FALSE(canonicalSteamId("-1").has_value());
    EXPECT_FALSE(canonicalSteamId("12a").has_value());
    EXPECT_FALSE(canonicalSteamId(" 12").has_value());
}

TEST(SteamDepotRules, ValidatesDescriptionAndBranch) {
    EXPECT_NO_THROW(validateSteamBuildDescription("Nightly build 42"));
    EXPECT_EQ(errorOf([] { validateSteamBuildDescription("say \"hi\""); }),
              "steam-build-description must not contain '\"' or '\\'");
    EXPECT_EQ(errorOf([] { validateSteamBuildDescription("C:\\build"); }),
              "steam-build-description must not contain '\"' or '\\'");
    EXPECT_CONTAINS(errorOf([] { validateSteamBuildDescription("two\nlines"); }), "line breaks");

    EXPECT_NO_THROW(validateSteamSetLiveBranch("beta-1.2_rc"));
    EXPECT_EQ(errorOf([] { validateSteamSetLiveBranch("Default"); }),
              "steam-set-live cannot be 'default': Steam sets the default branch live only "
              "through the App Admin panel");
    EXPECT_EQ(errorOf([] { validateSteamSetLiveBranch("bad branch"); }),
              "invalid steam-set-live branch 'bad branch'; use letters, digits, '_', '.', or '-'");
    EXPECT_EQ(errorOf([] { validateSteamSetLiveBranch(""); }),
              "invalid steam-set-live branch ''; use letters, digits, '_', '.', or '-'");
}

TEST(SteamDepotRules, SelectsPlatformKeys) {
    const StoreProfile &steam = storeProfile(StoreKind::Steam);
    EXPECT_TRUE(isSteamDepotPlatform("linux-arm64"));
    EXPECT_FALSE(isSteamDepotPlatform("android"));
    EXPECT_EQ(storeDepotPlatformKey(steam, DepotOs::Windows, "x64"), "windows");
    EXPECT_EQ(storeDepotPlatformKey(steam, DepotOs::MacOS, "x64"), "macos");
    EXPECT_EQ(storeDepotPlatformKey(steam, DepotOs::MacOS, "arm64"), "macos");
    EXPECT_EQ(storeDepotPlatformKey(steam, DepotOs::Linux, "x64"), "linux");
    EXPECT_EQ(storeDepotPlatformKey(steam, DepotOs::Linux, "arm64"), "linux-arm64");
    EXPECT_EQ(errorOf([&] { storeDepotPlatformKey(steam, DepotOs::Windows, "arm64"); }),
              "Steam Windows depots are x64-only: Valve ships no Windows arm64 Steamworks "
              "redistributable");
    ASSERT_EQ(steam.requiredExports.size(), static_cast<size_t>(10));
    EXPECT_EQ(steam.providerMarker, "SteamAPI_InitFlat");
}

TEST(SteamDepotRules, RendersAppBuildScript) {
    const std::string script = renderSteamAppBuildScript(
        "480", "Nightly build", "beta", {{"windows", "481"}, {"linux-arm64", "483"}});
    const std::string expected = "\"AppBuild\"\n"
                                 "{\n"
                                 "\t\"AppID\" \"480\"\n"
                                 "\t\"Desc\" \"Nightly build\"\n"
                                 "\t\"ContentRoot\" \"../content/\"\n"
                                 "\t\"BuildOutput\" \"../output/\"\n"
                                 "\t\"SetLive\" \"beta\"\n"
                                 "\t\"Depots\"\n"
                                 "\t{\n"
                                 "\t\t\"481\"\n"
                                 "\t\t{\n"
                                 "\t\t\t\"FileMapping\"\n"
                                 "\t\t\t{\n"
                                 "\t\t\t\t\"LocalPath\" \"windows/*\"\n"
                                 "\t\t\t\t\"DepotPath\" \".\"\n"
                                 "\t\t\t\t\"recursive\" \"1\"\n"
                                 "\t\t\t}\n"
                                 "\t\t}\n"
                                 "\t\t\"483\"\n"
                                 "\t\t{\n"
                                 "\t\t\t\"FileMapping\"\n"
                                 "\t\t\t{\n"
                                 "\t\t\t\t\"LocalPath\" \"linux-arm64/*\"\n"
                                 "\t\t\t\t\"DepotPath\" \".\"\n"
                                 "\t\t\t\t\"recursive\" \"1\"\n"
                                 "\t\t\t}\n"
                                 "\t\t}\n"
                                 "\t}\n"
                                 "}\n";
    EXPECT_EQ(script, expected);
    EXPECT_CONTAINS(errorOf([] { renderSteamAppBuildScript("480", "bad \"desc\"", "", {}); }),
                    "must not contain");
}

//===----------------------------------------------------------------------===//
// Manifest directives
//===----------------------------------------------------------------------===//

namespace {

/// @brief Load a project whose manifest ends with @p steamLines.
/// @param tree Temporary tree receiving the project.
/// @param steamLines Extra manifest lines.
/// @param error Receives the diagnostic message on failure.
/// @return Loaded configuration, or nullopt on a manifest error.
std::optional<il::tools::common::ProjectConfig> loadWith(const TempTree &tree,
                                                         const std::string &steamLines,
                                                         std::string &error) {
    const fs::path dir = tree.root / "project";
    writeText(dir / "main.zia", "module main;\nfunc start() {}\n");
    writeText(dir / "zanna.project",
              "project depotapp\nversion 1.0.0\nlang zia\nentry main.zia\n" + steamLines);
    auto project = il::tools::common::resolveProject(utf8(dir));
    if (!project) {
        error = project.error().message;
        return std::nullopt;
    }
    return project.value();
}

} // namespace

TEST(SteamManifestDirectives, ParsesValidDirectives) {
    TempTree tree("manifest_valid");
    std::string error;
    const auto project = loadWith(tree,
                                  "steam-app-id 000480\n"
                                  "steam-redist \"../steam sdk\"\n"
                                  "steam-depot windows 481\n"
                                  "steam-depot linux-arm64 00483\n"
                                  "steam-build-description \"Nightly build\"\n"
                                  "steam-set-live beta\n",
                                  error);
    ASSERT_TRUE(project.has_value());
    const PackageConfig &pkg = project->packageConfig;
    EXPECT_EQ(pkg.steamAppId, "480");
    EXPECT_EQ(pkg.steamRedist, "../steam sdk");
    ASSERT_EQ(pkg.steamDepots.size(), static_cast<size_t>(2));
    EXPECT_EQ(pkg.steamDepots[0].platform, "windows");
    EXPECT_EQ(pkg.steamDepots[0].depotId, "481");
    EXPECT_EQ(pkg.steamDepots[1].platform, "linux-arm64");
    EXPECT_EQ(pkg.steamDepots[1].depotId, "483");
    EXPECT_EQ(pkg.steamBuildDescription, "Nightly build");
    EXPECT_EQ(pkg.steamSetLive, "beta");
    EXPECT_TRUE(pkg.hasPackageConfig());
}

TEST(SteamManifestDirectives, RejectsInvalidDirectives) {
    struct Case {
        const char *lines;
        const char *message;
    };

    const Case cases[] = {
        {"steam-app-id 0\n", "invalid steam-app-id '0'; expected an integer in 1..4294967295"},
        {"steam-app-id 480\nsteam-app-id 481\n", "duplicate directive 'steam-app-id'"},
        {"steam-depot windows\n", "steam-depot requires <platform> <depot-id>; got 'windows'"},
        {"steam-depot android 5\n",
         "invalid steam-depot platform 'android'; expected windows, macos, linux, or linux-arm64"},
        {"steam-depot linux 4294967296\n",
         "invalid steam-depot id '4294967296'; expected an integer in 1..4294967295"},
        {"steam-depot linux 5\nsteam-depot linux 6\n", "duplicate steam-depot platform 'linux'"},
        {"steam-depot linux 5\nsteam-depot macos 005\n",
         "steam-depot id 5 is already mapped to linux"},
        {"steam-build-description \"say \\\"hi\\\"\"\n",
         "steam-build-description must not contain '\"' or '\\'"},
        {"steam-set-live default\n",
         "steam-set-live cannot be 'default': Steam sets the default branch live only through the "
         "App Admin panel"},
        {"steam-set-live \"two words\"\n",
         "invalid steam-set-live branch 'two words'; use letters, digits, '_', '.', or '-'"},
    };
    for (const Case &c : cases) {
        TempTree tree("manifest_invalid");
        std::string error;
        const auto project = loadWith(tree, c.lines, error);
        EXPECT_FALSE(project.has_value());
        EXPECT_CONTAINS(error, c.message);
    }
}

//===----------------------------------------------------------------------===//
// Depot planning
//===----------------------------------------------------------------------===//

TEST(StoreDepotPlan, ResolvesRedistributableLayouts) {
    TempTree tree("plan_layouts");
    StoreDepotParams params = linuxDepot(tree);
    const std::string expected =
        utf8((tree.root / "sdk" / "redistributable_bin" / "linux64" / "libsteam_api.so")
                 .lexically_normal());

    StoreDepotPlan plan = planStoreDepot(params);
    EXPECT_EQ(plan.redistSource, expected);
    EXPECT_EQ(plan.redistStagedPath, "libsteam_api.so");
    EXPECT_EQ(plan.platformKey, "linux");
    EXPECT_EQ(plan.depotId, "482");
    EXPECT_EQ(plan.launchPath, "depotapp");
    EXPECT_TRUE(plan.executableInspected);
    EXPECT_TRUE(plan.usesProvider);
    EXPECT_TRUE(plan.warnings.empty());

    params.redistSource = utf8(tree.root / "sdk" / "redistributable_bin");
    EXPECT_EQ(planStoreDepot(params).redistSource, expected);

    fs::create_directories(tree.root / "steamworks");
    fs::rename(tree.root / "sdk", tree.root / "steamworks" / "sdk");
    params.redistSource = "steamworks";
    EXPECT_CONTAINS(planStoreDepot(params).redistSource, "steamworks");

    params.redistSource = "missing";
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable not found: expected " +
                  utf8((tree.root / "missing" / "linux64" / "libsteam_api.so").lexically_normal()) +
                  " (from steam-redist 'missing')");
}

TEST(StoreDepotPlan, RequiresAppIdAndValidMappings) {
    TempTree tree("plan_config");
    StoreDepotParams params = linuxDepot(tree);
    params.pkgConfig.steamAppId.clear();
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam depot packaging requires steam-app-id in zanna.project");

    params = linuxDepot(tree);
    params.pkgConfig.steamDepots = {{"linux", "5"}, {"macos", "5"}};
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "steam-depot id 5 is already mapped to linux");

    params = linuxDepot(tree);
    params.arch = "arm64";
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "executable '" + params.executablePath + "' does not contain arm64 code");
}

TEST(StoreDepotPlan, ValidatesRedistributableBinaries) {
    TempTree tree("plan_redist");
    StoreDepotParams params = linuxDepot(tree);
    const fs::path so = tree.root / "sdk" / "redistributable_bin" / "linux64" / "libsteam_api.so";
    const std::string label = utf8(so.lexically_normal());

    writeBytes(so, makePe(0x8664, true, steamCoreExports()));
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable '" + label + "' is not an ELF shared object");

    writeBytes(so, makeElf(kElfArm64, 3, steamCoreExports()));
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable '" + label + "' does not contain x64 code");

    std::vector<std::string> partial = steamCoreExports();
    partial.pop_back();
    writeBytes(so, makeElf(kElfX64, 3, partial));
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable '" + label +
                  "' is not a Steamworks SDK 1.61-1.65 redistributable: export name '" +
                  steamCoreExports().back() + "' not found");

    params.os = DepotOs::Windows;
    writeBytes(tree.root / "bin" / "depotapp", makePe(0x8664, false, {"SteamAPI_InitFlat"}));
    const fs::path dll = tree.root / "sdk" / "redistributable_bin" / "win64" / "steam_api64.dll";
    writeBytes(dll, makePe(0x8664, false, steamCoreExports()));
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable '" + utf8(dll.lexically_normal()) + "' is not a PE32+ DLL");

    params.os = DepotOs::MacOS;
    params.arch = "arm64";
    params.pkgConfig.macosSignMode = "none";
    writeBytes(tree.root / "bin" / "depotapp",
               makeFatMachO({makeMachO(kMachX64, 2, {}), makeMachO(kMachArm64, 2, {})},
                            {"SteamAPI_InitFlat"}));
    const fs::path dylib = tree.root / "sdk" / "redistributable_bin" / "osx" / "libsteam_api.dylib";
    writeBytes(dylib, makeMachO(kMachArm64, 6, steamCoreExports()));
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam redistributable '" + utf8(dylib.lexically_normal()) +
                  "' does not contain x64 code");
    writeBytes(dylib,
               makeFatMachO({makeMachO(kMachX64, 6, {}), makeMachO(kMachArm64, 6, {})},
                            steamCoreExports()));
    const StoreDepotPlan plan = planStoreDepot(params);
    EXPECT_EQ(plan.launchPath, "DepotApp.app");
    EXPECT_EQ(plan.executableRelativePath, "DepotApp.app/Contents/MacOS/depotapp");
    EXPECT_EQ(plan.redistStagedPath, "DepotApp.app/Contents/MacOS/libsteam_api.dylib");
    EXPECT_EQ(plan.requiredEntitlements.size(), static_cast<size_t>(2));
}

TEST(StoreDepotPlan, ChecksProviderUse) {
    TempTree tree("plan_provider");
    StoreDepotParams params = linuxDepot(tree);
    params.redistSource.clear();
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "the executable uses the Zanna.Services Steam provider, but no Steamworks "
              "redistributable is configured; set steam-redist or pass --steam-redist");

    params = linuxDepot(tree, false);
    StoreDepotPlan plan = planStoreDepot(params);
    EXPECT_FALSE(plan.usesProvider);
    ASSERT_EQ(plan.warnings.size(), static_cast<size_t>(1));
    EXPECT_EQ(plan.warnings[0],
              "depotapp does not use Zanna.Services; the staged Steamworks redistributable is "
              "not loaded");

    params.redistSource.clear();
    plan = planStoreDepot(params);
    EXPECT_TRUE(plan.warnings.empty());
    EXPECT_TRUE(plan.redistStagedPath.empty());

    params = linuxDepot(tree);
    params.executablePath.clear();
    plan = planStoreDepot(params);
    EXPECT_FALSE(plan.executableInspected);
    EXPECT_EQ(plan.redistStagedPath, "libsteam_api.so");
}

TEST(StoreDepotPlan, RejectsOutputInsideAssetSource) {
    TempTree tree("plan_output");
    StoreDepotParams params = linuxDepot(tree);
    writeText(tree.root / "project" / "data" / "readme.txt", "hello\n");
    params.pkgConfig.assets.push_back({"data", "data"});
    params.outputRoot = utf8(tree.root / "project" / "data" / "steam");
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam depot output directory '" + params.outputRoot +
                  "' is inside asset source 'data'");

    params.pkgConfig.assets.clear();
    params.extraSourcePaths = {"data"};
    EXPECT_CONTAINS(errorOf([&] { planStoreDepot(params); }), "is inside asset source 'data'");

    params.extraSourcePaths.clear();
    writeText(tree.root / "out-file", "not a directory\n");
    params.outputRoot = utf8(tree.root / "out-file");
    EXPECT_EQ(errorOf([&] { planStoreDepot(params); }),
              "Steam depot output '" + params.outputRoot + "' exists and is not a directory");
}

//===----------------------------------------------------------------------===//
// Depot staging
//===----------------------------------------------------------------------===//

TEST(StoreDepotBuild, StagesLinuxDepotWithManifestAndScript) {
    TempTree tree("build_linux");
    StoreDepotParams params = linuxDepot(tree);
    writeText(tree.root / "project" / "config.txt", "config\n");
    writeText(tree.root / "project" / "levels" / "one.txt", "level one\n");
    writeText(tree.root / "project" / "levels" / "nested" / "two.txt", "level two\n");
    params.pkgConfig.assets = {{"config.txt", ""}, {"levels", "data/levels"}};
    writeText(tree.root / "packs" / "depotapp-core.zpak", "ZPAK");
    params.packFiles = {utf8(tree.root / "packs" / "depotapp-core.zpak")};

    const StoreDepotResult result = buildStoreDepot(params);
    const fs::path content = tree.root / "out" / "content" / "linux";
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(result.trust, "unsigned");
    ASSERT_EQ(result.files.size(), static_cast<size_t>(6));
    EXPECT_TRUE(hasFile(result, "depotapp"));
    EXPECT_TRUE(hasFile(result, "libsteam_api.so"));
    EXPECT_TRUE(hasFile(result, "config.txt"));
    EXPECT_TRUE(hasFile(result, "data/levels/one.txt"));
    EXPECT_TRUE(hasFile(result, "data/levels/nested/two.txt"));
    EXPECT_TRUE(hasFile(result, "depotapp-core.zpak"));
    EXPECT_TRUE(std::is_sorted(
        result.files.begin(),
        result.files.end(),
        [](const StoreDepotFile &a, const StoreDepotFile &b) { return a.path < b.path; }));
    EXPECT_EQ(readText(content / "data" / "levels" / "nested" / "two.txt"), "level two\n");
#if !ZANNA_HOST_WINDOWS
    const fs::perms exec = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    EXPECT_TRUE((fs::status(content / "depotapp").permissions() & exec) == exec);
    EXPECT_TRUE((fs::status(content / "libsteam_api.so").permissions() & exec) == exec);
    EXPECT_TRUE((fs::status(content / "config.txt").permissions() & exec) == fs::perms::none);
#endif

    const std::vector<uint8_t> exeBytes = readFile(content / "depotapp");
    const std::string manifest = readText(tree.root / "out" / "manifests" / "linux.json");
    EXPECT_CONTAINS(manifest, "\"schema_version\": 1,");
    EXPECT_CONTAINS(manifest, "\"store\": \"steam\",");
    EXPECT_CONTAINS(manifest, "\"platform\": \"linux\",");
    EXPECT_CONTAINS(manifest, "\"app_id\": \"480\",");
    EXPECT_CONTAINS(manifest, "\"depot_id\": \"482\",");
    EXPECT_CONTAINS(manifest, "\"launch\": \"depotapp\",");
    EXPECT_CONTAINS(manifest, "\"trust\": \"unsigned\",");
    EXPECT_CONTAINS(manifest,
                    "{\"path\": \"depotapp\", \"size\": " + std::to_string(exeBytes.size()) +
                        ", \"sha256\": \"" + sha256Hex(exeBytes.data(), exeBytes.size()) + "\"}");

    ASSERT_EQ(result.buildScripts.size(), static_cast<size_t>(1));
    const std::string script = readText(tree.root / "out" / "scripts" / "app_build_480.vdf");
    EXPECT_CONTAINS(script, "\t\"Desc\" \"DepotApp 1.2.3\"\n");
    EXPECT_CONTAINS(script, "\t\t\"482\"\n");
    EXPECT_CONTAINS(script, "\"LocalPath\" \"linux/*\"");
    EXPECT_EQ(script.find("SetLive"), std::string::npos);

    size_t entries = 0;
    for (const auto &entry : fs::directory_iterator(tree.root / "out" / "content")) {
        (void)entry;
        ++entries;
    }
    EXPECT_EQ(entries, static_cast<size_t>(1));
}

TEST(StoreDepotBuild, AccumulatesPlatformsInOneBuildScript) {
    TempTree tree("build_accumulate");
    StoreDepotParams params = linuxDepot(tree);
    params.pkgConfig.steamDepots = {{"linux", "482"}, {"linux-arm64", "483"}};
    params.pkgConfig.steamSetLive = "beta";
    (void)buildStoreDepot(params);

    writeBytes(tree.root / "bin" / "depotapp", makeElf(kElfArm64, 2, {"SteamAPI_InitFlat"}));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "linuxarm64" / "libsteam_api.so",
               makeElf(kElfArm64, 3, steamCoreExports()));
    params.arch = "arm64";
    (void)buildStoreDepot(params);

    writeBytes(tree.root / "bin" / "depotapp", makePe(0x8664, false, {"SteamAPI_InitFlat"}));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "win64" / "steam_api64.dll",
               makePe(0x8664, true, steamCoreExports()));
    params.os = DepotOs::Windows;
    params.arch = "x64";
    const StoreDepotResult windows = buildStoreDepot(params);
    ASSERT_EQ(windows.warnings.size(), static_cast<size_t>(1));
    EXPECT_EQ(windows.warnings[0],
              "no steam-depot is configured for windows; scripts/app_build_480.vdf does not "
              "upload content/windows");
    EXPECT_TRUE(hasFile(windows, "depotapp.exe"));
    EXPECT_TRUE(hasFile(windows, "steam_api64.dll"));

    const std::string script = readText(tree.root / "out" / "scripts" / "app_build_480.vdf");
    EXPECT_EQ(script,
              renderSteamAppBuildScript(
                  "480", "DepotApp 1.2.3", "beta", {{"linux", "482"}, {"linux-arm64", "483"}}));
    EXPECT_TRUE(fs::exists(tree.root / "out" / "manifests" / "linux.json"));
    EXPECT_TRUE(fs::exists(tree.root / "out" / "manifests" / "linux-arm64.json"));
    EXPECT_TRUE(fs::exists(tree.root / "out" / "manifests" / "windows.json"));
}

TEST(StoreDepotBuild, SkipsBuildScriptWithoutAnyDepotMapping) {
    TempTree tree("build_unmapped");
    StoreDepotParams params = linuxDepot(tree);
    params.pkgConfig.steamDepots.clear();
    const StoreDepotResult result = buildStoreDepot(params);
    EXPECT_TRUE(result.buildScripts.empty());
    ASSERT_EQ(result.warnings.size(), static_cast<size_t>(1));
    EXPECT_CONTAINS(result.warnings[0], "no steam-depot is configured for linux");
    EXPECT_FALSE(fs::exists(tree.root / "out" / "scripts"));
    EXPECT_CONTAINS(readText(tree.root / "out" / "manifests" / "linux.json"),
                    "\"depot_id\": null,");
}

TEST(StoreDepotBuild, WindowsDepotSignsOnlyApplicationCode) {
    TempTree tree("build_windows_sign");
    StoreDepotParams params = linuxDepot(tree);
    params.os = DepotOs::Windows;
    params.pkgConfig.steamDepots = {{"windows", "481"}};
    writeBytes(tree.root / "bin" / "depotapp", makePe(0x8664, false, {"SteamAPI_InitFlat"}));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "win64" / "steam_api64.dll",
               makePe(0x8664, true, steamCoreExports()));
    writeBytes(tree.root / "project" / "plugins" / "extra.dll", makePe(0x8664, true, {}));
    params.pkgConfig.windowsDlls = {"plugins/extra.dll"};

    std::vector<std::string> signedNames;
    const std::string mark = "SIGNED";
    params.windowsSigner = [&](std::string_view name, const std::vector<uint8_t> &bytes) {
        signedNames.emplace_back(name);
        std::vector<uint8_t> out = bytes;
        out.insert(out.end(), mark.begin(), mark.end());
        return out;
    };

    const StoreDepotResult result = buildStoreDepot(params);
    EXPECT_EQ(result.trust, "authenticode");
    ASSERT_EQ(signedNames.size(), static_cast<size_t>(2));
    EXPECT_EQ(signedNames[0], "depotapp.exe");
    EXPECT_EQ(signedNames[1], "plugins/extra.dll");
    const fs::path content = tree.root / "out" / "content" / "windows";
    const auto endsWithMark = [&](const fs::path &path) {
        const std::string text = readText(path);
        return text.size() >= mark.size() &&
               text.compare(text.size() - mark.size(), mark.size(), mark) == 0;
    };
    EXPECT_TRUE(endsWithMark(content / "depotapp.exe"));
    EXPECT_TRUE(endsWithMark(content / "plugins" / "extra.dll"));
    EXPECT_FALSE(endsWithMark(content / "steam_api64.dll"));
}

TEST(StoreDepotBuild, RejectsContentCollisions) {
    TempTree tree("build_collision");
    StoreDepotParams params = linuxDepot(tree);
    params.os = DepotOs::Windows;
    params.pkgConfig.steamDepots = {{"windows", "481"}};
    writeBytes(tree.root / "bin" / "depotapp", makePe(0x8664, false, {"SteamAPI_InitFlat"}));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "win64" / "steam_api64.dll",
               makePe(0x8664, true, steamCoreExports()));
    writeText(tree.root / "project" / "STEAM_API64.DLL", "shadow\n");
    params.pkgConfig.assets = {{"STEAM_API64.DLL", ""}};
    EXPECT_EQ(errorOf([&] { buildStoreDepot(params); }),
              "Steam depot content path collision: STEAM_API64.DLL");
    EXPECT_FALSE(fs::exists(tree.root / "out" / "content" / "windows"));
}

TEST(StoreDepotBuild, FailedBuildKeepsPreviousContent) {
    TempTree tree("build_keep_previous");
    StoreDepotParams params = linuxDepot(tree);
    (void)buildStoreDepot(params);
    const fs::path content = tree.root / "out" / "content" / "linux";
    const std::string manifestBefore = readText(tree.root / "out" / "manifests" / "linux.json");

    writeText(tree.root / "project" / "dev" / "Steam_AppID.txt", "480\n");
    params.pkgConfig.assets = {{"dev", "dev"}};
    EXPECT_EQ(errorOf([&] { buildStoreDepot(params); }),
              "steam_appid.txt must not ship in a Steam depot (found dev/Steam_AppID.txt); "
              "Zanna.Services.Platform.Init sets SteamAppId itself");
    EXPECT_TRUE(fs::exists(content / "depotapp"));
    EXPECT_FALSE(fs::exists(content / "dev"));
    EXPECT_EQ(readText(tree.root / "out" / "manifests" / "linux.json"), manifestBefore);

    size_t entries = 0;
    for (const auto &entry : fs::directory_iterator(tree.root / "out" / "content")) {
        EXPECT_EQ(utf8(entry.path().filename()), "linux");
        ++entries;
    }
    EXPECT_EQ(entries, static_cast<size_t>(1));

    params.pkgConfig.assets.clear();
    writeText(tree.root / "project" / "notes.txt", "rebuilt\n");
    params.pkgConfig.assets = {{"notes.txt", ""}};
    const StoreDepotResult rebuilt = buildStoreDepot(params);
    EXPECT_TRUE(hasFile(rebuilt, "notes.txt"));
    EXPECT_TRUE(fs::exists(content / "notes.txt"));
}

TEST(StoreDepotBuild, StagesUnsignedMacOSBundle) {
    TempTree tree("build_macos");
    StoreDepotParams params = linuxDepot(tree);
    params.os = DepotOs::MacOS;
    params.arch = "arm64";
    params.pkgConfig.macosSignMode = "none";
    params.pkgConfig.displayName = "Depot App";
    params.pkgConfig.steamDepots = {{"macos", "484"}};
    writeBytes(tree.root / "bin" / "depotapp", makeMachO(kMachArm64, 2, {"SteamAPI_InitFlat"}));
    writeBytes(tree.root / "sdk" / "redistributable_bin" / "osx" / "libsteam_api.dylib",
               makeFatMachO({makeMachO(kMachX64, 6, {}), makeMachO(kMachArm64, 6, {})},
                            steamCoreExports()));
    writeText(tree.root / "project" / "data" / "readme.txt", "hello\n");
    params.pkgConfig.assets = {{"data", "data"}};
    writeText(tree.root / "packs" / "depotapp-core.zpak", "ZPAK");
    params.packFiles = {utf8(tree.root / "packs" / "depotapp-core.zpak")};

    const StoreDepotResult result = buildStoreDepot(params);
    EXPECT_EQ(result.trust, "none");
    EXPECT_TRUE(hasFile(result, "Depot App.app/Contents/Info.plist"));
    EXPECT_TRUE(hasFile(result, "Depot App.app/Contents/MacOS/depotapp"));
    EXPECT_TRUE(hasFile(result, "Depot App.app/Contents/MacOS/libsteam_api.dylib"));
    EXPECT_TRUE(hasFile(result, "Depot App.app/Contents/Resources/data/readme.txt"));
    EXPECT_TRUE(hasFile(result, "Depot App.app/Contents/Resources/depotapp-core.zpak"));
    const std::string manifest = readText(tree.root / "out" / "manifests" / "macos.json");
    EXPECT_CONTAINS(manifest, "\"launch\": \"Depot App.app\",");
    EXPECT_CONTAINS(manifest,
                    "\"redistributable\": \"Depot App.app/Contents/MacOS/libsteam_api.dylib\",");
#if !ZANNA_HOST_WINDOWS
    const fs::perms exec = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    EXPECT_TRUE((fs::status(tree.root / "out" / "content" / "macos" / "Depot App.app" / "Contents" /
                            "MacOS" / "libsteam_api.dylib")
                     .permissions() &
                 exec) == exec);
#endif
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
