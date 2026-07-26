//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ToolchainInstallManifest.hpp
// Purpose: Gather and validate the staged Zanna toolchain install tree for
//          native installer packaging.
//
// Key invariants:
//   - The staged tree is the source of truth for shipped files.
//   - Relative paths are normalized and preserved for platform packagers.
//   - Validation fails fast when required toolchain files are missing.
//
// Ownership/Lifetime: Manifest structs are plain value types owning their own
//                     strings/paths; callers own returned manifests.
//
// Links: PackageConfig.hpp, PkgUtils.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares staged-toolchain manifest value types and layout policies.
/// @details The API discovers an installed tree, records owned file metadata,
///          validates required Zanna components, and maps entries into native
///          installer destinations.

#include "PackageConfig.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Classification of a file in the Zanna toolchain install tree.
/// Used by mapInstallPath to assign the correct platform-specific destination
/// directory (bin/, lib/, include/, share/, etc.) for each file type.
enum class ToolchainFileKind {
    Binary,         ///< Executable tool (installed to bin/).
    RuntimeArchive, ///< Packaged runtime archive (e.g. .a/.lib shipped to lib/).
    SupportLibrary, ///< Zanna support shared library (graphics/audio/etc.).
    Library,        ///< Generic library file.
    Header,         ///< C/C++ header (installed to include/).
    CMakeConfig,    ///< CMake package-config file (lib/cmake/...).
    ManPage,        ///< Man page (share/man/...).
    Doc,            ///< Documentation file (share/doc/...).
    Extra,          ///< Anything not otherwise classified.
};

/// @brief A single file or symlink entry in the toolchain install manifest.
struct ToolchainFileEntry {
    ToolchainFileKind kind{ToolchainFileKind::Extra}; ///< Classification driving the install path.
    std::filesystem::path stagedAbsolutePath;         ///< Full path in the staging directory.
    std::string stagedRelativePath;                   ///< Path relative to the staging root.
    uint64_t sizeBytes{0};                            ///< File size in bytes (0 for symlinks).
    uint32_t unixMode{0};                             ///< POSIX permission bits.
    bool executable{false};                           ///< True if the file should be +x on install.
    bool symlink{false};                              ///< True if this entry is a symbolic link.
    std::string symlinkTarget;                        ///< Non-empty only when symlink == true.
};

/// @brief The full Zanna toolchain install manifest for one (arch, platform) pair.
struct ToolchainInstallManifest {
    std::string version;                     ///< Package-compatible version (e.g. "0.2.5").
    std::string productVersion;              ///< Exact configured version, including prerelease.
    std::string snapshot;                    ///< Optional git-describe build identity.
    std::string sourceCommit;                ///< Optional lowercase source commit hash.
    std::string sourceState{"unknown"};      ///< "clean", "dirty", or "unknown".
    std::string arch;                        ///< Target architecture ("x64"/"arm64").
    std::string platform;                    ///< Target platform ("windows"/"macos"/"linux").
    std::string license{"GPL-3.0-only"};     ///< SPDX license id for package metadata.
    std::string maintainer{"Zanna Project"}; ///< Maintainer/packager display name.
    std::string maintainerEmail{"splanck@users.noreply.github.com"}; ///< Package contact email.
    std::string homepage{"https://github.com/zannagames/zanna"};     ///< Project homepage URL.
    std::vector<ToolchainFileEntry> files;   ///< All staged files and symlinks.
    std::vector<FileAssoc> fileAssociations; ///< File-type associations to register.

    /// @brief Sum of sizeBytes for all non-symlink entries.
    /// @return Total installed payload size in bytes.
    /// @throws std::overflow_error If the sum exceeds `uint64_t`.
    uint64_t totalSizeBytes() const;
};

/// @brief Destination layout policy for mapInstallPath.
enum class InstallPathPolicy {
    WindowsProgramFilesRoot, ///< Flat layout under %ProgramFiles%\Zanna
    MacOSUsrLocalZannaRoot,  ///< FHS-like layout under /usr/local/zanna/
    LinuxUsrRoot,            ///< FHS layout under /usr/ (bin/, lib/, include/, etc.)
    PortableArchive,         ///< Relative paths for a platform-neutral .zip or .tar.gz
};

/// @brief Walk stagePrefix and build a ToolchainInstallManifest from its contents.
/// If installManifestPath is given, the file at that path provides version/arch/platform
/// metadata; otherwise these fields are inferred from the staged directory tree.
/// @param stagePrefix Existing root of the staged installation.
/// @param installManifestPath Optional CMake-generated file inventory.
/// @return Validated manifest sorted by staged-relative path.
/// @throws std::runtime_error If discovery, containment, metadata, or validation fails.
ToolchainInstallManifest gatherToolchainInstallManifest(
    const std::filesystem::path &stagePrefix,
    std::optional<std::filesystem::path> installManifestPath = std::nullopt);

/// @brief Validate that a manifest contains all required toolchain binaries and
/// libraries. Throws std::runtime_error describing the first missing required entry.
/// @param manifest Candidate manifest, including live staged source paths.
/// @throws std::runtime_error On invalid metadata, paths, sizes, symlinks, or inventory.
void validateToolchainInstallManifest(const ToolchainInstallManifest &manifest);

/// @brief Return the canonical binary tools that every toolchain installer must ship.
/// @return Logical binary base names without platform-specific extensions.
std::vector<std::string> requiredToolchainBinaryNames();

/// @brief Map a toolchain file entry to its destination install path string under policy.
/// Returns a relative path (no leading "/") for all policies except LinuxUsrRoot which
/// returns a path rooted at "/usr/".
/// @param file Manifest entry to map.
/// @param policy Platform destination-layout convention.
/// @return Sanitized destination path for the selected policy.
/// @throws std::runtime_error If the recorded staged-relative path is unsafe.
std::string mapInstallPath(const ToolchainFileEntry &file, InstallPathPolicy policy);

/// @brief Return the default file associations for Zanna toolchain packages
/// (.zia and .bas source files).
/// @return Stable Zia, BASIC, and IL association descriptors.
std::vector<FileAssoc> defaultToolchainFileAssociations();

} // namespace zanna::pkg
