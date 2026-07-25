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

#pragma once

#include "WindowsInstallerBrandDialog.hpp"

#include <functional>
#include <string_view>

namespace zanna::installer {

/// @brief Validate a branded page before any native resource is allocated.
void validateBrandedInstallerPage(HINSTANCE instance, const BrandedInstallerPage &page);

/// @brief Validate progress presentation and work before creating a native window.
void validateBrandedInstallerProgress(HINSTANCE instance,
                                      std::wstring_view windowTitle,
                                      std::wstring_view eyebrow,
                                      std::wstring_view heading,
                                      std::wstring_view body,
                                      const std::function<int()> &work);

} // namespace zanna::installer
