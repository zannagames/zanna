//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_cleanup_policy.cpp
// Purpose: Verify the detached Windows installer cleanup path policy.
//
// Key invariants:
//   - Only unambiguous absolute paths below a drive or UNC share are accepted.
//   - Device aliases, traversal, alternate streams, and root deletion fail closed.
//
// Ownership/Lifetime: Standalone dependency-free unit test executable.
//
// Links: src/tools/windows_installer/WindowsInstallerCleanupPolicy.cpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "tools/windows_installer/WindowsInstallerCleanupPolicy.hpp"

#include <array>
#include <string>
#include <string_view>

using zanna::installer::cleanup::isSafeAbsolutePath;
using zanna::installer::cleanup::parseProcessId;
using zanna::installer::cleanup::pathsEqual;

TEST(WindowsInstallerCleanupPolicy, AcceptsQualifiedChildPaths) {
    static constexpr std::array<std::wstring_view, 6> kAccepted = {
        LR"(C:\Users\Example\AppData\Local\Zanna\cache.exe)",
        LR"(d:/ProgramData/Zanna/Packages/1.0/zanna.exe)",
        LR"(\\?\C:\Users\Example\AppData\Local\Zanna\cache.exe)",
        LR"(\\server\share\Zanna\cache.exe)",
        LR"(\\?\UNC\server\share\Zanna\cache.exe)",
        LR"(C:\Zanna Cleanup\package-1\cleanup.exe)",
    };
    for (const std::wstring_view path : kAccepted)
        EXPECT_TRUE(isSafeAbsolutePath(path));
}

TEST(WindowsInstallerCleanupPolicy, RejectsRootsNamespacesAndTraversal) {
    static constexpr std::array<std::wstring_view, 16> kRejected = {
        LR"(C:\)",
        LR"(\\server\share)",
        LR"(\\server\share\)",
        LR"(\\?\C:\)",
        LR"(\\?\UNC\server\share\)",
        LR"(relative\cache.exe)",
        LR"(\rooted\cache.exe)",
        LR"(C:drive-relative.exe)",
        LR"(\\.\C:\cache.exe)",
        LR"(\\?\GLOBALROOT\Device\HarddiskVolume1\cache.exe)",
        LR"(\\?\Volume{00000000-0000-0000-0000-000000000000}\cache.exe)",
        LR"(C:\Zanna\..\victim.exe)",
        LR"(C:\Zanna\.\victim.exe)",
        LR"(C:\Zanna\\victim.exe)",
        LR"(\\server\\share\victim.exe)",
        LR"(\\server\share\\victim.exe)",
    };
    for (const std::wstring_view path : kRejected)
        EXPECT_FALSE(isSafeAbsolutePath(path));
}

TEST(WindowsInstallerCleanupPolicy, RejectsAmbiguousWin32Components) {
    static constexpr std::array<std::wstring_view, 19> kRejected = {
        LR"(C:\Zanna\CON)",       LR"(C:\Zanna\con.txt)",         LR"(C:\Zanna\PRN.log)",
        LR"(C:\Zanna\AUX)",       LR"(C:\Zanna\NUL.bin)",         LR"(C:\Zanna\CLOCK$)",
        LR"(C:\Zanna\CONIN$)",    LR"(C:\Zanna\CONOUT$.log)",     LR"(C:\Zanna\COM1.txt)",
        LR"(C:\Zanna\LPT9)",      L"C:\\Zanna\\COM\u00b9.log",    LR"(C:\Zanna\trailing.)",
        LR"(C:\Zanna\trailing )", LR"(C:\Zanna\file.exe:stream)", LR"(C:\Zanna\bad?.exe)",
        LR"(C:\Zanna\bad*.exe)",  LR"(C:\Zanna\bad|name.exe)",    L"C:\\Zanna\\bad\x1fname.exe",
        LR"(C:\Zanna\)",
    };
    for (const std::wstring_view path : kRejected)
        EXPECT_FALSE(isSafeAbsolutePath(path));
}

TEST(WindowsInstallerCleanupPolicy, RejectsMalformedUnicodeAndExtendedSeparators) {
    static constexpr std::array<std::wstring_view, 4> kRejected = {
        LR"(//?/C:/Zanna/cache.exe)",
        LR"(\\?/C:\Zanna\cache.exe)",
        LR"(\\?\C:/Zanna/cache.exe)",
        LR"(\\?\UNC/server/share/Zanna/cache.exe)",
    };
    for (const std::wstring_view path : kRejected)
        EXPECT_FALSE(isSafeAbsolutePath(path));

    std::wstring malformed = L"C:\\Zanna\\high-";
    malformed.push_back(static_cast<wchar_t>(0xd800));
    malformed += L".exe";
    EXPECT_FALSE(isSafeAbsolutePath(malformed));
    malformed = L"C:\\Zanna\\low-";
    malformed.push_back(static_cast<wchar_t>(0xdc00));
    malformed += L".exe";
    EXPECT_FALSE(isSafeAbsolutePath(malformed));
    malformed = L"C:\\Zanna\\reversed-";
    malformed.push_back(static_cast<wchar_t>(0xdc00));
    malformed.push_back(static_cast<wchar_t>(0xd800));
    malformed += L".exe";
    EXPECT_FALSE(isSafeAbsolutePath(malformed));
}

TEST(WindowsInstallerCleanupPolicy, ComparesCaseAndSeparatorsOrdinally) {
    EXPECT_TRUE(pathsEqual(LR"(C:\Zanna\Cache.EXE)", LR"(c:/zanna/cache.exe)"));
    EXPECT_TRUE(pathsEqual(LR"(\\Server\Share\Zanna)", LR"(//server/share/zanna)"));
    EXPECT_TRUE(pathsEqual(LR"(C:\Zanna\Cache.exe)", LR"(\\?\c:\zanna\cache.EXE)"));
    EXPECT_TRUE(pathsEqual(LR"(\\Server\Share\Zanna)", LR"(\\?\UNC\server\share\zanna)"));
    EXPECT_TRUE(pathsEqual(L"C:\\Zanna\\\u00c4.exe", L"c:/zanna/\u00e4.EXE"));
    EXPECT_FALSE(pathsEqual(LR"(C:\Zanna\Cache.exe)", LR"(C:\Zanna\Cache2.exe)"));
    EXPECT_FALSE(pathsEqual(LR"(C:\Server\Share\Zanna)", LR"(\\Server\Share\Zanna)"));
}

TEST(WindowsInstallerCleanupPolicy, ParsesOnlyCanonicalNonzeroProcessIds) {
    std::uint32_t processId = 99;
    EXPECT_TRUE(parseProcessId(L"1", processId));
    EXPECT_EQ(processId, 1U);
    EXPECT_TRUE(parseProcessId(L"4294967295", processId));
    EXPECT_EQ(processId, UINT32_MAX);

    static constexpr std::array<std::wstring_view, 9> kRejected = {
        L"",
        L"0",
        L"-1",
        L"+1",
        L" 1",
        L"1 ",
        L"1x",
        L"4294967296",
        L"999999999999999999999",
    };
    for (const std::wstring_view text : kRejected) {
        processId = 99;
        EXPECT_FALSE(parseProcessId(text, processId));
        EXPECT_EQ(processId, 0U);
    }
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
