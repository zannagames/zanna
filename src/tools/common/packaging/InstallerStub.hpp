//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/InstallerStub.hpp
// Purpose: Generate complete Windows installer and uninstaller stub code
//          using InstallerStubGen. Returns .text bytes + import list for
//          PEBuilder integration.
//
// Key invariants:
//   - Installer uses precomputed layout metadata and stored-overlay offsets to
//     extract bootstrap files directly from the packaged ZIP overlay.
//   - When compressedPayloadRelativePath is set, the installer extracts a
//     stored inner ZIP to disk and expands its DEFLATE-compressed entries with
//     Windows PowerShell's native archive support.
//   - Uninstaller uses the same layout metadata to delete installed files,
//     remove shortcuts, and unregister the app.
//   - Both stubs use Win32 APIs via IAT (no dynamic LoadLibrary).
//   - Bootstrap code is emitted as x86-64 or AArch64 machine code to match the
//     requested Windows payload architecture.
//
// Ownership/Lifetime:
//   - Pure functions returning result structs.
//
// Links: InstallerStubGen.hpp, PEBuilder.hpp, WindowsPackageBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares Windows installer and uninstaller bootstrap-code generation.
/// @details Validated package layout metadata is lowered into architecture-specific
///          x86-64 or AArch64 machine code, embedded read-only data, and an ordered
///          PE import list suitable for @ref PEBuilder integration.

#pragma once

#include "PEBuilder.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zanna::pkg {

/// @brief Base directory anchor for a file or directory entry in the installer.
/// @details The generated host resolves these logical roots at runtime according
///          to per-user or machine-wide installation scope.
enum class WindowsInstallRoot : uint64_t {
    InstallDir = 0,   ///< Relative to %ProgramFiles%\<installDirName>
    DesktopDir = 1,   ///< Relative to the user's Desktop folder
    StartMenuDir = 2, ///< Relative to %ProgramData%\Microsoft\Windows\Start Menu\Programs
};

/// @brief A directory that the installer should create or the uninstaller should remove.
/// @details @ref relativePath is interpreted beneath the runtime-resolved
///          @ref root and must already satisfy Windows package path validation.
struct WindowsPackageDirEntry {
    WindowsInstallRoot root{WindowsInstallRoot::InstallDir}; ///< Base directory anchor
    std::string relativePath; ///< Path relative to root (e.g. "assets\\fonts")
};

/// @brief A file to be extracted from the ZIP overlay by the installer.
/// @details Records both the destination and the exact stored-overlay location,
///          size, integrity metadata, optional component ownership, and original
///          payload path needed by extraction and upgrade cleanup.
struct WindowsPackageFileEntry {
    /// @brief Construct an empty install-root file record.
    WindowsPackageFileEntry() = default;

    /// @brief Construct a complete package file record.
    /// @param rootValue Runtime destination root.
    /// @param relativePathValue Destination path beneath @p rootValue.
    /// @param overlayDataOffsetValue Stored byte offset within the appended ZIP.
    /// @param sizeBytesValue Uncompressed file size.
    /// @param crc32Value CRC-32 of uncompressed bytes.
    /// @param sha256Value Optional SHA-256 of stored overlay bytes.
    /// @param componentIdValue Optional owning component identifier.
    /// @param sourcePathValue Source path in the inner or bootstrap archive.
    WindowsPackageFileEntry(WindowsInstallRoot rootValue,
                            std::string relativePathValue,
                            uint64_t overlayDataOffsetValue,
                            uint64_t sizeBytesValue,
                            uint32_t crc32Value = 0,
                            std::string sha256Value = {},
                            std::string componentIdValue = {},
                            std::string sourcePathValue = {})
        : root(rootValue), relativePath(std::move(relativePathValue)),
          overlayDataOffset(overlayDataOffsetValue), sizeBytes(sizeBytesValue), crc32(crc32Value),
          sha256(std::move(sha256Value)), componentId(std::move(componentIdValue)),
          sourcePath(std::move(sourcePathValue)) {}

    WindowsInstallRoot root{WindowsInstallRoot::InstallDir}; ///< Base directory anchor
    std::string relativePath;                                ///< Destination path relative to root
    uint64_t overlayDataOffset{0}; ///< Byte offset of this file's data within the ZIP overlay
    uint64_t sizeBytes{0};         ///< Uncompressed file size in bytes
    uint32_t crc32{0};             ///< CRC-32 checksum of the uncompressed data
    std::string sha256;            ///< Lowercase SHA-256 of stored overlay data when available
    std::string componentId;       ///< Empty for core; otherwise an optional-component id
    std::string sourcePath;        ///< Entry path in the inner payload or outer bootstrap ZIP.
};

/// @brief One user-selectable installer component.
/// @details Component identifiers tie wizard selection, silent-install
///          environment flags, and individual payload ownership together.
struct WindowsOptionalComponent {
    std::string id;             ///< Stable ASCII id used by silent flags and payload metadata
    std::string label;          ///< Checkbox label shown in the native setup wizard
    std::string description;    ///< Short explanatory text for diagnostics and future UIs
    bool defaultSelected{true}; ///< Initial checkbox state and quiet-install behavior
};

/// @brief A file-type association to register in the Windows registry.
/// @details Each association supplies the extension keys, ProgID metadata, MIME
///          value, and literal command arguments emitted by installer code.
struct WindowsFileAssociationEntry {
    std::string extension;            ///< Extension including leading dot (e.g. ".zia")
    std::string description;          ///< Human-readable type description
    std::string mimeType;             ///< MIME type string (e.g. "text/x-zia")
    std::string progId;               ///< ProgID to register (e.g. "Zanna.ZiaSource.1")
    std::string openCommandArguments; ///< Arguments appended after the exe path in the Open command
};

/// @brief Runtime-resolved shortcut used by the native installer host.
/// @details Unlike the legacy embedded .lnk payload, these fields do not bake
///          the package's default scope or destination into the link.
struct WindowsNativeShortcutEntry {
    WindowsInstallRoot root{WindowsInstallRoot::StartMenuDir}; ///< Shortcut destination root.
    std::string relativePath; ///< Link path relative to @ref root.
    std::string targetRoot;   ///< Logical root code for the target executable.
    std::string targetPath;   ///< Target path relative to @ref targetRoot.
    std::string workingRoot;  ///< Logical root code for the working directory.
    std::string workingPath;  ///< Working path relative to @ref workingRoot.
    std::string argumentPrefix; ///< Literal command argument prefix.
    std::string argumentPath;   ///< Optional rooted path appended to arguments.
    std::string description;    ///< User-visible shortcut description.
    std::string iconRoot;       ///< Logical root code for the icon resource.
    std::string iconPath;       ///< Icon path relative to @ref iconRoot.
    int32_t iconIndex{0};       ///< Resource index within the icon file.
    std::string componentId;    ///< Optional component controlling link creation.
};

/// @brief Full layout metadata consumed by the installer/uninstaller stub codegen.
/// @details This is the architecture-neutral contract between package assembly
///          and bootstrap generation. Paths, offsets, digests, registry metadata,
///          UI text, component ownership, and upgrade manifests must describe
///          the same finalized package payload.
struct WindowsPackageLayout {
    std::string displayName;    ///< User-visible application name (e.g. "Crackman")
    std::string installDirName; ///< Subdirectory under %ProgramFiles% (e.g. "Crackman")
    std::string version;        ///< Version string for Add/Remove Programs (e.g. "0.1.0")
    std::string identifier;     ///< Reverse-DNS identifier for registry keys
    std::string publisher;      ///< Publisher name shown in Add/Remove Programs
    std::string description;    ///< Human-readable comments shown in installer metadata.
    std::string contact;        ///< Support/contact string for Add/Remove Programs.
    std::string licenseText;    ///< License text shown by the native Windows wizard.
    std::string wizardSummary;  ///< Optional short summary shown in the first wizard page.
    std::string executableName; ///< Name of the main executable (e.g. "crackman.exe")
    uint64_t overlayFileOffset{
        0}; ///< Byte offset within the installer PE where the ZIP overlay begins
    bool createDesktopShortcut{false};   ///< Create a .lnk on the user's Desktop
    bool createStartMenuShortcut{false}; ///< Create a .lnk in the Start Menu Programs folder
    bool addToPath{false};               ///< Add installDir\pathRelativePath to the system Path
    bool cleanInstallRootBeforeInstall{
        false}; ///< Remove the install root before extracting (upgrade path)
    std::string
        compressedPayloadRelativePath; ///< Optional stored inner ZIP expanded into installDir.
    std::string
        compressedPayloadManifestRelativePath; ///< Optional next manifest used for stale cleanup.
    std::string
        installedManifestRelativePath; ///< Current installed-file manifest path under installDir.
    std::string pathRelativePath;      ///< Subdir within installDir to add to Path (e.g. "bin")
    std::string fileAssociationExecutableRelativePath; ///< Exe used for Open commands (relative to
                                                       ///< installDir)
    bool perUserInstall{false};    ///< Install under the current user profile and HKCU.
    std::string homepage;          ///< Optional support/update URL for Add/Remove Programs.
    std::string documentationUrl;  ///< Optional installed-documentation/support URL.
    std::string updateManifestUrl; ///< Optional HTTPS update-discovery manifest URL.
    std::string updateRsaModulus;  ///< Optional RSA public modulus for signed update manifests.
    std::string updateRsaExponent; ///< Optional RSA public exponent, lowercase hex.
    std::string releaseChannel{"stable"}; ///< Stable package update channel identifier.
    std::string sourceCommit;             ///< Optional lowercase source commit hash.
    std::string
        displayIconRelativePath;   ///< Icon path relative to installDir for Add/Remove Programs.
    uint32_t wizardImageWidth{0};  ///< Width of wizardImageRgba in pixels.
    uint32_t wizardImageHeight{0}; ///< Height of wizardImageRgba in pixels.
    std::vector<uint8_t>
        wizardImageRgba;         ///< Optional top-down RGBA artwork drawn by the native x64 wizard.
    uint32_t estimatedSizeKb{0}; ///< Approximate installed size in KiB for ARP.
    std::string installDate;     ///< YYYYMMDD packaging/install metadata date.
    std::string minimumWindowsVersion{"10.0.17763"};        ///< Supported Windows build floor.
    std::vector<WindowsPackageDirEntry> installDirectories; ///< Directories to create on install
    std::vector<WindowsPackageDirEntry>
        uninstallDirectories;                            ///< Directories to remove on uninstall
    std::vector<WindowsPackageFileEntry> installFiles;   ///< Files to extract on install
    std::vector<WindowsPackageFileEntry> installedFiles; ///< Files left on disk after install
    std::vector<WindowsPackageFileEntry> uninstallFiles; ///< Files to delete on uninstall
    std::vector<WindowsOptionalComponent>
        optionalComponents; ///< Default-on payload groups exposed by the setup wizard
    std::vector<WindowsFileAssociationEntry>
        fileAssociations; ///< File associations to register/deregister
    std::vector<WindowsNativeShortcutEntry>
        nativeShortcuts; ///< Destination-aware shortcuts generated by the native host.
};

/// @brief Result of building an installer/uninstaller stub.
/// @details The PE builder places @ref textSection as executable code, appends
///          @ref stubData to read-only data at @ref stubDataRVAOffset, and emits
///          the ordered @ref imports expected by generated IAT slot calls.
struct StubResult {
    std::vector<uint8_t> textSection; ///< Machine code for .text
    std::vector<uint8_t> stubData;    ///< Embedded string data (appended to .rdata)
    std::vector<PEImport> imports;    ///< DLL imports needed
    std::string peArch{"x64"};        ///< PE machine type to use for the bootstrap executable.
    uint32_t stubDataRVAOffset{0};    ///< Offset within .rdata where stubData starts
};

/// @brief Build the installer stub machine code.
///
/// @param layout Package layout and extraction metadata.
/// @param arch   Payload architecture ("x64" or "arm64").
/// @return StubResult with .text bytes, data, and import list.
/// @throws std::runtime_error When layout invariants, architecture selection,
///         import ordering, or generated branch/data limits are invalid.
StubResult buildInstallerStub(const WindowsPackageLayout &layout, const std::string &arch);

/// @brief Build the uninstaller stub machine code.
///
/// @param layout Package layout and uninstall metadata.
/// @param arch   Payload architecture ("x64" or "arm64").
/// @return StubResult with .text bytes, data, and import list.
/// @throws std::runtime_error When layout invariants, architecture selection,
///         import ordering, or generated branch/data limits are invalid.
StubResult buildUninstallerStub(const WindowsPackageLayout &layout, const std::string &arch);

} // namespace zanna::pkg
