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

/// @file
/// @brief Implements fail-closed validation for branded Windows installer models.
/// @details Text, identifiers, action relationships, and progress callbacks are
///          checked before the native dialog layer allocates any resources.

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

/// @brief Test whether a UTF-16 code unit begins a surrogate pair.
/// @param value Code unit to classify.
/// @return `true` when @p value is in the high-surrogate range.
bool isHighSurrogate(wchar_t value) noexcept {
    const unsigned codeUnit = static_cast<unsigned>(value);
    return codeUnit >= 0xD800U && codeUnit <= 0xDBFFU;
}

/// @brief Test whether a UTF-16 code unit completes a surrogate pair.
/// @param value Code unit to classify.
/// @return `true` when @p value is in the low-surrogate range.
bool isLowSurrogate(wchar_t value) noexcept {
    const unsigned codeUnit = static_cast<unsigned>(value);
    return codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU;
}

/// @brief Validate one native installer text field before creating controls.
/// @details Enforces required-field and length constraints, rejects embedded
///          NULs, and verifies that every UTF-16 surrogate is correctly paired.
/// @param field Human-readable field name included in validation errors.
/// @param value Borrowed UTF-16 text to validate.
/// @param maximumUnits Maximum permitted number of UTF-16 code units.
/// @param required Whether an empty value is invalid.
/// @throws std::runtime_error If any text constraint is violated.
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

/// @brief Test whether a branded page declares an action with a given identifier.
/// @param page Page model whose action list is searched.
/// @param id Native control identifier to locate.
/// @return `true` when one action has identifier @p id.
bool containsAction(const BrandedInstallerPage &page, int id) noexcept {
    /// @brief Test whether one page action has the requested control identifier.
    /// @param action Action definition to inspect.
    /// @return `true` when the action identifier equals the captured identifier.
    return std::any_of(page.actions.begin(), page.actions.end(), [id](const auto &action) {
        return action.id == id;
    });
}

/// @brief Test whether an identifier belongs to cancellation or internal controls.
/// @param id Native control identifier to classify.
/// @return `true` when callers must not assign @p id to a page action.
bool isReservedControlId(int id) noexcept {
    return id == IDCANCEL ||
           std::find(kInternalControlIds.begin(), kInternalControlIds.end(), id) !=
               kInternalControlIds.end();
}

} // namespace

/// @brief Validate a branded installer page before allocating native resources.
/// @details Requires a module instance and a bounded, unambiguous action model;
///          validates all visible text and ensures default, close, and
///          verification behavior refers to declared actions.
/// @param instance Module instance that will own the native dialog.
/// @param page Borrowed branded page model to validate.
/// @throws std::runtime_error If the model cannot be represented safely.
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

/// @brief Validate the branded progress page and its work callback.
/// @details Applies the same bounded UTF-16 rules used by ordinary branded
///          pages and requires both a native module instance and executable
///          work before window creation begins.
/// @param instance Module instance that will own the native progress dialog.
/// @param windowTitle Native window title.
/// @param eyebrow Optional short context label above the heading.
/// @param heading Required primary progress heading.
/// @param body Optional explanatory body text.
/// @param work Callback that performs the installer operation.
/// @throws std::runtime_error If required state is missing or text is invalid.
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
