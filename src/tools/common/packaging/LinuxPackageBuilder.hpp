//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/LinuxPackageBuilder.hpp
// Purpose: Build Linux .deb packages and .tar.gz archives from scratch.
//
// Key invariants:
//   - .deb = ar(debian-binary + control.tar.gz + data.tar.gz).
//   - .tar.gz uses FHS-compliant paths (/usr/bin, /usr/share, etc.).
//   - All format bytes emitted directly — no dpkg-deb or tar dependency.
//   - md5sums file contains hex digest + two-space + path for every data file.
//
// Ownership/Lifetime:
//   - Free functions consume caller-provided paths and write one requested artifact.
//
// Links: ArWriter.hpp, TarWriter.hpp, PkgGzip.hpp, PkgMD5.hpp,
//        DesktopEntryGenerator.hpp, PackageConfig.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares Linux application and toolchain package construction entry points.
/// @details Supports dependency-free DEB, tarball, and self-extracting bundle
///          assembly, plus RPM construction/signing through standard host tools.

#pragma once

#include "PackageConfig.hpp"
#include "ToolchainInstallManifest.hpp"

#include <string>

namespace zanna::pkg {

/// @brief Parameters for building a Linux .deb or .tar.gz package.
struct LinuxBuildParams {
    std::string projectName;    ///< Project name used to derive package and executable names.
    std::string version;        ///< Package version, defaulted to `0.0.0` when allowed and empty.
    std::string executablePath; ///< Path to the compiled native application binary.
    std::string projectRoot;    ///< Absolute project root used to resolve configured assets.
    PackageConfig pkgConfig;    ///< Manifest-derived package metadata and integration settings.
    std::string outputPath;     ///< Destination artifact path.
    std::string archStr;        ///< Format-specific architecture (`amd64`/`arm64` or `x64`/`arm64`).
};

/// @brief Build a Debian .deb package.
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildDebPackage(const LinuxBuildParams &params);

/// @brief Build a portable .tar.gz archive.
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildTarball(const LinuxBuildParams &params);

/// @brief Build a self-extracting Linux `.run` bundle for an end-user application.
/// @details Lays the payload out as a portable tree (app binary at `usr/bin/<exe>`
///          with an `AppRun` symlink entry point, bundled assets under
///          `usr/share/<pkg>/`, and a `.desktop` launcher plus icon at the payload
///          root), then wraps it in the shared FUSE-less self-extracting runtime
///          stub. `params.archStr` must be the portable form ("x64" or "arm64").
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildAppImage(const LinuxBuildParams &params);

/// @brief Build an RPM package for an end-user application (requires rpmbuild on PATH).
/// @details Reuses the shared FHS layout from the Debian application builder. Throws
///          a clear diagnostic when rpmbuild is unavailable. `params.archStr` must be
///          the portable form ("x64" or "arm64").
/// @param params Build parameters.
/// @throws std::runtime_error on failure.
void buildRpmPackage(const LinuxBuildParams &params);

/// @brief GPG-sign a built Debian (.deb) or RPM (.rpm) package in place.
/// @details Shells out to the standard signing tool (rpmsign for RPM, dpkg-sig for
///          Debian), mirroring the macOS/Windows external-tool signing approach.
///          Throws a clear diagnostic when the tool is not on PATH or signing fails.
/// @param packagePath Path to the built package (signed in place).
/// @param gpgKeyId GPG key id or name to sign with.
/// @param isRpm True to sign an RPM, false to sign a Debian package.
/// @throws std::runtime_error on failure.
void signLinuxPackage(const std::string &packagePath, const std::string &gpgKeyId, bool isRpm);

/// @brief Parameters for building Linux toolchain packages from a staged install tree.
struct LinuxToolchainBuildParams {
    ToolchainInstallManifest manifest; ///< Validated staged file inventory and release metadata.
    std::string outputPath;            ///< Destination artifact path.
    std::string packageName{"zanna"};  ///< Package/base name, normalized by the selected format.
};

/// @brief Build a Debian toolchain package from a staged install manifest.
/// @param params Manifest, output path, and package name.
/// @throws std::runtime_error on failure.
void buildToolchainDebPackage(const LinuxToolchainBuildParams &params);

/// @brief Build an RPM toolchain package from a staged install manifest.
/// @param params Manifest, output path, and package name.
/// @throws std::runtime_error on failure.
void buildToolchainRpmPackage(const LinuxToolchainBuildParams &params);

/// @brief Build a portable toolchain tarball from a staged install manifest.
/// @param params Manifest, output path, and package name.
/// @throws std::runtime_error on failure.
void buildToolchainTarball(const LinuxToolchainBuildParams &params);

/// @brief Build a self-extracting Linux `.run` bundle from a staged install manifest.
/// @param params Manifest, output path, and package name.
/// @throws std::runtime_error on failure.
void buildToolchainBundle(const LinuxToolchainBuildParams &params);

} // namespace zanna::pkg
