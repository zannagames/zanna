//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/MacOSEntitlements.hpp
// Purpose: Merge store-required boolean entitlements into a project's macOS
//          entitlements property list before code signing.
// Key invariants:
//   - Only XML property lists are read; binary property lists are refused
//     with a conversion hint.
//   - Every existing key and value is preserved; required keys that are
//     absent are appended as <true/>, and existing required keys must already
//     be <true/>.
//   - The top-level value must be a dictionary without duplicate keys.
// Ownership/Lifetime:
//   - Pure functions over caller-owned text; results are owned values.
// Links: StoreDepotBuilder.hpp, MacOSPackageBuilder.hpp,
//        docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares macOS entitlements merging for store packaging.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zanna::pkg {

/// @brief Result of merging required entitlements into a property list.
struct EntitlementsMergeResult {
    std::string xml; ///< Complete XML property list for `codesign --entitlements`.
    std::vector<std::string> addedKeys; ///< Required keys that were absent and were added as true.
};

/// @brief Merge required boolean entitlements into entitlements property list text.
/// @details Diagnostics use the forms documented in ADR 0354:
///          - "macOS entitlements '<label>' is a binary property list; convert it with
///            'plutil -convert xml1'"
///          - "macOS entitlements '<label>' is not a valid XML property list: <detail>"
///          - "macOS entitlements '<label>' set <key> to <value>, but <store> requires true"
///          - "macOS entitlements '<label>' enable <key>, which <store> does not support"
/// @param sourceText Existing property list text; empty text means an empty dictionary.
/// @param sourceLabel File name used in diagnostics.
/// @param requiredTrue Keys that must be true in the result.
/// @param forbiddenTrue Keys that must not be true.
/// @param storeName Store display name used in diagnostics, for example "Steam".
/// @return Merged property list and the keys that were added.
/// @throws std::runtime_error on unreadable, malformed, or conflicting entitlements.
EntitlementsMergeResult mergeMacOSEntitlements(std::string_view sourceText,
                                               const std::string &sourceLabel,
                                               const std::vector<std::string> &requiredTrue,
                                               const std::vector<std::string> &forbiddenTrue,
                                               const std::string &storeName);

} // namespace zanna::pkg
