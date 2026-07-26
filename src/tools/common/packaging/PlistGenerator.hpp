//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PlistGenerator.hpp
// Purpose: Generate macOS Info.plist XML for .app bundles.
//
// Key invariants:
//   - Output is valid XML conforming to Apple's PropertyList-1.0 DTD.
//   - CFBundleExecutable must match the actual binary filename.
//   - NSHighResolutionCapable is always set to true.
//
// Ownership/Lifetime:
//   - Pure function, no state.
//
// Links: PackageConfig.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares macOS `Info.plist` and `PkgInfo` generation interfaces.

#include "PackageConfig.hpp"

#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Parameters for Info.plist generation.
/// @details Optional strings may be empty; required fields and file-association
///          metadata are validated before any XML is returned.
struct PlistParams {
    std::string executableName;              ///< Binary filename in Contents/MacOS/
    std::string bundleId;                    ///< CFBundleIdentifier (reverse DNS)
    std::string bundleName;                  ///< CFBundleName (display name)
    std::string version;                     ///< CFBundleVersion
    std::string iconFile;                    ///< CFBundleIconFile (without .icns extension)
    std::string minOsVersion;                ///< LSMinimumSystemVersion (default "10.13")
    std::string appCategory;                 ///< Optional LSApplicationCategoryType value.
    std::vector<FileAssoc> fileAssociations; ///< CFBundleDocumentTypes
};

/// @brief Generate an Info.plist XML string.
/// @details Emits required bundle identity keys, Retina support, a default
///          minimum macOS version, and optional icon/category/document metadata.
/// @param params Bundle metadata to validate, escape, and encode.
/// @return Complete PropertyList-1.0 XML content with a trailing newline.
/// @throws std::runtime_error If a field violates package metadata constraints.
std::string generatePlist(const PlistParams &params);

/// @brief Generate a PkgInfo file content (always "APPL????").
/// @return 8-byte PkgInfo content.
std::string generatePkgInfo();

} // namespace zanna::pkg
