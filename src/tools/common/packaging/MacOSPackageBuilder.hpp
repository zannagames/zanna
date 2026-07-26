//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/MacOSPackageBuilder.hpp
// Purpose: Build macOS .app bundles packaged inside a ZIP archive.
//
// Key invariants:
//   - Produces a ZIP containing a valid .app bundle structure.
//   - Executable has Unix mode 0755 in ZIP external attributes.
//   - All other files have 0644, directories have 040755.
//   - ICNS icon is generated from source PNG if package-icon is specified.
//
// Ownership/Lifetime:
//   - Free functions stage temporary trees and write one requested artifact.
//
// Links: ZipWriter.hpp, PlistGenerator.hpp, PkgPNG.hpp, PackageConfig.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares macOS application and toolchain package construction entry points.
/// @details Supports application ZIP/DMG output and native toolchain PKG/DMG
///          installers with metadata, permissions, signing, and presentation assets.

#pragma once

#include "PackageConfig.hpp"
#include "ToolchainInstallManifest.hpp"

#include <string>

namespace zanna::pkg {

/// @brief Parameters for building a macOS .app-in-.zip package.
struct MacOSBuildParams {
    std::string projectName;    ///< Project name used to derive bundle and executable names.
    std::string version;        ///< Dotted-numeric bundle version; empty defaults to `0.0.0`.
    std::string executablePath; ///< Path to the compiled native application binary.
    std::string projectRoot;    ///< Absolute trusted root used to resolve assets and entitlements.
    PackageConfig pkgConfig;    ///< Manifest-derived bundle, signing, and DMG configuration.
    std::string outputPath;     ///< Destination ZIP or DMG artifact path.
};

/// @brief Build a macOS .app bundle inside a ZIP archive.
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildMacOSPackage(const MacOSBuildParams &params);

/// @brief Build a macOS .app bundle wrapped in a drag-to-install .dmg (macOS-only, hdiutil).
/// @details Stages and signs the bundle exactly like buildMacOSPackage, then wraps it (with an
///          /Applications symlink) into a compressed .dmg. `params.outputPath` is the .dmg path.
/// @param params Build parameters.
/// @throws std::runtime_error on failure or when run off macOS.
void buildMacOSAppDmg(const MacOSBuildParams &params);

/// @brief Parameters for building a macOS toolchain installer package.
struct MacOSToolchainBuildParams {
    ToolchainInstallManifest manifest; ///< Staged files, architecture, and release metadata.
    std::string outputPath;            ///< Destination flat product-package path.
    std::string identifier{"org.zanna.toolchain"}; ///< Component and product receipt identifier.
    std::string displayName{"Zanna Toolchain"}; ///< User-visible Installer.app product name.
    std::string packageVersion; ///< Optional dotted-numeric override for installer metadata.
    std::string minimumMacOSVersion; ///< Optional minimum OS; defaults by payload architecture.
    std::string licenseFilePath; ///< Optional installer license pane text; otherwise auto-selected.
    std::string backgroundImagePath; ///< Optional light-appearance installer background PNG.
    std::string applicationSignIdentity; ///< Developer ID identity used for nested Mach-O content.
};

/// @brief Build a macOS `.pkg` installer for the staged toolchain.
/// @param params Manifest, output path, identifier, and display metadata.
/// @throws std::runtime_error on failure.
void buildMacOSToolchainPackage(const MacOSToolchainBuildParams &params);

/// @brief Parameters for wrapping a built toolchain `.pkg` in a styled `.dmg` disk image.
struct MacOSToolchainDmgParams {
    std::string pkgPath;       ///< Path to the already-built, non-empty installer package.
    std::string outputPath;    ///< Destination compressed disk-image path.
    std::string volumeName{"Zanna Toolchain"}; ///< Mounted-volume and Finder-window title.
    std::string pkgDisplayName{"Zanna Toolchain.pkg"}; ///< Installer leaf name inside the image.
    std::string backgroundPng; ///< Optional absolute Finder-window background PNG path.
    std::string volumeIcns;    ///< Optional absolute volume-icon ICNS path.
};

/// @brief Wrap a built toolchain `.pkg` in a compressed, styled `.dmg` ("double-click to install").
/// @details macOS-only: shells to `hdiutil`, with best-effort `osascript`/`SetFile` styling so a
///          headless run still yields a valid image.
/// @param params Input/output paths, names, and optional styling assets.
/// @throws std::runtime_error on failure or when run off macOS.
void buildMacOSToolchainDmg(const MacOSToolchainDmgParams &params);

} // namespace zanna::pkg
