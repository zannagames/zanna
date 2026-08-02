//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/WindowsPackageBuilder.hpp
// Purpose: Assemble a Windows self-extracting installer .exe from a compiled
//          native binary and package assets.
//
// Key invariants:
//   - Output is a valid PE32+ executable with a stored ZIP bootstrap overlay.
//   - The main application/toolchain payload is a DEFLATE-compressed inner ZIP,
//     while small bootstrap files are stored for direct extraction by the stub.
//   - Overlay data contains the compressed payload, shortcuts, and a packaged
//     uninstaller PE.
//   - PE .text section contains a native installer stub for the requested
//     Windows architecture, writes uninstall metadata, and installs shortcuts.
//   - Resource section embeds RT_MANIFEST for UAC elevation.
//   - Overlay data (ZIP) is appended after the last PE section.
//
// Ownership/Lifetime:
//   - Single-use builder functions.
//
// Links: PEBuilder.hpp, ZipWriter.hpp, LnkWriter.hpp, IconGenerator.hpp,
//        PackageConfig.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares native Windows application and toolchain installer builders.
/// @details Both builders produce self-extracting PE32+ setup executables backed
///          by a deterministic metadata contract and nested ZIP payload.

#include "PackageConfig.hpp"
#include "ToolchainInstallManifest.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::pkg {

/// @brief Validate a Windows package version and extract its VERSIONINFO core.
/// @param version Bounded SemVer-compatible version with one to four numeric components.
/// @return Four 16-bit components with omitted trailing values zero-filled.
/// @throws std::runtime_error If the version cannot be consumed by the Windows installer.
std::array<uint16_t, 4> windowsVersionPartsForResource(std::string_view version);

/// @brief Transform one complete PE image into its signed form before packaging.
/// @details The logical name is an install-relative path used for diagnostics and
///          temporary filenames. Implementations return the complete signed bytes
///          and must preserve the image architecture. An empty callback selects
///          unsigned developer-package behavior.
using WindowsPeSigner = std::function<std::vector<uint8_t>(std::string_view logicalName,
                                                           const std::vector<uint8_t> &unsignedPe)>;

/// @brief Parameters for building a Windows self-extracting installer.
struct WindowsBuildParams {
    std::string projectName;          ///< Project name (used for display name fallback).
    std::string version;              ///< Version string (e.g. "1.2.0").
    std::string executablePath;       ///< Path to compiled native .exe binary.
    std::string projectRoot;          ///< Project root directory (for resolving assets).
    PackageConfig pkgConfig;          ///< Package manifest configuration.
    std::string outputPath;           ///< Output .exe path.
    std::string archStr;              ///< Payload architecture string ("x64" or "arm64").
    std::string installerHostPath;    ///< Statically linked native installer host template.
    std::string installerCleanupPath; ///< Statically linked detached cleanup helper template.
    WindowsPeSigner peSigner;         ///< Optional nested PE signer applied before payload hashing.
};

/// @brief Build a Windows self-extracting installer .exe.
///
/// Creates a PE32+ executable containing:
/// 1. PE headers with RT_MANIFEST resource (UAC elevation).
/// 2. x64 .text stub implementing installation logic.
/// 3. ZIP overlay containing a compressed inner payload, shortcuts, and the
///    generated uninstaller.
///
/// The outer ZIP bootstrap is structured as:
///   meta/payload.zip           - compressed install-root payload
///   meta/install_manifest.next - next installed-file manifest for upgrades
///   meta/start_menu.lnk        - Start Menu shortcut (if enabled)
///   meta/desktop.lnk           - Desktop shortcut (if enabled)
///
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildWindowsPackage(const WindowsBuildParams &params);

/// @brief Return imported DLL names from a PE32+ import table.
///
/// Returns an empty vector when the input is not a supported PE32+ image or the
/// import directory cannot be parsed safely.
/// @param data Complete candidate PE bytes.
/// @return Lowercase imported DLL names, or an empty vector when unavailable.
std::vector<std::string> importedDllNamesFromPe(const std::vector<uint8_t> &data);

/// @brief Parameters for building a Windows toolchain installer from a staged manifest.
struct WindowsToolchainBuildParams {
    ToolchainInstallManifest
        manifest;               ///< Staged file list produced by gatherToolchainInstallManifest.
    std::string outputPath;     ///< Output .exe path for the installer.
    std::string archStr{"x64"}; ///< Payload architecture ("x64" or "arm64").
    std::string displayName{
        "Zanna Toolchain"}; ///< Human-readable product name shown in Add/Remove Programs.
    std::string publisher{
        "Zanna Project"}; ///< Publisher string written to the uninstall registry key.
    std::string identifier{
        "org.zanna.toolchain"}; ///< Unique product identifier used as the registry key name.
    std::string installDirName{"Zanna"}; ///< Directory name under the selected install root.
    std::string homepage{"https://github.com/zannagames/zanna"}; ///< Support/update URL.
    std::string documentationUrl;         ///< Documentation URL; defaults to homepage.
    std::string updateManifestUrl;        ///< Optional HTTPS signed update-manifest URL.
    std::string updateRsaModulus;         ///< RSA public modulus for update signature verification.
    std::string updateRsaExponent;        ///< RSA public exponent as lowercase big-endian hex.
    std::string releaseChannel{"stable"}; ///< Package update channel.
    std::string sourceCommit; ///< Lowercase source revision embedded in package metadata.
    std::string installScope{
        "user"};          ///< "user" for LocalAppData/HKCU, "machine" for ProgramFiles/HKLM.
    bool addToPath{true}; ///< Add bin/ to the selected PATH registry value.
    bool registerFileAssociations{true}; ///< Register .zia/.bas/.il file associations.
    bool createStartMenuShortcuts{true}; ///< Create Zanna developer shortcuts in the Start Menu.
    std::string installerHostPath; ///< Native host template; auto-detected in staged bin/ if empty.
    std::string installerCleanupPath; ///< Detached cleanup helper; auto-detected in staged bin/.
    WindowsPeSigner peSigner; ///< Optional signer for Zanna-owned payload PEs and uninstall.exe.
};

/// @brief Build a Windows toolchain installer .exe from a staged install manifest.
///
/// Packages every staged file into a compressed inner ZIP carried by a PE32+
/// self-extracting stub. The stub installs files under the selected Zanna install
/// root, writes an uninstall registry key, updates PATH, and creates Start Menu
/// shortcuts when requested. All required toolchain components (zanna binary,
/// CMake config, runtime archives) must already be validated in the manifest by
/// validateToolchainInstallManifest before calling this function.
///
/// @param params Build parameters.
/// @throws std::runtime_error on any I/O, validation, or PE assembly failure.
void buildWindowsToolchainInstaller(const WindowsToolchainBuildParams &params);

} // namespace zanna::pkg
