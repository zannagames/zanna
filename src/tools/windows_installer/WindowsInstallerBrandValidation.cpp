//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerBrandValidation.cpp
// Purpose: Validate branded native installer page and progress models.
//
// Key invariants:
//   - Native control identifiers survive WM_COMMAND without truncation.
//   - Visible and accessible text has one bounded, well-formed interpretation.
//   - A page can never publish an action that its model does not contain.
//
// Ownership/Lifetime:
//   - All input remains caller-owned and no validation state escapes a call.
//
// Links: WindowsInstallerBrandValidation.hpp, WindowsInstallerBrandDialog.cpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerBrandValidation.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace zanna::installer {
namespace {

constexpr size_t kMaximumWindowTitleUnits = 256U;
constexpr size_t kMaximumEyebrowUnits = 256U;
constexpr size_t kMaximumHeadingUnits = 1024U;
constexpr size_t kMaximumBodyUnits = 4096U;
constexpr size_t kMaximumMetadataUnits = 2048U;
constexpr size_t kMaximumDetailsLabelUnits = 256U;
constexpr size_t kMaximumDetailsTextUnits = 262144U;
constexpr size_t kMaximumVerificationUnits = 2048U;
constexpr size_t kMaximumCancelTextUnits = 128U;
constexpr size_t kMaximumActionTitleUnits = 160U;
constexpr size_t kMaximumActionDescriptionUnits = 320U;
constexpr size_t kMaximumActionCount = 12U;
constexpr int kMaximumNativeControlId = 0xFFFF;
constexpr std::array<int, 8> kInternalControlIds = {3101, 3102, 3103, 3104, 3105, 3106, 3107, 3108};

bool isHighSurrogate(wchar_t value) noexcept {
    const unsigned codeUnit = static_cast<unsigned>(value);
    return codeUnit >= 0xD800U && codeUnit <= 0xDBFFU;
}

bool isLowSurrogate(wchar_t value) noexcept {
    const unsigned codeUnit = static_cast<unsigned>(value);
    return codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU;
}

void validateText(std::string_view field,
                  std::wstring_view value,
                  size_t maximumUnits,
                  bool required) {
    if (required && value.empty())
        throw std::runtime_error(std::string(field) + " must not be empty");
    if (value.size() > maximumUnits)
        throw std::runtime_error(std::string(field) + " is too long");

    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t codeUnit = value[index];
        if (codeUnit == L'\0')
            throw std::runtime_error(std::string(field) + " contains an embedded NUL");
        if (isHighSurrogate(codeUnit)) {
            if (index + 1U >= value.size() || !isLowSurrogate(value[index + 1U]))
                throw std::runtime_error(std::string(field) + " contains malformed UTF-16");
            ++index;
        } else if (isLowSurrogate(codeUnit)) {
            throw std::runtime_error(std::string(field) + " contains malformed UTF-16");
        }
    }
}

bool containsAction(const BrandedInstallerPage &page, int id) noexcept {
    return std::any_of(page.actions.begin(), page.actions.end(), [id](const auto &action) {
        return action.id == id;
    });
}

bool isReservedControlId(int id) noexcept {
    return id == IDCANCEL ||
           std::find(kInternalControlIds.begin(), kInternalControlIds.end(), id) !=
               kInternalControlIds.end();
}

} // namespace

void validateBrandedInstallerPage(HINSTANCE instance, const BrandedInstallerPage &page) {
    if (!instance)
        throw std::runtime_error("branded setup page requires a module instance");
    if (page.actions.empty())
        throw std::runtime_error("branded setup page has no actions");
    if (page.actions.size() > kMaximumActionCount)
        throw std::runtime_error("branded setup page has too many actions");

    validateText("branded setup window title", page.windowTitle, kMaximumWindowTitleUnits, true);
    validateText("branded setup eyebrow", page.eyebrow, kMaximumEyebrowUnits, false);
    validateText("branded setup heading", page.heading, kMaximumHeadingUnits, true);
    validateText("branded setup body", page.body, kMaximumBodyUnits, false);
    validateText("branded setup metadata", page.metadata, kMaximumMetadataUnits, false);
    validateText(
        "branded setup details label", page.detailsLabel, kMaximumDetailsLabelUnits, false);
    validateText("branded setup details", page.detailsText, kMaximumDetailsTextUnits, false);
    validateText(
        "branded setup verification", page.verificationText, kMaximumVerificationUnits, false);
    validateText(
        "branded setup cancel action", page.cancelText, kMaximumCancelTextUnits, page.showCancel);
    if (!page.detailsText.empty() && page.detailsLabel.empty())
        throw std::runtime_error("branded setup details require an accessible label");

    std::unordered_set<int> actionIds;
    actionIds.reserve(page.actions.size());
    bool requiresVerification = false;
    for (const BrandedInstallerAction &action : page.actions) {
        if (action.id <= 0 || action.id > kMaximumNativeControlId ||
            isReservedControlId(action.id)) {
            throw std::runtime_error("branded setup action has a reserved or invalid identifier");
        }
        if (!actionIds.insert(action.id).second)
            throw std::runtime_error("branded setup action identifier is duplicated");
        validateText("branded setup action title", action.title, kMaximumActionTitleUnits, true);
        validateText("branded setup action description",
                     action.description,
                     kMaximumActionDescriptionUnits,
                     false);
        requiresVerification = requiresVerification || action.requiresVerification;
    }
    if (!containsAction(page, page.defaultAction))
        throw std::runtime_error("branded setup default action does not exist");
    if (page.closeAction != IDCANCEL && !containsAction(page, page.closeAction))
        throw std::runtime_error("branded setup close action does not exist");
    if (requiresVerification && page.verificationText.empty())
        throw std::runtime_error("branded setup verification action has no checkbox text");
}

void validateBrandedInstallerProgress(HINSTANCE instance,
                                      std::wstring_view windowTitle,
                                      std::wstring_view eyebrow,
                                      std::wstring_view heading,
                                      std::wstring_view body,
                                      const std::function<int()> &work) {
    if (!instance)
        throw std::runtime_error("branded setup progress requires a module instance");
    if (!work)
        throw std::runtime_error("branded setup progress requires work");
    validateText("branded progress window title", windowTitle, kMaximumWindowTitleUnits, true);
    validateText("branded progress eyebrow", eyebrow, kMaximumEyebrowUnits, false);
    validateText("branded progress heading", heading, kMaximumHeadingUnits, true);
    validateText("branded progress body", body, kMaximumBodyUnits, false);
}

} // namespace zanna::installer
