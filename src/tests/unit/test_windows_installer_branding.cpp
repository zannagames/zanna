//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_branding.cpp
// Purpose: Prove that installer brand surfaces present the packaged product rather
//          than the Zanna toolchain, and that the toolchain defaults are unchanged.
// Key invariants:
//   - The default branding is the toolchain identity, so toolchain setup is untouched.
//   - An application override replaces every brand field the package supplies.
//   - Empty override fields never blank an existing brand field.
// Ownership/Lifetime:
//   - Branding is process-wide; each case restores the toolchain defaults on exit.
// Links: src/tools/windows_installer/WindowsInstallerTheme.cpp,
//        src/tools/windows_installer/WindowsInstallerWizard.cpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerTheme.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using zanna::installer::InstallerBranding;
using zanna::installer::installerBranding;
using zanna::installer::setInstallerBranding;

int testsRun = 0;
int testsPassed = 0;

/// @brief Record one boolean expectation and report the failing case by name.
/// @param condition Result under test.
/// @param message Case description printed when the expectation fails.
void expect(bool condition, std::string_view message) {
    ++testsRun;
    if (condition) {
        ++testsPassed;
    } else {
        std::cerr << "FAIL: " << message << '\n';
    }
}

/// @brief Restore the toolchain identity so cases remain order-independent.
void restoreToolchainBranding() {
    setInstallerBranding({L"ZANNA", L"DEVELOPER PLATFORM", L"CODE. CREATE.\r\nCOMPILE. CONQUER."});
}

/// @brief Given no override, When branding is read, Then the toolchain identity is active.
void testToolchainDefaultIdentity() {
    restoreToolchainBranding();
    const InstallerBranding &branding = installerBranding();
    expect(branding.wordmark == L"ZANNA", "default wordmark stays ZANNA for the toolchain");
    expect(branding.category == L"DEVELOPER PLATFORM", "default category stays the toolchain line");
    expect(branding.tagline.find(L"COMPILE. CONQUER.") != std::wstring::npos,
           "default tagline stays the toolchain tagline");
}

/// @brief Given an application package, When branding is applied, Then no Zanna text remains.
void testApplicationOverrideReplacesToolchainText() {
    restoreToolchainBranding();
    setInstallerBranding({L"LEGACY BASEBALL", L"SETUP", L"Legacy Baseball\r\n0.1.0"});
    const InstallerBranding &branding = installerBranding();
    expect(branding.wordmark == L"LEGACY BASEBALL", "application wordmark replaces the toolchain");
    expect(branding.category == L"SETUP", "application category replaces the toolchain line");
    expect(branding.tagline.find(L"Legacy Baseball") != std::wstring::npos,
           "application tagline names the product");
    expect(branding.wordmark.find(L"ZANNA") == std::wstring::npos,
           "application wordmark carries no toolchain name");
    expect(branding.category.find(L"DEVELOPER") == std::wstring::npos,
           "application category carries no toolchain vocabulary");
    expect(branding.tagline.find(L"CONQUER") == std::wstring::npos,
           "application tagline carries no toolchain tagline");
    restoreToolchainBranding();
}

/// @brief Given empty override fields, When branding is applied, Then current values survive.
void testEmptyFieldsPreserveCurrentBranding() {
    restoreToolchainBranding();
    setInstallerBranding({L"LEGACY BASEBALL", L"", L""});
    const InstallerBranding &branding = installerBranding();
    expect(branding.wordmark == L"LEGACY BASEBALL", "supplied field is adopted");
    expect(branding.category == L"DEVELOPER PLATFORM", "empty category keeps the previous value");
    expect(branding.tagline.find(L"COMPILE. CONQUER.") != std::wstring::npos,
           "empty tagline keeps the previous value");
    restoreToolchainBranding();
}

} // namespace

/// @brief Run every installer branding case.
/// @return Zero when all expectations hold; one otherwise.
int main() {
    testToolchainDefaultIdentity();
    testApplicationOverrideReplacesToolchainText();
    testEmptyFieldsPreserveCurrentBranding();
    std::cout << "windows installer branding: " << testsPassed << '/' << testsRun
              << " expectations passed\n";
    return testsPassed == testsRun ? 0 : 1;
}
