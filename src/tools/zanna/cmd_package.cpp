//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements native application packaging for the `zanna package` command.
/// @details Resolves a project, compiles or accepts its native executable, and
///          packages the result for a selected platform. The implementation also
///          validates package metadata and payloads, applies signing policy, and
///          supports a dependency-free JSON dry-run description.
//
// Key invariants:
//   - Resolves project, compiles to native, then packages.
//   - Default target is the host platform.
//   - Non-host package formats can package a caller-supplied executable even
//     when the current host cannot build the target-native payload itself.
//
// Ownership/Lifetime:
//   - Temporary files (IL, native binary) are cleaned up after packaging.
//
// Links: cmd_run.cpp (compile pattern), MacOSPackageBuilder.hpp,
//        native_compiler.hpp, project_loader.hpp
//
//===----------------------------------------------------------------------===//

#include "cli.hpp"
#include "common/Environment.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/RunProcess.hpp"
#include "tools/common/native_compiler.hpp"
#include "tools/common/packaging/LinuxPackageBuilder.hpp"
#include "tools/common/packaging/LinuxRuntimeStubGen.hpp"
#include "tools/common/packaging/MacOSPackageBuilder.hpp"
#include "tools/common/packaging/PkgHash.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "tools/common/packaging/PkgVerify.hpp"
#include "tools/common/packaging/WindowsPackageBuilder.hpp"
#include "tools/common/project_loader.hpp"

#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <windows.h>
#endif

namespace {

using namespace il::tools::common;
using namespace il::support;
namespace fs = std::filesystem;

/// @brief Package formats accepted by the package command.
/// @details Covers native macOS, Debian, Windows, portable tar, self-extracting
///          Linux bundle, RPM, and DMG outputs; Auto selects the host default.
enum class PackageTarget { MacOS, Linux, Windows, Tarball, AppImage, Rpm, Dmg, Auto };

/// @brief Native executable container formats recognized during validation.
enum class ExecutableFormat { Unknown, MachO, ELF, PE };

/// @brief Identifying details of a native executable detected from its header.
struct ExecutableInfo {
    ExecutableFormat format{ExecutableFormat::Unknown}; ///< Detected container format.
    std::string arch;                                   ///< Architecture string ("x64"/"arm64").
};

/// @brief Print usage information for `zanna package`.
/// @param out Stream that receives the formatted help text.
void packageUsage(std::ostream &out = std::cerr) {
    out << "Usage: zanna package [project] [options]\n"
        << "\n"
        << "  Build a Zanna project and package it for a target platform.\n"
        << "\n"
        << "  [project]  Path to a directory or zanna.project file (default: .)\n"
        << "\n"
        << "Options:\n"
        << "  --target <platform>       macos, linux, windows, linux-bundle, rpm, dmg, or "
           "tarball "
           "(default: host)\n"
        << "  --target=<platform>       Inline form of --target\n"
        << "  --arch <arch>             Target architecture: x64 or arm64 (default: host)\n"
        << "  --arch=<arch>             Inline form of --arch\n"
        << "  --executable <path>       Package a prebuilt native executable instead of compiling\n"
        << "  --executable=<path>       Inline form of --executable\n"
        << "  -o, --output <path>       Output file path\n"
        << "  --output=<path>           Inline form of --output\n"
        << "  --dry-run                 List package contents without building or signing\n"
        << "  --json                    With --dry-run, print the package plan as JSON\n"
        << "  --keep-failed-artifact    Preserve generated artifacts after a failed package step\n"
        << "  --verbose, -v             Show detailed packaging output\n"
        << "  --help, -h                Show this help\n"
        << "\n"
        << "Target-specific install/signing options are documented in docs/tools/cli.md.\n"
        << "\n"
        << "Examples:\n"
        << "  zanna package                       Package current dir for host platform\n"
        << "  zanna package myapp/ --target linux  Build .deb for Linux\n"
        << "  zanna package . --target windows -o myapp-setup.exe\n"
        << "\n"
        << "Output formats:\n"
        << "  macOS:    .app bundle in .zip (Finder-native, drag to /Applications)\n"
        << "  DMG:      .app bundle in a .dmg with an /Applications drag-target (macOS host)\n"
        << "  Linux:    .deb package (dpkg -i), includes .desktop + MIME types\n"
        << "  Bundle:   self-extracting Linux .run bundle (portable, no install required)\n"
        << "  RPM:      .rpm package (dnf/yum) — requires rpmbuild on the build host\n"
        << "  Windows:  PE32+ .exe with embedded ZIP (assets, shortcuts, uninstaller)\n"
        << "  Tarball:  .tar.gz portable archive\n"
        << "\n"
        << "macOS code-signing/notarization run only on a macOS host; Windows Authenticode\n"
        << "signing runs only where signtool is available. .dmg builds on a macOS host and\n"
        << ".rpm on an rpmbuild-capable host.\n"
        << "Manifest directives and signing options are documented in docs/tools/cli.md.\n";
}

/// @brief Parsed command-line arguments for the `zanna package` subcommand.
/// @details Covers the target/output selection plus the macOS signing and Windows
///          installer/signing option families; *Set companion flags record whether
///          an option was explicitly provided so manifest defaults can apply.
struct PackageArgs {
    std::string target; ///< Project directory or manifest supplied positionally.
    PackageTarget platformTarget{PackageTarget::Auto}; ///< Requested output format.
    std::string outputPath;                            ///< Explicit artifact output path.
    std::string archOverride;                          ///< Requested `x64` or `arm64` architecture.
    std::string executablePath; ///< Optional prebuilt native executable to package.
    std::string macosSignMode;  ///< Command-line macOS signing mode override.
    bool macosSignModeSet{false}; ///< Whether @c macosSignMode was explicitly supplied.
    std::string macosSignIdentity; ///< Command-line macOS signing identity override.
    bool macosSignIdentitySet{false}; ///< Whether the signing identity was supplied.
    std::string macosEntitlements; ///< Command-line entitlements path override.
    bool macosEntitlementsSet{false}; ///< Whether the entitlements option was supplied.
    std::string macosNotaryProfile; ///< Command-line notary profile override.
    bool macosNotaryProfileSet{false}; ///< Whether the notary profile was supplied.
    bool macosHardenedRuntime{false}; ///< Whether hardened runtime was requested.
    bool macosHardenedRuntimeSet{false}; ///< Whether the hardened-runtime flag was supplied.
    bool macosStaple{false}; ///< Whether notarization tickets should be stapled.
    bool macosStapleSet{false}; ///< Whether the staple flag was supplied.
    std::string windowsInstallScope; ///< Machine/user installation scope override.
    bool windowsInstallScopeSet{false}; ///< Whether the install scope was supplied.
    std::string windowsInstallDir; ///< Windows installation-directory name override.
    bool windowsInstallDirSet{false}; ///< Whether the install-directory option was supplied.
    bool windowsSign{false}; ///< Whether Authenticode signing was requested.
    bool windowsSignSet{false}; ///< Whether the Windows signing flag was supplied.
    std::string windowsSignPfx; ///< PFX certificate path override.
    bool windowsSignPfxSet{false}; ///< Whether a PFX path was supplied.
    std::string windowsSignThumbprint; ///< Certificate-store SHA-1 thumbprint override.
    bool windowsSignThumbprintSet{false}; ///< Whether a thumbprint was supplied.
    std::string windowsTimestampUrl; ///< Authenticode timestamp service URL override.
    bool windowsTimestampUrlSet{false}; ///< Whether a timestamp URL was supplied.
    std::string windowsSigntoolPath; ///< Authenticode signing tool path override.
    bool windowsSigntoolPathSet{false}; ///< Whether a signing tool path was supplied.
    bool windowsSignNoVerify{false}; ///< Whether post-signature verification is disabled.
    bool windowsSignNoVerifySet{false}; ///< Whether the no-verify flag was supplied.
    std::string linuxSignKey; ///< OpenPGP key selector for Linux package signing.
    bool linuxSignKeySet{false}; ///< Whether a Linux signing key was supplied.
    bool dryRun{false}; ///< Whether to describe the package without building it.
    bool jsonOutput{false}; ///< Whether dry-run output uses JSON.
    bool keepFailedArtifact{false}; ///< Whether partial artifacts survive failure.
    bool verbose{false}; ///< Whether detailed progress output is enabled.
    bool help{false}; ///< Whether help was printed and normal execution should stop.
};

/// @brief Determine the packaging target matching the host build platform.
/// @return Native package target selected from the compile-time host capabilities.
PackageTarget detectHostPlatform() {
#if ZANNA_HOST_MACOS
    return PackageTarget::MacOS;
#elif ZANNA_HOST_LINUX
    return PackageTarget::Linux;
#elif ZANNA_HOST_WINDOWS
    return PackageTarget::Windows;
#else
    return PackageTarget::Tarball;
#endif
}

/// @brief Parse a package target name from the command line.
/// @param value User supplied target value.
/// @param out Receives the parsed target on success.
/// @return True when @p value is one of macos, linux, windows, or tarball.
bool parsePackageTargetValue(std::string_view value, PackageTarget &out) {
    if (value == "macos")
        out = PackageTarget::MacOS;
    else if (value == "linux")
        out = PackageTarget::Linux;
    else if (value == "windows")
        out = PackageTarget::Windows;
    else if (value == "tarball")
        out = PackageTarget::Tarball;
    else if (value == "linux-bundle" || value == "appimage")
        out = PackageTarget::AppImage;
    else if (value == "rpm")
        out = PackageTarget::Rpm;
    else if (value == "dmg")
        out = PackageTarget::Dmg;
    else
        return false;
    return true;
}

/// @brief Return true when @p value is a supported package architecture name.
/// @param value Architecture spelling to validate.
/// @return true for `x64` or `arm64`; false otherwise.
bool isPackageArchName(std::string_view value) {
    return value == "x64" || value == "arm64";
}

/// @brief Remove a generated artifact unless the user requested failure preservation.
/// @details Package failures are often easier to diagnose by inspecting the
///          partially generated executable or installer. This helper centralizes
///          the `--keep-failed-artifact` policy so success cleanup remains
///          unchanged while failure paths are consistent.
/// @param path Artifact path to remove.
/// @param keep True when `--keep-failed-artifact` was supplied.
/// @param ec Receives any best-effort filesystem removal error.
void removeFailedArtifactUnlessKept(std::string_view path, bool keep, std::error_code &ec) {
    if (keep || path.empty())
        return;
    fs::remove(zanna::filesystem::pathFromUtf8(path), ec);
}

/// @brief Return the lowercase platform name for a target (e.g. "macos").
/// @param t Package target to describe.
/// @return Normalized platform label used in metadata and filenames.
std::string platformName(PackageTarget t) {
    switch (t) {
        case PackageTarget::MacOS:
            return "macos";
        case PackageTarget::Linux:
            return "linux";
        case PackageTarget::Windows:
            return "windows";
        case PackageTarget::Tarball:
            return "tarball";
        case PackageTarget::AppImage:
            return "linux";
        case PackageTarget::Rpm:
            return "linux";
        case PackageTarget::Dmg:
            return "macos";
        default:
            return "unknown";
    }
}

/// @brief Return the output file extension for a target (e.g. ".deb").
/// @param t Package target whose artifact suffix is requested.
/// @return Conventional filename extension including the leading period.
std::string platformExtension(PackageTarget t) {
    switch (t) {
        case PackageTarget::MacOS:
            return ".zip";
        case PackageTarget::Linux:
            return ".deb";
        case PackageTarget::Windows:
            return ".exe";
        case PackageTarget::Tarball:
            return ".tar.gz";
        case PackageTarget::AppImage:
            return ".run";
        case PackageTarget::Rpm:
            return ".rpm";
        case PackageTarget::Dmg:
            return ".dmg";
        default:
            return ".zip";
    }
}

/// @brief Escape a string for JSON output.
/// @details The package plan JSON is intentionally hand-written to avoid adding
///          dependencies. This helper handles the JSON string escapes needed for
///          paths, metadata fields, and diagnostic-friendly text values.
/// @param text Raw UTF-8 byte string.
/// @return JSON string literal body without surrounding quotes.
std::string jsonEscape(const std::string &text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

/// @brief Emit a JSON property containing an array of strings.
/// @details Used by dry-run JSON output to keep repeated string-array formatting
///          consistent while preserving the repository's dependency-free JSON
///          writer approach.
/// @param out Stream receiving the JSON property.
/// @param name Property name to emit.
/// @param values Raw string values to escape and write.
void writeJsonStringArray(std::ostream &out,
                          const char *name,
                          const std::vector<std::string> &values) {
    out << "  \"" << name << "\": [";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            out << ", ";
        out << "\"" << jsonEscape(values[i]) << "\"";
    }
    out << "]";
}

/// @brief Sanitize a string for use as a filename component.
/// @details Keeps alphanumerics and `._-+~`, replaces other characters with `_`,
///          strips leading `.`/`-`, and returns @p fallback if the result is empty
///          or "."/"..".
/// @param text Raw metadata component to sanitize.
/// @param fallback Value returned if sanitization produces no safe component.
/// @return A filesystem-safe, non-special filename component.
std::string sanitizeOutputFileComponent(const std::string &text, const std::string &fallback) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '_' || c == '-' || c == '+' || c == '~')
            out.push_back(c);
        else
            out.push_back('_');
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '-'))
        out.erase(out.begin());
    if (out.empty() || out == "." || out == "..")
        return fallback;
    return out;
}

/// @brief Build the default output filename `<name>-<version>-<platform>-<arch><ext>`.
/// @details Each component is sanitized; used when the user does not pass `-o`.
/// @param proj Loaded project supplying the package name.
/// @param version Effective package version.
/// @param target Selected output package format.
/// @param archStr Selected native architecture name.
/// @return Sanitized artifact filename in the current directory.
std::string defaultPackageOutputPath(const ProjectConfig &proj,
                                     const std::string &version,
                                     PackageTarget target,
                                     const std::string &archStr) {
    return sanitizeOutputFileComponent(proj.name, "package") + "-" +
           sanitizeOutputFileComponent(version, "0.0.0") + "-" + platformName(target) + "-" +
           sanitizeOutputFileComponent(archStr, "native") + platformExtension(target);
}

/// @brief Validate a version string for portable archive naming.
/// @details Portable application archives do not need Debian package syntax.
///          This accepts any non-empty single-line version that is not "." or
///          ".." and contains no path separators. Characters outside the
///          filesystem-friendly set are normalized later by
///          portableArchiveVersionComponent(), preserving support for metadata
///          separators such as Debian epochs without making tarball dry-runs
///          stricter than the tarball builder.
/// @param version Version text from project metadata.
/// @throws std::runtime_error when the version cannot be used in an archive path.
void validatePortablePackageVersion(const std::string &version) {
    if (version.empty())
        throw std::runtime_error("package version must not be empty");
    zanna::pkg::validateSingleLineField(version, "package version");
    if (version == "." || version == "..")
        throw std::runtime_error("package version must not be a special path segment");
    if (version.find('/') != std::string::npos || version.find('\\') != std::string::npos)
        throw std::runtime_error("package version must not contain path separators: " + version);
}

/// @brief Sanitize a version string into a portable filename component.
/// @details Keeps alphanumerics and `._+~-`, replaces any other safe metadata
///          separator with `_`, and validates the raw version first so the
///          result cannot be empty or a special path segment.
/// @param version Version text from project metadata.
/// @return Filesystem-safe version component for portable archive top dirs.
std::string portableArchiveVersionComponent(const std::string &version) {
    validatePortablePackageVersion(version);
    std::string out;
    out.reserve(version.size());
    for (char c : version) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '_' || c == '+' || c == '~' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty() || out == "." || out == "..")
        throw std::runtime_error("package version does not form a portable archive path");
    return out;
}

/// @brief Read a little-endian uint16 at @p off (caller must bounds-check).
/// @param data Byte buffer containing the integer.
/// @param off Offset of the first encoded byte.
/// @return Decoded 16-bit unsigned value.
uint16_t readLE16(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
}

/// @brief Read a little-endian uint32 at @p off (caller must bounds-check).
/// @param data Byte buffer containing the integer.
/// @param off Offset of the first encoded byte.
/// @return Decoded 32-bit unsigned value.
uint32_t readLE32(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
           (static_cast<uint32_t>(data[off + 2]) << 16) |
           (static_cast<uint32_t>(data[off + 3]) << 24);
}

/// @brief Read a big-endian uint32 at @p off (caller must bounds-check).
/// @param data Byte buffer containing the integer.
/// @param off Offset of the first encoded byte.
/// @return Decoded 32-bit unsigned value.
uint32_t readBE32(const std::vector<uint8_t> &data, size_t off) {
    return (static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
           (static_cast<uint32_t>(data[off + 2]) << 8) | static_cast<uint32_t>(data[off + 3]);
}

/// @brief Read up to 64 KiB of an executable's leading bytes for format detection.
/// @param path UTF-8 path of the executable to inspect.
/// @return Leading file bytes, capped at 64 KiB.
/// @throws std::runtime_error on open/size/seek/read failure.
std::vector<uint8_t> readExecutableHeader(const std::string &path) {
    constexpr std::streamoff kMaxHeaderBytes = 64 * 1024;
    std::ifstream f(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("cannot read executable: " + path);
    const std::streamoff fileSize = f.tellg();
    if (fileSize < 0)
        throw std::runtime_error("cannot determine executable size: " + path);
    const std::streamoff readSize = std::min(fileSize, kMaxHeaderBytes);
    f.seekg(0);
    if (!f)
        throw std::runtime_error("cannot seek executable: " + path);
    std::vector<uint8_t> data(static_cast<size_t>(readSize));
    if (!data.empty()) {
        f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f || f.gcount() != static_cast<std::streamsize>(data.size()))
            throw std::runtime_error("incomplete read of executable header: " + path);
    }
    return data;
}

/// @brief Return a human-readable name for an executable format (e.g. "Mach-O").
/// @param format Executable container format.
/// @return Stable diagnostic label for @p format.
std::string formatName(ExecutableFormat format) {
    switch (format) {
        case ExecutableFormat::Unknown:
            return "unknown";
        case ExecutableFormat::MachO:
            return "Mach-O";
        case ExecutableFormat::ELF:
            return "ELF";
        case ExecutableFormat::PE:
            return "PE";
    }
    return "unknown";
}

/// @brief Format a uint16 as a hexadecimal string (used in error messages).
/// @param value Integer to format without a prefix.
/// @return Lowercase hexadecimal representation of @p value.
std::string hexU16(uint16_t value) {
    std::ostringstream os;
    os << std::hex << value;
    return os.str();
}

/// @brief Return the native executable format expected for a package target.
/// @param target Package target whose executable payload is being validated.
/// @return Required native executable container format.
/// @throws std::runtime_error for the tarball target (no executable format).
ExecutableFormat expectedExecutableFormat(PackageTarget target) {
    switch (target) {
        case PackageTarget::MacOS:
            return ExecutableFormat::MachO;
        case PackageTarget::Linux:
            return ExecutableFormat::ELF;
        case PackageTarget::Windows:
            return ExecutableFormat::PE;
        case PackageTarget::Tarball:
            throw std::runtime_error(
                "tarball packages do not require a platform executable format");
        case PackageTarget::AppImage:
            return ExecutableFormat::ELF;
        case PackageTarget::Rpm:
            return ExecutableFormat::ELF;
        case PackageTarget::Dmg:
            return ExecutableFormat::MachO;
        case PackageTarget::Auto:
        default:
            return ExecutableFormat::ELF;
    }
}

/// @brief Detect the format and architecture of a native executable.
/// @details Inspects the leading header bytes for Mach-O, ELF, or PE magic and
///          decodes the machine field into an "x64"/"arm64" arch string.
/// @param path UTF-8 path of the native executable.
/// @return Detected container format and normalized architecture.
/// @throws std::runtime_error when the file is too small or unrecognized.
ExecutableInfo inspectExecutable(const std::string &path) {
    const auto data = readExecutableHeader(path);
    if (data.size() < 20)
        throw std::runtime_error("executable is too small to identify: " + path);

    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        if (data[4] != 2 || data[5] != 1)
            throw std::runtime_error("ELF executable must be 64-bit little-endian: " + path);
        const uint16_t machine = readLE16(data, 18);
        if (machine == 62)
            return {ExecutableFormat::ELF, "x64"};
        if (machine == 183)
            return {ExecutableFormat::ELF, "arm64"};
        throw std::runtime_error("ELF executable has unsupported machine type: " +
                                 std::to_string(machine));
    }

    if (data[0] == 'M' && data[1] == 'Z') {
        if (data.size() < 64)
            throw std::runtime_error("PE executable is truncated: " + path);
        const size_t peOff = readLE32(data, 60);
        if (peOff >= data.size() || peOff > data.size() - 24 || data[peOff] != 'P' ||
            data[peOff + 1] != 'E' || data[peOff + 2] != 0 || data[peOff + 3] != 0) {
            throw std::runtime_error("PE executable has an invalid header: " + path);
        }
        const uint16_t machine = readLE16(data, peOff + 4);
        if (machine == 0x8664)
            return {ExecutableFormat::PE, "x64"};
        if (machine == 0xAA64)
            return {ExecutableFormat::PE, "arm64"};
        throw std::runtime_error("PE executable has unsupported machine type: 0x" +
                                 hexU16(machine));
    }

    const uint32_t magicBE = readBE32(data, 0);
    const uint32_t magicLE = readLE32(data, 0);
    if (magicLE == 0xFEEDFACF || magicBE == 0xFEEDFACF) {
        const bool little = magicLE == 0xFEEDFACF;
        const uint32_t cputype = little ? readLE32(data, 4) : readBE32(data, 4);
        if (cputype == 0x01000007u)
            return {ExecutableFormat::MachO, "x64"};
        if (cputype == 0x0100000Cu)
            return {ExecutableFormat::MachO, "arm64"};
        throw std::runtime_error("Mach-O executable has unsupported CPU type: " +
                                 std::to_string(cputype));
    }
    if ((magicBE == 0xCAFEBABEu || magicBE == 0xCAFEBABFu) && data.size() >= 8) {
        const uint32_t count = readBE32(data, 4);
        const size_t archSize = magicBE == 0xCAFEBABFu ? 32u : 20u;
        if (count == 0 || count > 64 || data.size() < 8 + static_cast<size_t>(count) * archSize)
            throw std::runtime_error("Mach-O universal binary has an invalid fat header: " + path);
        bool hasX64 = false;
        bool hasArm64 = false;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t off = 8 + static_cast<size_t>(i) * archSize;
            const uint32_t cputype = readBE32(data, off);
            if (cputype == 0x01000007u)
                hasX64 = true;
            else if (cputype == 0x0100000Cu)
                hasArm64 = true;
        }
        if (hasX64 && hasArm64)
            return {ExecutableFormat::MachO, "universal"};
        if (hasX64)
            return {ExecutableFormat::MachO, "x64"};
        if (hasArm64)
            return {ExecutableFormat::MachO, "arm64"};
        throw std::runtime_error("Mach-O universal binary does not contain x64 or arm64: " + path);
    }

    throw std::runtime_error("executable format is not Mach-O, ELF, or PE: " + path);
}

/// @brief Verify a prebuilt executable matches the requested target and arch.
/// @details Checks the container format against the target and the architecture
///          against @p archStr (universal binaries match any). Tarball packages
///          are portable archives, so prebuilt payloads are only required to be
///          readable by the tarball builder and are not inspected as native
///          Mach-O/ELF/PE executables here.
/// @param path UTF-8 path of the executable payload.
/// @param target Package target that determines the required container format.
/// @param archStr Required `x64` or `arm64` architecture.
/// @throws std::runtime_error describing the mismatch.
void validateExecutableForPackageTarget(const std::string &path,
                                        PackageTarget target,
                                        const std::string &archStr) {
    if (target == PackageTarget::Tarball)
        return;

    const ExecutableInfo info = inspectExecutable(path);
    const ExecutableFormat expected = expectedExecutableFormat(target);
    if (info.format != expected) {
        throw std::runtime_error("executable format " + formatName(info.format) +
                                 " does not match package target " + platformName(target) +
                                 " (expected " + formatName(expected) + ")");
    }
    if (info.arch != "universal" && info.arch != archStr) {
        throw std::runtime_error("executable architecture '" + info.arch +
                                 "' does not match selected package architecture '" + archStr +
                                 "'");
    }
}

/// @brief Read an environment variable, returning "" when it is unset.
/// @param name Environment variable name, or null for an empty lookup.
/// @return UTF-8 value when present; otherwise an empty string.
std::string getenvOrEmpty(const char *name) {
    const auto value =
        zanna::environment::getUtf8(name ? std::string_view(name) : std::string_view{});
    return value.value_or(std::string{});
}

/// @brief Locate one statically linked native Windows installer support executable.
/// @details An explicit environment override supports controlled cross-packaging. On Windows,
///          normal installed and build-tree layouts place the executable beside zanna.exe.
/// @param proj Loaded project used to resolve relative override paths.
/// @param environmentName Name of the environment variable providing an override.
/// @param fileName Support executable filename to search for.
/// @return Resolved regular-file path, or an empty path if no candidate exists.
/// @throws std::runtime_error if an explicit override is not a regular file.
fs::path findWindowsInstallerSupportExecutable(const ProjectConfig &proj,
                                               const char *environmentName,
                                               std::string_view fileName) {
    const std::string overridePath = getenvOrEmpty(environmentName);
    if (!overridePath.empty()) {
        fs::path resolved = zanna::filesystem::pathFromUtf8(overridePath);
        if (!resolved.is_absolute())
            resolved = zanna::filesystem::pathFromUtf8(proj.rootDir) / resolved;
        resolved = resolved.lexically_normal();
        if (!fs::is_regular_file(resolved))
            throw std::runtime_error(std::string(environmentName) + " is not a regular file: " +
                                     zanna::filesystem::pathToUtf8(resolved));
        return resolved;
    }
#if ZANNA_HOST_WINDOWS
    std::wstring modulePath(512, L'\0');
    while (modulePath.size() <= 32768) {
        const DWORD length =
            GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length == 0)
            break;
        if (length < modulePath.size() - 1) {
            modulePath.resize(length);
            const fs::path executableDir = fs::path(modulePath).parent_path();
            const std::array<fs::path, 2> candidates = {
                executableDir / zanna::filesystem::pathFromUtf8(fileName),
                executableDir.parent_path().parent_path().parent_path() / executableDir.filename() /
                    zanna::filesystem::pathFromUtf8(fileName)};
            for (const fs::path &candidate : candidates) {
                if (fs::is_regular_file(candidate))
                    return candidate;
            }
            break;
        }
        modulePath.resize(modulePath.size() * 2U);
    }
#else
    (void)proj;
#endif
    return {};
}

/// @brief Resolve @p pathText relative to @p projectRoot (absolute paths kept as-is).
/// @param projectRoot UTF-8 project root directory.
/// @param pathText UTF-8 absolute or project-relative path.
/// @return Lexically normalized filesystem path.
fs::path resolveOptionalProjectPath(const std::string &projectRoot, const std::string &pathText) {
    fs::path p = zanna::filesystem::pathFromUtf8(pathText);
    if (p.is_absolute())
        return p.lexically_normal();
    return (zanna::filesystem::pathFromUtf8(projectRoot) / p).lexically_normal();
}

/// @brief Return true if the package config requests Windows Authenticode signing.
/// @param pkg Effective package configuration.
/// @return true when signing is enabled or certificate material was specified.
bool windowsSigningRequested(const zanna::pkg::PackageConfig &pkg) {
    return pkg.windowsSign || !pkg.windowsSignPfx.empty() || !pkg.windowsSignThumbprint.empty();
}

/// @brief Sign a Windows installer artifact when signing is requested.
/// @details Resolves the PFX path / certificate thumbprint (falling back to the
///          ZANNA_WINDOWS_SIGN_* environment variables), then invokes signtool;
///          a no-op success when no signing was requested.
/// @param proj Loaded project containing effective signing configuration.
/// @param artifactPath Installer artifact to sign in place.
/// @param verbose Whether to report successful signing.
/// @param err Stream that receives configuration, signing, and verification diagnostics.
/// @return true on success or when signing was not requested; false on failure.
bool signWindowsInstallerArtifact(const ProjectConfig &proj,
                                  const fs::path &artifactPath,
                                  bool verbose,
                                  std::ostream &err) {
    const auto &pkg = proj.packageConfig;
    if (!windowsSigningRequested(pkg))
        return true;

    std::string pfxPath = pkg.windowsSignPfx;
    if (pfxPath.empty())
        pfxPath = getenvOrEmpty("ZANNA_WINDOWS_SIGN_PFX");
    std::string thumbprint = pkg.windowsSignThumbprint;
    if (thumbprint.empty())
        thumbprint = getenvOrEmpty("ZANNA_WINDOWS_SIGN_THUMBPRINT");
    try {
        thumbprint = zanna::pkg::normalizeWindowsCertificateThumbprint(
            thumbprint, "Windows signing thumbprint");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }
    if (pfxPath.empty() && thumbprint.empty()) {
        err << "error: Windows signing requested but no PFX was provided "
               "and no certificate thumbprint was provided (use --windows-sign-pfx, "
               "windows-sign-pfx, ZANNA_WINDOWS_SIGN_PFX, --windows-sign-thumbprint, "
               "windows-sign-thumbprint, or ZANNA_WINDOWS_SIGN_THUMBPRINT)\n";
        return false;
    }
    fs::path resolvedPfx;
    std::string password;
    if (!pfxPath.empty()) {
        resolvedPfx = resolveOptionalProjectPath(proj.rootDir, pfxPath);
        if (!fs::is_regular_file(resolvedPfx)) {
            err << "error: Windows signing PFX not found: "
                << zanna::filesystem::pathToUtf8(resolvedPfx) << "\n";
            return false;
        }
        password = getenvOrEmpty("ZANNA_WINDOWS_SIGN_PASSWORD");
        if (password.empty()) {
            err << "error: Windows PFX signing requires ZANNA_WINDOWS_SIGN_PASSWORD\n";
            return false;
        }
        const std::string allowPasswordArgv = getenvOrEmpty("ZANNA_WINDOWS_SIGN_PASSWORD_ARGV_OK");
        if (allowPasswordArgv != "1" && allowPasswordArgv != "true" &&
            allowPasswordArgv != "TRUE") {
            err << "error: PFX password signing passes the password to signtool argv; use "
                   "certificate-store thumbprint signing or set "
                   "ZANNA_WINDOWS_SIGN_PASSWORD_ARGV_OK=1 to acknowledge the exposure\n";
            return false;
        }
    }

    std::string timestampUrl = pkg.windowsTimestampUrl;
    if (timestampUrl.empty())
        timestampUrl = getenvOrEmpty("ZANNA_WINDOWS_TIMESTAMP_URL");
    if (timestampUrl.empty())
        timestampUrl = "https://timestamp.digicert.com";
    try {
        zanna::pkg::validateHttpsPackageUrl(timestampUrl, "Windows timestamp URL");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }

    std::string signtool = pkg.windowsSigntoolPath;
    if (signtool.empty())
        signtool = getenvOrEmpty("ZANNA_WINDOWS_SIGNTOOL");
    if (signtool.empty())
        signtool = "signtool.exe";

    std::vector<std::string> signCmd = {
        signtool, "sign", "/fd", "SHA256", "/tr", timestampUrl, "/td", "SHA256"};
    if (!thumbprint.empty()) {
        signCmd.push_back("/sha1");
        signCmd.push_back(thumbprint);
    } else {
        signCmd.push_back("/f");
        signCmd.push_back(zanna::filesystem::pathToUtf8(resolvedPfx));
        signCmd.push_back("/p");
        signCmd.push_back(password);
    }
    const std::string artifactPathUtf8 = zanna::filesystem::pathToUtf8(artifactPath);
    signCmd.push_back(artifactPathUtf8);
    const RunResult signResult = run_process(signCmd);
    if (signResult.exit_code != 0) {
        err << "error: signtool sign failed with exit code " << signResult.exit_code << "\n"
            << signResult.out << signResult.err;
        return false;
    }

    if (!pkg.windowsSignNoVerify) {
        const RunResult verifyResult =
            run_process({signtool, "verify", "/pa", "/all", "/tw", "/v", artifactPathUtf8});
        if (verifyResult.exit_code != 0) {
            err << "error: signtool verify failed with exit code " << verifyResult.exit_code << "\n"
                << verifyResult.out << verifyResult.err;
            return false;
        }
    }

    if (verbose)
        err << "  Windows signing: passed\n";
    return true;
}

/// @brief Authenticode-sign one in-memory payload PE without changing project outputs.
/// @details Nested application binaries and the generated uninstaller are copied to a
///          private temporary directory, signed and verified under the same policy as
///          the outer setup executable, read back, and removed on every exit path.
/// @param proj Loaded project containing effective signing configuration.
/// @param logicalName Payload name used for the temporary file and diagnostics.
/// @param unsignedPe Unsigned PE image bytes.
/// @param verbose Whether successful signing should be reported.
/// @return Signed PE image bytes.
/// @throws std::runtime_error when temporary I/O or signing fails.
std::vector<uint8_t> signWindowsPeBytes(const ProjectConfig &proj,
                                        std::string_view logicalName,
                                        const std::vector<uint8_t> &unsignedPe,
                                        bool verbose) {
    const fs::path tempDir =
        zanna::pkg::createUniqueTempDirectory(fs::temp_directory_path(), "zanna-pe-sign");
    try {
        fs::path leaf = zanna::filesystem::pathFromUtf8(logicalName).filename();
        if (leaf.empty())
            leaf = "payload.exe";
        const fs::path tempPe = tempDir / leaf;
        zanna::pkg::writeFileAtomic(tempPe, unsignedPe);

        std::ostringstream signingError;
        if (!signWindowsInstallerArtifact(proj, tempPe, verbose, signingError)) {
            throw std::runtime_error("Authenticode signing failed for '" +
                                     std::string(logicalName) + "': " + signingError.str());
        }
        std::vector<uint8_t> signedPe = zanna::pkg::readFile(tempPe);
        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        return signedPe;
    } catch (...) {
        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        throw;
    }
}

/// @brief Consume a package option value from inline or following-argument syntax.
/// @details Matches either `--name=value` or `--name value`, advances @p index
///          only for the separated form, and optionally rejects an empty value.
///          Keeping this in one place makes target-specific package options
///          behave like the top-level target/output options.
/// @param arg Current argument token.
/// @param optionName Option name including leading dashes.
/// @param index Current argv index, advanced when the value is the next token.
/// @param argc Total argument count.
/// @param argv Argument vector.
/// @param value Receives the parsed value when the option matched.
/// @param description User-facing noun for missing-value diagnostics.
/// @param allowEmpty Whether `--name=` or `--name ""` is accepted.
/// @param matched Receives whether @p arg matched @p optionName at all.
/// @return True when @p arg matched @p optionName and @p value was populated.
bool consumePackageOptionValue(std::string_view arg,
                               std::string_view optionName,
                               int &index,
                               int argc,
                               char **argv,
                               std::string &value,
                               std::string_view description,
                               bool allowEmpty,
                               bool &matched) {
    matched = false;
    std::string_view inlineValue;
    if (ilc::splitInlineOptionValue(arg, optionName, inlineValue)) {
        matched = true;
        if (!allowEmpty && inlineValue.empty()) {
            std::cerr << "error: " << optionName << " requires " << description << "\n";
            return false;
        }
        value = std::string(inlineValue);
        return true;
    }
    if (arg != optionName)
        return false;
    matched = true;
    if (index + 1 >= argc) {
        std::cerr << "error: " << optionName << " requires " << description << "\n";
        return false;
    }
    value = argv[++index];
    if (!allowEmpty && value.empty()) {
        std::cerr << "error: " << optionName << " requires " << description << "\n";
        return false;
    }
    return true;
}

/// @brief Parse the `zanna package` command-line arguments into @p args.
/// @details Handles the target/output/arch/executable options plus the macOS and
///          Windows signing option families; prints usage and returns false on a
///          malformed or missing-value argument.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and package subcommand.
/// @param args Output structure populated with parsed options and defaults.
/// @return true on a successful parse.
bool parsePackageArgs(int argc, char **argv, PackageArgs &args) {
    for (int i = 0; i < argc; i++) {
        std::string arg = argv[i];
        bool matched = false;
        std::string val;
        if (consumePackageOptionValue(
                arg, "--target", i, argc, argv, val, "a value", false, matched)) {
            if (!parsePackageTargetValue(val, args.platformTarget)) {
                std::cerr << "error: unknown target '" << val
                          << "'; expected macos, linux, windows, linux-bundle, rpm, dmg, or "
                             "tarball\n";
                return false;
            }
            if (val == "appimage") {
                std::cerr << "warning: --target appimage is a legacy alias for linux-bundle; "
                             "Zanna's self-extractor is emitted as .run, not the AppImage "
                             "specification\n";
            }
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--arch",
                                             i,
                                             argc,
                                             argv,
                                             args.archOverride,
                                             "a value",
                                             false,
                                             matched)) {
            if (!isPackageArchName(args.archOverride)) {
                std::cerr << "error: unknown arch '" << args.archOverride
                          << "'; expected x64 or arm64\n";
                return false;
            }
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--executable",
                                             i,
                                             argc,
                                             argv,
                                             args.executablePath,
                                             "a path",
                                             false,
                                             matched)) {
            // Parsed above.
        } else if (matched) {
            return false;
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires a path\n";
                return false;
            }
            args.outputPath = argv[++i];
            if (args.outputPath.empty()) {
                std::cerr << "error: -o requires a path\n";
                return false;
            }
        } else if (consumePackageOptionValue(
                       arg, "--output", i, argc, argv, args.outputPath, "a path", false, matched)) {
            // Parsed above.
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--macos-sign-mode",
                                             i,
                                             argc,
                                             argv,
                                             args.macosSignMode,
                                             "a value",
                                             true,
                                             matched)) {
            args.macosSignModeSet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--macos-sign-identity",
                                             i,
                                             argc,
                                             argv,
                                             args.macosSignIdentity,
                                             "a value",
                                             true,
                                             matched)) {
            args.macosSignIdentitySet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--macos-entitlements",
                                             i,
                                             argc,
                                             argv,
                                             args.macosEntitlements,
                                             "a path",
                                             true,
                                             matched)) {
            args.macosEntitlementsSet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--macos-notary-profile",
                                             i,
                                             argc,
                                             argv,
                                             args.macosNotaryProfile,
                                             "a value",
                                             true,
                                             matched)) {
            args.macosNotaryProfileSet = true;
        } else if (matched) {
            return false;
        } else if (arg == "--macos-hardened-runtime") {
            args.macosHardenedRuntime = true;
            args.macosHardenedRuntimeSet = true;
        } else if (arg == "--macos-staple") {
            args.macosStaple = true;
            args.macosStapleSet = true;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-install-scope",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsInstallScope,
                                             "a value",
                                             true,
                                             matched)) {
            args.windowsInstallScopeSet = true;
            if (!args.windowsInstallScope.empty() && args.windowsInstallScope != "machine" &&
                args.windowsInstallScope != "user") {
                std::cerr << "error: unknown Windows install scope '" << args.windowsInstallScope
                          << "'; expected machine or user\n";
                return false;
            }
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-install-dir",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsInstallDir,
                                             "a value",
                                             true,
                                             matched)) {
            args.windowsInstallDirSet = true;
        } else if (matched) {
            return false;
        } else if (arg == "--windows-sign") {
            args.windowsSign = true;
            args.windowsSignSet = true;
        } else if (consumePackageOptionValue(arg,
                                             "--linux-sign-key",
                                             i,
                                             argc,
                                             argv,
                                             args.linuxSignKey,
                                             "a value",
                                             true,
                                             matched)) {
            args.linuxSignKeySet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-sign-pfx",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsSignPfx,
                                             "a path",
                                             true,
                                             matched)) {
            args.windowsSignPfxSet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-sign-thumbprint",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsSignThumbprint,
                                             "a SHA-1 thumbprint",
                                             true,
                                             matched)) {
            args.windowsSignThumbprintSet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-timestamp-url",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsTimestampUrl,
                                             "a URL",
                                             true,
                                             matched)) {
            args.windowsTimestampUrlSet = true;
        } else if (matched) {
            return false;
        } else if (consumePackageOptionValue(arg,
                                             "--windows-signtool",
                                             i,
                                             argc,
                                             argv,
                                             args.windowsSigntoolPath,
                                             "a path",
                                             true,
                                             matched)) {
            args.windowsSigntoolPathSet = true;
        } else if (matched) {
            return false;
        } else if (arg == "--windows-sign-no-verify") {
            args.windowsSignNoVerify = true;
            args.windowsSignNoVerifySet = true;
        } else if (arg == "--dry-run") {
            args.dryRun = true;
        } else if (arg == "--json") {
            args.jsonOutput = true;
        } else if (arg == "--keep-failed-artifact") {
            args.keepFailedArtifact = true;
        } else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            packageUsage(std::cout);
            args.help = true;
            return true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            packageUsage();
            return false;
        } else if (args.target.empty()) {
            args.target = arg;
        } else {
            std::cerr << "error: unexpected argument '" << arg << "'\n";
            return false;
        }
    }

    if (args.target.empty())
        args.target = ".";

    if (args.platformTarget == PackageTarget::Auto)
        args.platformTarget = detectHostPlatform();

    return true;
}

/// @brief Verify a project-relative package source path exists on disk.
/// @details Resolves @p path against the project root and checks it is a regular
///          file (or directory when @p allowDirectory); prints an error otherwise.
/// @param proj Loaded project supplying the resolution root.
/// @param path Absolute or project-relative source path.
/// @param fieldName User-facing field name used in diagnostics.
/// @param allowDirectory Whether a directory is accepted in addition to a regular file.
/// @return true when the path exists and is of an acceptable type.
bool validatePackageSourcePathExists(const ProjectConfig &proj,
                                     const std::string &path,
                                     const char *fieldName,
                                     bool allowDirectory = true) {
    fs::path resolved;
    try {
        resolved = zanna::pkg::resolvePackageSourcePath(proj.rootDir, path, fieldName);
    } catch (const std::exception &ex) {
        std::cerr << "error: invalid " << fieldName << " '" << path << "': " << ex.what() << "\n";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec)) {
        if (ec)
            std::cerr << "error: cannot inspect " << fieldName << " '" << path
                      << "': " << ec.message() << "\n";
        else
            std::cerr << "error: " << fieldName << " not found: " << path << "\n";
        return false;
    }
    ec.clear();
    const bool regular = fs::is_regular_file(resolved, ec);
    if (ec) {
        std::cerr << "error: cannot inspect " << fieldName << " '" << path << "': " << ec.message()
                  << "\n";
        return false;
    }
    ec.clear();
    const bool directory = fs::is_directory(resolved, ec);
    if (ec) {
        std::cerr << "error: cannot inspect " << fieldName << " '" << path << "': " << ec.message()
                  << "\n";
        return false;
    }
    if (!regular && !(allowDirectory && directory)) {
        std::cerr << "error: " << fieldName
                  << (allowDirectory ? " is not a regular file or directory: "
                                     : " is not a regular file: ")
                  << path << "\n";
        return false;
    }
    return true;
}

/// @brief Validate the package config (icon, assets, signing) for a given target.
/// @details Checks that referenced source paths exist and that target-specific
///          signing/installer settings are well-formed before building, printing
///          errors to @p err.
/// @param proj Loaded project and effective package configuration to validate.
/// @param target Concrete output format whose policy is applied.
/// @param archStr Selected native architecture name.
/// @param err Stream that receives configuration diagnostics.
/// @param requireSigningCredentials When false, dry-run validation keeps structural
///        signing checks but skips credential/material presence checks that are only
///        needed for an actual package build.
/// @return true when the configuration is valid for @p target / @p archStr.
bool validatePackageConfigForTarget(const ProjectConfig &proj,
                                    PackageTarget target,
                                    const std::string &archStr,
                                    std::ostream &err,
                                    bool requireSigningCredentials = true) {
    const auto &pkg = proj.packageConfig;
    const std::string displayName = pkg.displayName.empty() ? proj.name : pkg.displayName;
    const std::string version = proj.version.empty() ? "0.0.0" : proj.version;

    try {
        zanna::pkg::validateSingleLineField(displayName, "package display name");
        zanna::pkg::validateSingleLineField(pkg.author, "package author");
        zanna::pkg::validateSingleLineField(pkg.description, "package description");
        zanna::pkg::validateSingleLineField(pkg.license, "package license");
        zanna::pkg::validateSingleLineField(pkg.welcomeText, "package welcome text");
        zanna::pkg::validatePackageUrl(pkg.homepage, "package homepage");
        zanna::pkg::validatePackageFileAssociations(pkg.fileAssociations);
        for (const auto &asset : pkg.assets) {
            (void)zanna::pkg::sanitizePackageRelativePath(asset.targetPath, "asset target path");
            if (!validatePackageSourcePathExists(proj, asset.sourcePath, "asset source path"))
                return false;
        }
        if (!pkg.iconPath.empty() &&
            !validatePackageSourcePathExists(proj, pkg.iconPath, "package icon", false))
            return false;
        if (!pkg.licenseFilePath.empty() &&
            !validatePackageSourcePathExists(
                proj, pkg.licenseFilePath, "package license file", false))
            return false;
        if (!pkg.readmeFilePath.empty() &&
            !validatePackageSourcePathExists(proj, pkg.readmeFilePath, "package readme", false))
            return false;

        switch (target) {
            case PackageTarget::MacOS:
            case PackageTarget::Dmg:
                if (displayName.empty() || displayName.find('/') != std::string::npos ||
                    displayName.find('\\') != std::string::npos ||
                    displayName.find(':') != std::string::npos) {
                    throw std::runtime_error(
                        "macOS bundle display name must not be empty or contain path separators");
                }
                zanna::pkg::validateWindowsFileName(displayName, "macOS bundle display name");
                zanna::pkg::validateMacOSBundleIdentifier(pkg.identifier,
                                                          "macOS bundle identifier");
                zanna::pkg::validateDottedNumericVersion(version, "macOS package version");
                if (!pkg.minOsMacos.empty())
                    zanna::pkg::validateDottedNumericVersion(pkg.minOsMacos,
                                                             "minimum macOS version");
                if (requireSigningCredentials)
                    zanna::pkg::validateMacOSSigningConfig(pkg);
                else if (!zanna::pkg::isValidMacOSSignModeText(pkg.macosSignMode)) {
                    throw std::runtime_error("macOS sign mode must be none, preserve, adhoc, or "
                                             "developer-id: " +
                                             pkg.macosSignMode);
                } else {
                    const std::string mode = zanna::pkg::resolveMacOSSignModeForHost(pkg);
                    if (!pkg.macosNotaryProfile.empty() && mode != "developer-id") {
                        throw std::runtime_error(
                            "macOS notarization requires macos-sign-mode developer-id");
                    }
                    if (pkg.macosStaple) {
                        if (mode != "developer-id")
                            throw std::runtime_error(
                                "macos-staple requires macos-sign-mode developer-id");
                        if (pkg.macosNotaryProfile.empty())
                            throw std::runtime_error(
                                "macos-staple requires macos-notary-profile for this package "
                                "build");
                    }
                }
                if (!pkg.macosEntitlements.empty() &&
                    !validatePackageSourcePathExists(
                        proj, pkg.macosEntitlements, "macOS entitlements", false)) {
                    return false;
                }
                if (!pkg.macosDmgBackground.empty() &&
                    !validatePackageSourcePathExists(
                        proj, pkg.macosDmgBackground, "macOS DMG background", false)) {
                    return false;
                }
                if (!pkg.macosDmgIcon.empty() &&
                    !validatePackageSourcePathExists(
                        proj, pkg.macosDmgIcon, "macOS DMG icon", false)) {
                    return false;
                }
                break;
            case PackageTarget::Linux: {
                const std::string debArch = archStr == "x64" ? "amd64" : archStr;
                if (debArch != "amd64" && debArch != "arm64" && debArch != "all") {
                    throw std::runtime_error(
                        "Debian package architecture must be amd64, arm64, or all: " + debArch);
                }
                zanna::pkg::validateDebVersion(version, "package version");
                zanna::pkg::validateDesktopCategories(pkg.category);
                zanna::pkg::validateSingleLineField(pkg.linuxStartupWmClass,
                                                    "Linux StartupWMClass");
                zanna::pkg::validateSingleLineField(pkg.linuxKeywords, "Linux keywords");
                zanna::pkg::validateSingleLineField(pkg.appstreamId, "Linux AppStream id");
                for (const auto &dep : pkg.depends)
                    zanna::pkg::validateDebDependency(dep);
                zanna::pkg::validatePackageHooksAllowed(pkg);
                (void)zanna::pkg::normalizePackageHookScript(pkg.postInstallScript,
                                                             "post-install script");
                (void)zanna::pkg::normalizePackageHookScript(pkg.preUninstallScript,
                                                             "pre-uninstall script");
                (void)zanna::pkg::normalizeDebName(proj.name);
                (void)zanna::pkg::normalizeExecName(proj.name);
                break;
            }
            case PackageTarget::Windows:
                zanna::pkg::validateWindowsFileName(displayName, "Windows display name");
                zanna::pkg::validateWindowsProgIdBase(pkg.identifier, "Windows package identifier");
                zanna::pkg::validateSingleLineField(version, "Windows package version");
                zanna::pkg::validateSingleLineField(pkg.windowsPublisher, "Windows publisher");
                zanna::pkg::validateSingleLineField(pkg.windowsWizardSummary,
                                                    "Windows wizard summary");
                if (!pkg.windowsInstallScope.empty() && pkg.windowsInstallScope != "machine" &&
                    pkg.windowsInstallScope != "user") {
                    throw std::runtime_error("Windows install scope must be machine or user: " +
                                             pkg.windowsInstallScope);
                }
                if (!pkg.windowsInstallDir.empty())
                    zanna::pkg::validateWindowsFileName(pkg.windowsInstallDir,
                                                        "Windows install directory");
                if (!pkg.minOsWindows.empty())
                    zanna::pkg::validateDottedNumericVersion(pkg.minOsWindows,
                                                             "minimum Windows version");
                zanna::pkg::validateHttpsPackageUrl(pkg.windowsTimestampUrl,
                                                    "Windows timestamp URL");
                zanna::pkg::validateSingleLineField(pkg.windowsSigntoolPath,
                                                    "Windows signtool path");
                zanna::pkg::validateWindowsCertificateThumbprint(pkg.windowsSignThumbprint,
                                                                 "Windows signing thumbprint");
                if (requireSigningCredentials && !pkg.windowsSignPfx.empty()) {
                    const fs::path pfx =
                        resolveOptionalProjectPath(proj.rootDir, pkg.windowsSignPfx);
                    if (!fs::is_regular_file(pfx)) {
                        throw std::runtime_error("Windows signing PFX not found: " +
                                                 zanna::filesystem::pathToUtf8(pfx));
                    }
                }
                for (const auto &dllPath : pkg.windowsDlls) {
                    if (!validatePackageSourcePathExists(
                            proj, dllPath, "Windows DLL dependency", false))
                        return false;
                }
                (void)zanna::pkg::normalizeExecName(proj.name);
                break;
            case PackageTarget::Tarball:
                validatePortablePackageVersion(version);
                (void)zanna::pkg::sanitizePackageRelativePath(
                    zanna::pkg::normalizeDebName(proj.name) + "-" +
                        portableArchiveVersionComponent(version),
                    "tarball top-level directory");
                (void)zanna::pkg::normalizeExecName(proj.name);
                break;
            case PackageTarget::AppImage: {
                if (archStr != "x64" && archStr != "arm64") {
                    throw std::runtime_error("Linux bundle architecture must be x64 or arm64: " +
                                             archStr);
                }
                validatePortablePackageVersion(version);
                zanna::pkg::validateDesktopCategories(pkg.category);
                zanna::pkg::validateSingleLineField(pkg.linuxStartupWmClass,
                                                    "Linux StartupWMClass");
                zanna::pkg::validateSingleLineField(pkg.linuxKeywords, "Linux keywords");
                zanna::pkg::validateSingleLineField(pkg.appstreamId, "Linux AppStream id");
                (void)zanna::pkg::normalizeDebName(proj.name);
                (void)zanna::pkg::normalizeExecName(proj.name);
                break;
            }
            case PackageTarget::Rpm: {
                if (archStr != "x64" && archStr != "arm64") {
                    throw std::runtime_error("RPM architecture must be x64 or arm64: " + archStr);
                }
                zanna::pkg::validateRpmVersion(version, "package version");
                zanna::pkg::validateDesktopCategories(pkg.category);
                zanna::pkg::validateSingleLineField(pkg.linuxStartupWmClass,
                                                    "Linux StartupWMClass");
                zanna::pkg::validateSingleLineField(pkg.linuxKeywords, "Linux keywords");
                zanna::pkg::validateSingleLineField(pkg.appstreamId, "Linux AppStream id");
                for (const auto &dep : pkg.rpmDepends)
                    zanna::pkg::validateRpmDependency(dep);
                (void)zanna::pkg::normalizeDebName(proj.name);
                (void)zanna::pkg::normalizeExecName(proj.name);
                break;
            }
            case PackageTarget::Auto:
                break;
        }
    } catch (const std::exception &ex) {
        err << "error: package configuration invalid: " << ex.what() << "\n";
        return false;
    }
    return true;
}

/// @brief Overlay command-line `--macos-*` / `--windows-*` signing & install
///        options onto the project's package config (CLI flags win over the
///        manifest). Only set options override; unset ones leave the manifest
///        value intact.
/// @param proj Loaded project whose package configuration is updated.
/// @param args Parsed command-line overrides and explicit-set flags.
void applyPackageCliOverrides(ProjectConfig &proj, const PackageArgs &args) {
    if (args.macosSignModeSet)
        proj.packageConfig.macosSignMode = args.macosSignMode;
    if (args.macosSignIdentitySet)
        proj.packageConfig.macosSignIdentity = args.macosSignIdentity;
    if (args.macosEntitlementsSet)
        proj.packageConfig.macosEntitlements = args.macosEntitlements;
    if (args.macosNotaryProfileSet)
        proj.packageConfig.macosNotaryProfile = args.macosNotaryProfile;
    if (args.macosHardenedRuntimeSet)
        proj.packageConfig.macosHardenedRuntime = args.macosHardenedRuntime;
    if (args.macosStapleSet)
        proj.packageConfig.macosStaple = args.macosStaple;
    if (args.windowsInstallScopeSet)
        proj.packageConfig.windowsInstallScope = args.windowsInstallScope;
    if (args.windowsInstallDirSet)
        proj.packageConfig.windowsInstallDir = args.windowsInstallDir;
    if (args.windowsSignSet) {
        proj.packageConfig.windowsSign = args.windowsSign;
        proj.packageConfig.windowsSignSet = true;
    }
    if (args.windowsSignPfxSet)
        proj.packageConfig.windowsSignPfx = args.windowsSignPfx;
    if (args.windowsSignThumbprintSet)
        proj.packageConfig.windowsSignThumbprint = args.windowsSignThumbprint;
    if (args.windowsTimestampUrlSet)
        proj.packageConfig.windowsTimestampUrl = args.windowsTimestampUrl;
    if (args.windowsSigntoolPathSet)
        proj.packageConfig.windowsSigntoolPath = args.windowsSigntoolPath;
    if (args.windowsSignNoVerifySet)
        proj.packageConfig.windowsSignNoVerify = args.windowsSignNoVerify;
}

/// @brief Append one required payload path for package verification.
/// @details Joins @p payloadPrefix, @p targetDir, and @p leaf through the shared
///          packaging path sanitizer so verification expects the same normalized
///          layout that the builders emit.
/// @param paths Output list to which the normalized path is appended.
/// @param payloadPrefix Format-specific prefix inside the package payload.
/// @param targetDir Configured asset destination directory.
/// @param leaf Asset filename or directory-relative file path.
void appendRequiredPayloadPath(std::vector<std::string> &paths,
                               const std::string &payloadPrefix,
                               const std::string &targetDir,
                               const std::string &leaf) {
    const std::string underTarget =
        zanna::pkg::joinPackageRelativePath(targetDir, leaf, "asset path");
    paths.push_back(
        zanna::pkg::joinPackageRelativePath(payloadPrefix, underTarget, "asset payload path"));
}

/// @brief Return required payload paths for configured package assets.
/// @details Mirrors the package builders' asset layout: regular-file assets are
///          installed under their target directory using the source leaf name,
///          and directory assets install each regular file at its relative path
///          beneath the configured target directory. Directories themselves are
///          not required so empty-directory handling remains platform-specific.
/// @param proj Loaded project containing asset declarations and the source root.
/// @param payloadPrefix Format-specific prefix inside the package payload.
/// @return Normalized package paths that payload verification must find.
/// @throws std::runtime_error when an asset cannot be inspected or normalized.
std::vector<std::string> requiredAssetPayloadPaths(const ProjectConfig &proj,
                                                   const std::string &payloadPrefix) {
    std::vector<std::string> paths;
    for (const auto &asset : proj.packageConfig.assets) {
        const std::string sourceRel =
            zanna::pkg::sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string sourceLeaf = zanna::filesystem::genericPathToUtf8(
            zanna::filesystem::pathFromUtf8(sourceRel).filename());
        const std::string targetDir =
            zanna::pkg::sanitizePackageRelativePath(asset.targetPath, "asset target path");
        const fs::path srcPath = zanna::pkg::resolvePackageSourcePath(
            proj.rootDir, asset.sourcePath, "asset source path");

        std::error_code ec;
        if (fs::is_directory(srcPath, ec)) {
            if (ec)
                throw std::runtime_error("cannot inspect asset source path '" + asset.sourcePath +
                                         "': " + ec.message());
            /// @brief Append one safely resolved regular asset file to the required payload list.
            /// @param entry Directory entry with verified logical and physical paths.
            /// @throws std::runtime_error If the relative package path is invalid.
            zanna::pkg::safeDirectoryIterateResolved(
                srcPath, proj.rootDir, [&](const zanna::pkg::SafeDirectoryEntry &entry) {
                    if (!entry.regularFile)
                        return;
                    const std::string relPath = zanna::pkg::sanitizePackageRelativePath(
                        zanna::filesystem::genericPathToUtf8(
                            entry.logicalPath.lexically_relative(srcPath)),
                        "asset path");
                    appendRequiredPayloadPath(paths, payloadPrefix, targetDir, relPath);
                });
            continue;
        }
        if (ec)
            throw std::runtime_error("cannot inspect asset source path '" + asset.sourcePath +
                                     "': " + ec.message());

        ec.clear();
        if (fs::is_regular_file(srcPath, ec)) {
            if (ec)
                throw std::runtime_error("cannot inspect asset source path '" + asset.sourcePath +
                                         "': " + ec.message());
            appendRequiredPayloadPath(paths, payloadPrefix, targetDir, sourceLeaf);
            continue;
        }
        if (ec)
            throw std::runtime_error("cannot inspect asset source path '" + asset.sourcePath +
                                     "': " + ec.message());
        throw std::runtime_error("asset is not a regular file or directory: " + asset.sourcePath);
    }
    return paths;
}

} // namespace

/// @brief Build, inspect, sign, and verify an application package.
/// @details Loads the requested project, applies command-line overrides, obtains
///          a native executable, dispatches to the selected platform package
///          builder, and enforces format-specific payload verification.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and package subcommand.
/// @return Zero on success or help; nonzero for invalid input, build failures,
///         signing failures, or package-verification errors.
int cmdPackage(int argc, char **argv) {
    using namespace il::tools::common;
    namespace fs = std::filesystem;

    PackageArgs args;
    if (!parsePackageArgs(argc, argv, args))
        return 1;
    if (args.help)
        return 0;

    // Resolve project
    auto project = resolveProject(args.target);
    if (!project) {
        SourceManager sm;
        il::support::printDiag(project.error(), std::cerr, &sm);
        return 1;
    }

    ProjectConfig &proj = project.value();

    applyPackageCliOverrides(proj, args);

    // Check that package config is present
    if (!proj.packageConfig.hasPackageConfig()) {
        std::cerr << "warning: no package-* directives in zanna.project; "
                  << "using defaults\n";
    }

    // Determine display name and version
    std::string displayName =
        proj.packageConfig.displayName.empty() ? proj.name : proj.packageConfig.displayName;
    const std::string resolvedVersion = proj.version.empty() ? "0.0.0" : proj.version;

    // Determine architecture
    zanna::tools::TargetArch arch = zanna::tools::detectHostArch();
    std::string archStr;
    if (!args.archOverride.empty()) {
        arch = (args.archOverride == "arm64") ? zanna::tools::TargetArch::ARM64
                                              : zanna::tools::TargetArch::X64;
        archStr = args.archOverride;
    } else if (proj.packageConfig.targetArchitectures.size() == 1) {
        archStr = proj.packageConfig.targetArchitectures.front();
        arch =
            (archStr == "arm64") ? zanna::tools::TargetArch::ARM64 : zanna::tools::TargetArch::X64;
    } else {
        archStr = (arch == zanna::tools::TargetArch::ARM64) ? "arm64" : "x64";
    }
    if (!proj.packageConfig.targetArchitectures.empty()) {
        auto it = std::find(proj.packageConfig.targetArchitectures.begin(),
                            proj.packageConfig.targetArchitectures.end(),
                            archStr);
        if (it == proj.packageConfig.targetArchitectures.end()) {
            std::cerr << "error: selected package architecture '" << archStr
                      << "' is not listed by target-arch in zanna.project\n";
            return 1;
        }
    }

    if (args.outputPath.empty()) {
        args.outputPath =
            defaultPackageOutputPath(proj, resolvedVersion, args.platformTarget, archStr);
    }

    if (!validatePackageConfigForTarget(
            proj, args.platformTarget, archStr, std::cerr, !args.dryRun))
        return 1;

    // Dry-run mode: list what would be packaged, then exit
    if (args.dryRun) {
        if (args.jsonOutput) {
            std::cout << "{\n";
            std::cout << "  \"project\": \"" << jsonEscape(proj.name) << "\",\n";
            std::cout << "  \"displayName\": \"" << jsonEscape(displayName) << "\",\n";
            std::cout << "  \"version\": \"" << jsonEscape(resolvedVersion) << "\",\n";
            std::cout << "  \"target\": \"" << jsonEscape(platformName(args.platformTarget))
                      << "\",\n";
            std::cout << "  \"arch\": \"" << jsonEscape(archStr) << "\",\n";
            std::cout << "  \"output\": \"" << jsonEscape(args.outputPath) << "\",\n";
            std::cout << "  \"executable\": \""
                      << jsonEscape(args.executablePath.empty() ? proj.name : args.executablePath)
                      << "\",\n";
            std::cout << "  \"prebuiltExecutable\": "
                      << (args.executablePath.empty() ? "false" : "true") << ",\n";
            std::cout << "  \"icon\": \"" << jsonEscape(proj.packageConfig.iconPath) << "\",\n";
            std::cout << "  \"assets\": [";
            for (size_t i = 0; i < proj.packageConfig.assets.size(); ++i) {
                const auto &asset = proj.packageConfig.assets[i];
                if (i != 0)
                    std::cout << ", ";
                std::cout << "{\"source\":\"" << jsonEscape(asset.sourcePath) << "\",\"target\":\""
                          << jsonEscape(asset.targetPath) << "\"}";
            }
            std::cout << "],\n";
            std::cout << "  \"fileAssociations\": [";
            for (size_t i = 0; i < proj.packageConfig.fileAssociations.size(); ++i) {
                const auto &assoc = proj.packageConfig.fileAssociations[i];
                if (i != 0)
                    std::cout << ", ";
                std::cout << "{\"extension\":\"" << jsonEscape(assoc.extension)
                          << "\",\"description\":\"" << jsonEscape(assoc.description)
                          << "\",\"mimeType\":\"" << jsonEscape(assoc.mimeType)
                          << "\",\"windowsOpenArgs\":\"" << jsonEscape(assoc.openCommandArguments)
                          << "\"}";
            }
            std::cout << "],\n";
            writeJsonStringArray(std::cout, "debianDepends", proj.packageConfig.depends);
            std::cout << ",\n";
            writeJsonStringArray(std::cout, "rpmDepends", proj.packageConfig.rpmDepends);
            std::cout << ",\n";
            writeJsonStringArray(std::cout, "windowsDlls", proj.packageConfig.windowsDlls);
            std::cout << ",\n";
            std::cout << "  \"windowsInstallScope\": \""
                      << jsonEscape(proj.packageConfig.windowsInstallScope.empty()
                                        ? "user"
                                        : proj.packageConfig.windowsInstallScope)
                      << "\",\n";
            std::cout << "  \"windowsInstallDir\": \""
                      << jsonEscape(proj.packageConfig.windowsInstallDir) << "\",\n";
            std::cout << "  \"windowsPublisher\": \""
                      << jsonEscape(proj.packageConfig.windowsPublisher.empty()
                                        ? (proj.packageConfig.author.empty()
                                               ? displayName
                                               : proj.packageConfig.author)
                                        : proj.packageConfig.windowsPublisher)
                      << "\",\n";
            std::cout << "  \"macosBundleIdentifier\": \""
                      << jsonEscape(proj.packageConfig.identifier) << "\",\n";
            std::cout << "  \"linuxCategory\": \"" << jsonEscape(proj.packageConfig.category)
                      << "\",\n";
            std::cout << "  \"linuxAppStreamId\": \"" << jsonEscape(proj.packageConfig.appstreamId)
                      << "\"\n";
            std::cout << "}\n";
            return 0;
        }
        std::ostream &dryOut = std::cout;
        dryOut << "Dry run: " << displayName << " for " << platformName(args.platformTarget) << " ("
               << archStr << ")\n";
        dryOut << "  Output: " << args.outputPath << "\n";
        if (!args.executablePath.empty())
            dryOut << "  Executable: " << args.executablePath << " (prebuilt)\n";
        else
            dryOut << "  Executable: " << proj.name << " (build)\n";
        if (!proj.packageConfig.iconPath.empty()) {
            fs::path iconPath;
            dryOut << "  Icon: " << proj.packageConfig.iconPath;
            try {
                iconPath = zanna::pkg::resolvePackageSourcePath(
                    proj.rootDir, proj.packageConfig.iconPath, "package icon");
            } catch (const std::exception &ex) {
                dryOut << " [INVALID: " << ex.what() << "]";
            }
            std::error_code iconEc;
            if (!iconPath.empty() && !fs::exists(iconPath, iconEc))
                dryOut << " [NOT FOUND]";
            dryOut << "\n";
        }
        if (args.platformTarget == PackageTarget::MacOS) {
            const std::string signMode =
                zanna::pkg::resolveMacOSSignModeForHost(proj.packageConfig);
            dryOut << "  macOS signing: " << signMode << "\n";
            if (!proj.packageConfig.macosSignIdentity.empty())
                dryOut << "  macOS signing identity: " << proj.packageConfig.macosSignIdentity
                       << "\n";
            if (!proj.packageConfig.macosEntitlements.empty())
                dryOut << "  macOS entitlements: " << proj.packageConfig.macosEntitlements << "\n";
            if (proj.packageConfig.macosHardenedRuntime)
                dryOut << "  macOS hardened runtime: on\n";
            if (!proj.packageConfig.macosNotaryProfile.empty())
                dryOut << "  macOS notary profile: " << proj.packageConfig.macosNotaryProfile
                       << "\n";
            if (proj.packageConfig.macosStaple)
                dryOut << "  macOS staple: on\n";
        }
        if (args.platformTarget == PackageTarget::Windows) {
            const std::string scope = proj.packageConfig.windowsInstallScope.empty()
                                          ? "user"
                                          : proj.packageConfig.windowsInstallScope;
            dryOut << "  Windows install scope: " << scope << "\n";
            if (!proj.packageConfig.windowsInstallDir.empty())
                dryOut << "  Windows install directory: " << proj.packageConfig.windowsInstallDir
                       << "\n";
            if (windowsSigningRequested(proj.packageConfig)) {
                dryOut << "  Windows signing: on\n";
                if (!proj.packageConfig.windowsSignPfx.empty())
                    dryOut << "  Windows signing PFX: " << proj.packageConfig.windowsSignPfx
                           << "\n";
                if (!proj.packageConfig.windowsSignThumbprint.empty())
                    dryOut << "  Windows signing thumbprint: "
                           << proj.packageConfig.windowsSignThumbprint << "\n";
                if (!proj.packageConfig.windowsTimestampUrl.empty())
                    dryOut << "  Windows timestamp URL: " << proj.packageConfig.windowsTimestampUrl
                           << "\n";
            }
        }
        for (const auto &asset : proj.packageConfig.assets) {
            fs::path assetPath;
            dryOut << "  Asset: " << asset.sourcePath << " -> " << asset.targetPath;
            try {
                assetPath = zanna::pkg::resolvePackageSourcePath(
                    proj.rootDir, asset.sourcePath, "asset source path");
            } catch (const std::exception &ex) {
                dryOut << " [INVALID: " << ex.what() << "]";
            }
            std::error_code assetEc;
            if (!assetPath.empty() && !fs::exists(assetPath, assetEc))
                dryOut << " [NOT FOUND]";
            else if (!assetPath.empty() && fs::is_directory(assetPath, assetEc)) {
                size_t count = 0;
                try {
                    /// @brief Count one safely resolved regular asset file for dry-run output.
                    /// @param e Directory entry with verified logical and physical paths.
                    zanna::pkg::safeDirectoryIterateResolved(
                        assetPath, proj.rootDir, [&](const zanna::pkg::SafeDirectoryEntry &e) {
                            if (e.regularFile)
                                ++count;
                        });
                    dryOut << " (" << count << " files)";
                } catch (const std::exception &ex) {
                    dryOut << " [INVALID: " << ex.what() << "]";
                }
            }
            dryOut << "\n";
        }
        for (const auto &assoc : proj.packageConfig.fileAssociations) {
            dryOut << "  File assoc: " << assoc.extension << " (" << assoc.description << ")";
            if (!assoc.openCommandArguments.empty())
                dryOut << " [Windows open args: " << assoc.openCommandArguments << "]";
            dryOut << "\n";
        }
        if (!proj.packageConfig.category.empty())
            dryOut << "  Category: " << proj.packageConfig.category << "\n";
        if (!proj.packageConfig.depends.empty()) {
            dryOut << "  Depends:";
            for (const auto &d : proj.packageConfig.depends)
                dryOut << " " << d;
            dryOut << "\n";
        }
        return 0;
    }

    const PackageTarget hostPlatform = detectHostPlatform();
    if (args.executablePath.empty() && args.platformTarget != PackageTarget::Tarball &&
        args.platformTarget != hostPlatform) {
        std::cerr << "error: packaging for '" << platformName(args.platformTarget)
                  << "' from this host requires --executable because the built-in compile path "
                     "still targets the host platform\n";
        return 1;
    }
    if (args.executablePath.empty() && args.platformTarget == PackageTarget::Tarball &&
        arch != zanna::tools::detectHostArch()) {
        std::cerr << "error: tarball packaging for architecture '" << archStr
                  << "' requires --executable because the built-in compile path still targets "
                     "the host architecture\n";
        return 1;
    }

    std::string packageBinaryPath;
    bool cleanupPackagedBinary = false;

    if (!args.executablePath.empty()) {
        fs::path exePath = zanna::filesystem::pathFromUtf8(args.executablePath);
        if (!exePath.is_absolute()) {
            std::error_code cwdEc;
            auto cwd = fs::current_path(cwdEc);
            if (cwdEc) {
                std::cerr << "error: cannot resolve current directory: " << cwdEc.message() << "\n";
                return 1;
            }
            exePath = cwd / exePath;
        }
        std::error_code exeEc;
        const fs::path canonicalExe = fs::weakly_canonical(exePath, exeEc);
        packageBinaryPath =
            zanna::filesystem::pathToUtf8(exeEc ? exePath.lexically_normal() : canonicalExe);
        const fs::path packageBinaryNative = zanna::filesystem::pathFromUtf8(packageBinaryPath);
        exeEc.clear();
        if (!fs::exists(packageBinaryNative, exeEc)) {
            if (exeEc) {
                std::cerr << "error: cannot inspect prebuilt executable at " << packageBinaryPath
                          << ": " << exeEc.message() << "\n";
                return 1;
            }
            std::cerr << "error: prebuilt executable not found at " << packageBinaryPath << "\n";
            return 1;
        }
        exeEc.clear();
        if (!fs::is_regular_file(packageBinaryNative, exeEc)) {
            if (exeEc) {
                std::cerr << "error: cannot inspect prebuilt executable at " << packageBinaryPath
                          << ": " << exeEc.message() << "\n";
                return 1;
            }
            std::cerr << "error: prebuilt executable is not a regular file at " << packageBinaryPath
                      << "\n";
            return 1;
        }
        try {
            validateExecutableForPackageTarget(packageBinaryPath, args.platformTarget, archStr);
        } catch (const std::exception &ex) {
            std::cerr << "error: prebuilt executable is not valid for this package: " << ex.what()
                      << "\n";
            return 1;
        }
    } else {
        // Step 1: Compile to native binary (using build pipeline)
        std::cerr << "Compiling " << proj.name << "...\n";

        // Build through the same in-process path as `zanna build` so packaging
        // observes project manifests, frontend options, assets, and native
        // backend flags consistently.
        std::string tempBinaryExt;
#if ZANNA_HOST_WINDOWS
        tempBinaryExt = ".exe";
#endif
        std::string tempBinaryPath =
            zanna::tools::generateTempFilePath("zanna_package", tempBinaryExt.c_str());
        packageBinaryPath = tempBinaryPath;
        cleanupPackagedBinary = true;

        const int buildRc = buildProjectToNativeForPackage(
            args.target, tempBinaryPath, archStr, args.platformTarget == PackageTarget::Windows);
        if (buildRc != 0) {
            std::cerr << "error: compilation failed\n";
            std::error_code ec;
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
            return 1;
        }

        std::error_code compiledEc;
        if (!fs::exists(zanna::filesystem::pathFromUtf8(packageBinaryPath), compiledEc)) {
            if (compiledEc) {
                std::cerr << "error: cannot inspect compiled binary at " << packageBinaryPath
                          << ": " << compiledEc.message() << "\n";
                std::error_code cleanupEc;
                removeFailedArtifactUnlessKept(
                    packageBinaryPath, args.keepFailedArtifact, cleanupEc);
                return 1;
            }
            std::cerr << "error: compiled binary not found at " << packageBinaryPath << "\n";
            std::error_code ec;
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
            return 1;
        }
        try {
            validateExecutableForPackageTarget(packageBinaryPath, args.platformTarget, archStr);
        } catch (const std::exception &ex) {
            std::cerr << "error: compiled executable is not valid for this package: " << ex.what()
                      << "\n";
            std::error_code ec;
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
            return 1;
        }
    }

    // Step 3: Package
    std::cerr << "Packaging " << displayName << " for " << platformName(args.platformTarget) << " ("
              << archStr << ")...\n";

    if (args.verbose) {
        std::error_code sizeEc;
        auto binSize = fs::file_size(zanna::filesystem::pathFromUtf8(packageBinaryPath), sizeEc);
        std::cerr << "  Binary: " << packageBinaryPath;
        if (!sizeEc)
            std::cerr << " (" << binSize << " bytes)";
        else
            std::cerr << " (size unavailable: " << sizeEc.message() << ")";
        std::cerr << "\n";
        std::cerr << "  Output: " << args.outputPath << "\n";
        if (!proj.packageConfig.iconPath.empty())
            std::cerr << "  Icon: " << proj.packageConfig.iconPath << "\n";
        for (const auto &asset : proj.packageConfig.assets)
            std::cerr << "  Asset: " << asset.sourcePath << " -> " << asset.targetPath << "\n";
    }

    try {
        if (!args.linuxSignKey.empty() && args.platformTarget != PackageTarget::Linux &&
            args.platformTarget != PackageTarget::Rpm) {
            throw std::runtime_error(
                "--linux-sign-key applies only to --target linux or --target rpm");
        }
        const fs::path outputParent =
            zanna::filesystem::pathFromUtf8(args.outputPath).parent_path();
        if (!outputParent.empty()) {
            std::error_code mkdirEc;
            fs::create_directories(outputParent, mkdirEc);
            if (mkdirEc) {
                throw std::runtime_error("cannot create output directory '" +
                                         zanna::filesystem::pathToUtf8(outputParent) +
                                         "': " + mkdirEc.message());
            }
        }
        switch (args.platformTarget) {
            case PackageTarget::MacOS: {
                zanna::pkg::MacOSBuildParams params;
                params.projectName = proj.name;
                params.version = resolvedVersion;
                params.executablePath = packageBinaryPath;
                params.projectRoot = proj.rootDir;
                params.pkgConfig = proj.packageConfig;
                params.outputPath = args.outputPath;
                zanna::pkg::buildMacOSPackage(params);
                break;
            }
            case PackageTarget::Linux: {
                zanna::pkg::LinuxBuildParams lparams;
                lparams.projectName = proj.name;
                lparams.version = resolvedVersion;
                lparams.executablePath = packageBinaryPath;
                lparams.projectRoot = proj.rootDir;
                lparams.pkgConfig = proj.packageConfig;
                lparams.outputPath = args.outputPath;
                // Map architecture: Debian uses "amd64" not "x64"
                lparams.archStr = (archStr == "x64") ? "amd64" : archStr;
                zanna::pkg::buildDebPackage(lparams);
                if (!args.linuxSignKey.empty())
                    zanna::pkg::signLinuxPackage(args.outputPath,
                                                 args.linuxSignKey,
                                                 /*isRpm=*/false);
                break;
            }
            case PackageTarget::Windows: {
                zanna::pkg::WindowsBuildParams wparams;
                wparams.projectName = proj.name;
                wparams.version = resolvedVersion;
                wparams.executablePath = packageBinaryPath;
                wparams.projectRoot = proj.rootDir;
                wparams.pkgConfig = proj.packageConfig;
                wparams.outputPath = args.outputPath;
                wparams.archStr = archStr;
                wparams.installerHostPath =
                    zanna::filesystem::pathToUtf8(findWindowsInstallerSupportExecutable(
                        proj, "ZANNA_WINDOWS_INSTALLER_HOST", "zanna-installer-host.exe"));
                if (wparams.installerHostPath.empty()) {
                    throw std::runtime_error(
                        "native Windows installer host not found; install/build "
                        "zanna-installer-host or set ZANNA_WINDOWS_INSTALLER_HOST");
                }
                wparams.installerCleanupPath =
                    zanna::filesystem::pathToUtf8(findWindowsInstallerSupportExecutable(
                        proj, "ZANNA_WINDOWS_INSTALLER_CLEANUP", "zanna-installer-cleanup.exe"));
                if (wparams.installerCleanupPath.empty()) {
                    throw std::runtime_error(
                                         "native Windows installer cleanup helper not found; install/build "
                                         "zanna-installer-cleanup or set ZANNA_WINDOWS_INSTALLER_CLEANUP");
                }
                if (windowsSigningRequested(proj.packageConfig)) {
                    /// @brief Sign one generated Windows PE payload.
                    /// @param logicalName Logical payload name used in signing diagnostics.
                    /// @param unsignedPe Unsigned PE bytes.
                    /// @return Signed PE bytes.
                    /// @throws std::runtime_error If the configured signing command fails.
                    wparams.peSigner = [&](std::string_view logicalName,
                                           const std::vector<uint8_t> &unsignedPe) {
                        return signWindowsPeBytes(proj, logicalName, unsignedPe, args.verbose);
                    };
                }
                zanna::pkg::buildWindowsPackage(wparams);
                break;
            }
            case PackageTarget::Tarball: {
                zanna::pkg::LinuxBuildParams tparams;
                tparams.projectName = proj.name;
                tparams.version = resolvedVersion;
                tparams.executablePath = packageBinaryPath;
                tparams.projectRoot = proj.rootDir;
                tparams.pkgConfig = proj.packageConfig;
                tparams.outputPath = args.outputPath;
                tparams.archStr = archStr;
                zanna::pkg::buildTarball(tparams);
                break;
            }
            case PackageTarget::AppImage: {
                zanna::pkg::LinuxBuildParams aparams;
                aparams.projectName = proj.name;
                aparams.version = resolvedVersion;
                aparams.executablePath = packageBinaryPath;
                aparams.projectRoot = proj.rootDir;
                aparams.pkgConfig = proj.packageConfig;
                aparams.outputPath = args.outputPath;
                aparams.archStr = archStr; // Linux bundles use the portable x64/arm64 form.
                zanna::pkg::buildAppImage(aparams);
                break;
            }
            case PackageTarget::Rpm: {
                zanna::pkg::LinuxBuildParams rparams;
                rparams.projectName = proj.name;
                rparams.version = resolvedVersion;
                rparams.executablePath = packageBinaryPath;
                rparams.projectRoot = proj.rootDir;
                rparams.pkgConfig = proj.packageConfig;
                rparams.outputPath = args.outputPath;
                rparams.archStr = archStr; // RPM uses the portable x64/arm64 form
                zanna::pkg::buildRpmPackage(rparams);
                if (!args.linuxSignKey.empty())
                    zanna::pkg::signLinuxPackage(args.outputPath,
                                                 args.linuxSignKey,
                                                 /*isRpm=*/true);
                break;
            }
            case PackageTarget::Dmg: {
                zanna::pkg::MacOSBuildParams params;
                params.projectName = proj.name;
                params.version = resolvedVersion;
                params.executablePath = packageBinaryPath;
                params.projectRoot = proj.rootDir;
                params.pkgConfig = proj.packageConfig;
                params.outputPath = args.outputPath;
                zanna::pkg::buildMacOSAppDmg(params);
                break;
            }
            default:
                break;
        }
    } catch (const std::exception &e) {
        std::cerr << "error: packaging failed: " << e.what() << "\n";
        std::error_code ec;
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        removeFailedArtifactUnlessKept(args.outputPath, args.keepFailedArtifact, ec);
        return 1;
    }

    // Step 4: Verify the generated package
    std::error_code ec;
    const fs::path outputNative = zanna::filesystem::pathFromUtf8(args.outputPath);
    if (!fs::exists(outputNative, ec)) {
        if (ec)
            std::cerr << "error: cannot inspect generated package: " << ec.message() << "\n";
        else
            std::cerr << "error: package builder did not create output file: " << args.outputPath
                      << "\n";
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }

    if (args.platformTarget == PackageTarget::Windows &&
        !signWindowsInstallerArtifact(proj, outputNative, args.verbose, std::cerr)) {
        removeFailedArtifactUnlessKept(args.outputPath, args.keepFailedArtifact, ec);
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }

    std::vector<uint8_t> pkgData;
    try {
        pkgData = zanna::pkg::readFile(args.outputPath);
    } catch (const std::exception &ex) {
        std::cerr << "error: cannot read generated package for verification: " << ex.what() << "\n";
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }

    std::ostringstream verifyErr;
    bool valid = true;
    const std::string execName = zanna::pkg::normalizeExecName(proj.name);
    try {
        switch (args.platformTarget) {
            case PackageTarget::MacOS: {
                const auto requiredResources = requiredAssetPayloadPaths(proj, "");
                valid = zanna::pkg::verifyMacOSAppZipPayload(
                    pkgData, displayName + ".app", execName, requiredResources, verifyErr);
                break;
            }
            case PackageTarget::Linux: {
                std::vector<std::string> required = {"usr/bin/" + execName};
                auto assetPaths = requiredAssetPayloadPaths(
                    proj, "usr/share/" + zanna::pkg::normalizeDebName(proj.name));
                required.insert(required.end(), assetPaths.begin(), assetPaths.end());
                valid = zanna::pkg::verifyDebPayload(pkgData, required, verifyErr);
                break;
            }
            case PackageTarget::Windows: {
                std::vector<std::string> requiredInner = {execName + ".exe"};
                auto assetPaths = requiredAssetPayloadPaths(proj, "");
                requiredInner.insert(requiredInner.end(), assetPaths.begin(), assetPaths.end());
                valid = zanna::pkg::verifyWindowsNativeInstaller(pkgData, verifyErr) &&
                        zanna::pkg::verifyPEZipOverlayNestedPayload(pkgData,
                                                                    {"meta/payload.zip",
                                                                     "meta/installer-v2.txt",
                                                                     "meta/cleanup.exe",
                                                                     "meta/uninstall.exe"},
                                                                    "meta/payload.zip",
                                                                    requiredInner,
                                                                    verifyErr);
                break;
            }
            case PackageTarget::Tarball: {
                const std::string topDir = zanna::pkg::normalizeDebName(proj.name) + "-" +
                                           portableArchiveVersionComponent(resolvedVersion);
                std::vector<std::string> required = {
                    topDir + "/" + execName, topDir + "/README.install", topDir + "/LICENSE"};
                auto assetPaths = requiredAssetPayloadPaths(proj, topDir);
                required.insert(required.end(), assetPaths.begin(), assetPaths.end());
                valid = zanna::pkg::verifyTarGzPayload(pkgData, required, verifyErr);
                break;
            }
            case PackageTarget::AppImage: {
                std::string appImageErr;
                valid = zanna::pkg::verifyLinuxAppImage(pkgData, &appImageErr);
                if (!valid)
                    verifyErr << appImageErr;
                break;
            }
            case PackageTarget::Rpm: {
                valid = zanna::pkg::verifyRpm(pkgData, verifyErr);
                break;
            }
            case PackageTarget::Dmg: {
                valid = zanna::pkg::verifyMacOSDmg(pkgData, verifyErr);
#if ZANNA_HOST_MACOS
                if (valid) {
                    const RunResult nativeResult =
                        run_process({"hdiutil", "verify", args.outputPath});
                    if (nativeResult.exit_code != 0) {
                        valid = false;
                        verifyErr << "dmg: hdiutil verification failed\n"
                                  << nativeResult.out << nativeResult.err;
                    }
                }
#endif
                break;
            }
            default:
                break;
        }
    } catch (const std::exception &ex) {
        std::cerr << "error: cannot prepare package verification: " << ex.what() << "\n";
        removeFailedArtifactUnlessKept(args.outputPath, args.keepFailedArtifact, ec);
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }
    if (!valid) {
        std::cerr << "error: package verification failed:\n" << verifyErr.str();
        removeFailedArtifactUnlessKept(args.outputPath, args.keepFailedArtifact, ec);
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }
    if (args.verbose)
        std::cerr << "  Verification: passed\n";

    try {
        const std::string packageSha256 = zanna::pkg::sha256Hex(pkgData.data(), pkgData.size());
        const fs::path packagePath = zanna::filesystem::pathFromUtf8(args.outputPath);
        const std::string packageFilename = zanna::filesystem::pathToUtf8(packagePath.filename());
        zanna::pkg::writeTextFileAtomic(args.outputPath + ".sha256",
                                        packageSha256 + "  " + packageFilename + "\n");
        std::string trust = "checksum-only";
        if (args.platformTarget == PackageTarget::Windows)
            trust = windowsSigningRequested(proj.packageConfig) ? "authenticode" : "unsigned";
        else if (args.platformTarget == PackageTarget::Linux ||
                 args.platformTarget == PackageTarget::Rpm)
            trust = args.linuxSignKey.empty() ? "unsigned" : "openpgp";
        else if (args.platformTarget == PackageTarget::MacOS ||
                 args.platformTarget == PackageTarget::Dmg) {
            const std::string signMode =
                zanna::pkg::resolveMacOSSignModeForHost(proj.packageConfig);
            trust = signMode == "developer-id"
                        ? (proj.packageConfig.macosNotaryProfile.empty() ? "developer-id"
                                                                         : "developer-id+notarized")
                        : signMode;
        }
        std::ostringstream artifactManifest;
        artifactManifest << "{\n"
                         << "  \"schema_version\": 1,\n"
                         << "  \"artifact\": {\"file\": \"" << jsonEscape(packageFilename)
                         << "\", \"format\": \""
                         << jsonEscape(platformExtension(args.platformTarget))
                         << "\", \"platform\": \"" << jsonEscape(platformName(args.platformTarget))
                         << "\", \"arch\": \"" << jsonEscape(archStr) << "\", \"version\": \""
                         << jsonEscape(resolvedVersion) << "\", \"size\": " << pkgData.size()
                         << ", \"sha256\": \"" << packageSha256
                         << "\", \"verified\": true, \"trust\": \"" << jsonEscape(trust) << "\"}\n"
                         << "}\n";
        zanna::pkg::writeTextFileAtomic(args.outputPath + ".manifest.json", artifactManifest.str());
    } catch (const std::exception &ex) {
        std::cerr << "error: cannot write package checksum/manifest: " << ex.what() << "\n";
        fs::remove(zanna::filesystem::pathFromUtf8(args.outputPath + ".sha256"), ec);
        ec.clear();
        fs::remove(zanna::filesystem::pathFromUtf8(args.outputPath + ".manifest.json"), ec);
        removeFailedArtifactUnlessKept(args.outputPath, args.keepFailedArtifact, ec);
        if (cleanupPackagedBinary)
            removeFailedArtifactUnlessKept(packageBinaryPath, args.keepFailedArtifact, ec);
        return 1;
    }

    // Cleanup temp binary
    if (cleanupPackagedBinary) {
        ec.clear();
        fs::remove(zanna::filesystem::pathFromUtf8(packageBinaryPath), ec);
        if (ec && args.verbose) {
            std::cerr << "warning: failed to remove temporary packaged binary '"
                      << packageBinaryPath << "': " << ec.message() << "\n";
        }
    }

    std::cerr << "Package created: " << args.outputPath;
    ec.clear();
    if (args.verbose && fs::exists(outputNative, ec)) {
        ec.clear();
        auto finalPackageSize = fs::file_size(outputNative, ec);
        if (!ec)
            std::cerr << " (" << finalPackageSize << " bytes)";
    }
    std::cerr << "\n";
    return 0;
}
