//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/DesktopEntryGenerator.hpp
// Purpose: Generate freedesktop.org .desktop files and MIME type XML.
//
// Key invariants:
//   - .desktop files follow freedesktop.org Desktop Entry Specification.
//   - MIME XML follows freedesktop.org shared-mime-info format.
//
// Ownership/Lifetime:
//   - Output returned as std::string (caller-owned).
//
// Links: DesktopEntryGenerator.cpp, LinuxPackageBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares generation of freedesktop application and MIME metadata.
/// @details Caller-owned package settings are validated and escaped into
///          `.desktop` key/value text and shared-mime-info XML strings without
///          reading or writing the filesystem.

#pragma once

#include "PackageConfig.hpp"

#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Parameters for .desktop file generation.
/// @details Values are copied into generated text. Executable arguments are
///          tokenized and escaped separately so manifest text cannot inject
///          additional desktop-entry keys or reinterpret literal percent signs.
struct DesktopEntryParams {
    std::string name;           ///< Display name (e.g. "Zanna Studio")
    std::string comment;        ///< Short description
    std::string execPath;       ///< Path to executable (e.g. "/usr/bin/zannastudio")
    std::string execArguments;  ///< Additional literal Exec= arguments after the executable token.
    std::string iconName;       ///< Icon name (e.g. "zannastudio")
    std::string categories;     ///< freedesktop.org categories (e.g. "Development;TextEditor;")
    std::string startupWmClass; ///< Optional StartupWMClass for desktop/window matching.
    std::string keywords;       ///< Optional semicolon-separated Keywords= metadata.
    bool terminal{false};       ///< Whether to run in a terminal
    std::string workingDir;     ///< Working directory for asset resolution (Path= in .desktop)
    std::vector<FileAssoc> fileAssociations; ///< For MimeType= field
    bool acceptsFileArgument{false};         ///< Append %f to Exec= for file opens.
    bool noDisplay{false}; ///< Hide from menus while keeping MIME handler registration.
};

/// @brief Generate a .desktop file.
/// @param params Display, exec, icon, category, and MIME settings to emit.
/// @return .desktop file content as a string.
/// @throws std::runtime_error When fields, argument syntax, categories,
///         keywords, or file associations fail validation.
std::string generateDesktopEntry(const DesktopEntryParams &params);

/// @brief Generate a MIME type XML file for file associations.
/// @param packageName Reserved package identifier retained for API compatibility;
///        it currently does not alter the shared-mime-info document.
/// @param assocs The file associations to register.
/// @return MIME XML content, or an empty string when @p assocs is empty.
/// @throws std::runtime_error When an association fails package validation.
std::string generateMimeTypeXml(const std::string &packageName,
                                const std::vector<FileAssoc> &assocs);

} // namespace zanna::pkg
