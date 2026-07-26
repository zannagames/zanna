//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/LnkWriter.hpp
// Purpose: Generate Windows .lnk (shell link) shortcut files from scratch,
//          following the [MS-SHLLINK] binary format specification.
//
// Key invariants:
//   - ShellLinkHeader is always 76 bytes.
//   - LinkCLSID = {00021401-0000-0000-C000-000000000046}.
//   - HasLinkInfo + HasRelativePath + IsUnicode flags set.
//   - LinkInfo provides VolumeID + LocalBasePath for reliable resolution.
//   - String data uses UTF-16LE with 2-byte length prefix (character count).
//   - No LinkTargetIDList — LinkInfo + StringData provide reliable resolution.
//
// Ownership/Lifetime:
//   - Pure function returning byte vector.
//
// Links: WindowsPackageBuilder.hpp, [MS-SHLLINK] specification
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares Windows Shell Link (`.lnk`) serialization.
/// @details The writer constructs the documented MS-SHLLINK structures directly
///          and returns ownership of the resulting binary bytes to the caller.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Parameters for generating a .lnk shortcut file.
struct LnkParams {
    std::string targetPath;  ///< Target executable path, optionally containing `%VAR%` references.
    std::string workingDir;  ///< Optional working directory; omitted from the link when empty.
    std::string arguments;   ///< Optional command-line arguments passed verbatim to the target.
    std::string description; ///< Shortcut comment; the target path is used when empty.
    std::string iconPath;    ///< Optional icon resource path; the target icon is used when empty.
    int32_t iconIndex{0};    ///< Signed icon resource index serialized in the ShellLinkHeader.
};

/// @brief Generate a Windows .lnk shortcut file.
///
/// Produces a valid [MS-SHLLINK] shell link with:
///   - ShellLinkHeader (76 bytes) with HasLinkInfo | HasName | HasRelativePath |
///     IsUnicode (and HasWorkingDir/HasArguments/HasIconLocation/HasExpString as
///     applicable).
///   - LinkInfo (VolumeID + LocalBasePath, ANSI and Unicode) for reliable
///     resolution.
///   - StringData: NAME_STRING, RELATIVE_PATH, and — when set — WORKING_DIR,
///     COMMAND_LINE_ARGUMENTS, and ICON_LOCATION.
///   - An EnvironmentVariableDataBlock when the target contains %VAR% references.
///   - No LinkTargetIDList (LinkInfo + StringData are sufficient to resolve).
///
/// @param params Shortcut parameters.
/// @return .lnk file bytes.
/// @throws std::runtime_error If a serialized string exceeds its format limit.
std::vector<uint8_t> generateLnk(const LnkParams &params);

} // namespace zanna::pkg
