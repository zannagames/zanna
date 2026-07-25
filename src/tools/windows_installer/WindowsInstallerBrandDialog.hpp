//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerBrandDialog.hpp
// Purpose: Declare the branded native choice and progress surfaces for setup.
//
// Key invariants:
//   - Every actionable surface remains a native keyboard-focusable control.
//   - Accessible control text contains both the action title and description.
//   - Small work areas remain reachable through bounded native scrolling.
//   - Progress work stays off the UI thread and cancellation remains cooperative.
//
// Ownership/Lifetime:
//   - Page models are borrowed only for the duration of their modal call.
//   - Progress callbacks and worker threads are cleared and joined before return.
//
// Links: WindowsInstallerBrandDialog.cpp, WindowsInstallerTheme.hpp,
//        WindowsInstallerWizard.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "WindowsInstallerHost.hpp"
#include "WindowsInstallerTheme.hpp"

#include <functional>
#include <string>
#include <vector>

namespace zanna::installer {

/// @brief One keyboard-accessible action card on a branded setup page.
struct BrandedInstallerAction {
    int id{0};
    std::wstring title;
    std::wstring description;
    InstallerAccent accent{InstallerAccent::Teal};
    bool requiresVerification{false};
};

/// @brief Complete content model for one branded modal setup page.
struct BrandedInstallerPage {
    std::wstring windowTitle;
    std::wstring eyebrow;
    std::wstring heading;
    std::wstring body;
    std::wstring metadata;
    std::vector<BrandedInstallerAction> actions;
    int defaultAction{0};
    int closeAction{IDCANCEL};
    std::wstring detailsLabel;
    std::wstring detailsText;
    std::wstring verificationText;
    std::wstring cancelText{L"Cancel"};
    bool showCancel{true};
};

/// @brief Result collected from a branded modal setup page.
struct BrandedInstallerPageResult {
    int action{IDCANCEL};
    bool verificationChecked{false};
};

/// @brief Show one dark Zanna Games setup page.
BrandedInstallerPageResult showBrandedInstallerPage(HINSTANCE instance,
                                                    const BrandedInstallerPage &page);

/// @brief Run lifecycle work behind the branded cooperative progress surface.
int runBrandedInstallerProgress(HINSTANCE instance,
                                std::wstring_view windowTitle,
                                std::wstring_view eyebrow,
                                std::wstring_view heading,
                                std::wstring_view body,
                                Logger &logger,
                                const std::function<int()> &work);

} // namespace zanna::installer
