//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerBrandValidation.hpp
// Purpose: Declare fail-closed validation for branded native installer models.
//
// Key invariants:
//   - Native control identifiers are unique, representable, and non-reserved.
//   - Every native string is bounded, NUL-free, and valid UTF-16.
//   - Modal default, close, verification, and work actions are unambiguous.
//
// Ownership/Lifetime:
//   - Validation borrows page text and callbacks only for the duration of each call.
//
// Links: WindowsInstallerBrandValidation.cpp, WindowsInstallerBrandDialog.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares fail-closed validation for branded Windows installer models.
/// @details The validators reject malformed UTF-16, unsafe control identifiers,
///          inconsistent actions, and missing execution state before any native
///          dialog resources are allocated.

#pragma once

#include "WindowsInstallerBrandDialog.hpp"

#include <functional>
#include <string_view>

namespace zanna::installer {

/// @brief Validate a branded page before any native resource is allocated.
/// @param instance Module instance that will own the native dialog.
/// @param page Borrowed page model whose text and action relationships are checked.
/// @throws std::runtime_error If the page is incomplete, ambiguous, or unsafe.
void validateBrandedInstallerPage(HINSTANCE instance, const BrandedInstallerPage &page);

/// @brief Validate progress presentation and work before creating a native window.
/// @param instance Module instance that will own the native progress dialog.
/// @param windowTitle Native window title.
/// @param eyebrow Optional context label displayed above the heading.
/// @param heading Required primary heading.
/// @param body Optional explanatory text.
/// @param work Callback that performs the installer operation.
/// @throws std::runtime_error If required state is missing or text is invalid.
void validateBrandedInstallerProgress(HINSTANCE instance,
                                      std::wstring_view windowTitle,
                                      std::wstring_view eyebrow,
                                      std::wstring_view heading,
                                      std::wstring_view body,
                                      const std::function<int()> &work);

} // namespace zanna::installer
