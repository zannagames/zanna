//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_brand_validation.cpp
// Purpose: Verify fail-closed branded Windows installer model validation.
//
// Key invariants:
//   - Native control identifiers are unique, representable, and non-reserved.
//   - Visible and accessible text is bounded, complete, and valid UTF-16.
//   - Default, close, verification, and progress actions are unambiguous.
//
// Ownership/Lifetime:
//   - Every test model and callback is owned by this process.
//
// Links: src/tools/windows_installer/WindowsInstallerBrandValidation.cpp,
//        src/tools/windows_installer/WindowsInstallerBrandDialog.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerBrandValidation.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using zanna::installer::BrandedInstallerPage;
using zanna::installer::validateBrandedInstallerPage;
using zanna::installer::validateBrandedInstallerProgress;

int testsRun = 0;
int testsPassed = 0;

void expect(bool condition, std::string_view message) {
    ++testsRun;
    if (condition) {
        ++testsPassed;
    } else {
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Callback> bool throwsValidation(Callback &&callback) {
    try {
        callback();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

BrandedInstallerPage validPage() {
    BrandedInstallerPage page;
    page.windowTitle = L"Zanna Setup";
    page.eyebrow = L"DEVELOPER PLATFORM";
    page.heading = L"Choose an action";
    page.body = L"Select the next setup operation.";
    page.metadata = L"TRANSACTIONAL";
    page.actions = {
        {100, L"INSTALL", L"Install the selected tools."},
        {101, L"REPAIR", L"Repair the existing installation."},
    };
    page.defaultAction = 100;
    page.closeAction = IDCANCEL;
    page.cancelText = L"Cancel";
    return page;
}

} // namespace

int main() {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(static_cast<uintptr_t>(1));

    expect(!throwsValidation([&] { validateBrandedInstallerPage(instance, validPage()); }),
           "A complete branded page is accepted");
    expect(throwsValidation([&] { validateBrandedInstallerPage(nullptr, validPage()); }),
           "A null installer module is rejected");

    {
        BrandedInstallerPage page = validPage();
        page.actions[1].id = page.actions[0].id;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Duplicate action identifiers are rejected");
    }
    {
        BrandedInstallerPage page = validPage();
        page.actions[0].id = 0x10000;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Action identifiers that truncate through WM_COMMAND are rejected");
    }
    {
        BrandedInstallerPage page = validPage();
        page.actions[0].id = 3104;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Action identifiers cannot collide with internal controls");
    }
    {
        BrandedInstallerPage page = validPage();
        page.actions[0].title.clear();
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Nameless accessible actions are rejected");
    }
    {
        BrandedInstallerPage page = validPage();
        page.actions[0].description.assign(4000U, L'x');
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Oversized action text is rejected before native control creation");
    }
    {
        BrandedInstallerPage page = validPage();
        page.heading = std::wstring{L'a', L'\0', L'b'};
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Embedded NUL text cannot diverge from the visible native label");
    }
    {
        BrandedInstallerPage page = validPage();
        page.body = std::wstring{static_cast<wchar_t>(0xD800)};
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "Malformed UTF-16 is rejected");
    }
    {
        BrandedInstallerPage page = validPage();
        page.defaultAction = 999;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "The default action must identify a real action");
    }
    {
        BrandedInstallerPage page = validPage();
        page.closeAction = 999;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "The close result must be cancellation or a real action");
    }
    {
        BrandedInstallerPage page = validPage();
        page.actions[0].requiresVerification = true;
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "A verification-gated action requires an accessible checkbox");
    }
    {
        BrandedInstallerPage page = validPage();
        page.showCancel = true;
        page.cancelText.clear();
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "A visible cancel action must have a name");
    }
    {
        BrandedInstallerPage page = validPage();
        page.detailsText = L"Details";
        expect(throwsValidation([&] { validateBrandedInstallerPage(instance, page); }),
               "A details surface requires an accessible label");
    }

    const std::function<int()> work = [] { return 0; };
    expect(!throwsValidation([&] {
        validateBrandedInstallerProgress(
            instance, L"Zanna Setup", L"INSTALL", L"Working", L"Please wait.", work);
    }),
           "A complete progress model is accepted");
    expect(throwsValidation([&] {
               validateBrandedInstallerProgress(
                   instance, L"Zanna Setup", L"INSTALL", L"Working", L"Please wait.", {});
           }),
           "An empty progress callback is rejected before showing a window");
    expect(throwsValidation([&] {
               validateBrandedInstallerProgress(
                   instance, L"", L"INSTALL", L"Working", L"Please wait.", work);
           }),
           "A progress window requires an accessible title");
    expect(throwsValidation([&] {
               validateBrandedInstallerProgress(instance,
                                                L"Zanna Setup",
                                                L"INSTALL",
                                                std::wstring(5000U, L'x'),
                                                L"Please wait.",
                                                work);
           }),
           "Progress presentation text is bounded");

    std::cout << testsPassed << "/" << testsRun << " tests passed\n";
    return testsPassed == testsRun ? 0 : 1;
}
