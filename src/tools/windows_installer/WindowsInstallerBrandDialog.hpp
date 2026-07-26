//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the branded native action and cooperative-progress surfaces
///        used by Windows setup.
///
/// Actions remain keyboard-focusable native controls whose text includes title
/// and description. Small work areas use bounded native scrolling. Page models
/// are borrowed only during modal calls; progress workers are joined and logger
/// callbacks are cleared before returning.
///
/// @see WindowsInstallerBrandDialog.cpp
/// @see WindowsInstallerTheme.hpp
/// @see WindowsInstallerWizard.cpp
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
    int id{0};                         ///< Unique nonzero dialog command identifier.
    std::wstring title;                ///< Primary action label.
    std::wstring description;          ///< Secondary accessible action explanation.
    InstallerAccent accent{InstallerAccent::Teal}; ///< Owner-drawn accent color.
    bool requiresVerification{false};  ///< Whether verification must be checked.
};

/// @brief Complete content model for one branded modal setup page.
struct BrandedInstallerPage {
    std::wstring windowTitle;          ///< Native window caption.
    std::wstring eyebrow;              ///< Short brand/context label.
    std::wstring heading;              ///< Primary page heading.
    std::wstring body;                 ///< Explanatory page copy.
    std::wstring metadata;             ///< Compact package/version metadata.
    std::vector<BrandedInstallerAction> actions; ///< Ordered selectable actions.
    int defaultAction{0};              ///< Action activated by the default button.
    int closeAction{IDCANCEL};         ///< Result used when the window closes.
    std::wstring detailsLabel;         ///< Label above optional read-only details.
    std::wstring detailsText;          ///< Optional multiline detail contents.
    std::wstring verificationText;     ///< Optional verification checkbox label.
    std::wstring cancelText{L"Cancel"}; ///< Cancel-button label.
    bool showCancel{true};             ///< Whether to create a cancel button.
};

/// @brief Result collected from a branded modal setup page.
struct BrandedInstallerPageResult {
    int action{IDCANCEL};              ///< Selected action or close action.
    bool verificationChecked{false};   ///< Final verification-checkbox state.
};

/// @brief Show one dark Zanna Games setup page.
/// @param instance Module instance used for native resources.
/// @param page Borrowed validated content/action model.
/// @return Selected action and verification state.
/// @throws std::runtime_error On model validation, native creation, or queue errors.
BrandedInstallerPageResult showBrandedInstallerPage(HINSTANCE instance,
                                                    const BrandedInstallerPage &page);

/// @brief Run lifecycle work behind the branded cooperative progress surface.
/// @param instance Module instance used for native resources.
/// @param windowTitle Native window caption.
/// @param eyebrow Short brand/context label.
/// @param heading Primary operation heading.
/// @param body Explanatory operation copy.
/// @param logger Logger receiving temporary progress/cancellation callbacks.
/// @param work Operation executed on a worker thread.
/// @return Operation exit code.
/// @throws std::runtime_error On validation or native UI errors; propagates
///         exceptions raised by @p work after joining.
int runBrandedInstallerProgress(HINSTANCE instance,
                                std::wstring_view windowTitle,
                                std::wstring_view eyebrow,
                                std::wstring_view heading,
                                std::wstring_view body,
                                Logger &logger,
                                const std::function<int()> &work);

} // namespace zanna::installer
