//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/LinuxPackageBuilder.cpp
// Purpose: Assemble Linux .deb packages and .tar.gz archives from scratch.
//
// Key invariants:
//   - .deb member ordering: debian-binary, control.tar.gz, data.tar.gz.
//   - control file fields follow Debian Policy Manual format.
//   - md5sums: one line per data file, hex-digest + two-space + path.
//   - Architecture mapping: "x64" -> "amd64", "arm64" -> "arm64".
//   - FHS paths: /usr/bin/<exec>, /usr/share/<name>/<assets>,
//     /usr/share/applications/<name>.desktop,
//     /usr/share/icons/hicolor/<NxN>/apps/<name>.png.
//
// Ownership/Lifetime:
//   - Single-use builder.
//
// Links: LinuxPackageBuilder.hpp, ArWriter.hpp, TarWriter.hpp,
//        PkgGzip.hpp, PkgMD5.hpp, PkgPNG.hpp, DesktopEntryGenerator.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dependency-free Linux package/archive assembly and external RPM signing.
/// @details Provides shared FHS payload collection, metadata generation, archive
///          construction, toolchain packaging, and self-extracting bundle support.

#include "LinuxPackageBuilder.hpp"
#include "ArWriter.hpp"
#include "DesktopEntryGenerator.hpp"
#include "IconGenerator.hpp"
#include "LinuxRuntimeStubGen.hpp"
#include "PkgGzip.hpp"
#include "PkgMD5.hpp"
#include "PkgPNG.hpp"
#include "PkgUtils.hpp"
#include "TarWriter.hpp"
#include "common/RunProcess.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace zanna::pkg {
namespace {

/// @brief Track a data file for md5sums generation.
struct DataFile {
    std::string installPath; ///< Sanitized package-relative destination, such as `usr/bin/hello`.
    std::vector<uint8_t> data; ///< Regular-file payload bytes; empty for links/directories.
    uint32_t mode{0644};       ///< Unix permission bits stored in the archive.
    bool symlink{false};       ///< Whether this entry represents a symbolic link.
    bool directory{false};     ///< Whether this entry represents a directory.
    std::string symlinkTarget; ///< Link target when `symlink` is true.

    /// @brief Construct a regular data-file entry with mode 0644.
    /// @param path Package-relative installation path.
    /// @param bytes File payload bytes.
    DataFile(std::string path, std::vector<uint8_t> bytes)
        : installPath(std::move(path)), data(std::move(bytes)) {}

    /// @brief Construct a regular data-file entry with explicit Unix permissions.
    /// @param path Package-relative installation path.
    /// @param bytes File payload bytes.
    /// @param modeBits Unix permission bits stored in the archive.
    DataFile(std::string path, std::vector<uint8_t> bytes, uint32_t modeBits)
        : installPath(std::move(path)), data(std::move(bytes)), mode(modeBits) {}

    /// @brief Create a symbolic link entry pointing to `target`.
    /// @param path Package-relative link path.
    /// @param target Link target recorded in the archive.
    /// @return Link entry with mode 0777 and no payload bytes.
    static DataFile link(std::string path, std::string target) {
        DataFile file(std::move(path), {});
        file.symlink = true;
        file.symlinkTarget = std::move(target);
        file.mode = 0777;
        return file;
    }

    /// @brief Create a directory entry (no data; mode 0755).
    /// @param path Package-relative directory path.
    /// @return Directory entry with mode 0755.
    static DataFile dir(std::string path) {
        DataFile file(std::move(path), {});
        file.directory = true;
        file.mode = 0755;
        return file;
    }
};

/// @brief Map a Zanna arch string ("x64", "arm64") to the Debian architecture field value.
/// @param arch Portable architecture name.
/// @return `amd64` for `x64`, otherwise validated `arm64`.
/// @throws std::runtime_error If the architecture is unsupported.
std::string debArchFor(const std::string &arch) {
    validateToolchainArchitecture(arch);
    return arch == "arm64" ? "arm64" : "amd64";
}

/// @brief Return `text` with all ASCII letters converted to lowercase.
/// @param text Text to normalize in place.
/// @return Lowercase copy.
std::string lowerAscii(std::string text) {
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// @brief Read a little-endian 16-bit integer from an ELF byte buffer.
/// @param data Complete input byte buffer.
/// @param off Starting byte offset.
/// @return Decoded value, or zero when two bytes are unavailable.
uint16_t readLe16(const std::vector<uint8_t> &data, size_t off) {
    if (off + 2u > data.size())
        return 0;
    return (uint16_t)data[off] | ((uint16_t)data[off + 1u] << 8);
}

/// @brief Read a little-endian 32-bit integer from an ELF byte buffer.
/// @param data Complete input byte buffer.
/// @param off Starting byte offset.
/// @return Decoded value, or zero when four bytes are unavailable.
uint32_t readLe32(const std::vector<uint8_t> &data, size_t off) {
    if (off + 4u > data.size())
        return 0;
    return (uint32_t)data[off] | ((uint32_t)data[off + 1u] << 8) |
           ((uint32_t)data[off + 2u] << 16) | ((uint32_t)data[off + 3u] << 24);
}

/// @brief Read a little-endian 64-bit integer from an ELF byte buffer.
/// @param data Complete input byte buffer.
/// @param off Starting byte offset.
/// @return Decoded value assembled from two bounded 32-bit reads.
uint64_t readLe64(const std::vector<uint8_t> &data, size_t off) {
    uint64_t lo = readLe32(data, off);
    uint64_t hi = readLe32(data, off + 4u);
    return lo | (hi << 32);
}

/// @brief Return a file slice corresponding to an ELF virtual address range.
/// @details Maps @p vaddr through PT_LOAD program headers. This is enough for
///          the package dependency scanner to resolve DT_STRTAB into the
///          dynamic string table without invoking host tools.
/// @param data Full ELF file bytes.
/// @param vaddr Virtual address to map.
/// @param size Number of bytes needed.
/// @param phoff ELF program-header table offset.
/// @param phentsize Program-header entry size.
/// @param phnum Program-header count.
/// @return File offset on success, std::nullopt when no load segment covers it.
std::optional<size_t> elfFileOffsetForVaddr(const std::vector<uint8_t> &data,
                                            uint64_t vaddr,
                                            uint64_t size,
                                            uint64_t phoff,
                                            uint16_t phentsize,
                                            uint16_t phnum) {
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t off = (size_t)phoff + (size_t)i * phentsize;
        if (off + 56u > data.size())
            break;
        const uint32_t type = readLe32(data, off);
        if (type != 1u)
            continue;
        const uint64_t p_offset = readLe64(data, off + 8u);
        const uint64_t p_vaddr = readLe64(data, off + 16u);
        const uint64_t p_filesz = readLe64(data, off + 32u);
        if (vaddr < p_vaddr || vaddr - p_vaddr > p_filesz)
            continue;
        const uint64_t delta = vaddr - p_vaddr;
        if (size > p_filesz - delta)
            continue;
        if (p_offset > data.size() || delta > data.size() - (size_t)p_offset)
            continue;
        const size_t fileOff = (size_t)(p_offset + delta);
        if (fileOff <= data.size() && size <= data.size() - fileOff)
            return fileOff;
    }
    return std::nullopt;
}

/// @brief Read DT_NEEDED library names from a little-endian ELF64 binary.
/// @details The parser intentionally supports the ELF64 shape produced by
///          Zanna's Linux x64/arm64 toolchains. Unsupported or malformed inputs
///          simply return an empty list so callers can retain conservative
///          fallback dependencies.
/// @param data Full ELF file bytes.
/// @return SONAMEs referenced by DT_NEEDED entries.
std::vector<std::string> elfNeededLibraries(const std::vector<uint8_t> &data) {
    if (data.size() < 64u || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' ||
        data[3] != 'F' || data[4] != 2u || data[5] != 1u)
        return {};
    const uint64_t phoff = readLe64(data, 32u);
    const uint16_t phentsize = readLe16(data, 54u);
    const uint16_t phnum = readLe16(data, 56u);
    if (phoff == 0 || phentsize < 56u || phnum == 0)
        return {};

    uint64_t dynamicOff = 0;
    uint64_t dynamicSize = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t off = (size_t)phoff + (size_t)i * phentsize;
        if (off + 56u > data.size())
            return {};
        if (readLe32(data, off) == 2u) {
            dynamicOff = readLe64(data, off + 8u);
            dynamicSize = readLe64(data, off + 32u);
            break;
        }
    }
    if (dynamicOff == 0 || dynamicOff > data.size() ||
        dynamicSize > data.size() - (size_t)dynamicOff)
        return {};

    uint64_t strtabVaddr = 0;
    uint64_t strtabSize = 0;
    std::vector<uint64_t> neededOffsets;
    for (size_t off = (size_t)dynamicOff; off + 16u <= (size_t)(dynamicOff + dynamicSize);
         off += 16u) {
        const uint64_t tag = readLe64(data, off);
        const uint64_t value = readLe64(data, off + 8u);
        if (tag == 0)
            break;
        if (tag == 1)
            neededOffsets.push_back(value);
        else if (tag == 5)
            strtabVaddr = value;
        else if (tag == 10)
            strtabSize = value;
    }
    if (strtabVaddr == 0 || strtabSize == 0 || neededOffsets.empty())
        return {};
    const auto strtabOff =
        elfFileOffsetForVaddr(data, strtabVaddr, strtabSize, phoff, phentsize, phnum);
    if (!strtabOff)
        return {};

    std::vector<std::string> names;
    for (uint64_t nameOff : neededOffsets) {
        if (nameOff >= strtabSize)
            continue;
        const size_t start = *strtabOff + (size_t)nameOff;
        size_t end = start;
        while (end < data.size() && end < *strtabOff + (size_t)strtabSize && data[end] != 0)
            ++end;
        if (end > start)
            names.emplace_back(reinterpret_cast<const char *>(data.data() + start), end - start);
    }
    return names;
}

/// @brief Return Unix permission bits for a toolchain file.
/// Uses the stored `unixMode` if non-zero; otherwise defaults to 0755 for executables and 0644 for
/// data files.
/// @param file Manifest entry whose stored or inferred mode is required.
/// @return Stored permission bits, or an executable/data default when absent.
uint32_t permissionBitsFor(const ToolchainFileEntry &file) {
    const uint32_t bits = file.unixMode & 07777u;
    if (bits != 0)
        return bits;
    return file.executable ? 0755u : 0644u;
}

/// @brief Choose archive permission bits for a staged file from its on-disk mode.
/// @return 0755 when any execute bit is set on the source file, else 0644 (also
///         the fallback when the file cannot be stat'd).
/// @param path Filesystem path to inspect.
uint32_t permissionBitsForFilesystemPath(const fs::path &path) {
    std::error_code ec;
    const fs::perms perms = fs::status(path, ec).permissions();
    if (ec)
        return 0644u;
    const bool executable =
        (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
        fs::perms::none;
    return executable ? 0755u : 0644u;
}

/// @brief Validate a version string that will be normalized into a portable archive name.
/// @details Portable tarballs preserve existing support for Debian epoch versions
///          such as `2:1.0` by accepting `:` here, then mapping it to `_` in the
///          filename component. Path separators and special path components remain rejected.
/// @param version Version text from project metadata or a toolchain manifest.
/// @throws std::runtime_error If the value is empty, multiline, special, or contains a separator.
void validatePortableTarballVersion(const std::string &version) {
    if (version.empty())
        throw std::runtime_error("package version must not be empty");
    validateSingleLineField(version, "package version");
    if (version == "." || version == "..")
        throw std::runtime_error("package version must not be a special path segment");
    if (version.find('/') != std::string::npos || version.find('\\') != std::string::npos)
        throw std::runtime_error("package version must not contain path separators: " + version);
}

/// @brief Sanitize a package version into a portable filename component.
/// @details Keeps alphanumerics and `._+~-`, and replaces other safe metadata
///          separators such as Debian epoch `:` with `_` so the version can
///          appear safely in a tarball filename.
/// @param version Validated package version text.
/// @return Portable filename component derived from `version`.
/// @throws std::runtime_error If validation fails or no safe component results.
std::string portableArchiveVersionComponent(const std::string &version) {
    validatePortableTarballVersion(version);
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

/// @brief Append a hidden terminal-type .desktop file to `dataFiles` for the given file
/// associations. Writes a `noDisplay=true` desktop entry under
/// `usr/share/applications/<desktopName>`. No-op when `associations` is empty.
/// @param dataFiles Payload list to extend.
/// @param desktopName Filename for the generated desktop entry.
/// @param execPath Absolute installed command path.
/// @param execArguments Arguments placed after the executable.
/// @param associations File associations handled by the entry.
void addToolchainDesktopMetadata(std::vector<DataFile> &dataFiles,
                                 const std::string &desktopName,
                                 const std::string &execPath,
                                 const std::string &execArguments,
                                 const std::vector<FileAssoc> &associations) {
    if (associations.empty())
        return;

    DesktopEntryParams desktop;
    desktop.name = "Zanna Toolchain";
    desktop.comment = "Zanna source and IL tools";
    desktop.execPath = execPath;
    desktop.execArguments = execArguments;
    desktop.iconName = "zanna";
    desktop.categories = "Development;";
    desktop.terminal = true;
    desktop.fileAssociations = associations;
    desktop.acceptsFileArgument = true;
    desktop.noDisplay = true;

    const std::string desktopText = generateDesktopEntry(desktop);
    dataFiles.emplace_back("usr/share/applications/" + desktopName,
                           std::vector<uint8_t>(desktopText.begin(), desktopText.end()),
                           0644);
}

/// @brief Append MIME XML and separate .desktop files (one for .il, one for source types) to
/// `dataFiles`. Splits `manifest.fileAssociations` into IL vs. source groups so each gets its own
/// desktop handler. No-op when `manifest.fileAssociations` is empty.
/// @param dataFiles Payload list to extend.
/// @param manifest Manifest supplying file associations.
/// @param packageName Normalized package name used for generated filenames and MIME types.
/// @param zannaExecPath Installed path to the Zanna command.
void addToolchainFileAssociationMetadata(std::vector<DataFile> &dataFiles,
                                         const ToolchainInstallManifest &manifest,
                                         const std::string &packageName,
                                         const std::string &zannaExecPath) {
    if (manifest.fileAssociations.empty())
        return;

    std::vector<FileAssoc> sourceAssociations;
    std::vector<FileAssoc> ilAssociations;
    for (const auto &assoc : manifest.fileAssociations) {
        if (lowerAscii(assoc.extension) == ".il")
            ilAssociations.push_back(assoc);
        else
            sourceAssociations.push_back(assoc);
    }

    addToolchainDesktopMetadata(
        dataFiles, packageName + "-source.desktop", zannaExecPath, "run", sourceAssociations);
    addToolchainDesktopMetadata(
        dataFiles, packageName + "-il.desktop", zannaExecPath, "-run", ilAssociations);

    const std::string mimeXml = generateMimeTypeXml(packageName, manifest.fileAssociations);
    dataFiles.emplace_back("usr/share/mime/packages/" + packageName + ".xml",
                           std::vector<uint8_t>(mimeXml.begin(), mimeXml.end()),
                           0644);
}

/// @brief Append the visible Zanna Studio desktop launcher for Linux package menus.
/// @param dataFiles Payload list to extend.
/// @param execPath Installed path to the Zanna Studio executable.
void addZannaStudioDesktopMetadata(std::vector<DataFile> &dataFiles, const std::string &execPath) {
    DesktopEntryParams desktop;
    desktop.name = "Zanna Studio";
    desktop.comment = "Edit, build, and debug Zanna projects";
    desktop.execPath = execPath;
    desktop.iconName = "zanna";
    desktop.categories = "Development;IDE;TextEditor;";
    desktop.terminal = false;

    const std::string desktopText = generateDesktopEntry(desktop);
    dataFiles.emplace_back("usr/share/applications/zannastudio.desktop",
                           std::vector<uint8_t>(desktopText.begin(), desktopText.end()),
                           0644);
}

/// @brief Append hicolor theme icons for the built-in Zanna toolchain launcher.
/// @details Uses the dependency-free fallback image from IconGenerator and writes
///          standard hicolor PNG sizes so desktop menus and MIME handlers resolve
///          Icon=zanna in Debian/RPM installs and portable Linux tarballs.
/// @param dataFiles Package payload list to append to.
/// @param iconName Theme icon name, without an extension.
void addZannaToolchainHicolorIcons(std::vector<DataFile> &dataFiles, const std::string &iconName) {
    const auto icons = generateMultiSizePngs(defaultZannaToolchainIconImage());
    for (const auto &[size, png] : icons) {
        dataFiles.emplace_back("usr/share/icons/hicolor/" + std::to_string(size) + "x" +
                                   std::to_string(size) + "/apps/" + iconName + ".png",
                               png,
                               0644);
    }
}

/// @brief Collect all Linux install files from the manifest, mapping each to its FHS path under
/// /usr via `LinuxUsrRoot` policy, then appending generated file-association metadata entries.
/// @param manifest Validated staged toolchain manifest.
/// @param packageName Normalized package name used by generated metadata.
/// @return Complete regular-file/link payload list for Linux package formats.
std::vector<DataFile> collectToolchainLinuxFiles(const ToolchainInstallManifest &manifest,
                                                 const std::string &packageName) {
    std::vector<DataFile> dataFiles;
    dataFiles.reserve(manifest.files.size() + 2);
    for (const auto &file : manifest.files) {
        const std::string installPath = mapInstallPath(file, InstallPathPolicy::LinuxUsrRoot);
        const std::string relInstall = sanitizePackageRelativePath(
            installPath.size() > 1 ? installPath.substr(1) : installPath, "linux install path");
        if (file.symlink)
            dataFiles.push_back(DataFile::link(relInstall, file.symlinkTarget));
        else
            dataFiles.emplace_back(
                relInstall, readFile(file.stagedAbsolutePath.string()), permissionBitsFor(file));
    }
    addToolchainFileAssociationMetadata(dataFiles, manifest, packageName, "/usr/bin/zanna");
    addZannaStudioDesktopMetadata(dataFiles, "/usr/bin/zannastudio");
    addZannaToolchainHicolorIcons(dataFiles, "zanna");
    return dataFiles;
}

/// @brief Add all parent directory entries to `tar` for every path in `dataFiles`.
/// Deduplicates entries and sorts them so parent directories always precede their children.
/// @param tar Archive writer to populate.
/// @param dataFiles Payload paths whose parent directories are required.
void addDirectoriesForDataFiles(TarWriter &tar, const std::vector<DataFile> &dataFiles) {
    std::vector<std::string> dirs;
    /// @brief Record one normalized archive-directory path unless it is already present.
    /// @param dirPath Directory path to normalize and add.
    auto ensureDir = [&](const std::string &dirPath) {
        std::string d = dirPath;
        if (!d.empty() && d.back() != '/')
            d.push_back('/');
        if (std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(d);
    };

    for (const auto &df : dataFiles) {
        if (df.directory)
            ensureDir("./" + df.installPath);
        size_t pos = 0;
        while ((pos = df.installPath.find('/', pos)) != std::string::npos) {
            ensureDir("./" + df.installPath.substr(0, pos));
            ++pos;
        }
    }

    tar.addDirectory("./", 0755);
    std::sort(dirs.begin(), dirs.end());
    for (const auto &dir : dirs)
        tar.addDirectory(dir, 0755);
}

/// @brief Map a Zanna arch string ("x64", "arm64") to the RPM architecture name
/// ("x86_64"/"aarch64").
/// @param arch Portable architecture name.
/// @return RPM architecture identifier.
/// @throws std::runtime_error If the architecture is unsupported.
std::string rpmArchFor(const std::string &arch) {
    validateToolchainArchitecture(arch);
    return arch == "arm64" ? "aarch64" : "x86_64";
}

/// @brief Validate that `manifest` is a well-formed Linux toolchain manifest.
/// Throws if the manifest platform is not "linux", naming `packageKind` in the error message.
/// @param manifest Staged manifest to validate.
/// @param packageKind Human-readable package kind used in diagnostics.
/// @throws std::runtime_error If the manifest is invalid or targets another platform.
void requireLinuxToolchainManifest(const ToolchainInstallManifest &manifest,
                                   const char *packageKind) {
    validateToolchainInstallManifest(manifest);
    if (manifest.platform != "linux") {
        throw std::runtime_error(std::string(packageKind) +
                                 " requires a Linux staged toolchain manifest, got '" +
                                 manifest.platform + "'");
    }
}

/// @brief Join strings with ", " — used to render Debian Depends/Requires lines.
/// @param items Ordered strings to join.
/// @return Comma-and-space-delimited text, or an empty string for no items.
std::string joinCommaSeparated(const std::vector<std::string> &items) {
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            out << ", ";
        out << items[i];
    }
    return out.str();
}

/// @brief Test whether the manifest stages a support/library file whose filename
///        contains @p name (case-insensitive substring match).
/// @details Used to derive runtime package dependencies from which Zanna support
///          libraries are actually included in the staged tree.
/// @param manifest Manifest whose library entries are examined.
/// @param name Lowercase-insensitive filename fragment to locate.
/// @return `true` if a support/library filename contains `name`.
bool manifestHasSupportLibrary(const ToolchainInstallManifest &manifest, std::string_view name) {
    for (const auto &file : manifest.files) {
        if (file.kind != ToolchainFileKind::SupportLibrary &&
            file.kind != ToolchainFileKind::Library)
            continue;
        std::string filename = fs::path(file.stagedRelativePath).filename().string();
        filename = lowerAscii(std::move(filename));
        if (filename.find(name) != std::string::npos)
            return true;
    }
    return false;
}

/// @brief Return every DT_NEEDED SONAME discovered in staged ELF files.
/// @details Reads binary/support/library files from the manifest and parses
///          ELF64 dynamic sections. Malformed or non-ELF files are ignored so
///          package generation remains conservative rather than brittle.
/// @param manifest Toolchain staging manifest to inspect.
/// @return Sorted unique library SONAMEs.
std::vector<std::string> manifestNeededLibraries(const ToolchainInstallManifest &manifest) {
    std::vector<std::string> needed;
    for (const auto &file : manifest.files) {
        if (file.symlink || (file.kind != ToolchainFileKind::Binary &&
                             file.kind != ToolchainFileKind::SupportLibrary &&
                             file.kind != ToolchainFileKind::Library)) {
            continue;
        }
        std::error_code ec;
        if (!fs::is_regular_file(file.stagedAbsolutePath, ec) || ec)
            continue;
        try {
            std::vector<uint8_t> data = readFile(file.stagedAbsolutePath.string());
            std::vector<std::string> fileNeeded = elfNeededLibraries(data);
            needed.insert(needed.end(), fileNeeded.begin(), fileNeeded.end());
        } catch (const std::exception &) {
            continue;
        }
    }
    std::sort(needed.begin(), needed.end());
    needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
    return needed;
}

/// @brief True if the manifest includes graphics/GUI libraries that need X11.
/// @param manifest Staged toolchain manifest to inspect.
/// @return Whether ELF dependencies or bundled library names imply X11.
bool manifestNeedsX11(const ToolchainInstallManifest &manifest) {
    const auto needed = manifestNeededLibraries(manifest);
    if (std::find(needed.begin(), needed.end(), "libX11.so.6") != needed.end())
        return true;
    return manifestHasSupportLibrary(manifest, "zannagfx") ||
           manifestHasSupportLibrary(manifest, "zannagui");
}

/// @brief True if the manifest includes the audio library that needs ALSA.
/// @param manifest Staged toolchain manifest to inspect.
/// @return Whether ELF dependencies or bundled library names imply ALSA.
bool manifestNeedsAlsa(const ToolchainInstallManifest &manifest) {
    const auto needed = manifestNeededLibraries(manifest);
    if (std::find(needed.begin(), needed.end(), "libasound.so.2") != needed.end())
        return true;
    return manifestHasSupportLibrary(manifest, "zannaaud");
}

/// @brief Which C++ runtime the staged toolchain dynamically links.
enum class ToolchainCxxRuntime { Unknown, LibStdCxx, LibCxx };

/// @brief Detect the linked C++ runtime by scanning the primary staged ELF binary for the
///        libstdc++ / libc++ SONAME in its dynamic string table. Returns Unknown when the
///        binary is absent, not an ELF, or references neither/both — callers then keep the
///        conservative default dependency.
/// @param manifest Staged manifest containing candidate primary binaries.
/// @return Detected runtime family, or ToolchainCxxRuntime::Unknown.
ToolchainCxxRuntime detectToolchainCxxRuntime(const ToolchainInstallManifest &manifest) {
    const ToolchainFileEntry *primary = nullptr;
    for (const auto &entry : manifest.files) {
        if (entry.symlink || entry.kind != ToolchainFileKind::Binary)
            continue;
        const std::string base = fs::path(entry.stagedRelativePath).filename().string();
        if (base == "zanna" || base == "zanna.exe") {
            primary = &entry;
            break;
        }
        if (!primary)
            primary = &entry;
    }
    if (!primary)
        return ToolchainCxxRuntime::Unknown;
    std::error_code ec;
    if (!fs::is_regular_file(primary->stagedAbsolutePath, ec))
        return ToolchainCxxRuntime::Unknown;
    std::vector<uint8_t> data;
    try {
        data = readFile(primary->stagedAbsolutePath.string());
    } catch (const std::exception &) {
        return ToolchainCxxRuntime::Unknown;
    }
    if (data.size() < 4 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return ToolchainCxxRuntime::Unknown;
    const auto needed = elfNeededLibraries(data);
    bool hasStdCxx = std::find(needed.begin(), needed.end(), "libstdc++.so.6") != needed.end();
    bool hasLibCxx = std::find(needed.begin(), needed.end(), "libc++.so.1") != needed.end();
    if (needed.empty()) {
        const std::string_view view(reinterpret_cast<const char *>(data.data()), data.size());
        hasStdCxx = view.find("libstdc++.so.6") != std::string_view::npos;
        hasLibCxx = view.find("libc++.so.1") != std::string_view::npos;
    }
    if (hasStdCxx && !hasLibCxx)
        return ToolchainCxxRuntime::LibStdCxx;
    if (hasLibCxx && !hasStdCxx)
        return ToolchainCxxRuntime::LibCxx;
    return ToolchainCxxRuntime::Unknown;
}

/// @brief Return the Debian Depends line for a toolchain .deb.
/// @details Narrows the C++ runtime dependency to the library the staged binary actually links
///          (libstdc++6 or libc++1); falls back to the `libstdc++6 | libc++1` alternative when
///          detection is inconclusive.
/// @param manifest Manifest used for runtime and optional-library detection.
/// @return Debian control-file dependency expression.
std::string toolchainDebDepends(const ToolchainInstallManifest &manifest) {
    std::string cxxRuntime = "libstdc++6 | libc++1";
    switch (detectToolchainCxxRuntime(manifest)) {
        case ToolchainCxxRuntime::LibStdCxx:
            cxxRuntime = "libstdc++6";
            break;
        case ToolchainCxxRuntime::LibCxx:
            cxxRuntime = "libc++1";
            break;
        case ToolchainCxxRuntime::Unknown:
            break;
    }
    std::vector<std::string> deps = {
        "libc6",
        cxxRuntime,
        "libgcc-s1",
    };
    if (manifestNeedsX11(manifest))
        deps.push_back("libx11-6");
    if (manifestNeedsAlsa(manifest))
        deps.push_back("libasound2 | libasound2t64");
    return joinCommaSeparated(deps);
}

/// @brief Return optional developer and desktop integration tools for Debian metadata.
/// @details These improve native linking, CMake consumption, and system cache integration but are
///          not required to execute the already-built Zanna binaries.
std::string toolchainDebRecommends() {
    return joinCommaSeparated(
        {"cmake", "g++ | clang", "make", "desktop-file-utils", "shared-mime-info", "man-db"});
}

/// @brief Build the Debian `Depends:` list for application packages.
/// @details Adds conservative base runtime dependencies for generated native
///          applications, then appends validated user-specified dependencies
///          without duplicating exact entries.
/// @param pkg Package configuration containing any additional dependency terms.
/// @return Ordered dependency list suitable for a Debian control file.
std::vector<std::string> appDebDepends(const PackageConfig &pkg) {
    std::vector<std::string> deps = {
        "libc6",
        "libstdc++6 | libc++1",
    };
    for (const auto &dep : pkg.depends) {
        validateDebDependency(dep);
        if (std::find(deps.begin(), deps.end(), dep) == deps.end())
            deps.push_back(dep);
    }
    return deps;
}

/// @brief Build the RPM `Requires:` list for application packages.
/// @details Adds conservative base runtime dependencies, package-manager helper
///          tools when desktop/MIME metadata is emitted, then appends validated
///          `package-rpm-depends` entries without exact duplicates.
/// @param pkg Package metadata controlling desktop/MIME output and user deps.
/// @return Ordered RPM Requires entries.
std::vector<std::string> appRpmRequires(const PackageConfig &pkg) {
    std::vector<std::string> deps = {"glibc", "libstdc++", "libgcc"};
    const bool needDesktopEntry =
        pkg.shortcutMenu || pkg.shortcutDesktop || !pkg.fileAssociations.empty();
    if (needDesktopEntry)
        deps.push_back("desktop-file-utils");
    if (!pkg.fileAssociations.empty())
        deps.push_back("shared-mime-info");
    for (const auto &dep : pkg.rpmDepends) {
        validateRpmDependency(dep);
        if (std::find(deps.begin(), deps.end(), dep) == deps.end())
            deps.push_back(dep);
    }
    return deps;
}

/// @brief Escape text for simple AppStream XML text nodes and attributes.
/// @param text Raw metadata value.
/// @return XML-safe text with reserved characters escaped.
std::string appStreamXmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

/// @brief Generate minimal AppStream component metadata for a Linux application.
/// @details The generated file is intentionally small and schema-friendly: it
///          identifies the desktop launcher, name, summary, metadata license, and
///          optional homepage/project license. Richer screenshots and releases can
///          be added later through manifest fields without changing package layout.
/// @param pkgName Normalized Linux package name.
/// @param displayName User-visible application name.
/// @param version Package version string for release metadata.
/// @param pkg Package metadata from the manifest.
/// @return AppStream metainfo XML.
std::string generateAppStreamMetaInfo(const std::string &pkgName,
                                      const std::string &displayName,
                                      const std::string &version,
                                      const PackageConfig &pkg) {
    const std::string componentId =
        pkg.appstreamId.empty() ? pkgName + ".desktop" : pkg.appstreamId;
    const std::string summary = pkg.description.empty() ? displayName : pkg.description;
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<component type=\"desktop-application\">\n"
        << "  <id>" << appStreamXmlEscape(componentId) << "</id>\n"
        << "  <metadata_license>CC0-1.0</metadata_license>\n";
    if (!pkg.license.empty())
        xml << "  <project_license>" << appStreamXmlEscape(pkg.license) << "</project_license>\n";
    xml << "  <name>" << appStreamXmlEscape(displayName) << "</name>\n"
        << "  <summary>" << appStreamXmlEscape(summary) << "</summary>\n"
        << "  <launchable type=\"desktop-id\">" << appStreamXmlEscape(pkgName)
        << ".desktop</launchable>\n";
    if (!pkg.homepage.empty())
        xml << "  <url type=\"homepage\">" << appStreamXmlEscape(pkg.homepage) << "</url>\n";
    xml << "  <releases>\n"
        << "    <release version=\"" << appStreamXmlEscape(version) << "\"/>\n"
        << "  </releases>\n"
        << "</component>\n";
    return xml.str();
}

/// @brief Build the RPM "Requires" list for a toolchain package.
/// @details Always requires the base C/C++ runtime and build tools, and adds
///          libX11 / alsa-lib when the manifest stages the graphics/audio support
///          libraries.
/// @param manifest Manifest used for runtime and optional-library detection.
/// @return Ordered RPM requirement names.
std::vector<std::string> toolchainRpmRequires(const ToolchainInstallManifest &manifest) {
    std::string cxxRuntime = "libstdc++";
    if (detectToolchainCxxRuntime(manifest) == ToolchainCxxRuntime::LibCxx)
        cxxRuntime = "libcxx";
    std::vector<std::string> deps = {
        "glibc",
        cxxRuntime,
        "libgcc",
    };
    if (manifestNeedsX11(manifest))
        deps.push_back("libX11");
    if (manifestNeedsAlsa(manifest))
        deps.push_back("alsa-lib");
    return deps;
}

/// @brief Return optional developer and desktop integration tools for RPM metadata.
std::vector<std::string> toolchainRpmRecommends() {
    return {"cmake", "gcc-c++", "make", "desktop-file-utils", "shared-mime-info", "man-db"};
}

/// @brief Sort @p paths and remove exact duplicates (taken by value).
/// @param paths Paths to sort and deduplicate.
/// @return Sorted unique paths.
std::vector<std::string> sortedUniquePaths(std::vector<std::string> paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

/// @brief Render the PREFIX-relative installed-files manifest written into tarballs.
/// @details Sorts and de-duplicates @p paths and emits one path per line under a
///          header comment, so the uninstall script knows what to remove.
/// @param paths Prefix-relative installed paths.
/// @return Text manifest containing one sorted unique path per line.
std::string renderInstallManifest(std::vector<std::string> paths) {
    paths = sortedUniquePaths(std::move(paths));
    std::ostringstream out;
    out << "# Zanna toolchain installed files, relative to PREFIX.\n";
    for (const auto &path : paths)
        out << path << "\n";
    return out.str();
}

/// @brief Shared shell helpers for generated Linux install/uninstall scripts.
std::string linuxPathSafetyShellFunctions() {
    return R"ZANNA_SCRIPT(
validate_manifest_relpath() {
    rel=$1
    case "$rel" in
        ""|\#*) return 1 ;;
        /*|..|../*|*/../*|*/..) echo "Unsafe manifest path: $rel" >&2; exit 2 ;;
    esac
    return 0
}

check_no_symlink_path() {
    path=$1
    case "$path" in
        /*) current=/; rest=${path#/} ;;
        *) current=.; rest=$path ;;
    esac
    while [ -n "$rest" ]; do
        component=${rest%%/*}
        if [ "$component" = "$rest" ]; then
            rest=
        else
            rest=${rest#*/}
        fi
        [ -z "$component" ] && continue
        if [ "$current" = "/" ]; then
            current="/$component"
        else
            current="$current/$component"
        fi
        if [ -L "$current" ]; then
            echo "Refusing to operate through symlink path component: $current" >&2
            exit 2
        fi
    done
}

check_no_symlink_parents() {
    path=$1
    case "$path" in
        */*) parent=${path%/*} ;;
        *) parent=. ;;
    esac
    check_no_symlink_path "$parent"
}

path_exists_or_link() {
    [ -e "$1" ] || [ -L "$1" ]
}

)ZANNA_SCRIPT";
}

/// @brief Return the POSIX `install.sh` script bundled in the portable tarball.
/// @details Copies the staged tree under PREFIX (default /usr/local), honoring
///          DESTDIR, and records what it installed for the matching uninstaller.
std::string linuxTarballInstallScript() {
    return std::string(R"ZANNA_SCRIPT(#!/bin/sh
set -eu
umask 022

dry_run=0
force=0
quiet=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1 ;;
        --force) force=1 ;;
        --quiet) quiet=1 ;;
        --help|-h)
            echo "Usage: ./install.sh [--dry-run] [--force] [--quiet]"
            echo "Environment: PREFIX=/usr/local DESTDIR=... NO_COLOR=1"
            exit 0 ;;
        *) echo "Unknown install option: $1" >&2; exit 2 ;;
    esac
    shift
done

info() {
    [ "$quiet" = 1 ] && return
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
        printf '\033[1;34mZanna installer\033[0m  %s\n' "$*"
    else
        printf 'Zanna installer: %s\n' "$*"
    fi
}
fail() { printf 'Zanna installer error: %s\n' "$*" >&2; exit 2; }

prefix=${PREFIX:-/usr/local}
destdir=${DESTDIR:-}

case "$prefix" in
    /*) ;;
    *) fail "PREFIX must be an absolute path" ;;
esac
case "$destdir" in
    ""|/*) ;;
    *) fail "DESTDIR must be empty or an absolute path" ;;
esac

root=$(CDPATH= cd "$(dirname "$0")" && pwd)
install_root=${destdir%/}$prefix
old_manifest="$install_root/share/zanna/install_manifest.txt"
new_manifest="$root/share/zanna/install_manifest.txt"
)ZANNA_SCRIPT") +
           linuxPathSafetyShellFunctions() + R"ZANNA_SCRIPT(
[ -f "$new_manifest" ] || fail "package install manifest is missing: $new_manifest"

set --
for item in "$root"/* "$root"/.[!.]* "$root"/..?*; do
    path_exists_or_link "$item" || continue
    name=${item##*/}
    case "$name" in install.sh|uninstall.sh|README.install) continue ;; esac
    set -- "$@" "$name"
done

if [ "$#" -eq 0 ]; then
    fail "no installable Zanna payload directories were found"
fi

# Refuse to overwrite unrelated paths before making any changes. A prior Zanna
# manifest proves ownership; --force is required for all other existing files.
conflicts=0
changes=0
while IFS= read -r rel || [ -n "$rel" ]; do
    validate_manifest_relpath "$rel" || continue
    destination="$install_root/$rel"
    check_no_symlink_parents "$destination"
    action=install
    if path_exists_or_link "$destination"; then
        if [ -d "$destination" ] && [ ! -L "$destination" ]; then
            printf 'Conflict: expected a file but found a directory: %s\n' "$destination" >&2
            conflicts=$((conflicts + 1))
            continue
        fi
        owned=0
        if [ -f "$old_manifest" ] && grep -F -x -- "$rel" "$old_manifest" >/dev/null 2>&1; then
            owned=1
        fi
        if [ "$owned" != 1 ] && [ "$force" != 1 ]; then
            printf 'Conflict: refusing to replace unowned path: %s\n' "$destination" >&2
            conflicts=$((conflicts + 1))
            continue
        fi
        action=replace
    fi
    changes=$((changes + 1))
    if [ "$dry_run" = 1 ]; then printf '%s %s\n' "$action" "$destination"; fi
done < "$new_manifest"

[ "$conflicts" = 0 ] || fail "$conflicts path conflict(s); inspect them or rerun with --force"
if [ "$dry_run" = 1 ]; then
    info "dry run complete: $changes owned path(s) would be installed under $install_root"
    exit 0
fi

check_no_symlink_path "$install_root"
install_parent=${install_root%/*}
[ -n "$install_parent" ] || install_parent=/
check_no_symlink_path "$install_parent"
mkdir -p "$install_parent"
work_dir=$(mktemp -d "$install_parent/.zanna-install.XXXXXX")
stage_dir=$work_dir/stage
backup_dir=$work_dir/backup
committed_file=$work_dir/committed
stale_file=$work_dir/stale
mkdir "$stage_dir" "$backup_dir"
: > "$committed_file"
: > "$stale_file"
commit_complete=0
rollback_install() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$commit_complete" != 1 ] && [ -f "$committed_file" ]; then
        info "rolling back incomplete installation"
        while IFS= read -r rel || [ -n "$rel" ]; do
            validate_manifest_relpath "$rel" || continue
            destination="$install_root/$rel"
            rm -rf "$destination"
            if path_exists_or_link "$backup_dir/$rel"; then
                mkdir -p "$(dirname "$destination")"
                mv "$backup_dir/$rel" "$destination" || true
            fi
        done < "$committed_file"
        while IFS= read -r rel || [ -n "$rel" ]; do
            validate_manifest_relpath "$rel" || continue
            if path_exists_or_link "$backup_dir/stale/$rel"; then
                destination="$install_root/$rel"
                mkdir -p "$(dirname "$destination")"
                mv "$backup_dir/stale/$rel" "$destination" || true
            fi
        done < "$stale_file"
    fi
    rm -rf "$work_dir"
    exit "$status"
}
trap rollback_install EXIT HUP INT TERM

info "staging payload"
(cd "$root" && tar cf - "$@") | (cd "$stage_dir" && tar xpf -)
mkdir -p "$stage_dir/share/zanna"
cp "$root/uninstall.sh" "$stage_dir/share/zanna/uninstall.sh"
chmod 755 "$stage_dir/share/zanna/uninstall.sh"
{
    printf '%s\n' "$prefix"
    printf '%s\n' "$destdir"
} > "$stage_dir/share/zanna/install_metadata"
chmod 644 "$stage_dir/share/zanna/install_metadata"

info "committing $changes owned path(s) to $install_root"
while IFS= read -r rel || [ -n "$rel" ]; do
    validate_manifest_relpath "$rel" || continue
    source_path="$stage_dir/$rel"
    destination="$install_root/$rel"
    if ! path_exists_or_link "$source_path"; then
        fail "staged payload is missing manifest path: $rel"
    fi
    check_no_symlink_parents "$destination"
    mkdir -p "$(dirname "$destination")"
    printf '%s\n' "$rel" >> "$committed_file"
    if path_exists_or_link "$destination"; then
        mkdir -p "$backup_dir/$(dirname "$rel")"
        mv "$destination" "$backup_dir/$rel"
    fi
    mv "$source_path" "$destination"
done < "$new_manifest"

# Remove files owned by the previous Zanna tarball only after the new payload
# has committed successfully.
old_snapshot="$backup_dir/share/zanna/install_manifest.txt"
if [ -f "$old_snapshot" ]; then
    while IFS= read -r rel || [ -n "$rel" ]; do
        validate_manifest_relpath "$rel" || continue
        if ! grep -F -x -- "$rel" "$new_manifest" >/dev/null 2>&1; then
            destination="$install_root/$rel"
            check_no_symlink_parents "$destination"
            if [ -d "$destination" ] && [ ! -L "$destination" ]; then
                fail "refusing to remove stale directory recorded as a file: $destination"
            fi
            if path_exists_or_link "$destination"; then
                mkdir -p "$backup_dir/stale/$(dirname "$rel")"
                printf '%s\n' "$rel" >> "$stale_file"
                mv "$destination" "$backup_dir/stale/$rel"
            fi
        fi
    done < "$old_snapshot"
fi

commit_complete=1
rm -rf "$work_dir"
trap - EXIT HUP INT TERM

if [ -z "$destdir" ]; then
    if command -v mandb >/dev/null 2>&1; then
        mandb >/dev/null 2>&1 || true
    fi
    if [ -d "$install_root/share/mime" ] && command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "$install_root/share/mime" || true
    fi
    if [ -d "$install_root/share/applications" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$install_root/share/applications" || true
    fi
fi

info "installed Zanna toolchain under $install_root"
info "verify with: $install_root/bin/zanna --version"
info "uninstall with: $install_root/share/zanna/uninstall.sh"
)ZANNA_SCRIPT";
}

/// @brief Return the POSIX `uninstall.sh` script bundled in the portable tarball.
/// @details Removes the files recorded by install.sh's manifest under PREFIX.
std::string linuxTarballUninstallScript() {
    return std::string(R"ZANNA_SCRIPT(#!/bin/sh
set -eu
umask 022

dry_run=0
quiet=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1 ;;
        --quiet) quiet=1 ;;
        --help|-h)
            echo "Usage: uninstall.sh [--dry-run] [--quiet]"
            echo "Use the same PREFIX and DESTDIR values supplied during installation."
            exit 0 ;;
        *) echo "Unknown uninstall option: $1" >&2; exit 2 ;;
    esac
    shift
done

info() {
    [ "$quiet" = 1 ] && return
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
        printf '\033[1;34mZanna uninstaller\033[0m  %s\n' "$*"
    else
        printf 'Zanna uninstaller: %s\n' "$*"
    fi
}
fail() { printf 'Zanna uninstaller error: %s\n' "$*" >&2; exit 2; }

prefix=${PREFIX:-/usr/local}
destdir=${DESTDIR:-}

case "$prefix" in
    /*) ;;
    *) fail "PREFIX must be an absolute path" ;;
esac
case "$destdir" in
    ""|/*) ;;
    *) fail "DESTDIR must be empty or an absolute path" ;;
esac

install_root=${destdir%/}$prefix
manifest="$install_root/share/zanna/install_manifest.txt"
metadata="$install_root/share/zanna/install_metadata"
)ZANNA_SCRIPT") +
           linuxPathSafetyShellFunctions() + R"ZANNA_SCRIPT(
if [ ! -f "$manifest" ]; then
    fail "Zanna install manifest not found: $manifest"
fi

if [ -f "$metadata" ]; then
    {
        IFS= read -r installed_prefix || installed_prefix=
        IFS= read -r installed_destdir || installed_destdir=
    } < "$metadata"
    if [ "$installed_prefix" != "$prefix" ] || [ "$installed_destdir" != "$destdir" ]; then
        fail "this installation used PREFIX=$installed_prefix DESTDIR=$installed_destdir; rerun with matching values"
    fi
fi

owned_count=0
while IFS= read -r rel || [ -n "$rel" ]; do
    validate_manifest_relpath "$rel" || continue
    destination="$install_root/$rel"
    check_no_symlink_parents "$destination"
    if [ -d "$destination" ] && [ ! -L "$destination" ]; then
        fail "refusing to remove directory recorded as a file: $destination"
    fi
    if path_exists_or_link "$destination"; then
        owned_count=$((owned_count + 1))
        if [ "$dry_run" = 1 ]; then printf 'remove %s\n' "$destination"; fi
    fi
done < "$manifest"

if [ "$dry_run" = 1 ]; then
    info "dry run complete: $owned_count owned path(s) would be removed from $install_root"
    exit 0
fi

install_parent=${install_root%/*}
[ -n "$install_parent" ] || install_parent=/
check_no_symlink_path "$install_parent"
work_dir=$(mktemp -d "$install_parent/.zanna-uninstall.XXXXXX")
moved_file=$work_dir/moved
: > "$moved_file"
collect_complete=0
rollback_uninstall() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$collect_complete" != 1 ] && [ -f "$moved_file" ]; then
        info "rolling back incomplete removal"
        while IFS= read -r rel || [ -n "$rel" ]; do
            validate_manifest_relpath "$rel" || continue
            if path_exists_or_link "$work_dir/files/$rel"; then
                destination="$install_root/$rel"
                mkdir -p "$(dirname "$destination")"
                mv "$work_dir/files/$rel" "$destination" || true
            fi
        done < "$moved_file"
    fi
    rm -rf "$work_dir"
    exit "$status"
}
trap rollback_uninstall EXIT HUP INT TERM

info "collecting $owned_count owned path(s) for removal"
while IFS= read -r rel || [ -n "$rel" ]; do
    validate_manifest_relpath "$rel" || continue
    destination="$install_root/$rel"
    check_no_symlink_parents "$destination"
    if path_exists_or_link "$destination"; then
        mkdir -p "$work_dir/files/$(dirname "$rel")"
        printf '%s\n' "$rel" >> "$moved_file"
        mv "$destination" "$work_dir/files/$rel"
    fi
done < "$manifest"

collect_complete=1
rm -rf "$work_dir"
trap - EXIT HUP INT TERM

for dir in \
    share/zanna \
    share/applications \
    share/mime/packages \
    share/mime \
    share/doc/zanna \
    lib/cmake/Zanna \
    lib/cmake \
    bin lib include share/doc share; do
    rmdir "$install_root/$dir" 2>/dev/null || true
done

if [ -z "$destdir" ]; then
    if command -v mandb >/dev/null 2>&1; then
        mandb >/dev/null 2>&1 || true
    fi
    if [ -d "$install_root/share/mime" ] && command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "$install_root/share/mime" || true
    fi
    if [ -d "$install_root/share/applications" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$install_root/share/applications" || true
    fi
fi

info "removed $owned_count Zanna-owned path(s) from $install_root"
)ZANNA_SCRIPT";
}

/// @brief Return the README text bundled in the portable toolchain tarball.
std::string linuxTarballReadme() {
    return R"ZANNA_TEXT(Zanna Toolchain Tarball

This archive is a portable Linux toolchain layout. You can run tools directly
from the extracted bin/ directory, or install the layout into a prefix.

Install:
  sudo ./install.sh

Preview without changing the filesystem:
  ./install.sh --dry-run

Install under a custom prefix:
  PREFIX=/opt/zanna sudo ./install.sh

Use without installing:
  ./bin/zanna --version
  ./bin/zannastudio --version

If you install to a custom non-system prefix, add that prefix's bin directory to
your shell PATH. For example:
  export PATH=/opt/zanna/bin:$PATH

Stage into a package root without refreshing system caches:
  DESTDIR=/tmp/zanna-root PREFIX=/usr ./install.sh

Verify after install:
  zanna --version
  zannastudio --version

Uninstall:
  sudo /usr/local/share/zanna/uninstall.sh

For a custom prefix, invoke its installed helper with the same PREFIX value:
  PREFIX=/opt/zanna sudo /opt/zanna/share/zanna/uninstall.sh

Before copying a new tarball payload, install.sh removes files listed in the
currently installed manifest when those files are absent from the new manifest.
The installer refuses to overwrite paths not owned by a prior Zanna manifest;
review conflicts before using --force. Payloads are staged on the destination
filesystem and committed with rollback backups. The chosen PREFIX and DESTDIR are
recorded in share/zanna/install_metadata. The uninstaller removes only files
listed in share/zanna/install_manifest.txt and supports --dry-run.
Desktop, MIME, and manpage caches are refreshed when the relevant host tools are
available.
)ZANNA_TEXT";
}

/// @brief Return true if the `rpmbuild` tool is available on PATH (exit code 0).
bool rpmbuildAvailable() {
    const RunResult rr = run_process({"rpmbuild", "--version"});
    return rr.exit_code == 0;
}

/// @brief Find the .rpm produced by rpmbuild under `tmpRoot/RPMS/<arch>/`.
/// Expects exactly one file matching `<packageName>-<version>-*.<arch>.rpm`; throws if none or more
/// than one.
/// @param tmpRoot Temporary rpmbuild top directory.
/// @param packageName Expected RPM package name.
/// @param version Expected RPM version.
/// @param arch RPM architecture subdirectory and filename suffix.
/// @return Path to the sole matching generated RPM.
/// @throws std::runtime_error If zero or multiple matching artifacts exist.
fs::path findGeneratedRpm(const fs::path &tmpRoot,
                          const std::string &packageName,
                          const std::string &version,
                          const std::string &arch) {
    const fs::path rpmDir = tmpRoot / "RPMS" / arch;
    const std::string prefix = packageName + "-" + version + "-";
    const std::string suffix = "." + arch + ".rpm";
    std::vector<fs::path> matches;
    std::error_code ec;
    if (fs::exists(rpmDir, ec)) {
        for (const auto &entry : fs::directory_iterator(rpmDir, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file(ec) || ec)
                continue;
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0 && name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                matches.push_back(entry.path());
            }
        }
    }
    if (matches.empty()) {
        throw std::runtime_error("rpmbuild did not produce an rpm matching " + prefix + "*" +
                                 suffix + " under " + rpmDir.string());
    }
    if (matches.size() > 1) {
        std::sort(matches.begin(), matches.end());
        throw std::runtime_error("rpmbuild produced multiple matching rpm artifacts; first was " +
                                 matches.front().string());
    }
    return matches.front();
}

/// @brief Run rpmbuild with deterministic metadata controls when SOURCE_DATE_EPOCH is set.
/// @param tmpRoot Temporary rpmbuild top directory.
/// @param specPath Path to the generated RPM spec.
/// @return Process launch result, including exit status and captured diagnostics.
RunResult runRpmBuild(const fs::path &tmpRoot, const fs::path &specPath) {
    std::vector<std::string> args = {"rpmbuild",
                                     "--define",
                                     "_topdir " + tmpRoot.string(),
                                     "--define",
                                     "_sourcedir " + (tmpRoot / "SOURCES").string(),
                                     "--define",
                                     "_specdir " + (tmpRoot / "SPECS").string()};
    if (const char *epoch = std::getenv("SOURCE_DATE_EPOCH")) {
        const std::string epochText(epoch);
        (void)parseSourceDateEpoch(epochText);
        args.insert(args.end(),
                    {"--define",
                     "_source_date_epoch " + epochText,
                     "--define",
                     "clamp_mtime_to_source_date_epoch 1",
                     "--define",
                     "use_source_date_epoch_as_buildtime 1",
                     "--define",
                     "_buildhost reproducible.zanna.local",
                     "--define",
                     "_build_id_links none"});
    }
    args.push_back("-bb");
    args.push_back(specPath.string());
    return run_process(args);
}

/// @brief Map a freedesktop.org Category string to the closest Debian section name.
/// Falls back to "utils" for unrecognized or empty categories.
/// @param category Semicolon-delimited desktop category string.
/// @return Debian section name derived from the first category.
std::string debSectionFor(const std::string &category) {
    if (category.empty())
        return "utils";
    const size_t semi = category.find(';');
    const std::string first = semi == std::string::npos ? category : category.substr(0, semi);
    if (first.empty())
        return "utils";
    if (first == "Development" || first == "IDE" || first == "Debugger" ||
        first == "RevisionControl")
        return "devel";
    if (first == "Game")
        return "games";
    if (first == "Graphics" || first == "Photography" || first == "2DGraphics" ||
        first == "3DGraphics")
        return "graphics";
    if (first == "Network" || first == "WebBrowser" || first == "Email")
        return "net";
    if (first == "AudioVideo" || first == "Audio" || first == "Video" || first == "Player")
        return "sound";
    if (first == "Office" || first == "Spreadsheet" || first == "WordProcessor" ||
        first == "Presentation")
        return "editors";
    if (first == "Science" || first == "Education")
        return "science";
    if (first == "System" || first == "Settings" || first == "Utility")
        return "utils";
    return "utils";
}

/// @brief Validate all metadata fields required for a Debian package.
/// Checks display name, version format, architecture, author, description, homepage URL,
/// license, categories, dependency syntax, and file association entries.
/// @param pkg Manifest package configuration.
/// @param displayName Resolved user-visible package name.
/// @param version Resolved Debian version.
/// @param arch Debian architecture field.
/// @throws std::runtime_error If any metadata field violates its format contract.
void validateDebMetadata(const PackageConfig &pkg,
                         const std::string &displayName,
                         const std::string &version,
                         const std::string &arch) {
    validateSingleLineField(displayName, "package display name");
    validateDebVersion(version, "package version");
    validateSingleLineField(arch, "package architecture");
    validateSingleLineField(pkg.author, "package author");
    validateSingleLineField(pkg.description, "package description");
    validatePackageUrl(pkg.homepage, "package homepage");
    validateSingleLineField(pkg.license, "package license");
    validateDesktopCategories(pkg.category);
    for (const auto &dep : pkg.depends)
        validateDebDependency(dep);
    validatePackageFileAssociations(pkg.fileAssociations);
}

/// @brief Validate metadata fields required for a portable tarball.
/// Checks display name, version format, author, description, homepage URL, and license.
/// @param pkg Manifest package configuration.
/// @param displayName Resolved user-visible package name.
/// @param version Portable archive version.
/// @throws std::runtime_error If any metadata field violates its format contract.
void validatePortableMetadata(const PackageConfig &pkg,
                              const std::string &displayName,
                              const std::string &version) {
    validateSingleLineField(displayName, "package display name");
    validatePortableTarballVersion(version);
    validateSingleLineField(pkg.author, "package author");
    validateSingleLineField(pkg.description, "package description");
    validatePackageUrl(pkg.homepage, "package homepage");
    validateSingleLineField(pkg.license, "package license");
}

/// @brief Build the Debian `Maintainer:` field from `pkg.author`.
/// Appends `<noreply@example.invalid>` when the author string does not already contain an email,
/// satisfying the required `Name <email>` format.
/// @param pkg Package configuration supplying author and optional maintainer email.
/// @param displayName Fallback maintainer name when the author is empty.
/// @return Validated Debian maintainer field.
std::string debMaintainerFor(const PackageConfig &pkg, const std::string &displayName) {
    std::string maintainer = trimAsciiWhitespace(pkg.author);
    if (maintainer.empty())
        maintainer = displayName.empty() ? std::string("Zanna Project") : displayName;
    validateSingleLineField(maintainer, "package maintainer");
    if (maintainer.find('<') != std::string::npos && maintainer.find('>') != std::string::npos)
        return maintainer;
    if (maintainer.find('@') != std::string::npos)
        return maintainer;
    std::string email = trimAsciiWhitespace(pkg.maintainerEmail);
    if (email.empty())
        email = "noreply@example.invalid";
    validateSingleLineField(email, "package maintainer email");
    return maintainer + " <" + email + ">";
}

/// @brief Build the Debian/RPM maintainer string for a toolchain package from the manifest.
/// @details Uses manifest.maintainer (default "Zanna Project") and manifest.maintainerEmail,
///          falling back to the project's GitHub contact address when no email is configured.
///          Always yields the required `Name <email>` form.
/// @param manifest Toolchain manifest supplying maintainer metadata.
/// @return Validated `Name <email>` maintainer string.
std::string toolchainMaintainer(const ToolchainInstallManifest &manifest) {
    std::string name = trimAsciiWhitespace(manifest.maintainer);
    if (name.empty())
        name = "Zanna Project";
    validateSingleLineField(name, "toolchain package maintainer");
    if (name.find('<') != std::string::npos && name.find('>') != std::string::npos)
        return name;
    if (name.find('@') != std::string::npos)
        return name;
    std::string email = trimAsciiWhitespace(manifest.maintainerEmail);
    if (email.empty())
        email = "splanck@users.noreply.github.com";
    validateSingleLineField(email, "toolchain package maintainer email");
    return name + " <" + email + ">";
}

/// @brief Return @p value as a POSIX shell single-quoted literal.
/// @details Escapes embedded single quotes using the standard close/escape/reopen
///          sequence. Used for package-generated maintainer scripts where
///          normalized package names are still embedded as data, not syntax.
/// @param value Arbitrary text to quote as one shell word.
/// @return POSIX single-quoted literal.
std::string shellSingleQuote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

/// @brief Append the opt-in Linux desktop-shortcut install snippet.
/// @details Copies the packaged .desktop launcher into existing root/home
///          Desktop directories and then restores ownership to the directory's
///          owner when `stat` is available. Failures remain non-fatal because
///          per-user desktop folders vary widely across Linux systems.
/// @param script Maintainer-script stream to append to.
/// @param pkgName Normalized package name used for the desktop filename.
void appendHomeDesktopShortcutInstallScript(std::ostream &script, const std::string &pkgName) {
    const std::string desktopName = shellSingleQuote(pkgName + ".desktop");
    const std::string source = shellSingleQuote("/usr/share/applications/" + pkgName + ".desktop");
    script << "for d in /root/Desktop /home/*/Desktop; do\n"
              "    [ -d \"$d\" ] && [ ! -L \"$d\" ] || continue\n"
              "    target=\"$d/\""
           << desktopName
           << "\n"
              "    [ ! -L \"$target\" ] || continue\n"
              "    tmp=$(mktemp \"$d/.zanna-desktop.XXXXXX\" 2>/dev/null) || continue\n"
              "    owner=$(stat -c %U \"$d\" 2>/dev/null || printf '')\n"
              "    group=$(stat -c %G \"$d\" 2>/dev/null || printf '')\n"
              "    cp "
           << source
           << " \"$tmp\" || { rm -f \"$tmp\"; continue; }\n"
              "    chmod 755 \"$tmp\" || true\n"
              "    if [ -n \"$owner\" ] && [ \"$owner\" != \"UNKNOWN\" ]; then\n"
              "        if [ -n \"$group\" ] && [ \"$group\" != \"UNKNOWN\" ]; then\n"
              "            chown \"$owner:$group\" \"$tmp\" || true\n"
              "        else\n"
              "            chown \"$owner\" \"$tmp\" || true\n"
              "        fi\n"
              "    fi\n"
              "    mv -f \"$tmp\" \"$target\" || rm -f \"$tmp\"\n"
              "done\n";
}

/// @brief Append the opt-in Linux desktop-shortcut removal snippet.
/// @details Removes only the exact launcher filename emitted by the matching
///          install script, ignoring missing or inaccessible user Desktop
///          directories so package removal does not fail on home-directory
///          permission edge cases.
/// @param script Maintainer-script stream to append to.
/// @param pkgName Normalized package name used for the desktop filename.
void appendHomeDesktopShortcutRemovalScript(std::ostream &script, const std::string &pkgName) {
    const std::string desktopName = shellSingleQuote(pkgName + ".desktop");
    script << "for d in /root/Desktop /home/*/Desktop; do\n"
              "    [ -d \"$d\" ] && [ ! -L \"$d\" ] || continue\n"
              "    target=\"$d/\""
           << desktopName
           << "\n"
              "    [ -f \"$target\" ] && [ ! -L \"$target\" ] && rm -f \"$target\" || true\n"
              "done\n";
}

/// @brief Return README text bundled in portable application tarballs.
/// @details The tarball layout is intentionally self-contained; this text tells
///          users where the executable and optional asset payloads live without
///          requiring an installer script.
/// @param displayName User-visible application name.
/// @param version Package version displayed in the title.
/// @param exeName Executable filename shown in run instructions.
/// @param pkg Package metadata used for optional description and attribution.
/// @return Generated README text.
std::string appTarballReadme(const std::string &displayName,
                             const std::string &version,
                             const std::string &exeName,
                             const PackageConfig &pkg) {
    std::ostringstream out;
    out << displayName << " " << version << "\n\n";
    if (!pkg.description.empty())
        out << pkg.description << "\n\n";
    out << "Run:\n  ./" << exeName << "\n";
    if (!pkg.assets.empty())
        out << "\nBundled assets are stored alongside the executable under their configured "
               "relative paths.\n";
    if (!pkg.homepage.empty())
        out << "\nHomepage: " << pkg.homepage << "\n";
    if (!pkg.author.empty())
        out << "Author: " << pkg.author << "\n";
    if (!pkg.license.empty())
        out << "License: " << pkg.license << "\n";
    return out.str();
}

/// @brief Return the license metadata text bundled in portable application tarballs.
/// @details Project manifests currently carry an SPDX-style license identifier,
///          not full license body text, so the packaged file records the declared
///          identifier and points consumers back to the project distribution for
///          complete terms when needed.
/// @param displayName User-visible application name.
/// @param pkg Package configuration supplying the license identifier.
/// @return Generated package license metadata text.
std::string appTarballLicenseText(const std::string &displayName, const PackageConfig &pkg) {
    std::ostringstream out;
    out << displayName << "\n";
    if (!pkg.license.empty())
        out << "SPDX-License-Identifier: " << pkg.license << "\n";
    else
        out << "SPDX-License-Identifier: NOASSERTION\n";
    return out.str();
}

/// @brief Return a portable tarball install helper for application packages.
/// @details The helper installs the unpacked application tree under
///          `${PREFIX:-$HOME/.local}/share/<package>` and writes a small launcher
///          into `${PREFIX}/bin` that runs from the installed app directory, giving
///          asset-relative applications the same working directory each time.
/// @param pkgName Normalized package directory name.
/// @param exeName Executable filename inside the tarball.
/// @return POSIX shell script text for `install.sh`.
std::string appTarballInstallScript(const std::string &pkgName, const std::string &exeName) {
    const std::string qPkg = shellSingleQuote(pkgName);
    const std::string qExe = shellSingleQuote(exeName);
    std::ostringstream script;
    script << "#!/bin/sh\n"
              "set -eu\n"
              "PREFIX=${PREFIX:-${HOME:-}/.local}\n"
              "if [ -z \"$PREFIX\" ]; then echo \"PREFIX or HOME must be set\" >&2; exit 2; fi\n"
              "app_name="
           << qPkg
           << "\n"
              "exe_name="
           << qExe
           << "\n"
              "bindir=$PREFIX/bin\n"
              "appdir=$PREFIX/share/$app_name\n"
              "mkdir -p \"$bindir\" \"$appdir\"\n"
              "find . -mindepth 1 -maxdepth 1 ! -name install.sh ! -name uninstall.sh "
              "! -name README.install -exec cp -R {} \"$appdir\" \\;\n"
              "cat > \"$bindir/$exe_name\" <<EOF\n"
              "#!/bin/sh\n"
              "cd \"$appdir\"\n"
              "exec \"./$exe_name\" \"\\$@\"\n"
              "EOF\n"
              "chmod +x \"$bindir/$exe_name\"\n"
              "echo \"Installed $app_name to $appdir\"\n";
    return script.str();
}

/// @brief Return a portable tarball uninstall helper for application packages.
/// @details Removes only the launcher and installed share directory written by
///          appTarballInstallScript(); it intentionally leaves parent directories
///          such as `$PREFIX/bin` in place.
/// @param pkgName Normalized package directory name.
/// @param exeName Executable filename installed as the launcher.
/// @return POSIX shell script text for `uninstall.sh`.
std::string appTarballUninstallScript(const std::string &pkgName, const std::string &exeName) {
    const std::string qPkg = shellSingleQuote(pkgName);
    const std::string qExe = shellSingleQuote(exeName);
    std::ostringstream script;
    script << "#!/bin/sh\n"
              "set -eu\n"
              "PREFIX=${PREFIX:-${HOME:-}/.local}\n"
              "if [ -z \"$PREFIX\" ]; then echo \"PREFIX or HOME must be set\" >&2; exit 2; fi\n"
              "app_name="
           << qPkg
           << "\n"
              "exe_name="
           << qExe
           << "\n"
              "rm -f \"$PREFIX/bin/$exe_name\"\n"
              "rm -rf \"$PREFIX/share/$app_name\"\n"
              "echo \"Removed $app_name from $PREFIX\"\n";
    return script.str();
}

/// @brief Return a small generated PNG used for toolchain bundle desktop metadata.
std::vector<uint8_t> defaultZannaAppImageIconPng() {
    return pngEncode(imageResize(defaultZannaToolchainIconImage(), 256, 256));
}

/// @brief Append Linux bundle desktop/icon metadata at the payload root.
/// @param tar Payload archive writer to extend.
/// @param packageName Normalized name used for desktop and icon filenames.
void addToolchainBundleMetadata(TarWriter &tar, const std::string &packageName) {
    DesktopEntryParams desktop;
    desktop.name = "Zanna Toolchain";
    desktop.comment = "Zanna source and IL tools";
    desktop.execPath = "AppRun";
    desktop.iconName = packageName;
    desktop.categories = "Development;";
    desktop.terminal = false;
    const std::string desktopText = generateDesktopEntry(desktop);
    tar.addFileString(packageName + ".desktop", desktopText, 0644);
    const auto icon = defaultZannaAppImageIconPng();
    tar.addFileVec(packageName + ".png", icon, 0644);
    tar.addFileVec(".DirIcon", icon, 0644);
}

/// @brief Return the self-extracting bundle AppRun launcher script for the Zanna toolchain.
/// @details When launched from a desktop shell with no arguments it starts
///          Zanna Studio. When invoked from a terminal or file association with
///          arguments, it preserves CLI behavior by delegating to `bin/zanna`.
/// @return POSIX shell script text installed as executable `AppRun`.
std::string toolchainAppRunScript() {
    return "#!/bin/sh\n"
           "set -eu\n"
           "self=$0\n"
           "case \"$self\" in /*) appdir=$(dirname -- \"$self\") ;;\n"
           "  *) appdir=$(CDPATH= cd -- \"$(dirname -- \"$self\")\" && pwd) ;;\n"
           "esac\n"
           "if [ \"$#\" -eq 0 ] && [ -x \"$appdir/bin/zannastudio\" ]; then\n"
           "  exec \"$appdir/bin/zannastudio\"\n"
           "fi\n"
           "exec \"$appdir/bin/zanna\" \"$@\"\n";
}

/// @brief Validate all install paths in `dataFiles` are normalized and unique.
/// Throws on path traversal, duplicate paths, or non-normalized separators.
/// @param dataFiles Payload entries to validate.
/// @throws std::runtime_error If a path is unsafe, non-normalized, or duplicated.
void validateDataFilePaths(const std::vector<DataFile> &dataFiles) {
    std::set<std::string> seen;
    for (const auto &df : dataFiles) {
        const std::string clean = sanitizePackageRelativePath(df.installPath, "linux install path");
        if (clean != df.installPath)
            throw std::runtime_error("linux install path was not normalized: " + df.installPath);
        if (!seen.insert(clean).second)
            throw std::runtime_error("duplicate linux package path: " + df.installPath);
    }
}

/// @brief Validate that `path` is normalized for portable archive use (no `..`, no absolute
/// prefix). Throws with `fieldName` in the error message if the path is not canonical.
/// @param path Package-relative path to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If `path` is unsafe or non-normalized.
void validatePortableArchivePath(const std::string &path, const char *fieldName) {
    const std::string clean = sanitizePackageRelativePath(path, fieldName);
    if (clean != path)
        throw std::runtime_error(std::string(fieldName) + " was not normalized: " + path);
}

/// @brief Format a relative install path for use in an RPM spec `%files` section.
/// Prepends `/`, escapes `%` characters (RPM macro start), and quotes the result if it
/// contains spaces or tabs.
/// @param path Normalized package-relative payload path.
/// @return Absolute, escaped RPM spec path expression.
/// @throws std::runtime_error If `path` is unsafe or non-normalized.
std::string rpmSpecFilePath(const std::string &path) {
    const std::string clean = sanitizePackageRelativePath(path, "rpm payload path");
    if (clean != path)
        throw std::runtime_error("rpm payload path was not normalized: " + path);

    std::string escaped = "/" + path;
    bool needsQuotes = false;
    std::string out;
    out.reserve(escaped.size() + 8);
    for (char c : escaped) {
        if (c == ' ' || c == '\t')
            needsQuotes = true;
        if (c == '%') {
            out += "%%";
        } else if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
            needsQuotes = true;
        } else {
            out.push_back(c);
        }
    }
    if (!needsQuotes)
        return out;
    return "\"" + out + "\"";
}

/// @brief Return true when an RPM payload path represents license terms.
/// @param path Package-relative payload path.
/// @return Whether the case-insensitive leaf name denotes common license terms.
bool isRpmLicensePath(const std::string &path) {
    std::string leaf = fs::path(path).filename().string();
    /// @brief Convert one path byte to lowercase for case-insensitive matching.
    /// @param c Byte to fold.
    /// @return Lowercase representation of `c` in the active C locale.
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return leaf == "license" || leaf == "license.txt" || leaf == "copying" || leaf == "copyright";
}

/// @brief Format a payload file with the appropriate RPM `%license`/`%doc` marker.
/// @param path Normalized package-relative payload path.
/// @return Escaped `%files` line, optionally prefixed with `%license` or `%doc`.
std::string rpmSpecOwnedFileLine(const std::string &path) {
    const std::string formatted = rpmSpecFilePath(path);
    if (isRpmLicensePath(path))
        return "%license " + formatted;
    if (path.rfind("usr/share/doc/", 0) == 0)
        return "%doc " + formatted;
    return formatted;
}

/// @brief Validate that `path` can safely appear in an RPM spec `%files` section.
/// Must pass the standard normalize check and must not contain embedded line breaks.
/// @param path Package-relative payload path to validate.
/// @throws std::runtime_error If the path is unsafe, non-normalized, or multiline.
void validateRpmSpecPath(const std::string &path) {
    (void)rpmSpecFilePath(path);
    for (char c : path) {
        if (c == '\n' || c == '\r')
            throw std::runtime_error("rpm payload path must not contain line breaks: " + path);
    }
}

/// @brief Return the platform name used in portable archive filenames for the current host.
std::string portableArchivePlatformName() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

/// @brief Create a unique temporary packaging directory under the system temp root.
/// @details Delegates to the shared exclusive-creation helper so concurrent packaging
///          invocations never remove or reuse another process's workspace.
/// @param stem Prefix to use for the generated directory name.
/// @return Path to the newly-created directory.
fs::path uniqueTempPackagingDir(std::string_view stem) {
    return createUniqueTempDirectory(fs::temp_directory_path(), stem);
}

/// @brief RAII guard that removes the given directory tree on destruction.
/// Used to clean up the rpmbuild temp workspace on success or failure.
class TempDirGuard {
  public:
    /// @brief Take ownership of a temporary directory path.
    /// @param path Directory tree to remove at destruction.
    explicit TempDirGuard(fs::path path) : path_(std::move(path)) {}

    /// @brief Best-effort removal of the owned directory tree.
    ~TempDirGuard() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

  private:
    fs::path path_;
};

} // namespace

/// @brief Collect the FHS-mapped data files (executable, assets, .desktop launcher,
///        icons, MIME XML) for an end-user Linux application package.
/// @details Shared by the Debian (.deb) and RPM application builders so both emit
///          an identical install layout (`/usr/bin/<exe>`, `/usr/share/<pkg>/...`,
///          `/usr/share/applications/<pkg>.desktop`, hicolor icons, MIME XML).
/// @param params Application inputs and package configuration.
/// @param pkgName Normalized Linux package name.
/// @param exeName Normalized installed executable name.
/// @param displayName Resolved user-visible application name.
/// @return Validated FHS payload entries shared by DEB and RPM builders.
static std::vector<DataFile> collectAppLinuxDataFiles(const LinuxBuildParams &params,
                                                      const std::string &pkgName,
                                                      const std::string &exeName,
                                                      const std::string &displayName) {
    const auto &pkg = params.pkgConfig;
    std::vector<DataFile> dataFiles;

    // The executable
    auto execData = readFile(params.executablePath);
    dataFiles.emplace_back("usr/bin/" + exeName, std::move(execData), 0755);

    // Assets
    for (const auto &asset : pkg.assets) {
        const std::string sourceRel =
            sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string sourceLeaf = fs::path(sourceRel).filename().generic_string();
        fs::path srcPath =
            resolvePackageSourcePath(params.projectRoot, asset.sourcePath, "asset source path");
        std::string targetDir = sanitizePackageRelativePath(asset.targetPath, "asset target path");

        std::string sharePrefix = joinPackageRelativePath("usr/share/" + pkgName, targetDir);

        if (!fs::exists(srcPath))
            throw std::runtime_error("asset not found: " + asset.sourcePath);

        if (fs::is_directory(srcPath)) {
            dataFiles.push_back(DataFile::dir(sharePrefix));
            /// @brief Convert one safely resolved asset-tree entry into a Linux payload item.
            /// @param entry Directory entry with verified logical and physical paths.
            /// @throws std::runtime_error If an entry path or file read is invalid.
            safeDirectoryIterateResolved(
                srcPath, params.projectRoot, [&](const SafeDirectoryEntry &entry) {
                    auto relPath = sanitizePackageRelativePath(
                        entry.logicalPath.lexically_relative(srcPath).generic_string(),
                        "asset path");
                    if (entry.directory) {
                        dataFiles.push_back(DataFile::dir(
                            joinPackageRelativePath(sharePrefix, relPath, "asset path")));
                    } else if (entry.regularFile) {
                        auto fileData = readFile(entry.resolvedPath.string());
                        dataFiles.emplace_back(
                            joinPackageRelativePath(sharePrefix, relPath, "asset path"),
                            std::move(fileData),
                            permissionBitsForFilesystemPath(entry.resolvedPath));
                    }
                });
        } else if (fs::is_regular_file(srcPath)) {
            auto fileData = readFile(srcPath.string());
            dataFiles.emplace_back(joinPackageRelativePath(sharePrefix, sourceLeaf, "asset path"),
                                   std::move(fileData),
                                   permissionBitsForFilesystemPath(srcPath));
        } else {
            throw std::runtime_error("asset is not a regular file or directory: " +
                                     asset.sourcePath);
        }
    }

    // .desktop file. File associations require a desktop handler even if it is
    // hidden from normal application menus.
    const bool needDesktopEntry =
        pkg.shortcutMenu || pkg.shortcutDesktop || !pkg.fileAssociations.empty();
    if (needDesktopEntry) {
        DesktopEntryParams dep;
        dep.name = displayName;
        dep.comment = pkg.description;
        dep.execPath = "/usr/bin/" + exeName;
        dep.iconName = exeName;
        dep.categories = pkg.category;
        dep.startupWmClass = pkg.linuxStartupWmClass;
        dep.keywords = pkg.linuxKeywords;
        dep.terminal = false;
        dep.workingDir = "/usr/share/" + pkgName;
        dep.fileAssociations = pkg.fileAssociations;
        dep.acceptsFileArgument = !pkg.fileAssociations.empty();
        dep.noDisplay = !pkg.shortcutMenu && !pkg.shortcutDesktop && !pkg.fileAssociations.empty();
        auto desktop = generateDesktopEntry(dep);
        std::vector<uint8_t> ddata(desktop.begin(), desktop.end());
        dataFiles.push_back({"usr/share/applications/" + pkgName + ".desktop", ddata});
        if (pkg.shortcutDesktop)
            dataFiles.push_back({"usr/share/" + pkgName + "/" + pkgName + ".desktop", ddata});
    }

    // Icon PNGs at standard sizes (via IconGenerator). Menu-visible apps get a
    // generated fallback so Icon=<exeName> never points at a missing theme icon.
    if (!pkg.iconPath.empty() || needDesktopEntry) {
        std::map<uint32_t, std::vector<uint8_t>> pngs;
        if (!pkg.iconPath.empty()) {
            fs::path iconSrc =
                resolvePackageSourcePath(params.projectRoot, pkg.iconPath, "package icon");
            if (!fs::is_regular_file(iconSrc))
                throw std::runtime_error("package icon not found: " + pkg.iconPath);
            auto srcImage = pngRead(iconSrc.string());
            pngs = generateMultiSizePngs(srcImage);
        } else {
            pngs = generateMultiSizePngs(defaultZannaToolchainIconImage());
        }
        for (const auto &[sz, pngData] : pngs) {
            std::string iconPath = "usr/share/icons/hicolor/" + std::to_string(sz) + "x" +
                                   std::to_string(sz) + "/apps/" + exeName + ".png";
            dataFiles.push_back({iconPath, pngData});
        }
    }

    // MIME type XML
    if (!pkg.fileAssociations.empty()) {
        auto mimeXml = generateMimeTypeXml(pkgName, pkg.fileAssociations);
        std::vector<uint8_t> mdata(mimeXml.begin(), mimeXml.end());
        dataFiles.push_back({"usr/share/mime/packages/" + pkgName + ".xml", mdata});
    }
    if (!pkg.appstreamId.empty()) {
        const std::string version = params.version.empty() ? "0.0.0" : params.version;
        const std::string appstream = generateAppStreamMetaInfo(pkgName, displayName, version, pkg);
        std::vector<uint8_t> adata(appstream.begin(), appstream.end());
        dataFiles.push_back({"usr/share/metainfo/" + pkgName + ".metainfo.xml", adata});
    }

    const std::string readme =
        pkg.readmeFilePath.empty()
            ? appTarballReadme(
                  displayName, params.version.empty() ? "0.0.0" : params.version, exeName, pkg)
            : readPackageTextFile(params.projectRoot, pkg.readmeFilePath, "package readme");
    dataFiles.emplace_back("usr/share/doc/" + pkgName + "/README",
                           std::vector<uint8_t>(readme.begin(), readme.end()),
                           0644);
    const std::string license =
        pkg.licenseFilePath.empty()
            ? appTarballLicenseText(displayName, pkg)
            : readPackageTextFile(params.projectRoot, pkg.licenseFilePath, "package license file");
    dataFiles.emplace_back("usr/share/doc/" + pkgName + "/copyright",
                           std::vector<uint8_t>(license.begin(), license.end()),
                           0644);
    validateDataFilePaths(dataFiles);
    return dataFiles;
}

/// @brief Build a Debian .deb package from the given build parameters.
/// Assembles control.tar.gz (control + md5sums + maintainer scripts) and data.tar.gz
/// (binary, assets, .desktop, icons, MIME XML) then wraps them in an ar archive.
/// @param params Application metadata, payload paths, architecture, and output destination.
/// @throws std::runtime_error If validation, input reading, archive assembly, or output fails.
void buildDebPackage(const LinuxBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    std::string pkgName = normalizeDebName(params.projectName);
    std::string exeName = normalizeExecName(params.projectName);
    std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    validatePackageHooksAllowed(pkg);
    validateDebMetadata(pkg, displayName, version, params.archStr);
    if (params.archStr != "amd64" && params.archStr != "arm64" && params.archStr != "all")
        throw std::runtime_error("Debian package architecture must be amd64, arm64, or all: " +
                                 params.archStr);
    if (!fs::is_regular_file(params.executablePath))
        throw std::runtime_error("Linux package executable is not a regular file: " +
                                 params.executablePath);

    // Collect all data files (for md5sums and data.tar)
    std::vector<DataFile> dataFiles =
        collectAppLinuxDataFiles(params, pkgName, exeName, displayName);
    // Recomputed here for the maintainer-script section below; the shared helper
    // applies the same condition when it emits the .desktop entry.
    const bool needDesktopEntry =
        pkg.shortcutMenu || pkg.shortcutDesktop || !pkg.fileAssociations.empty();

    // ─── Build data.tar ────────────────────────────────────────────────

    TarWriter dataTar;

    // Collect unique directories
    std::vector<std::string> dirs;
    /// @brief Record one normalized Debian archive directory unless already present.
    /// @param dirPath Directory path to normalize and add.
    auto ensureDir = [&](const std::string &dirPath) {
        std::string d = dirPath;
        if (!d.empty() && d.back() != '/')
            d.push_back('/');
        for (const auto &existing : dirs) {
            if (existing == d)
                return;
        }
        dirs.push_back(d);
    };

    for (const auto &df : dataFiles) {
        // Ensure all parent directories exist
        std::string path = df.installPath;
        if (df.directory)
            ensureDir("./" + path);
        size_t pos = 0;
        while ((pos = path.find('/', pos)) != std::string::npos) {
            ensureDir("./" + path.substr(0, pos));
            pos++;
        }
    }

    // Add root directory
    dataTar.addDirectory("./", 0755);

    // Add directories in sorted order
    std::sort(dirs.begin(), dirs.end());
    for (const auto &d : dirs)
        dataTar.addDirectory(d, 0755);

    // Add files
    for (const auto &df : dataFiles) {
        if (df.directory)
            continue;
        if (df.symlink)
            dataTar.addSymlink("./" + df.installPath, df.symlinkTarget);
        else
            dataTar.addFile("./" + df.installPath, df.data.data(), df.data.size(), df.mode);
    }

    auto dataTarBytes = dataTar.finish();
    auto dataTarGz = gzip(dataTarBytes.data(), dataTarBytes.size());

    // ─── Build control.tar ─────────────────────────────────────────────

    TarWriter controlTar;
    controlTar.addDirectory("./", 0755);

    // control file
    {
        std::ostringstream ctl;
        ctl << "Package: " << pkgName << "\n";
        ctl << "Version: " << version << "\n";
        ctl << "Section: " << debSectionFor(pkg.category) << "\n";
        ctl << "Priority: optional\n";
        ctl << "Architecture: " << params.archStr << "\n";
        ctl << "Maintainer: " << debMaintainerFor(pkg, displayName) << "\n";

        // Installed-Size in KiB
        uint64_t totalBytes = 0;
        for (const auto &df : dataFiles) {
            checkedAddU64(
                totalBytes, static_cast<uint64_t>(df.data.size()), "Debian package installed size");
        }
        ctl << "Installed-Size: " << roundedKiB(totalBytes, "Debian package") << "\n";

        // Dependencies
        ctl << "Depends: " << joinCommaSeparated(appDebDepends(pkg)) << "\n";

        ctl << "Description: ";
        if (!pkg.description.empty())
            ctl << pkg.description;
        else
            ctl << displayName;
        ctl << "\n";

        if (!pkg.homepage.empty())
            ctl << "Homepage: " << pkg.homepage << "\n";

        auto ctlStr = ctl.str();
        controlTar.addFileString("./control", ctlStr, 0644);
    }

    // md5sums file
    {
        std::ostringstream md5s;
        for (const auto &df : dataFiles) {
            if (!df.symlink && !df.directory) {
                auto hex = md5hex(df.data.data(), df.data.size());
                md5s << hex << "  " << df.installPath << "\n";
            }
        }
        controlTar.addFileString("./md5sums", md5s.str(), 0644);
    }

    // postinst script (update MIME database, desktop database, custom hooks)
    bool needPostinst = needDesktopEntry || pkg.shortcutDesktop || !pkg.postInstallScript.empty();
    if (needPostinst) {
        std::ostringstream pi;
        pi << "#!/bin/sh\n";
        pi << "set -e\n";
        if (!pkg.fileAssociations.empty())
            pi << "if command -v update-mime-database >/dev/null 2>&1; then "
                  "update-mime-database /usr/share/mime; fi\n";
        if (needDesktopEntry)
            pi << "if command -v update-desktop-database >/dev/null 2>&1; then "
                  "update-desktop-database /usr/share/applications; fi\n";
        if (pkg.shortcutDesktop && pkg.allowHomeDesktopShortcuts)
            appendHomeDesktopShortcutInstallScript(pi, pkgName);
        if (!pkg.postInstallScript.empty()) {
            pi << "# zanna user post-install hook begin\n";
            pi << normalizePackageHookScript(pkg.postInstallScript, "post-install script") << "\n";
            pi << "# zanna user post-install hook end\n";
        }
        controlTar.addFileString("./postinst", pi.str(), 0755);
    }

    // prerm script (custom hooks + desktop shortcut cleanup before files go away)
    bool needPrerm = pkg.shortcutDesktop || !pkg.preUninstallScript.empty();
    if (needPrerm) {
        std::ostringstream pr;
        pr << "#!/bin/sh\n";
        pr << "set -e\n";
        if (!pkg.preUninstallScript.empty()) {
            pr << "# zanna user pre-uninstall hook begin\n";
            pr << normalizePackageHookScript(pkg.preUninstallScript, "pre-uninstall script")
               << "\n";
            pr << "# zanna user pre-uninstall hook end\n";
        }
        if (pkg.shortcutDesktop && pkg.allowHomeDesktopShortcuts)
            appendHomeDesktopShortcutRemovalScript(pr, pkgName);
        controlTar.addFileString("./prerm", pr.str(), 0755);
    }

    // postrm script refreshes caches after package payload files have been removed.
    const bool needPostrm = !pkg.fileAssociations.empty() || needDesktopEntry;
    if (needPostrm) {
        std::ostringstream po;
        po << "#!/bin/sh\n";
        po << "set -e\n";
        if (!pkg.fileAssociations.empty())
            po << "if command -v update-mime-database >/dev/null 2>&1; then "
                  "update-mime-database /usr/share/mime || true; fi\n";
        if (needDesktopEntry)
            po << "if command -v update-desktop-database >/dev/null 2>&1; then "
                  "update-desktop-database /usr/share/applications || true; fi\n";
        controlTar.addFileString("./postrm", po.str(), 0755);
    }

    auto controlTarBytes = controlTar.finish();
    auto controlTarGz = gzip(controlTarBytes.data(), controlTarBytes.size());

    // ─── Assemble .deb (ar archive) ────────────────────────────────────

    ArWriter ar;

    // debian-binary: "2.0\n"
    ar.addMemberString("debian-binary", "2.0\n");

    // control.tar.gz
    ar.addMemberVec("control.tar.gz", controlTarGz);

    // data.tar.gz
    ar.addMemberVec("data.tar.gz", dataTarGz);

    ar.finishToFile(params.outputPath);
}

/// @brief Build a portable .tar.gz archive from the given build parameters.
/// Creates a top-level `<name>-<version>/` directory containing the binary and assets.
/// @param params Application metadata, payload paths, and output destination.
/// @throws std::runtime_error If validation, input reading, archive assembly, or output fails.
void buildTarball(const LinuxBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    std::string pkgName = normalizeDebName(params.projectName);
    std::string exeName = normalizeExecName(params.projectName);
    std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    validatePortableMetadata(pkg, displayName, version);
    if (!fs::is_regular_file(params.executablePath))
        throw std::runtime_error("tarball executable is not a regular file: " +
                                 params.executablePath);

    // Top-level directory in the tarball
    std::string topDir =
        sanitizePackageRelativePath(pkgName + "-" + portableArchiveVersionComponent(version),
                                    "tarball top-level directory") +
        "/";

    TarWriter tar;
    tar.addDirectory(topDir, 0755);

    // Executable
    auto execData = readFile(params.executablePath);
    tar.addFile(topDir + exeName, execData.data(), execData.size(), 0755);

    // Assets
    for (const auto &asset : pkg.assets) {
        const std::string sourceRel =
            sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string sourceLeaf = fs::path(sourceRel).filename().generic_string();
        fs::path srcPath =
            resolvePackageSourcePath(params.projectRoot, asset.sourcePath, "asset source path");
        std::string targetDir = sanitizePackageRelativePath(asset.targetPath, "asset target path");

        std::string prefix = joinPackageRelativePath(topDir, targetDir, "asset target path");

        if (!fs::exists(srcPath))
            throw std::runtime_error("asset not found: " + asset.sourcePath);

        if (fs::is_directory(srcPath)) {
            if (!targetDir.empty())
                tar.addDirectory(prefix, 0755);
            /// @brief Append one safely resolved asset-tree entry to the generic Linux archive.
            /// @param entry Directory entry with verified logical and physical paths.
            /// @throws std::runtime_error If an entry cannot be represented or read safely.
            safeDirectoryIterateResolved(
                srcPath, params.projectRoot, [&](const SafeDirectoryEntry &entry) {
                    if (entry.directory) {
                        auto relPath = sanitizePackageRelativePath(
                            entry.logicalPath.lexically_relative(srcPath).generic_string(),
                            "asset path");
                        tar.addDirectory(joinPackageRelativePath(prefix, relPath, "asset path"),
                                         0755);
                    } else if (entry.regularFile) {
                        auto relPath = sanitizePackageRelativePath(
                            entry.logicalPath.lexically_relative(srcPath).generic_string(),
                            "asset path");
                        auto fileData = readFile(entry.resolvedPath.string());
                        tar.addFile(joinPackageRelativePath(prefix, relPath, "asset path"),
                                    fileData.data(),
                                    fileData.size(),
                                    permissionBitsForFilesystemPath(entry.resolvedPath));
                    }
                });
        } else if (fs::is_regular_file(srcPath)) {
            auto fileData = readFile(srcPath.string());
            tar.addFile(joinPackageRelativePath(prefix, sourceLeaf, "asset path"),
                        fileData.data(),
                        fileData.size(),
                        permissionBitsForFilesystemPath(srcPath));
        } else {
            throw std::runtime_error("asset is not a regular file or directory: " +
                                     asset.sourcePath);
        }
    }

    const std::string readme = appTarballReadme(displayName, version, exeName, pkg);
    tar.addFile(topDir + "README.install",
                reinterpret_cast<const uint8_t *>(readme.data()),
                readme.size(),
                0644);
    if (!pkg.readmeFilePath.empty()) {
        const std::string projectReadme =
            readPackageTextFile(params.projectRoot, pkg.readmeFilePath, "package readme");
        tar.addFile(topDir + "README",
                    reinterpret_cast<const uint8_t *>(projectReadme.data()),
                    projectReadme.size(),
                    0644);
    }
    const std::string licenseText =
        pkg.licenseFilePath.empty()
            ? appTarballLicenseText(displayName, pkg)
            : readPackageTextFile(params.projectRoot, pkg.licenseFilePath, "package license file");
    tar.addFile(topDir + "LICENSE",
                reinterpret_cast<const uint8_t *>(licenseText.data()),
                licenseText.size(),
                0644);
    const std::string installScript = appTarballInstallScript(pkgName, exeName);
    tar.addFile(topDir + "install.sh",
                reinterpret_cast<const uint8_t *>(installScript.data()),
                installScript.size(),
                0755);
    const std::string uninstallScript = appTarballUninstallScript(pkgName, exeName);
    tar.addFile(topDir + "uninstall.sh",
                reinterpret_cast<const uint8_t *>(uninstallScript.data()),
                uninstallScript.size(),
                0755);

    auto tarBytes = tar.finish();
    auto tarGz = gzip(tarBytes.data(), tarBytes.size());

    writeFileAtomic(params.outputPath, tarGz);
}

/// @brief Build a self-extracting Linux `.run` bundle for an end-user application.
/// @details Reuses the same runtime stub and gzip-tar payload format as the
///          toolchain bundle path (buildToolchainBundle), but sources its
///          single executable, bundled assets, `.desktop` launcher, and icon from
///          an application's `LinuxBuildParams`/`PackageConfig` rather than a
///          staged toolchain manifest.
/// @param params Application metadata, payload paths, portable architecture, and output path.
/// @throws std::runtime_error If validation, stub generation, payload assembly, or output fails.
void buildAppImage(const LinuxBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    const std::string pkgName = normalizeDebName(params.projectName);
    const std::string exeName = normalizeExecName(params.projectName);
    const std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    validatePortableMetadata(pkg, displayName, version);
    if (params.archStr != "x64" && params.archStr != "arm64")
        throw std::runtime_error("Linux bundle architecture must be x64 or arm64: " +
                                 params.archStr);
    if (!fs::is_regular_file(params.executablePath))
        throw std::runtime_error("Linux bundle executable is not a regular file: " +
                                 params.executablePath);

    TarWriter tar;
    tar.addDirectory("./", 0755);
    // Entry point: AppRun -> usr/bin/<exe>, matching the toolchain bundle layout.
    tar.addSymlink("AppRun", "usr/bin/" + exeName);

    // Application executable.
    auto execData = readFile(params.executablePath);
    tar.addFile("usr/bin/" + exeName, execData.data(), execData.size(), 0755);

    // Bundled assets under usr/share/<pkg>/ (mirrors the .deb layout).
    const std::string assetBase = "usr/share/" + pkgName;
    for (const auto &asset : pkg.assets) {
        const std::string sourceRel =
            sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string sourceLeaf = fs::path(sourceRel).filename().generic_string();
        const fs::path srcPath =
            resolvePackageSourcePath(params.projectRoot, asset.sourcePath, "asset source path");
        const std::string targetDir =
            sanitizePackageRelativePath(asset.targetPath, "asset target path");
        const std::string prefix =
            joinPackageRelativePath(assetBase, targetDir, "asset target path");

        if (!fs::exists(srcPath))
            throw std::runtime_error("asset not found: " + asset.sourcePath);

        if (fs::is_directory(srcPath)) {
            tar.addDirectory(prefix, 0755);
            /// @brief Append one safely resolved asset-tree entry to the RPM source archive.
            /// @param entry Directory entry with verified logical and physical paths.
            /// @throws std::runtime_error If an entry cannot be represented or read safely.
            safeDirectoryIterateResolved(
                srcPath, params.projectRoot, [&](const SafeDirectoryEntry &entry) {
                    const auto relPath = sanitizePackageRelativePath(
                        entry.logicalPath.lexically_relative(srcPath).generic_string(),
                        "asset path");
                    if (entry.directory) {
                        tar.addDirectory(joinPackageRelativePath(prefix, relPath, "asset path"),
                                         0755);
                    } else if (entry.regularFile) {
                        auto fileData = readFile(entry.resolvedPath.string());
                        tar.addFile(joinPackageRelativePath(prefix, relPath, "asset path"),
                                    fileData.data(),
                                    fileData.size(),
                                    permissionBitsForFilesystemPath(entry.resolvedPath));
                    }
                });
        } else if (fs::is_regular_file(srcPath)) {
            auto fileData = readFile(srcPath.string());
            tar.addFile(joinPackageRelativePath(prefix, sourceLeaf, "asset path"),
                        fileData.data(),
                        fileData.size(),
                        permissionBitsForFilesystemPath(srcPath));
        } else {
            throw std::runtime_error("asset is not a regular file or directory: " +
                                     asset.sourcePath);
        }
    }

    // Desktop launcher at the payload root (Exec points at the AppRun entry).
    {
        DesktopEntryParams dep;
        dep.name = displayName;
        dep.comment = pkg.description;
        dep.execPath = "AppRun";
        dep.iconName = exeName;
        dep.categories = pkg.category;
        dep.startupWmClass = pkg.linuxStartupWmClass;
        dep.keywords = pkg.linuxKeywords;
        dep.terminal = false;
        dep.fileAssociations = pkg.fileAssociations;
        dep.acceptsFileArgument = !pkg.fileAssociations.empty();
        const std::string desktopText = generateDesktopEntry(dep);
        tar.addFileString(exeName + ".desktop", desktopText, 0644);
    }

    if (!pkg.appstreamId.empty()) {
        const std::string appstream = generateAppStreamMetaInfo(pkgName, displayName, version, pkg);
        tar.addDirectory("usr/share", 0755);
        tar.addDirectory("usr/share/metainfo", 0755);
        tar.addFileString("usr/share/metainfo/" + pkgName + ".metainfo.xml", appstream, 0644);
    }

    // Icon at the payload root (<exe>.png); falls back to a generated default icon.
    {
        std::vector<uint8_t> iconPng;
        if (!pkg.iconPath.empty()) {
            const fs::path iconSrc =
                resolvePackageSourcePath(params.projectRoot, pkg.iconPath, "package icon");
            if (!fs::is_regular_file(iconSrc))
                throw std::runtime_error("package icon not found: " + pkg.iconPath);
            const auto srcImage = pngRead(iconSrc.string());
            iconPng = pngEncode(srcImage);
        } else {
            iconPng = defaultZannaAppImageIconPng();
        }
        tar.addFileVec(exeName + ".png", iconPng, 0644);
        tar.addFileVec(".DirIcon", iconPng, 0644);
    }

    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    LinuxRuntimeStubParams stub;
    stub.cacheName =
        pkgName + "-" + portableArchiveVersionComponent(version) + "-linux-" + params.archStr;
    stub.entryPath = "AppRun";
    stub.appImageInterface = true;
    const auto appImage = buildLinuxAppImage(stub, tarGz);
    writeFileAtomic(params.outputPath, appImage);
    std::error_code ec;
    fs::permissions(params.outputPath,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);
    if (ec)
        throw std::runtime_error("cannot mark Linux bundle executable: " + ec.message());
}

/// @brief Build an RPM package for an end-user application using rpmbuild.
/// @details Mirrors the toolchain RPM path (buildToolchainRpmPackage): assembles a
///          source tarball and .spec from the application's shared FHS data files
///          (collectAppLinuxDataFiles), then invokes rpmbuild. Requires rpmbuild on
///          PATH (Fedora/RHEL build hosts) and throws a clear diagnostic otherwise.
/// @param params Application metadata, payload paths, portable architecture, and output path.
/// @throws std::runtime_error If validation fails, rpmbuild is unavailable/fails,
///         or the generated artifact cannot be located or copied.
void buildRpmPackage(const LinuxBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    const std::string pkgName = normalizeDebName(params.projectName);
    const std::string exeName = normalizeExecName(params.projectName);
    const std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    if (params.archStr != "x64" && params.archStr != "arm64")
        throw std::runtime_error("RPM architecture must be x64 or arm64: " + params.archStr);
    validateRpmVersion(version, "package version");
    if (!fs::is_regular_file(params.executablePath))
        throw std::runtime_error("RPM package executable is not a regular file: " +
                                 params.executablePath);
    const std::string arch = rpmArchFor(params.archStr);
    const auto dataFiles = collectAppLinuxDataFiles(params, pkgName, exeName, displayName);
    if (!rpmbuildAvailable()) {
        throw std::runtime_error(
            "rpmbuild is required to generate RPM application packages; install rpm-build "
            "or use --target linux, appimage, or tarball");
    }

    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-app-rpm-" + version + "-" + arch);
    TempDirGuard cleanup(tmpRoot);
    fs::create_directories(tmpRoot / "BUILD");
    fs::create_directories(tmpRoot / "BUILDROOT");
    fs::create_directories(tmpRoot / "RPMS");
    fs::create_directories(tmpRoot / "SOURCES");
    fs::create_directories(tmpRoot / "SPECS");
    fs::create_directories(tmpRoot / "SRPMS");

    const std::string sourceTopDir = pkgName + "-" + version + "/";
    TarWriter tar;
    tar.addDirectory(sourceTopDir, 0755);
    for (const auto &file : dataFiles) {
        std::string sourcePath = file.installPath;
        if (sourcePath.rfind("usr/", 0) == 0)
            sourcePath = sourcePath.substr(4);
        validateRpmSpecPath(file.installPath);
        if (file.symlink) {
            tar.addSymlink(sourceTopDir + sourcePath, file.symlinkTarget);
        } else if (!file.directory) {
            tar.addFile(sourceTopDir + sourcePath, file.data.data(), file.data.size(), file.mode);
        }
    }
    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    const fs::path sourceTar = tmpRoot / "SOURCES" / (pkgName + "-" + version + ".tar.gz");
    writeFileAtomic(sourceTar, tarGz);

    const std::string summary = pkg.description.empty() ? displayName : pkg.description;
    std::ostringstream spec;
    spec << "Name: " << pkgName << "\n";
    spec << "Version: " << version << "\n";
    spec << "Release: 1%{?dist}\n";
    {
        std::string summaryLine = summary;
        validateSingleLineField(summaryLine, "package summary");
        spec << "Summary: " << summaryLine << "\n";
    }
    spec << "Packager: " << debMaintainerFor(pkg, displayName) << "\n";
    {
        std::string license = trimAsciiWhitespace(pkg.license);
        if (license.empty())
            license = "Proprietary";
        validateSingleLineField(license, "package license");
        spec << "License: " << license << "\n";
    }
    if (!pkg.homepage.empty()) {
        validatePackageUrl(pkg.homepage, "package homepage");
        spec << "URL: " << pkg.homepage << "\n";
    }
    spec << "BuildArch: " << arch << "\n";
    spec << "Source0: %{name}-%{version}.tar.gz\n";
    for (const auto &dep : appRpmRequires(pkg))
        spec << "Requires: " << dep << "\n";
    spec << "\n";
    spec << "%description\n" << summary << "\n\n";
    spec << "%prep\n%setup -q\n\n";
    spec << "%build\n:\n\n";
    spec << "%install\nrm -rf %{buildroot}\nmkdir -p %{buildroot}/usr\ncp -a . "
            "%{buildroot}/usr/\n\n";
    const bool needDesktopEntry =
        pkg.shortcutMenu || pkg.shortcutDesktop || !pkg.fileAssociations.empty();
    if (needDesktopEntry || !pkg.fileAssociations.empty()) {
        spec << "%post\n";
        if (!pkg.fileAssociations.empty())
            spec << "if command -v update-mime-database >/dev/null 2>&1; then update-mime-database "
                    "/usr/share/mime || true; fi\n";
        if (needDesktopEntry)
            spec << "if command -v update-desktop-database >/dev/null 2>&1; then "
                    "update-desktop-database /usr/share/applications || true; fi\n";
        spec << "\n";
        spec << "%postun\n";
        if (!pkg.fileAssociations.empty())
            spec << "if command -v update-mime-database >/dev/null 2>&1; then update-mime-database "
                    "/usr/share/mime || true; fi\n";
        if (needDesktopEntry)
            spec << "if command -v update-desktop-database >/dev/null 2>&1; then "
                    "update-desktop-database /usr/share/applications || true; fi\n";
        spec << "\n";
    }
    spec << "%files\n";
    for (const auto &file : dataFiles) {
        validateRpmSpecPath(file.installPath);
        if (file.directory)
            spec << "%dir " << rpmSpecFilePath(file.installPath) << "\n";
        else
            spec << rpmSpecOwnedFileLine(file.installPath) << "\n";
    }

    const fs::path specPath = tmpRoot / "SPECS" / (pkgName + ".spec");
    {
        std::ofstream out(specPath);
        if (!out)
            throw std::runtime_error("cannot write rpm spec file: " + specPath.string());
        out << spec.str();
    }

    const RunResult rr = runRpmBuild(tmpRoot, specPath);
    if (rr.exit_code != 0) {
        throw std::runtime_error("rpmbuild failed while generating application rpm:\n" + rr.out +
                                 rr.err);
    }

    const fs::path rpmPath = findGeneratedRpm(tmpRoot, pkgName, version, arch);
    std::error_code copyEc;
    fs::copy_file(rpmPath, params.outputPath, fs::copy_options::overwrite_existing, copyEc);
    if (copyEc)
        throw std::runtime_error("cannot copy generated rpm to " + params.outputPath + ": " +
                                 copyEc.message());
}

/// @brief Build a Debian .deb toolchain package from a staged install manifest.
/// Validates the manifest, collects FHS-mapped files, generates control/md5sums/postinst/postrm,
/// and assembles the ar-format .deb output file.
/// @param params Staged manifest, package name, and output destination.
/// @throws std::runtime_error If validation, payload reading, archive assembly, or output fails.
void buildToolchainDebPackage(const LinuxToolchainBuildParams &params) {
    const auto &manifest = params.manifest;
    requireLinuxToolchainManifest(manifest, "Debian toolchain package");
    const std::string packageName =
        params.packageName.empty() ? std::string("zanna") : normalizeDebName(params.packageName);
    if (manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    const std::string version = manifest.version;
    validateDebVersion(version, "toolchain package version");
    validateToolchainArchitecture(manifest.arch, "toolchain package architecture");
    const std::string archStr = debArchFor(manifest.arch);
    const auto dataFiles = collectToolchainLinuxFiles(manifest, packageName);
    validateDataFilePaths(dataFiles);

    TarWriter dataTar;
    addDirectoriesForDataFiles(dataTar, dataFiles);
    for (const auto &df : dataFiles) {
        if (df.symlink)
            dataTar.addSymlink("./" + df.installPath, df.symlinkTarget);
        else if (df.directory)
            continue;
        else
            dataTar.addFile("./" + df.installPath, df.data.data(), df.data.size(), df.mode);
    }
    const auto dataTarBytes = dataTar.finish();
    const auto dataTarGz = gzip(dataTarBytes.data(), dataTarBytes.size());

    TarWriter controlTar;
    controlTar.addDirectory("./", 0755);
    {
        std::ostringstream ctl;
        ctl << "Package: " << packageName << "\n";
        ctl << "Version: " << version << "\n";
        ctl << "Section: devel\n";
        ctl << "Priority: optional\n";
        ctl << "Architecture: " << archStr << "\n";
        ctl << "Maintainer: " << toolchainMaintainer(manifest) << "\n";
        ctl << "Depends: " << toolchainDebDepends(manifest) << "\n";
        ctl << "Recommends: " << toolchainDebRecommends() << "\n";
        uint64_t totalBytes = 0;
        for (const auto &df : dataFiles) {
            checkedAddU64(totalBytes,
                          static_cast<uint64_t>(df.data.size()),
                          "Debian toolchain package installed size");
        }
        ctl << "Installed-Size: " << roundedKiB(totalBytes, "Debian toolchain package") << "\n";
        if (!manifest.homepage.empty()) {
            validatePackageUrl(manifest.homepage, "toolchain package homepage");
            ctl << "Homepage: " << manifest.homepage << "\n";
        }
        ctl << "Description: Zanna compiler toolchain\n";
        controlTar.addFileString("./control", ctl.str(), 0644);
    }
    {
        std::ostringstream md5s;
        for (const auto &df : dataFiles)
            if (!df.symlink && !df.directory)
                md5s << md5hex(df.data.data(), df.data.size()) << "  " << df.installPath << "\n";
        controlTar.addFileString("./md5sums", md5s.str(), 0644);
    }
    /// @brief Determine whether a manifest entry installs a manual page.
    /// @param entry Manifest entry inspected by the `std::any_of` predicate.
    /// @return `true` when `entry` is classified as a manual page.
    const bool hasManPages = std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.kind == ToolchainFileKind::ManPage;
        });
    const bool hasFileAssociations = !manifest.fileAssociations.empty();
    if (hasManPages || hasFileAssociations) {
        std::ostringstream postinst;
        postinst << "#!/bin/sh\nset -e\n";
        if (hasManPages)
            postinst
                << "if command -v mandb >/dev/null 2>&1; then mandb >/dev/null 2>&1 || true; fi\n";
        if (hasFileAssociations) {
            postinst << "if command -v update-mime-database >/dev/null 2>&1; then "
                        "update-mime-database /usr/share/mime || true; fi\n";
            postinst << "if command -v update-desktop-database >/dev/null 2>&1; then "
                        "update-desktop-database /usr/share/applications || true; fi\n";
        }
        controlTar.addFileString("./postinst", postinst.str(), 0755);
        std::ostringstream postrm;
        postrm << "#!/bin/sh\nset -e\n";
        if (hasManPages)
            postrm
                << "if command -v mandb >/dev/null 2>&1; then mandb >/dev/null 2>&1 || true; fi\n";
        if (hasFileAssociations) {
            postrm << "if command -v update-mime-database >/dev/null 2>&1; then "
                      "update-mime-database /usr/share/mime || true; fi\n";
            postrm << "if command -v update-desktop-database >/dev/null 2>&1; then "
                      "update-desktop-database /usr/share/applications || true; fi\n";
        }
        controlTar.addFileString("./postrm", postrm.str(), 0755);
    }
    const auto controlTarBytes = controlTar.finish();
    const auto controlTarGz = gzip(controlTarBytes.data(), controlTarBytes.size());

    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberVec("control.tar.gz", controlTarGz);
    ar.addMemberVec("data.tar.gz", dataTarGz);
    ar.finishToFile(params.outputPath);
}

/// @brief Build a portable toolchain tarball from a staged install manifest.
/// Supports Linux, macOS, and Windows payloads; universal arch is accepted for macOS only.
/// Writes a `<name>-<version>-<platform>-<arch>/` top-level directory into a .tar.gz file.
/// @param params Staged manifest, package name, and output destination.
/// @throws std::runtime_error If validation, payload reading, archive assembly, or output fails.
void buildToolchainTarball(const LinuxToolchainBuildParams &params) {
    const auto &manifest = params.manifest;
    validateToolchainInstallManifest(manifest);
    const std::string packageName =
        params.packageName.empty() ? std::string("zanna") : normalizeDebName(params.packageName);
    if (manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    const std::string version = manifest.version;
    validateDebVersion(version, "toolchain package version");
    const std::string platform =
        manifest.platform.empty() ? portableArchivePlatformName() : manifest.platform;
    validateToolchainPlatform(platform);
    if (manifest.arch == "universal") {
        if (platform != "macos") {
            throw std::runtime_error("universal toolchain tarball architecture is only valid "
                                     "for macOS payloads");
        }
    } else {
        validateToolchainArchitecture(manifest.arch, "toolchain package architecture");
    }
    const std::string topDir =
        sanitizePackageRelativePath(packageName + "-" + portableArchiveVersionComponent(version) +
                                        "-" + platform + "-" + manifest.arch,
                                    "toolchain tarball top-level directory") +
        "/";

    TarWriter tar;
    tar.addDirectory(topDir, 0755);
    std::vector<std::string> installManifestPaths;
    for (const auto &file : manifest.files) {
        const std::string relPath = mapInstallPath(file, InstallPathPolicy::PortableArchive);
        validatePortableArchivePath(relPath, "toolchain tarball path");
        if (platform == "linux")
            installManifestPaths.push_back(relPath);
        if (file.symlink) {
            tar.addSymlink(topDir + relPath, file.symlinkTarget);
        } else {
            const auto data = readFile(file.stagedAbsolutePath.string());
            tar.addFile(topDir + relPath, data.data(), data.size(), permissionBitsFor(file));
        }
    }
    if (platform == "linux") {
        std::vector<DataFile> generated;
        if (!manifest.fileAssociations.empty())
            addToolchainFileAssociationMetadata(generated, manifest, packageName, "zanna");
        addZannaStudioDesktopMetadata(generated, "zannastudio");
        addZannaToolchainHicolorIcons(generated, "zanna");
        for (const auto &df : generated) {
            const std::string portablePath = sanitizePackageRelativePath(
                df.installPath.rfind("usr/", 0) == 0 ? df.installPath.substr(4) : df.installPath,
                "toolchain tarball generated metadata path");
            installManifestPaths.push_back(portablePath);
            tar.addFile(topDir + portablePath, df.data.data(), df.data.size(), df.mode);
        }
    }
    if (platform == "linux") {
        installManifestPaths.push_back("share/zanna/install_manifest.txt");
        installManifestPaths.push_back("share/zanna/install_metadata");
        installManifestPaths.push_back("share/zanna/uninstall.sh");
        const std::string manifestText = renderInstallManifest(installManifestPaths);
        tar.addFile(topDir + "share/zanna/install_manifest.txt",
                    reinterpret_cast<const uint8_t *>(manifestText.data()),
                    manifestText.size(),
                    0644);

        const std::string installScript = linuxTarballInstallScript();
        tar.addFile(topDir + "install.sh",
                    reinterpret_cast<const uint8_t *>(installScript.data()),
                    installScript.size(),
                    0755);
        const std::string uninstallScript = linuxTarballUninstallScript();
        tar.addFile(topDir + "uninstall.sh",
                    reinterpret_cast<const uint8_t *>(uninstallScript.data()),
                    uninstallScript.size(),
                    0755);
        const std::string readme = linuxTarballReadme();
        tar.addFile(topDir + "README.install",
                    reinterpret_cast<const uint8_t *>(readme.data()),
                    readme.size(),
                    0644);
    }

    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    writeFileAtomic(params.outputPath, tarGz);
}

/// @brief Build a self-extracting Linux `.run` bundle from a staged install manifest.
/// @param params Linux staged manifest, package name, and output destination.
/// @throws std::runtime_error If validation, stub generation, payload assembly, or output fails.
void buildToolchainBundle(const LinuxToolchainBuildParams &params) {
    const auto &manifest = params.manifest;
    requireLinuxToolchainManifest(manifest, "Linux self-extracting bundle");
    const std::string packageName =
        params.packageName.empty() ? std::string("zanna") : normalizeDebName(params.packageName);
    if (manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    const std::string version = manifest.version;
    validateDebVersion(version, "toolchain package version");
    validateToolchainArchitecture(manifest.arch, "toolchain package architecture");

    TarWriter tar;
    tar.addDirectory("./", 0755);
    const std::string appRun = toolchainAppRunScript();
    tar.addFile("AppRun", reinterpret_cast<const uint8_t *>(appRun.data()), appRun.size(), 0755);
    for (const auto &file : manifest.files) {
        const std::string relPath = mapInstallPath(file, InstallPathPolicy::PortableArchive);
        validatePortableArchivePath(relPath, "Linux bundle payload path");
        if (file.symlink) {
            tar.addSymlink(relPath, file.symlinkTarget);
        } else {
            const auto data = readFile(file.stagedAbsolutePath.string());
            tar.addFile(relPath, data.data(), data.size(), permissionBitsFor(file));
        }
    }
    {
        std::vector<DataFile> generated;
        if (!manifest.fileAssociations.empty())
            addToolchainFileAssociationMetadata(generated, manifest, packageName, "zanna");
        addZannaStudioDesktopMetadata(generated, "zannastudio");
        addZannaToolchainHicolorIcons(generated, "zanna");
        for (const auto &df : generated) {
            const std::string portablePath = sanitizePackageRelativePath(
                df.installPath.rfind("usr/", 0) == 0 ? df.installPath.substr(4) : df.installPath,
                "Linux bundle generated metadata path");
            tar.addFile(portablePath, df.data.data(), df.data.size(), df.mode);
        }
    }
    addToolchainBundleMetadata(tar, packageName);

    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    LinuxRuntimeStubParams stub;
    stub.cacheName =
        packageName + "-" + portableArchiveVersionComponent(version) + "-linux-" + manifest.arch;
    stub.entryPath = "AppRun";
    const auto bundle = buildLinuxAppImage(stub, tarGz);
    writeFileAtomic(params.outputPath, bundle);
    std::error_code ec;
    fs::permissions(params.outputPath,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);
    if (ec)
        throw std::runtime_error("cannot mark Linux bundle executable: " + ec.message());
}

/// @brief Build an RPM toolchain package from a staged install manifest using rpmbuild.
/// Creates a temporary rpmbuild workspace, generates a source tarball and .spec file,
/// invokes rpmbuild, then copies the resulting .rpm to `params.outputPath`.
/// Throws if rpmbuild is not on PATH or if the build fails.
/// @param params Linux staged manifest, package name, and output destination.
/// @throws std::runtime_error If validation fails, rpmbuild is unavailable/fails,
///         or the generated artifact cannot be located or copied.
void buildToolchainRpmPackage(const LinuxToolchainBuildParams &params) {
    const auto &manifest = params.manifest;
    requireLinuxToolchainManifest(manifest, "RPM toolchain package");
    const std::string packageName =
        params.packageName.empty() ? std::string("zanna") : normalizeDebName(params.packageName);
    if (manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    const std::string version = manifest.version;
    validateRpmVersion(version, "toolchain RPM version");
    validateToolchainArchitecture(manifest.arch, "toolchain package architecture");
    const std::string arch = rpmArchFor(manifest.arch);
    const auto dataFiles = collectToolchainLinuxFiles(manifest, packageName);
    validateDataFilePaths(dataFiles);
    if (!rpmbuildAvailable()) {
        throw std::runtime_error(
            "rpmbuild is required to generate RPM toolchain packages; install rpm-build "
            "or request linux-deb/tarball output");
    }

    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-rpm-" + version + "-" + arch);
    TempDirGuard cleanup(tmpRoot);
    fs::create_directories(tmpRoot / "BUILD");
    fs::create_directories(tmpRoot / "BUILDROOT");
    fs::create_directories(tmpRoot / "RPMS");
    fs::create_directories(tmpRoot / "SOURCES");
    fs::create_directories(tmpRoot / "SPECS");
    fs::create_directories(tmpRoot / "SRPMS");

    const std::string sourceTopDir = packageName + "-" + version + "/";
    TarWriter tar;
    tar.addDirectory(sourceTopDir, 0755);
    for (const auto &file : dataFiles) {
        std::string sourcePath = file.installPath;
        if (sourcePath.rfind("usr/", 0) == 0)
            sourcePath = sourcePath.substr(4);
        validateRpmSpecPath(file.installPath);
        if (file.symlink) {
            tar.addSymlink(sourceTopDir + sourcePath, file.symlinkTarget);
        } else if (!file.directory) {
            tar.addFile(sourceTopDir + sourcePath, file.data.data(), file.data.size(), file.mode);
        }
    }
    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    const fs::path sourceTar = tmpRoot / "SOURCES" / (packageName + "-" + version + ".tar.gz");
    writeFileAtomic(sourceTar, tarGz);

    std::ostringstream spec;
    spec << "Name: " << packageName << "\n";
    spec << "Version: " << version << "\n";
    spec << "Release: 1%{?dist}\n";
    spec << "Summary: Zanna compiler toolchain\n";
    spec << "Packager: " << toolchainMaintainer(manifest) << "\n";
    {
        std::string license = trimAsciiWhitespace(manifest.license);
        if (license.empty())
            license = "GPL-3.0-only";
        validateSingleLineField(license, "toolchain package license");
        spec << "License: " << license << "\n";
    }
    if (!manifest.homepage.empty()) {
        validatePackageUrl(manifest.homepage, "toolchain package homepage");
        spec << "URL: " << manifest.homepage << "\n";
    }
    spec << "BuildArch: " << arch << "\n";
    spec << "Source0: %{name}-%{version}.tar.gz\n";
    for (const auto &dep : toolchainRpmRequires(manifest))
        spec << "Requires: " << dep << "\n";
    for (const auto &dep : toolchainRpmRecommends())
        spec << "Recommends: " << dep << "\n";
    spec << "\n";
    spec << "%description\nZanna compiler toolchain\n\n";
    spec << "%prep\n%setup -q\n\n";
    spec << "%build\n:\n\n";
    spec << "%install\nrm -rf %{buildroot}\nmkdir -p %{buildroot}/usr\ncp -a . "
            "%{buildroot}/usr/\n\n";
    /// @brief Determine whether a manifest entry installs a manual page.
    /// @param entry Manifest entry inspected by the `std::any_of` predicate.
    /// @return `true` when `entry` is classified as a manual page.
    const bool hasManPages = std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.kind == ToolchainFileKind::ManPage;
        });
    const bool hasFileAssociations = !manifest.fileAssociations.empty();
    if (hasManPages || hasFileAssociations) {
        spec << "%post\n";
        if (hasManPages)
            spec << "if command -v mandb >/dev/null 2>&1; then mandb >/dev/null 2>&1 || true; fi\n";
        if (hasFileAssociations) {
            spec << "if command -v update-mime-database >/dev/null 2>&1; then update-mime-database "
                    "/usr/share/mime || true; fi\n";
            spec << "if command -v update-desktop-database >/dev/null 2>&1; then "
                    "update-desktop-database /usr/share/applications || true; fi\n\n";
        } else {
            spec << "\n";
        }
        if (hasFileAssociations) {
            spec << "%postun\n";
            if (hasManPages)
                spec << "if command -v mandb >/dev/null 2>&1; then mandb >/dev/null 2>&1 || true; "
                        "fi\n";
            spec << "if command -v update-mime-database >/dev/null 2>&1; then update-mime-database "
                    "/usr/share/mime || true; fi\n";
            spec << "if command -v update-desktop-database >/dev/null 2>&1; then "
                    "update-desktop-database /usr/share/applications || true; fi\n\n";
        } else if (hasManPages) {
            spec << "%postun\nif command -v mandb >/dev/null 2>&1; then mandb >/dev/null 2>&1 || "
                    "true; fi\n\n";
        }
    }
    spec << "%files\n";
    for (const auto &file : dataFiles) {
        validateRpmSpecPath(file.installPath);
        spec << rpmSpecOwnedFileLine(file.installPath) << "\n";
    }

    const fs::path specPath = tmpRoot / "SPECS" / (packageName + ".spec");
    {
        std::ofstream out(specPath);
        if (!out)
            throw std::runtime_error("cannot write rpm spec file: " + specPath.string());
        out << spec.str();
    }

    const RunResult rr = runRpmBuild(tmpRoot, specPath);
    if (rr.exit_code != 0) {
        throw std::runtime_error("rpmbuild failed while generating toolchain rpm:\n" + rr.out +
                                 rr.err);
    }

    const fs::path rpmPath = findGeneratedRpm(tmpRoot, packageName, version, arch);
    std::error_code copyEc;
    fs::copy_file(rpmPath, params.outputPath, fs::copy_options::overwrite_existing, copyEc);
    if (copyEc)
        throw std::runtime_error("cannot copy generated rpm to " + params.outputPath + ": " +
                                 copyEc.message());
}

/// @brief GPG-sign a built Debian (.deb) or RPM (.rpm) package in place.
/// @details rpmsign and dpkg-sig are the standard signing tools and are not present on
///          every host. run_process reports a missing binary via launch_failed, which is
///          surfaced here distinctly from a signing failure.
/// @param packagePath Existing package artifact to modify in place.
/// @param gpgKeyId GPG key identifier passed to the external signing tool.
/// @param isRpm Selects `rpmsign` when true and `dpkg-sig` otherwise.
/// @throws std::runtime_error If inputs are invalid, the tool cannot launch, or signing fails.
void signLinuxPackage(const std::string &packagePath, const std::string &gpgKeyId, bool isRpm) {
    if (gpgKeyId.empty())
        throw std::runtime_error("Linux package signing key must not be empty");
    validateSingleLineField(gpgKeyId, "Linux signing key");

    const char *tool = isRpm ? "rpmsign" : "dpkg-sig";
    std::vector<std::string> argv;
    if (isRpm)
        argv = {"rpmsign", "--addsign", "--define", "_gpg_name " + gpgKeyId, packagePath};
    else
        argv = {"dpkg-sig", "--sign", "builder", "-k", gpgKeyId, packagePath};

    const RunResult rr = run_process(argv);
    if (rr.launch_failed) {
        throw std::runtime_error(
            std::string(tool) + " is required to sign " + (isRpm ? "RPM" : "Debian") +
            " packages but was not found on PATH; install " + tool + " or omit --linux-sign-key");
    }
    if (rr.exit_code != 0) {
        throw std::runtime_error(std::string(tool) + " failed to sign " + packagePath +
                                 " (check that GPG key '" + gpgKeyId + "' is available):\n" +
                                 rr.out + rr.err);
    }

    RunResult verifyResult;
    std::string verifyTool;
    if (isRpm) {
        verifyTool = "rpmkeys";
        verifyResult = run_process({"rpmkeys", "--checksig", "--verbose", packagePath});
        if (verifyResult.launch_failed) {
            verifyTool = "rpm";
            verifyResult = run_process({"rpm", "--checksig", "--verbose", packagePath});
        }
    } else {
        verifyTool = "dpkg-sig";
        verifyResult = run_process({"dpkg-sig", "--verify", packagePath});
    }
    if (verifyResult.launch_failed) {
        throw std::runtime_error("signed Linux package could not be verified because " +
                                 verifyTool + " was not found on PATH");
    }
    std::string verificationText = verifyResult.out + verifyResult.err;
    /// @brief Convert one verification-output byte to uppercase.
    /// @param c Byte to fold.
    /// @return Uppercase representation of `c` in the active C locale.
    std::transform(verificationText.begin(),
                   verificationText.end(),
                   verificationText.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const bool signatureReported = isRpm ? verificationText.find("OK") != std::string::npos
                                         : verificationText.find("GOODSIG") != std::string::npos;
    if (verifyResult.exit_code != 0 || !signatureReported) {
        throw std::runtime_error(verifyTool + " did not confirm the signature on " + packagePath +
                                 ":\n" + verifyResult.out + verifyResult.err);
    }
}

} // namespace zanna::pkg
