//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/LnkWriter.cpp
// Purpose: Generate Windows .lnk (shell link) shortcut files.
//
// Key invariants:
//   - ShellLinkHeader: 76 bytes, HeaderSize=0x4C, CLSID at offset 4.
//   - LinkFlags at offset 20:
//       HasName          = 0x00000004
//       HasRelativePath  = 0x00000008
//       HasWorkingDir    = 0x00000010
//       HasIconLocation  = 0x00000040
//       IsUnicode        = 0x00000080
//   - StringData entries: 2-byte character count (uint16_t) + UTF-16LE chars.
//   - All numeric fields are little-endian.
//
// Ownership/Lifetime:
//   - Pure function, returns byte vector.
//
// Links: LnkWriter.hpp, [MS-SHLLINK] §2.1-2.4
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dependency-free serialization of Windows Shell Link files.
/// @details Emits little-endian ShellLinkHeader, LinkInfo, Unicode StringData,
///          optional environment expansion data, and the terminal block.

#include "LnkWriter.hpp"
#include "PkgUtils.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace zanna::pkg {

namespace {

/// @brief Append a 16-bit little-endian value to `buf`.
/// @param buf Destination byte buffer.
/// @param val Value to serialize.
void appendLE16(std::vector<uint8_t> &buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

/// @brief Append a 32-bit little-endian value to `buf`.
/// @param buf Destination byte buffer.
/// @param val Value to serialize.
void appendLE32(std::vector<uint8_t> &buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

/// @brief Append a UTF-16LE StringData entry: charCount(2) + UTF-16LE chars.
/// @param buf Destination Shell Link buffer.
/// @param str UTF-8 text to transcode.
/// @throws std::runtime_error If the UTF-16 code-unit count exceeds 65535.
void appendStringData(std::vector<uint8_t> &buf, const std::string &str) {
    const auto units = utf8ToUtf16CodeUnits(str);
    if (units.size() > std::numeric_limits<uint16_t>::max())
        throw std::runtime_error("lnk: string data is too long");
    uint16_t charCount = static_cast<uint16_t>(units.size());
    appendLE16(buf, charCount);
    for (uint16_t unit : units) {
        buf.push_back(static_cast<uint8_t>(unit & 0xFF));
        buf.push_back(static_cast<uint8_t>((unit >> 8) & 0xFF));
    }
}

/// @brief Convert `str` to a NUL-terminated ANSI byte sequence for LinkInfo's `LocalBasePath`
/// field. Non-printable or high-byte characters are replaced with `'?'`.
/// @param str UTF-8 path used to construct a compatibility fallback.
/// @return NUL-terminated printable-ASCII byte sequence.
std::vector<uint8_t> ansiPathFallback(const std::string &str) {
    std::vector<uint8_t> out;
    out.reserve(str.size() + 1);
    for (unsigned char c : str)
        out.push_back((c >= 0x20 && c < 0x7F) ? c : static_cast<uint8_t>('?'));
    out.push_back(0);
    return out;
}

/// @brief Return true if `str` contains a Windows environment variable reference (`%VAR%`).
/// Used to decide whether to emit an `EnvironmentVariableDataBlock` in the .lnk file.
/// @param str Candidate target path.
/// @return Whether at least two percent delimiters occur.
bool containsEnvironmentVariableReference(const std::string &str) {
    const std::size_t first = str.find('%');
    if (first == std::string::npos)
        return false;
    return str.find('%', first + 1) != std::string::npos;
}

/// @brief Append a MAX_PATH (260-byte) zero-padded ANSI path for the `EnvironmentVariableDataBlock`
/// `TargetAnsi` field. Throws if `str` is 260 characters or longer.
/// @param buf Destination ExtraData buffer.
/// @param str Target path to encode with an ANSI fallback.
/// @throws std::runtime_error If the source has 260 or more bytes.
void appendFixedAnsiPath(std::vector<uint8_t> &buf, const std::string &str) {
    if (str.size() >= 260)
        throw std::runtime_error("lnk: environment target path is too long");
    const auto ansi = ansiPathFallback(str);
    const std::size_t start = buf.size();
    buf.resize(start + 260, 0);
    std::memcpy(buf.data() + start, ansi.data(), ansi.size());
}

/// @brief Append a MAX_PATH (520-byte = 260 UTF-16 chars) zero-padded UTF-16LE path for the
/// `EnvironmentVariableDataBlock` `TargetUnicode` field. Throws if `str` encodes to 260+ code
/// units.
/// @param buf Destination ExtraData buffer.
/// @param str UTF-8 target path to transcode.
/// @throws std::runtime_error If the target uses 260 or more UTF-16 code units.
void appendFixedUtf16Path(std::vector<uint8_t> &buf, const std::string &str) {
    const auto units = utf8ToUtf16CodeUnits(str);
    if (units.size() >= 260)
        throw std::runtime_error("lnk: environment target path is too long");
    const std::size_t start = buf.size();
    buf.resize(start + 520, 0);
    for (std::size_t i = 0; i < units.size(); ++i) {
        buf[start + i * 2] = static_cast<uint8_t>(units[i] & 0xFF);
        buf[start + i * 2 + 1] = static_cast<uint8_t>((units[i] >> 8) & 0xFF);
    }
}

} // namespace

/// @brief Generate a Windows .lnk shortcut file conforming to [MS-SHLLINK].
/// Emits ShellLinkHeader, LinkInfo (VolumeID + LocalBasePath for reliable resolution),
/// StringData entries (NAME_STRING, RELATIVE_PATH, optional WORKING_DIR,
/// COMMAND_LINE_ARGUMENTS, and ICON_LOCATION),
/// and an optional EnvironmentVariableDataBlock when the target contains `%VAR%` references.
/// @param params Target, presentation, argument, working-directory, and icon settings.
/// @return Complete binary Shell Link file bytes.
/// @throws std::runtime_error If a StringData or environment-target field exceeds
///         the size representable by its on-disk structure.
std::vector<uint8_t> generateLnk(const LnkParams &params) {
    std::vector<uint8_t> buf;
    buf.reserve(512);

    // ─── ShellLinkHeader (76 bytes) ────────────────────────────────────

    // HeaderSize (4 bytes) = 0x0000004C
    appendLE32(buf, 0x4C);

    // LinkCLSID (16 bytes) = {00021401-0000-0000-C000-000000000046}
    // Stored as: Data1(LE32) Data2(LE16) Data3(LE16) Data4(8 bytes, big-endian)
    appendLE32(buf, 0x00021401);
    appendLE16(buf, 0x0000);
    appendLE16(buf, 0x0000);
    buf.push_back(0xC0);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x46);

    // LinkFlags (4 bytes)
    uint32_t linkFlags = 0;
    linkFlags |= 0x00000004; // HasName (NAME_STRING = description)
    linkFlags |= 0x00000008; // HasRelativePath
    linkFlags |= 0x00000080; // IsUnicode

    bool hasWorkDir = !params.workingDir.empty();
    bool hasArguments = !params.arguments.empty();
    bool hasIcon = !params.iconPath.empty();
    const bool hasEnvTarget = containsEnvironmentVariableReference(params.targetPath);
    const bool hasLinkInfo = !hasEnvTarget;

    if (hasLinkInfo)
        linkFlags |= 0x00000002; // HasLinkInfo
    if (hasWorkDir)
        linkFlags |= 0x00000010; // HasWorkingDir
    if (hasArguments)
        linkFlags |= 0x00000020; // HasArguments
    if (hasIcon)
        linkFlags |= 0x00000040; // HasIconLocation
    if (hasEnvTarget)
        linkFlags |= 0x00000200; // HasExpString

    appendLE32(buf, linkFlags);

    // FileAttributes (4 bytes) = FILE_ATTRIBUTE_NORMAL (0x80)
    appendLE32(buf, 0x00000080);

    // CreationTime (8 bytes) = 0
    appendLE32(buf, 0);
    appendLE32(buf, 0);

    // AccessTime (8 bytes) = 0
    appendLE32(buf, 0);
    appendLE32(buf, 0);

    // WriteTime (8 bytes) = 0
    appendLE32(buf, 0);
    appendLE32(buf, 0);

    // FileSize (4 bytes) = 0
    appendLE32(buf, 0);

    // IconIndex (4 bytes)
    appendLE32(buf, static_cast<uint32_t>(params.iconIndex));

    // ShowCommand (4 bytes) = SW_SHOWNORMAL (1)
    appendLE32(buf, 1);

    // HotKey (2 bytes) = 0
    appendLE16(buf, 0);

    // Reserved1 (2 bytes)
    appendLE16(buf, 0);

    // Reserved2 (4 bytes)
    appendLE32(buf, 0);

    // Reserved3 (4 bytes)
    appendLE32(buf, 0);

    // Header should be exactly 76 bytes at this point

    // ─── LinkInfo (§2.3) ──────────────────────────────────────────────
    // Provides VolumeID + LocalBasePath for reliable shortcut resolution.
    // Structure: LinkInfoSize(4) + LinkInfoHeaderSize(4) + LinkInfoFlags(4)
    //          + VolumeIDOffset(4) + LocalBasePathOffset(4)
    //          + CommonNetworkRelativeLinkOffset(4) + CommonPathSuffixOffset(4)
    //          + VolumeID + LocalBasePath(NUL) + CommonPathSuffix(NUL)
    if (hasLinkInfo) {
        // Build LinkInfo in a temporary buffer, then append
        std::vector<uint8_t> li;

        // LinkInfoHeaderSize = 0x24 includes Unicode path offsets.
        uint32_t liHeaderSize = 0x24;

        // VolumeID structure: VolumeIDSize(4) + DriveType(4) + DriveSerialNumber(4)
        //                   + VolumeLabelOffset(4) + VolumeLabel("C:\0")
        // DriveType = DRIVE_FIXED (3)
        std::vector<uint8_t> volumeId;
        appendLE32(volumeId, 0);  // VolumeIDSize placeholder
        appendLE32(volumeId, 3);  // DriveType = DRIVE_FIXED
        appendLE32(volumeId, 0);  // DriveSerialNumber = 0
        appendLE32(volumeId, 16); // VolumeLabelOffset = 16 (after this header)
        volumeId.push_back(0);    // Volume label = "" (NUL terminated)
        // Fill in VolumeIDSize
        uint32_t volIdSize = static_cast<uint32_t>(volumeId.size());
        volumeId[0] = static_cast<uint8_t>(volIdSize & 0xFF);
        volumeId[1] = static_cast<uint8_t>((volIdSize >> 8) & 0xFF);
        volumeId[2] = static_cast<uint8_t>((volIdSize >> 16) & 0xFF);
        volumeId[3] = static_cast<uint8_t>((volIdSize >> 24) & 0xFF);

        // LocalBasePath: ANSI fallback plus Unicode path for non-ASCII-correct resolution.
        std::vector<uint8_t> lbp = ansiPathFallback(params.targetPath);
        std::vector<uint8_t> lbpUnicode = utf8ToUtf16LEBytes(params.targetPath, true);

        // CommonPathSuffix: empty (NUL terminated)
        std::vector<uint8_t> cps = {0};
        std::vector<uint8_t> cpsUnicode = {0, 0};

        // Calculate offsets
        uint32_t volumeIdOffset = liHeaderSize;
        uint32_t localBasePathOffset = volumeIdOffset + volIdSize;
        uint32_t commonNetworkOffset = 0; // Not present
        uint32_t commonPathSuffixOffset = localBasePathOffset + static_cast<uint32_t>(lbp.size());
        uint32_t localBasePathOffsetUnicode =
            commonPathSuffixOffset + static_cast<uint32_t>(cps.size());
        uint32_t commonPathSuffixOffsetUnicode =
            localBasePathOffsetUnicode + static_cast<uint32_t>(lbpUnicode.size());
        uint32_t linkInfoSize =
            commonPathSuffixOffsetUnicode + static_cast<uint32_t>(cpsUnicode.size());

        // Write LinkInfo header
        appendLE32(li, linkInfoSize);
        appendLE32(li, liHeaderSize);
        appendLE32(li, 0x00000001); // LinkInfoFlags: VolumeIDAndLocalBasePath
        appendLE32(li, volumeIdOffset);
        appendLE32(li, localBasePathOffset);
        appendLE32(li, commonNetworkOffset);
        appendLE32(li, commonPathSuffixOffset);
        appendLE32(li, localBasePathOffsetUnicode);
        appendLE32(li, commonPathSuffixOffsetUnicode);

        // VolumeID
        li.insert(li.end(), volumeId.begin(), volumeId.end());

        // LocalBasePath
        li.insert(li.end(), lbp.begin(), lbp.end());

        // CommonPathSuffix
        li.insert(li.end(), cps.begin(), cps.end());

        // Unicode LocalBasePath and CommonPathSuffix
        li.insert(li.end(), lbpUnicode.begin(), lbpUnicode.end());
        li.insert(li.end(), cpsUnicode.begin(), cpsUnicode.end());

        buf.insert(buf.end(), li.begin(), li.end());
    }

    // ─── StringData ────────────────────────────────────────────────────
    // Order (when present): NAME_STRING, RELATIVE_PATH, WORKING_DIR,
    // COMMAND_LINE_ARGUMENTS, ICON_LOCATION

    // NAME_STRING (description)
    std::string name = params.description.empty() ? params.targetPath : params.description;
    appendStringData(buf, name);

    // RELATIVE_PATH (the target path)
    appendStringData(buf, params.targetPath);

    // WORKING_DIR
    if (hasWorkDir)
        appendStringData(buf, params.workingDir);

    // COMMAND_LINE_ARGUMENTS
    if (hasArguments)
        appendStringData(buf, params.arguments);

    // ICON_LOCATION
    if (hasIcon)
        appendStringData(buf, params.iconPath);

    if (hasEnvTarget) {
        // EnvironmentVariableDataBlock: 0x314 bytes, signature 0xA0000001.
        appendLE32(buf, 0x00000314);
        appendLE32(buf, 0xA0000001);
        appendFixedAnsiPath(buf, params.targetPath);
        appendFixedUtf16Path(buf, params.targetPath);
    }

    // ExtraData terminal block: four zero bytes terminate the Shell Link stream.
    appendLE32(buf, 0);

    return buf;
}

} // namespace zanna::pkg
