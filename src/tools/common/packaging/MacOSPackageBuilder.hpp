//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/MacOSPackageBuilder.hpp
// Purpose: Build macOS .app bundles packaged inside a ZIP archive or disk
//          image, or staged into a directory for store depots.
//
// Key invariants:
//   - Produces a ZIP containing a valid .app bundle structure.
//   - Executable has Unix mode 0755 in ZIP external attributes.
//   - All other files have 0644, directories have 040755.
//   - ICNS icon is generated from source PNG if package-icon is specified.
//   - Generated .zpak packs are copied into Contents/Resources (ADR 0355).
//   - Nested code added to a directory-staged bundle is signed before the
//     bundle that contains it.
//
// Ownership/Lifetime:
//   - Free functions stage temporary trees and write one requested artifact.
//
// Links: ZipWriter.hpp, PlistGenerator.hpp, PkgPNG.hpp, PackageConfig.hpp,
//        MacOSEntitlements.hpp, StoreDepotBuilder.hpp
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
#include <vector>

namespace zanna::pkg {

/// @brief Parameters for building a macOS .app-in-.zip package.
struct MacOSBuildParams {
    std::string projectName;    ///< Project name used to derive bundle and executable names.
    std::string version;        ///< Dotted-numeric bundle version; empty defaults to `0.0.0`.
    std::string executablePath; ///< Path to the compiled native application binary.
    std::string projectRoot;    ///< Absolute trusted root used to resolve assets and entitlements.
    PackageConfig pkgConfig;    ///< Manifest-derived bundle, signing, and DMG configuration.
    std::string outputPath;     ///< Destination ZIP or DMG artifact path.
    /// Generated `.zpak` pack groups (trusted paths) copied into `Contents/Resources`, where the
    /// runtime mounts them at startup (ADR 0355).
    std::vector<std::string> packFiles;
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

/// @brief One file copied into a staged application bundle before signing.
struct MacOSBundleExtraFile {
    std::string sourcePath;         ///< Trusted absolute source file.
    std::string bundleRelativePath; ///< Destination below the bundle, starting with `Contents/`.
    bool nestedCode{false}; ///< Sign the file individually before the bundle (dylibs, helpers).
};

/// @brief Parameters for staging an application bundle into a directory (store depots).
struct MacOSAppDirectoryParams {
    MacOSBuildParams app; ///< Bundle metadata, executable, assets, and signing; outputPath unused.
    std::string destinationDir; ///< Existing directory that receives `<Name>.app`.
    std::vector<MacOSBundleExtraFile> extraFiles;   ///< Files added before signing.
    std::vector<std::string> requiredEntitlements;  ///< Keys forced to true when signing.
    std::vector<std::string> forbiddenEntitlements; ///< Keys that must not be true when signing.
    std::string entitlementsOwner; ///< Name used in entitlement diagnostics, for example "Steam".
};

/// @brief Result of staging an application bundle into a directory.
struct MacOSAppDirectoryResult {
    std::string appPath;                ///< Absolute path of the staged `<Name>.app`.
    std::string executableRelativePath; ///< Main executable relative to the destination directory.
    bool bundleSigned{false};           ///< Whether the bundle was signed (adhoc or developer-id).
    std::vector<std::string>
        addedEntitlements; ///< Required keys added to the project entitlements.
};

/// @brief Stage an application bundle into a directory and sign it with nested code first.
/// @details Uses the same staging as buildMacOSPackage (Info.plist, PkgInfo, icon, assets,
///          packs), then copies @ref MacOSAppDirectoryParams::extraFiles.
///          When the effective sign mode is `adhoc` or `developer-id`, every nested-code file
///          is signed with the bundle identity (with a secure timestamp for Developer ID),
///          the project entitlements are merged with the required keys, and the bundle is
///          signed, verified, and optionally notarized and stapled. Other sign modes leave
///          the bundle unsigned.
/// @param params Staging parameters.
/// @return Paths and signing facts of the staged bundle.
/// @throws std::runtime_error on validation, staging, entitlement, or signing failure.
MacOSAppDirectoryResult buildMacOSAppDirectory(const MacOSAppDirectoryParams &params);

/// @brief Parameters for building a macOS toolchain installer package.
struct MacOSToolchainBuildParams {
    ToolchainInstallManifest manifest; ///< Staged files, architecture, and release metadata.
    std::string outputPath;            ///< Destination flat product-package path.
    std::string identifier{"org.zanna.toolchain"}; ///< Component and product receipt identifier.
    std::string displayName{"Zanna Toolchain"};    ///< User-visible Installer.app product name.
    std::string packageVersion;      ///< Optional dotted-numeric override for installer metadata.
    std::string minimumMacOSVersion; ///< Optional minimum OS; defaults by payload architecture.
    std::string licenseFilePath; ///< Optional installer license pane text; otherwise auto-selected.
    std::string backgroundImagePath;     ///< Optional light-appearance installer background PNG.
    std::string applicationSignIdentity; ///< Developer ID identity used for nested Mach-O content.
};

/// @brief Build a macOS `.pkg` installer for the staged toolchain.
/// @param params Manifest, output path, identifier, and display metadata.
/// @throws std::runtime_error on failure.
void buildMacOSToolchainPackage(const MacOSToolchainBuildParams &params);

/// @brief Parameters for wrapping a built toolchain `.pkg` in a styled `.dmg` disk image.
struct MacOSToolchainDmgParams {
    std::string pkgPath;    ///< Path to the already-built, non-empty installer package.
    std::string outputPath; ///< Destination compressed disk-image path.
    std::string volumeName{"Zanna Toolchain"};         ///< Mounted-volume and Finder-window title.
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
