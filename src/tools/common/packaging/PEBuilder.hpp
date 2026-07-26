//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PEBuilder.hpp
// Purpose: Emit PE32+ (x86-64) executables from scratch — DOS header, COFF
//          header, optional header, section headers, import table, resource
//          section, and raw section data.
//
// Key invariants:
//   - Virtual alignment = 0x1000 (4 KiB pages).
//   - File alignment = 0x200 (512 bytes).
//   - COFF Machine = 0x8664 (AMD64) by default, 0xAA64 (ARM64) when requested.
//   - Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI (2) by default.
//   - Import directory references kernel32.dll for minimal APIs.
//   - Resource section embeds RT_MANIFEST for UAC elevation.
//
// Ownership/Lifetime:
//   - Pure build functions return owned bytes; the file helper writes caller-owned bytes.
//
// Links: WindowsPackageBuilder.hpp, Microsoft PE Format specification
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares dependency-free PE32+ executable and manifest generation.
/// @details Models import/resource metadata and emits complete AMD64 or ARM64
///          Windows images with optional raw overlays.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Functions imported from a single DLL for the import directory.
struct PEImport {
    std::string dllName;                ///< NUL-terminated DLL name emitted in `.rdata`.
    std::vector<std::string> functions; ///< Ordered name imports forming ILT and IAT slots.
};

/// @brief VERSIONINFO metadata embedded as an RT_VERSION resource.
struct PEVersionInfo {
    bool enabled{false};                            ///< Emit the resource only when true.
    std::array<uint16_t, 4> fileVersion{0, 0, 0, 0};    ///< Binary file version (a.b.c.d).
    std::array<uint16_t, 4> productVersion{0, 0, 0, 0}; ///< Binary product version (a.b.c.d).
    std::string companyName;        ///< CompanyName string-table value.
    std::string fileDescription;    ///< FileDescription string-table value.
    std::string fileVersionText;    ///< FileVersion display string.
    std::string internalName;       ///< InternalName string-table value.
    std::string originalFilename;   ///< OriginalFilename string-table value.
    std::string productName;        ///< ProductName string-table value.
    std::string productVersionText; ///< ProductVersion display string.
};

/// @brief Parameters for building a PE32+ executable.
struct PEBuildParams {
    /// Machine code for the .text section.
    std::vector<uint8_t> textSection;

    /// Read-only data (.rdata) — import tables, strings, etc.
    /// If empty, the builder generates import tables automatically.
    std::vector<uint8_t> rdataSection;

    /// DLL imports (builder generates import directory from these).
    std::vector<PEImport> imports;

    /// UAC manifest XML (embedded as RT_MANIFEST in .rsrc section).
    /// Empty string = no manifest.
    std::string manifest;

    /// ICO file data for RT_GROUP_ICON + RT_ICON resources.
    /// If non-empty, the icon is embedded in the .rsrc section so that
    /// Windows Explorer displays the icon for the .exe file.
    std::vector<uint8_t> iconData;

    /// Optional VERSIONINFO resource for Explorer, SmartScreen, and Add/Remove Programs.
    PEVersionInfo versionInfo;

    /// Raw data to append after all PE sections (e.g. ZIP payload).
    std::vector<uint8_t> overlay;

    /// Target architecture: "x64" (default) or "arm64".
    /// Selects PE machine type: 0x8664 (AMD64) or 0xAA64 (ARM64).
    std::string arch{"x64"};

    /// Address of entry point relative to start of .text section.
    uint32_t entryPointOffset{0};

    /// Subsystem: 2 = WINDOWS_GUI, 3 = WINDOWS_CUI (console).
    uint16_t subsystem{2};

    /// DLL characteristics flags.
    uint16_t dllCharacteristics{0x8160}; // HIGH_ENTROPY_VA | DYNAMIC_BASE |
                                         // NX_COMPAT | TERMINAL_SERVER_AWARE.

    /// Emit a minimal base-relocation directory so generated PE images can opt
    /// into relocation-based ASLR even when the machine code itself has no
    /// absolute image-base fixups.
    bool emitRelocations{true};

    /// Stack/heap sizes.
    uint64_t stackReserve{0x100000};
    uint64_t stackCommit{0x1000};
    uint64_t heapReserve{0x100000};
    uint64_t heapCommit{0x1000};
};

/// @brief Build a complete PE32+ executable.
///
/// Generates: DOS header (64) + DOS stub (64) + PE signature (4) +
/// COFF header (20) + Optional header PE32+ (240) + Section headers +
/// .text + .rdata (imports) + .rsrc (manifest) + overlay.
///
/// @param params Build parameters.
/// @return Complete PE file bytes.
std::vector<uint8_t> buildPE(const PEBuildParams &params);

/// @brief Write a PE to a file.
/// @param pe Complete PE image bytes.
/// @param path Destination file path.
/// @throws std::runtime_error on write failure.
void writePEToFile(const std::vector<uint8_t> &pe, const std::string &path);

/// @brief Generate a basic UAC manifest requesting admin elevation.
/// @return `requireAdministrator` application-manifest XML without compatibility declarations.
std::string generateUacManifest();

/// @brief Generate UAC manifest with optional Windows compatibility metadata.
/// @param minOsWindows Optional minimum dotted-numeric Windows version.
/// @return `requireAdministrator` application-manifest XML.
/// @throws std::runtime_error If the version syntax or component count is invalid.
std::string generateUacManifest(const std::string &minOsWindows);

/// @brief Generate a UAC manifest requesting asInvoker (no elevation).
/// @return `asInvoker` application-manifest XML without compatibility declarations.
std::string generateAsInvokerManifest();

/// @brief Generate an asInvoker manifest with optional Windows compatibility metadata.
/// @param minOsWindows Optional minimum dotted-numeric Windows version.
/// @return `asInvoker` application-manifest XML.
/// @throws std::runtime_error If the version syntax or component count is invalid.
std::string generateAsInvokerManifest(const std::string &minOsWindows);

} // namespace zanna::pkg
