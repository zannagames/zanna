//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/NativeBinaryInspector.hpp
// Purpose: Identify the container format, file kind, and CPU architectures of
//          Mach-O, ELF, and PE files from their headers, and search a binary
//          for exported symbol names, for packagers that stage third-party
//          native libraries.
// Key invariants:
//   - Inspection never executes or maps the file; it only parses bounded,
//     validated header fields from the supplied bytes.
//   - Architectures use the packaging spellings "x64" and "arm64"; slices of
//     other CPU types are ignored, and a file with none reports no
//     architectures.
// Ownership/Lifetime:
//   - Results are plain values; the caller owns the byte buffers it passes.
// Links: StoreDepotBuilder.hpp, docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares header-level inspection of native executables and libraries.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::pkg {

/// @brief Native binary container format.
enum class NativeBinaryFormat {
    Unknown, ///< Not a recognized Mach-O, ELF, or PE file.
    MachO,   ///< Thin or universal (fat) Mach-O.
    ELF,     ///< 64-bit little-endian ELF.
    PE,      ///< PE32 or PE32+ image.
};

/// @brief What a native binary is for.
enum class NativeBinaryKind {
    Unknown,       ///< Recognized format but an unclassified file type.
    Executable,    ///< MH_EXECUTE, ELF ET_EXEC, or a PE image without the DLL flag.
    SharedLibrary, ///< MH_DYLIB, ELF ET_DYN (also used by position-independent executables),
                   ///< or a PE image with IMAGE_FILE_DLL.
};

/// @brief Header facts about one native binary.
struct NativeBinaryInfo {
    NativeBinaryFormat format{NativeBinaryFormat::Unknown}; ///< Container format.
    NativeBinaryKind kind{NativeBinaryKind::Unknown};       ///< Kind of every inspected slice.
    std::vector<std::string> architectures; ///< Supported slices, "x64" and/or "arm64", sorted.
    bool pe32Plus{false};                   ///< PE only: optional header is PE32+.
    bool universal{false};                  ///< Mach-O only: file is a fat container.
};

/// @brief Inspect native binary bytes.
/// @details Universal Mach-O files report the union of their supported slices;
///          the kind is reported only when every supported slice agrees.
/// @param data Complete file contents (fat Mach-O slices may lie anywhere in the file).
/// @return Header facts, or std::nullopt when the bytes are not a well-formed
///         Mach-O, ELF, or PE header.
std::optional<NativeBinaryInfo> inspectNativeBinary(const std::vector<uint8_t> &data);

/// @brief Human-readable format name for diagnostics.
/// @param format Container format.
/// @return "Mach-O", "ELF", "PE", or "unknown".
std::string nativeBinaryFormatName(NativeBinaryFormat format);

/// @brief Report whether a binary contains a symbol name as a NUL-terminated string.
/// @details Symbol string tables in Mach-O (`_name`), ELF `.dynstr`, and the PE
///          export name table all store exported names NUL-terminated, so a
///          name followed by NUL is present exactly when some table holds it.
///          This is a pre-flight sanity check, not an export-table parser.
/// @param data Complete file contents.
/// @param name Symbol name without platform decoration.
/// @return True when @p name followed by a NUL byte occurs in @p data.
bool binaryContainsSymbolName(const std::vector<uint8_t> &data, std::string_view name);

} // namespace zanna::pkg
