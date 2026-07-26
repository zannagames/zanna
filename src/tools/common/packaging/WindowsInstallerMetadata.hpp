//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/WindowsInstallerMetadata.hpp
// Purpose: Define the deterministic metadata exchanged by the Windows package
//          builder and the native installer/maintenance host.
//
// Key invariants:
//   - Schema 3 is line-oriented, UTF-8, deterministic, and strictly parsed.
//   - Every variable field is percent-escaped before tab-delimited encoding.
//   - Payload records carry the install-relative path, SHA-256, byte size, and
//     owning component needed for preflight, repair, and transactional removal.
//   - The schema contains no host paths or build timestamps.
//
// Ownership/Lifetime:
//   - Metadata values own all strings and vectors; parsers do not retain views.
//
// Links: WindowsPackageBuilder.cpp, WindowsInstallerHost.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares schema-3 native Windows installer metadata value types.
/// @details The schema is a deterministic, strictly validated contract shared
///          by package construction, setup, maintenance, repair, and removal.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::pkg {

/// @brief Current native Windows installer metadata schema.
inline constexpr uint32_t kWindowsInstallerMetadataSchema = 3;

/// @brief One selectable payload component shown by the native wizard.
struct WindowsInstallerComponentMetadata {
    std::string id;             ///< Stable lowercase identifier.
    std::string label;          ///< Accessible user-visible label.
    std::string description;    ///< Concise component purpose.
    bool required{false};       ///< Required components cannot be deselected.
    bool defaultSelected{true}; ///< Selection for a clean Typical install.
    uint64_t sizeBytes{0};      ///< Uncompressed payload size owned by the component.
};

/// @brief One file inside meta/payload.zip.
struct WindowsInstallerPayloadMetadata {
    std::string path;        ///< Validated install-relative slash path.
    std::string sha256;      ///< Lowercase SHA-256 of the uncompressed bytes.
    uint64_t sizeBytes{0};   ///< Uncompressed file size.
    std::string componentId; ///< Empty means the required core component.
};

/// @brief One shortcut generated at install time for the selected destination.
struct WindowsInstallerShortcutMetadata {
    std::string root;           ///< "desktop" or "start-menu".
    std::string relativePath;   ///< Root-relative destination path.
    std::string targetRoot;     ///< "install" or "windows".
    std::string targetPath;     ///< Root-relative target executable path.
    std::string workingRoot;    ///< "install", "profile", or "windows".
    std::string workingPath;    ///< Optional root-relative working directory.
    std::string argumentPrefix; ///< Optional fixed argument such as /k or /c.
    std::string argumentPath;   ///< Optional install-relative path passed as one quoted argument.
    std::string description;    ///< Accessible Shell Link description.
    std::string iconRoot;       ///< Empty, "install", or "windows".
    std::string iconPath;       ///< Optional root-relative icon path.
    int32_t iconIndex{0};       ///< Icon resource index.
    std::string componentId;    ///< Empty or the component controlling the shortcut.
};

/// @brief One ordinary installed file carried by a stored outer-ZIP entry.
/// @details This is used for the signed maintenance executable, which cannot be
///          recursively embedded in the repair payload it carries itself.
struct WindowsInstallerOuterFileMetadata {
    std::string overlayPath; ///< Entry in the outer ZIP containing the bytes.
    std::string path;        ///< Install-relative destination path.
    std::string sha256;      ///< Lowercase SHA-256 of the stored file bytes.
    uint64_t sizeBytes{0};   ///< Exact stored and installed byte count.
    std::string componentId; ///< Empty for core, otherwise the owning component id.
};

/// @brief One safe Open-With registration owned by the package.
struct WindowsInstallerAssociationMetadata {
    std::string extension; ///< Extension including the leading dot.
    std::string description; ///< User-visible file-type description.
    std::string mimeType;    ///< Optional registered media type.
    std::string progId;      ///< Stable registry class identifier.
    std::string arguments; ///< Optional arguments placed before the quoted file path.
};

/// @brief Complete package contract consumed by the native installer host.
struct WindowsInstallerMetadata {
    uint32_t schemaVersion{kWindowsInstallerMetadataSchema}; ///< Required schema number.
    std::string packageMode{"setup"};       ///< "setup" or "maintenance".
    std::string productKind{"application"}; ///< "application" or "toolchain".
    std::string identifier;                  ///< Stable package identity.
    std::string displayName;                 ///< User-visible product name.
    std::string version;                     ///< Product version shown to users.
    std::string publisher;                   ///< Publisher display name.
    std::string description;                 ///< Optional product summary.
    std::string contact;                     ///< Optional support contact.
    std::string homepage;                    ///< Optional HTTPS project URL.
    std::string documentationUrl;            ///< Optional HTTPS documentation URL.
    std::string updateManifestUrl;           ///< Optional signed-update manifest URL.
    std::string updateRsaModulus;            ///< Update key modulus as lowercase big-endian hex.
    std::string updateRsaExponent;           ///< Update key exponent as lowercase big-endian hex.
    std::string architecture;                ///< Payload architecture: `x64` or `arm64`.
    std::string channel{"stable"};           ///< Lowercase release channel.
    std::string commit;                      ///< Optional lowercase source commit hash.
    std::string defaultScope{"user"}; ///< "user" or "machine".
    std::string defaultInstallDir;            ///< Default Windows destination leaf.
    std::string executableName;               ///< Install-relative primary executable path.
    std::string associationExecutable;        ///< Executable used for Open-With commands.
    std::string pathRelativePath;              ///< Directory offered for PATH integration.
    std::string displayIconRelativePath;       ///< Optional installed icon/executable path.
    std::string payloadEntry{"meta/payload.zip"}; ///< Nested repair/install payload ZIP.
    std::string cleanupEntry{"meta/cleanup.exe"}; ///< Detached cleanup helper overlay entry.
    std::string cleanupSha256; ///< SHA-256 of the signed detached cleanup helper.
    std::string licenseEntry{"meta/license.txt"}; ///< Optional displayed license overlay entry.
    std::string readmeEntry{"meta/readme.txt"}; ///< Optional displayed readme overlay entry.
    std::string installedManifestRelativePath{".zanna-install-manifest.txt"}; ///< File inventory output.
    std::string stateRelativePath{".zanna-install-state.v2"}; ///< Maintenance state output.
    std::string uninstallerRelativePath{"uninstall.exe"}; ///< Installed maintenance executable.
    std::string minimumWindowsVersion{"10.0.17763"}; ///< Minimum supported dotted OS version.
    bool addToPath{false};                    ///< Whether PATH integration is offered.
    bool registerFileAssociations{false};     ///< Whether association records are actionable.
    bool createShortcuts{false};              ///< Whether shortcut records are actionable.
    uint64_t installedSizeBytes{0};           ///< Total declared installed payload size.
    std::vector<WindowsInstallerComponentMetadata> components; ///< Selectable component catalog.
    std::vector<WindowsInstallerPayloadMetadata> payloadFiles; ///< Nested ZIP file inventory.
    std::vector<WindowsInstallerOuterFileMetadata> outerFiles; ///< Stored overlay-file inventory.
    std::vector<WindowsInstallerShortcutMetadata> shortcuts; ///< Shell links to materialize.
    std::vector<WindowsInstallerAssociationMetadata> associations; ///< Open-With registrations.
};

/// @brief Serialize metadata into the canonical schema-3 UTF-8 representation.
/// @param metadata Complete package contract to validate and encode.
/// @return Deterministic tab-delimited UTF-8 document.
/// @throws std::runtime_error when required values or records are invalid.
std::string serializeWindowsInstallerMetadata(const WindowsInstallerMetadata &metadata);

/// @brief Parse and strictly validate canonical schema-3 metadata.
/// @param text Complete metadata document.
/// @return Fully owning parsed and semantically validated package contract.
/// @throws std::runtime_error on malformed, duplicate, unknown, or unsafe data.
WindowsInstallerMetadata parseWindowsInstallerMetadata(std::string_view text);

} // namespace zanna::pkg
