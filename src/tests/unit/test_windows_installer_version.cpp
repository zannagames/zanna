//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_version.cpp
// Purpose: Verify Windows installer version precedence and strict validation.
//
// Key invariants:
//   - SemVer 2 prerelease precedence is preserved exactly.
//   - Build metadata is ignored and malformed/ambiguous versions are rejected.
//
// Ownership/Lifetime: Standalone dependency-free unit test executable.
//
// Links: WindowsInstallerVersion.cpp, WindowsInstallerHost.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "tools/windows_installer/WindowsInstallerHost.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

using zanna::installer::compareInstallerVersions;

TEST(WindowsInstallerVersion, ImplementsCanonicalSemVerPrereleaseOrder) {
    static constexpr std::array<std::string_view, 8> kOrdered = {
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-beta",
        "1.0.0-beta.2",
        "1.0.0-beta.11",
        "1.0.0-rc.1",
        "1.0.0",
    };
    for (std::size_t index = 1; index < kOrdered.size(); ++index) {
        EXPECT_LT(compareInstallerVersions(kOrdered[index - 1U], kOrdered[index]), 0);
        EXPECT_GT(compareInstallerVersions(kOrdered[index], kOrdered[index - 1U]), 0);
    }
}

TEST(WindowsInstallerVersion, IgnoresBuildMetadataAndSupportsFourPartBuilds) {
    EXPECT_EQ(compareInstallerVersions("2.4.0+build.7", "2.4.0+build.99"), 0);
    EXPECT_GT(compareInstallerVersions("2.4.0.10", "2.4.0.9"), 0);
    EXPECT_EQ(compareInstallerVersions("2.4", "2.4.0"), 0);
}

TEST(WindowsInstallerVersion, NumericPrereleaseIdentifiersAreNumeric) {
    EXPECT_GT(compareInstallerVersions("1.0.0-beta.10", "1.0.0-beta.2"), 0);
    EXPECT_LT(compareInstallerVersions("1.0.0-1", "1.0.0-alpha"), 0);
}

TEST(WindowsInstallerVersion, RejectsAmbiguousOrMalformedVersions) {
    EXPECT_THROWS(compareInstallerVersions("01.2.3", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3-rc.01", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3-alpha..1", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3+", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3.4.5", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.65536.0", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3-rc_1", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3-\xc3\xa4", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions("1.2.3+build.\xc3\xa4", "1.2.3"), std::runtime_error);
    EXPECT_THROWS(compareInstallerVersions(std::string("1.2.3+") + std::string(123, 'a'), "1.2.3"),
                  std::runtime_error);
}

TEST(WindowsInstallerVersion, AcceptsMaximumWindowsNumericIdentity) {
    EXPECT_EQ(compareInstallerVersions("65535.65535.65535.65535", "65535.65535.65535.65535"), 0);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
