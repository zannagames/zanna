//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/MacOSPackageBuilder.cpp
// Purpose: Assemble macOS .app ZIPs and native flat .pkg toolchain installers
//          with proper Unix permissions and package metadata.
//
// Key invariants:
//   - .app/Contents/MacOS/<name> has mode 0100755.
//   - All other regular files have 0100644.
//   - Directories have 040755.
//   - ICNS icon generated from source PNG with multiple resolutions.
//
// Ownership/Lifetime:
//   - Single-use builder, writes output ZIP file.
//
// Links: MacOSPackageBuilder.hpp, ZipWriter.hpp, PlistGenerator.hpp,
//        IconGenerator.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements macOS application ZIP/DMG and toolchain PKG/DMG packaging.
/// @details Stages bundle layouts, metadata, permissions, scripts, signatures,
///          native package archives, and optional Finder disk-image presentation.

#include "MacOSPackageBuilder.hpp"
#include "CpioWriter.hpp"
#include "IconGenerator.hpp"
#include "PkgGzip.hpp"
#include "PkgUtils.hpp"
#include "PlistGenerator.hpp"
#include "XarWriter.hpp"
#include "ZipWriter.hpp"
#include "common/RunProcess.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace zanna::pkg {
namespace {

constexpr std::string_view kMacOSToolchainInstallRoot = "/usr/local/zanna";
constexpr std::string_view kMacOSLocalBinDir = "/usr/local/bin";
constexpr std::string_view kMacOSLocalCMakeZannaDir = "/usr/local/lib/cmake/Zanna";
constexpr std::string_view kMacOSLocalManDir = "/usr/local/share/man";
constexpr std::string_view kMacOSToolchainAppPath = "/Applications/Zanna Toolchain.app";

/// @brief Create a unique temporary packaging directory under the system temp directory.
/// @details Uses exclusive directory creation with a randomized suffix and never removes
///          a pre-existing path before creation.
/// @param stem Prefix used for the generated directory name.
/// @return Path to the newly created directory.
fs::path uniqueTempPackagingDir(std::string_view stem) {
    return createUniqueTempDirectory(fs::temp_directory_path(), stem);
}

/// @brief Convert arbitrary project text into a macOS bundle-id component.
/// @details Bundle identifier components must start and end with an alphanumeric
///          character and may contain hyphens internally. This helper lowercases
///          ASCII letters, converts all non-alphanumeric runs to a single hyphen,
///          trims leading/trailing hyphens, and falls back to `app` when no valid
///          characters remain.
/// @param text Project or executable text used as the default bundle-id suffix.
/// @return A bundle-identifier-safe component.
std::string macOSBundleIdentifierComponent(std::string_view text) {
    std::string out;
    bool lastHyphen = true;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            lastHyphen = false;
        } else if (!lastHyphen) {
            out.push_back('-');
            lastHyphen = true;
        }
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (out.empty())
        return "app";
    return out;
}

/// @brief Build and validate the default macOS bundle identifier.
/// @details The package manifest may override the identifier. When it does not,
///          Zanna uses a stable `com.zanna.<component>` identifier derived from
///          the project name with macOS-safe component rules.
/// @param projectName Project name from the manifest.
/// @return Valid reverse-DNS bundle identifier.
std::string defaultMacOSBundleIdentifier(const std::string &projectName) {
    std::string bundleId = "com.zanna." + macOSBundleIdentifierComponent(projectName);
    validateMacOSBundleIdentifier(bundleId, "macOS bundle identifier");
    return bundleId;
}

/// @brief Map freedesktop-style package categories to Apple bundle categories.
/// @details The manifest uses the existing cross-platform `package-category`
///          field. This helper maps the broad freedesktop categories Zanna
///          already validates to the nearest LaunchServices category string.
/// @param category Manifest category string.
/// @return Empty string when no reasonable macOS category is declared.
std::string macOSApplicationCategory(const std::string &category) {
    if (category.find("Game") != std::string::npos)
        return "public.app-category.games";
    if (category.find("Development") != std::string::npos)
        return "public.app-category.developer-tools";
    if (category.find("Graphics") != std::string::npos)
        return "public.app-category.graphics-design";
    if (category.find("Audio") != std::string::npos || category.find("Video") != std::string::npos)
        return "public.app-category.music";
    if (category.find("Education") != std::string::npos)
        return "public.app-category.education";
    if (category.find("Office") != std::string::npos)
        return "public.app-category.productivity";
    if (category.find("Network") != std::string::npos)
        return "public.app-category.utilities";
    if (category.find("Utility") != std::string::npos ||
        category.find("System") != std::string::npos)
        return "public.app-category.utilities";
    return {};
}

/// @brief RAII guard that removes the directory tree at `path_` on destruction.
class TempDirGuard {
  public:
    /// @brief Take ownership of a temporary directory.
    /// @param path Directory tree removed at destruction.
    explicit TempDirGuard(fs::path path) : path_(std::move(path)) {}

    /// @brief Best-effort recursive removal of the owned directory.
    ~TempDirGuard() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

  private:
    fs::path path_;
};

/// @brief Validate that `name` is a legal macOS bundle display name.
/// Must be non-empty, single-line, free of path separators (`/`, `\`, `:`), and pass Windows
/// filename checks.
/// @param name Candidate user-visible bundle name.
/// @throws std::runtime_error If the name is empty, multiline, or filename-unsafe.
void validateBundleDisplayName(const std::string &name) {
    if (name.empty())
        throw std::runtime_error("macOS bundle display name must not be empty");
    validateSingleLineField(name, "macOS bundle display name");
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos)
        throw std::runtime_error("macOS bundle display name must not contain path separators: " +
                                 name);
    validateWindowsFileName(name, "macOS bundle display name");
}

/// @brief Write `data` to `path`, creating parent directories as needed, and apply `perms`.
/// @param path Destination filesystem path.
/// @param data Bytes to write.
/// @param perms Final Unix permissions.
/// @throws std::runtime_error If directory creation, writing, or permission changes fail.
void writeFileBytes(const fs::path &path, const std::vector<uint8_t> &data, fs::perms perms) {
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty() && (!fs::create_directories(parent, ec) && ec)) {
        throw std::runtime_error("cannot create macOS package directory: " + parent.string() +
                                 ": " + ec.message());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write macOS package file: " + path.string());
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out)
        throw std::runtime_error("failed while writing macOS package file: " + path.string());
    out.close();
    fs::permissions(path, perms, fs::perm_options::replace, ec);
    if (ec)
        throw std::runtime_error("cannot set macOS package file permissions: " + path.string() +
                                 ": " + ec.message());
}

/// @brief Write `text` to `path` as UTF-8 bytes with the given Unix permissions.
/// @param path Destination filesystem path.
/// @param text UTF-8 text to write.
/// @param perms Final Unix permissions.
/// @throws std::runtime_error If writing or permission changes fail.
void writeFileString(const fs::path &path, const std::string &text, fs::perms perms) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
    writeFileBytes(path, std::vector<uint8_t>(bytes, bytes + text.size()), perms);
}

/// @brief Preserve whether a source file is executable while normalizing package permissions.
/// @param path Source file whose mode is inspected.
/// @return Mode 0755 when any execute bit is set, otherwise 0644.
/// @throws std::runtime_error If the source permissions cannot be read.
fs::perms normalizedPackageFilePermissions(const fs::path &path) {
    std::error_code ec;
    const fs::perms source = fs::status(path, ec).permissions();
    if (ec)
        throw std::runtime_error("cannot inspect package file permissions for '" + path.string() +
                                 "': " + ec.message());
    const fs::perms executableBits =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    if ((source & executableBits) != fs::perms::none) {
        return fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
               fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
               fs::perms::others_exec;
    }
    return fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::others_read;
}

/// @brief Copy a package asset (file or directory tree) from `srcPath` into the .app `Resources`
/// dir. Executable source files get mode 0755 and other regular files get 0644.
/// @param srcPath Resolved source file or directory.
/// @param projectRoot Trusted root used for safe recursive traversal.
/// @param resourcesDir Destination `.app/Contents/Resources` directory.
/// @param targetDir Sanitized package-relative resource subdirectory.
/// @param sourceText Original manifest path used in diagnostics.
/// @throws std::runtime_error If validation, traversal, reading, or writing fails.
void copyPackageAssetToResources(const fs::path &srcPath,
                                 const fs::path &projectRoot,
                                 const fs::path &resourcesDir,
                                 const std::string &targetDir,
                                 const std::string &sourceText) {
    const fs::path targetRoot = resourcesDir / fs::path(targetDir);
    const std::string sourceRel = sanitizePackageRelativePath(sourceText, "asset source path");
    const fs::path sourceLeaf = fs::path(sourceRel).filename();
    std::error_code ec;
    if (!fs::exists(srcPath, ec)) {
        if (ec)
            throw std::runtime_error("cannot stat asset '" + sourceText + "': " + ec.message());
        throw std::runtime_error("asset not found: " + sourceText);
    }

    if (fs::is_directory(srcPath, ec)) {
        if (ec)
            throw std::runtime_error("cannot inspect asset '" + sourceText + "': " + ec.message());
        if (!targetDir.empty()) {
            fs::create_directories(targetRoot, ec);
            if (ec)
                throw std::runtime_error("cannot create asset resource directory '" +
                                         targetRoot.string() + "': " + ec.message());
        }
        /// @brief Copy one safely resolved asset-tree entry into the application resources.
        /// @param entry Directory entry with verified logical and physical paths.
        /// @throws std::runtime_error If an entry path, directory creation, or file copy fails.
        safeDirectoryIterateResolved(srcPath, projectRoot, [&](const SafeDirectoryEntry &entry) {
            const auto relPath = sanitizePackageRelativePath(
                entry.logicalPath.lexically_relative(srcPath).generic_string(), "asset path");
            const fs::path dst = targetRoot / fs::path(relPath);
            if (entry.directory) {
                std::error_code createEc;
                fs::create_directories(dst, createEc);
                if (createEc)
                    throw std::runtime_error("cannot create asset directory '" + dst.string() +
                                             "': " + createEc.message());
            } else if (entry.regularFile) {
                writeFileBytes(dst,
                               readFile(entry.resolvedPath.string()),
                               normalizedPackageFilePermissions(entry.resolvedPath));
            }
        });
    } else if (fs::is_regular_file(srcPath, ec)) {
        if (ec)
            throw std::runtime_error("cannot inspect asset '" + sourceText + "': " + ec.message());
        writeFileBytes(targetRoot / sourceLeaf,
                       readFile(srcPath.string()),
                       normalizedPackageFilePermissions(srcPath));
    } else {
        throw std::runtime_error("asset is not a regular file or directory: " + sourceText);
    }
}

/// @brief Run a command and throw `std::runtime_error` if it exits non-zero.
/// The error message includes `what` plus the captured stdout and stderr.
/// @param args Executable and argument vector.
/// @param what Human-readable operation name used in failures.
/// @throws std::runtime_error If the process exits unsuccessfully.
void runChecked(const std::vector<std::string> &args, const std::string &what) {
    RunResult rr = run_process(args);
    if (rr.exit_code != 0)
        throw std::runtime_error(what + " failed:\n" + rr.out + rr.err);
}

/// @brief Decode the XML entities that can appear in an hdiutil plist string value.
/// @param text XML text to decode in place.
/// @return Text with the five predefined XML entities replaced.
std::string decodeSimpleXmlEntities(std::string text) {
    const std::pair<std::string_view, std::string_view> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    for (const auto &[encoded, decoded] : entities) {
        size_t pos = 0;
        while ((pos = text.find(encoded, pos)) != std::string::npos) {
            text.replace(pos, encoded.size(), decoded);
            pos += decoded.size();
        }
    }
    return text;
}

/// @brief Attach a writable DMG where Finder can see it and return hdiutil's actual mount point.
/// @param dmgPath Disk image to attach.
/// @param what Human-readable operation name used in failures.
/// @return Accessible mount point parsed from hdiutil's plist output.
/// @throws std::runtime_error If attachment fails or no usable mount point is reported.
fs::path attachMacOSDmgForStyling(const fs::path &dmgPath, const std::string &what) {
    const RunResult result =
        run_process({"hdiutil", "attach", dmgPath.string(), "-noverify", "-noautoopen", "-plist"});
    if (result.exit_code != 0)
        throw std::runtime_error(what + " failed:\n" + result.out + result.err);

    constexpr std::string_view key = "<key>mount-point</key>";
    constexpr std::string_view open = "<string>";
    constexpr std::string_view close = "</string>";
    const size_t keyPos = result.out.find(key);
    const size_t openPos = keyPos == std::string::npos ? std::string::npos
                                                       : result.out.find(open, keyPos + key.size());
    const size_t valueStart =
        openPos == std::string::npos ? std::string::npos : openPos + open.size();
    const size_t closePos =
        valueStart == std::string::npos ? std::string::npos : result.out.find(close, valueStart);
    if (valueStart == std::string::npos || closePos == std::string::npos) {
        throw std::runtime_error(what + " succeeded but hdiutil did not report a mounted volume");
    }
    const fs::path mountPoint =
        decodeSimpleXmlEntities(result.out.substr(valueStart, closePos - valueStart));
    if (!fs::is_directory(mountPoint))
        throw std::runtime_error(what +
                                 " reported an inaccessible mount point: " + mountPoint.string());
    return mountPoint;
}

/// @brief Quote arbitrary single-line text as an AppleScript string literal.
/// @param text Text to quote.
/// @param fieldName Human-readable field name used for validation errors.
/// @return Double-quoted AppleScript literal with slash and quote escaping.
/// @throws std::runtime_error If `text` contains a line break.
std::string appleScriptStringLiteral(std::string_view text, const char *fieldName) {
    validateSingleLineField(std::string(text), fieldName);
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('"');
    for (const char ch : text) {
        if (ch == '\\' || ch == '"')
            quoted.push_back('\\');
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

/// @brief Validate a leaf filename placed at the root of a disk image.
/// @param name Candidate leaf name.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Validated name.
/// @throws std::runtime_error If the name is empty, special, multiline, or contains separators.
std::string validateDmgItemName(std::string name, const char *fieldName) {
    validateSingleLineField(name, fieldName);
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find(':') != std::string::npos) {
        throw std::runtime_error(std::string(fieldName) +
                                 " must be a non-empty leaf name free of '/' and ':'");
    }
    return name;
}

/// @brief Run optional Finder styling and surface failures without invalidating the image.
/// @param args Executable and argument vector.
/// @param what Human-readable styling operation printed with warnings.
void runBestEffortMacOSStyling(const std::vector<std::string> &args, const std::string &what) {
    const RunResult result = run_process(args);
    if (result.exit_code != 0) {
        std::cerr << "warning: " << what << " was skipped (exit " << result.exit_code << ")";
        if (!result.err.empty())
            std::cerr << ": " << result.err;
        else
            std::cerr << "\n";
        if (!result.err.empty() && result.err.back() != '\n')
            std::cerr << "\n";
    }
}

/// @brief Mount a completed DMG read-only and verify its expected root items.
/// @param dmgPath Completed disk image to inspect.
/// @param regularFiles Expected non-empty regular-file leaf names.
/// @param directories Expected directory leaf names.
/// @param symlinks Expected symbolic-link leaf names.
/// @param what Human-readable operation name used in diagnostics.
/// @throws std::runtime_error If mounting, content validation, or detachment fails.
void verifyMountedMacOSDmgContents(const fs::path &dmgPath,
                                   const std::vector<std::string> &regularFiles,
                                   const std::vector<std::string> &directories,
                                   const std::vector<std::string> &symlinks,
                                   const std::string &what) {
    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-dmg-verify");
    TempDirGuard cleanup(tmpRoot);
    const fs::path mountPoint = tmpRoot / "mnt";
    fs::create_directories(mountPoint);
    runChecked({"hdiutil",
                "attach",
                "-readonly",
                dmgPath.string(),
                "-mountpoint",
                mountPoint.string(),
                "-nobrowse",
                "-noautoopen"},
               what + " read-only attach");

    std::string contentError;
    std::error_code ec;
    for (const std::string &name : regularFiles) {
        const fs::path path = mountPoint / validateDmgItemName(name, "DMG expected filename");
        if (!fs::is_regular_file(path, ec) || ec) {
            contentError = "missing expected regular file '" + name + "'";
            break;
        }
        if (fs::file_size(path, ec) == 0 || ec) {
            contentError = "expected regular file is empty: '" + name + "'";
            break;
        }
    }
    if (contentError.empty()) {
        for (const std::string &name : directories) {
            const fs::path path = mountPoint / validateDmgItemName(name, "DMG expected directory");
            if (!fs::is_directory(path, ec) || ec) {
                contentError = "missing expected directory '" + name + "'";
                break;
            }
        }
    }
    if (contentError.empty()) {
        for (const std::string &name : symlinks) {
            const fs::path path = mountPoint / validateDmgItemName(name, "DMG expected symlink");
            if (!fs::is_symlink(fs::symlink_status(path, ec)) || ec) {
                contentError = "missing expected symlink '" + name + "'";
                break;
            }
        }
    }

    if (run_process({"hdiutil", "detach", mountPoint.string()}).exit_code != 0) {
        runChecked({"hdiutil", "detach", "-force", mountPoint.string()},
                   what + " verification detach");
    }
    if (!contentError.empty())
        throw std::runtime_error(what + " mounted-content verification failed: " + contentError);
}

/// @brief Return true when a regular file begins with a supported Mach-O magic value.
/// @param path Candidate regular file.
/// @return Whether its first four bytes match a thin or fat Mach-O magic.
bool isMachOFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::array<uint8_t, 4> magic{};
    if (!in.read(reinterpret_cast<char *>(magic.data()),
                 static_cast<std::streamsize>(magic.size()))) {
        return false;
    }
    static constexpr std::array<std::array<uint8_t, 4>, 8> kMachOMagics{{
        {{0xCE, 0xFA, 0xED, 0xFE}},
        {{0xFE, 0xED, 0xFA, 0xCE}},
        {{0xCF, 0xFA, 0xED, 0xFE}},
        {{0xFE, 0xED, 0xFA, 0xCF}},
        {{0xCA, 0xFE, 0xBA, 0xBE}},
        {{0xBE, 0xBA, 0xFE, 0xCA}},
        {{0xCA, 0xFE, 0xBA, 0xBF}},
        {{0xBF, 0xBA, 0xFE, 0xCA}},
    }};
    return std::find(kMachOMagics.begin(), kMachOMagics.end(), magic) != kMachOMagics.end();
}

/// @brief Developer-ID-sign and verify every Mach-O payload file, then containing app bundles.
/// @param payloadRoot Staged package tree to scan recursively.
/// @param identity Developer ID Application identity; empty disables signing.
/// @throws std::runtime_error If validation, traversal, signing, or verification fails.
void signMacOSToolchainPayload(const fs::path &payloadRoot, const std::string &identity) {
    if (identity.empty())
        return;
    validateSingleLineField(identity, "macOS application signing identity");

    std::vector<fs::path> machOFiles;
    std::vector<fs::path> appBundles;
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(payloadRoot, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end;
         it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot inspect macOS payload for signing: " + ec.message());
        const fs::path path = it->path();
        if (it->is_directory(ec) && path.extension() == ".app")
            appBundles.push_back(path);
        else if (it->is_regular_file(ec) && isMachOFile(path))
            machOFiles.push_back(path);
        if (ec)
            throw std::runtime_error("cannot stat macOS payload signing candidate: " +
                                     ec.message());
    }
    std::sort(machOFiles.begin(), machOFiles.end());
    /// @brief Order application bundles from deepest to shallowest for nested signing.
    /// @param lhs Left-hand bundle path.
    /// @param rhs Right-hand bundle path.
    /// @return `true` when `lhs` has a longer native path than `rhs`.
    std::sort(appBundles.begin(), appBundles.end(), [](const fs::path &lhs, const fs::path &rhs) {
        return lhs.native().size() > rhs.native().size();
    });

    for (const fs::path &path : machOFiles) {
        runChecked({"codesign",
                    "--force",
                    "--sign",
                    identity,
                    "--options",
                    "runtime",
                    "--timestamp",
                    path.string()},
                   "macOS nested executable signing");
        runChecked({"codesign", "--verify", "--strict", "--verbose=2", path.string()},
                   "macOS nested executable signature verification");
    }
    for (const fs::path &app : appBundles) {
        runChecked({"codesign",
                    "--force",
                    "--sign",
                    identity,
                    "--options",
                    "runtime",
                    "--timestamp",
                    app.string()},
                   "macOS nested app signing");
        runChecked({"codesign", "--verify", "--deep", "--strict", "--verbose=2", app.string()},
                   "macOS nested app signature verification");
    }
}

/// @brief Resolve the version string for a macOS toolchain package.
/// Returns the validated override if non-empty; otherwise validates and returns the manifest
/// version.
/// @param manifestVersion Version from the staged manifest.
/// @param packageVersionOverride Optional installer-specific version override.
/// @return Validated dotted-numeric package version.
std::string resolveMacOSToolchainPackageVersion(const std::string &manifestVersion,
                                                const std::string &packageVersionOverride) {
    if (!packageVersionOverride.empty()) {
        validateDottedNumericVersion(packageVersionOverride,
                                     "macOS toolchain package version override");
        return packageVersionOverride;
    }
    validateDottedNumericVersion(manifestVersion, "macOS toolchain package version");
    return manifestVersion;
}

/// @brief Return a shell-safe single-quoted string for generated package scripts.
/// @param value Text to quote as one shell word.
/// @return POSIX single-quoted literal.
std::string shQuote(const std::string &value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

/// @brief Escape the five XML metacharacters for embedding in plist/Distribution XML.
/// @param text Raw XML text-node or attribute value.
/// @return XML-safe text.
std::string xmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
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
                out.push_back(ch);
                break;
        }
    }
    return out;
}

/// @brief Convert filesystem permissions to low POSIX permission bits.
/// @param path Filesystem entry whose mode is inspected.
/// @param executableFallback Select mode 0755 rather than 0644 if status is unavailable or empty.
/// @return Low nine permission bits.
uint32_t modeBitsForPath(const fs::path &path, bool executableFallback) {
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec)
        return executableFallback ? 0755u : 0644u;
    uint32_t mode = 0;
    const auto perms = status.permissions();
    using p = fs::perms;
    if ((perms & p::owner_read) != p::none)
        mode |= 0400u;
    if ((perms & p::owner_write) != p::none)
        mode |= 0200u;
    if ((perms & p::owner_exec) != p::none)
        mode |= 0100u;
    if ((perms & p::group_read) != p::none)
        mode |= 0040u;
    if ((perms & p::group_write) != p::none)
        mode |= 0020u;
    if ((perms & p::group_exec) != p::none)
        mode |= 0010u;
    if ((perms & p::others_read) != p::none)
        mode |= 0004u;
    if ((perms & p::others_write) != p::none)
        mode |= 0002u;
    if ((perms & p::others_exec) != p::none)
        mode |= 0001u;
    return mode == 0 ? (executableFallback ? 0755u : 0644u) : mode;
}

/// @brief Return a sorted list of paths under root, including root itself.
/// @param root Existing tree root.
/// @return Lexicographically sorted entry paths, starting with `root`.
/// @throws std::runtime_error If recursive traversal fails.
std::vector<fs::path> sortedTreeEntries(const fs::path &root) {
    std::vector<fs::path> entries;
    entries.push_back(root);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot traverse macOS package payload: " + ec.message());
        entries.push_back(it->path());
    }
    if (ec)
        throw std::runtime_error("cannot traverse macOS package payload: " + ec.message());
    /// @brief Order staged tree entries by their portable path spelling.
    /// @param a Left-hand filesystem path.
    /// @param b Right-hand filesystem path.
    /// @return `true` when `a` sorts before `b`.
    std::sort(entries.begin(), entries.end(), [](const fs::path &a, const fs::path &b) {
        return a.generic_string() < b.generic_string();
    });
    return entries;
}

/// @brief Add a staged filesystem tree to a portable ASCII CPIO archive with root-owned metadata.
/// @param cpio Archive writer to populate.
/// @param root Staging root represented as the archive's `.` entry.
/// @throws std::runtime_error If traversal, reading, path mapping, or archive insertion fails.
void addFilesystemTreeToCpio(CpioWriter &cpio, const fs::path &root) {
    std::set<std::string> emittedPaths;
    for (const fs::path &entryPath : sortedTreeEntries(root)) {
        std::error_code ec;
        const fs::path relPath = entryPath.lexically_relative(root);
        if (relPath.empty()) {
            throw std::runtime_error("cannot compute macOS package payload path for: " +
                                     entryPath.string());
        }
        auto relIt = relPath.begin();
        if (relIt != relPath.end() && *relIt == fs::path("..")) {
            throw std::runtime_error("macOS package payload entry escapes staging root: " +
                                     entryPath.string());
        }
        const std::string archivePath =
            relPath.empty() || relPath == fs::path(".") ? "." : relPath.generic_string();
        if (!emittedPaths.insert(archivePath).second)
            continue;
        const auto symlinkStatus = fs::symlink_status(entryPath, ec);
        if (ec)
            throw std::runtime_error("cannot stat macOS package payload entry: " + ec.message());
        if (fs::is_symlink(symlinkStatus)) {
            const fs::path target = fs::read_symlink(entryPath, ec);
            if (ec)
                throw std::runtime_error("cannot read macOS package symlink: " + ec.message());
            try {
                cpio.addSymlink(archivePath, target.generic_string());
            } catch (const std::exception &ex) {
                throw std::runtime_error("cannot add macOS CPIO symlink '" + entryPath.string() +
                                         "' as '" + archivePath + "': " + ex.what());
            }
        } else if (fs::is_directory(symlinkStatus)) {
            try {
                cpio.addDirectory(archivePath, modeBitsForPath(entryPath, true));
            } catch (const std::exception &ex) {
                throw std::runtime_error("cannot add macOS CPIO directory '" + entryPath.string() +
                                         "' as '" + archivePath + "': " + ex.what());
            }
        } else if (fs::is_regular_file(symlinkStatus)) {
            const auto data = readFile(entryPath.string());
            try {
                cpio.addFileVec(archivePath, data, modeBitsForPath(entryPath, false));
            } catch (const std::exception &ex) {
                throw std::runtime_error("cannot add macOS CPIO file '" + entryPath.string() +
                                         "' as '" + archivePath + "': " + ex.what());
            }
        }
    }
}

/// @brief Create or replace a symlink, making parent directories first.
/// @param linkPath Link to create.
/// @param target Target recorded in the symbolic link.
/// @throws std::runtime_error If link creation fails.
void createPackageSymlink(const fs::path &linkPath, const fs::path &target) {
    fs::create_directories(linkPath.parent_path());
    std::error_code ec;
    fs::remove(linkPath, ec);
    ec.clear();
    fs::create_symlink(target, linkPath, ec);
    if (ec) {
        throw std::runtime_error("cannot create package symlink '" + linkPath.string() + "' -> '" +
                                 target.generic_string() + "': " + ec.message());
    }
}

/// @brief Write text file and mark it executable.
/// @param path Destination script path.
/// @param text Script contents.
/// @throws std::runtime_error If writing or permission changes fail.
void writeExecutableScript(const fs::path &path, const std::string &text) {
    writeFileString(path,
                    text,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec);
}

/// @brief Collect the sorted, unique leaf names of the toolchain's `bin/` tools.
/// @details Includes manifest entries marked as Binary or located under `bin/`;
///          used to create CLI symlinks and reference tools in install scripts.
/// @param manifest Staged toolchain manifest.
/// @return Sorted unique executable leaf names.
std::vector<std::string> macOSToolNames(const ToolchainInstallManifest &manifest) {
    std::vector<std::string> names;
    for (const auto &file : manifest.files) {
        const std::string rel =
            sanitizePackageRelativePath(file.stagedRelativePath, "macOS tool path");
        std::string extension = fs::path(rel).extension().generic_string();
        /// @brief Convert one extension byte to lowercase for case-insensitive filtering.
        /// @param ch Byte to fold.
        /// @return Lowercase representation of `ch` in the active C locale.
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension == ".buildinfo")
            continue;
        if (file.kind != ToolchainFileKind::Binary && rel.rfind("bin/", 0) != 0)
            continue;
        names.push_back(fs::path(rel).filename().generic_string());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

/// @brief Collect the sorted, unique man-page paths (relative to `share/man/`).
/// @details Used to create man-page symlinks under the system man hierarchy.
/// @param manifest Staged toolchain manifest.
/// @return Sorted unique paths below `share/man/`.
std::vector<std::string> macOSManPagePaths(const ToolchainInstallManifest &manifest) {
    static constexpr std::string_view kManPrefix = "share/man/";
    std::vector<std::string> paths;
    for (const auto &file : manifest.files) {
        const std::string rel =
            sanitizePackageRelativePath(file.stagedRelativePath, "macOS manpage path");
        if (file.kind != ToolchainFileKind::ManPage && rel.rfind(kManPrefix, 0) != 0)
            continue;
        if (rel.rfind(kManPrefix, 0) != 0)
            continue;
        paths.push_back(rel.substr(kManPrefix.size()));
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

/// @brief Append the sorted, de-duplicated leaf filenames directly under @p dir.
/// @details Non-recursive; includes regular files and symlinks only. Permission
///          errors are treated as package-build failures so verification expectations
///          cannot silently omit files.
/// @param names Existing name list to extend and normalize.
/// @param dir Directory whose immediate entries are inspected.
/// @throws std::runtime_error If inspection or iteration fails.
void appendLeafNamesFromDirectory(std::vector<std::string> &names, const fs::path &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if (ec)
            throw std::runtime_error("cannot inspect directory '" + dir.string() +
                                     "': " + ec.message());
        return;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot iterate directory '" + dir.string() +
                                     "': " + ec.message());
        const auto status = it->symlink_status(ec);
        if (ec)
            throw std::runtime_error("cannot stat directory entry '" + it->path().string() +
                                     "': " + ec.message());
        if (!fs::is_regular_file(status) && !fs::is_symlink(status))
            continue;
        names.push_back(it->path().filename().generic_string());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
}

/// @brief Append every regular file/symlink under @p dir as a path relative to @p dir.
/// @details Recursive companion to appendLeafNamesFromDirectory; permission
///          errors are treated as package-build failures so post-build verification
///          cannot silently accept an incomplete expected payload set.
/// @param paths Existing relative-path list to extend and normalize.
/// @param dir Root directory to traverse.
/// @throws std::runtime_error If inspection, traversal, or path validation fails.
void appendRelativeFilePathsFromDirectory(std::vector<std::string> &paths, const fs::path &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if (ec)
            throw std::runtime_error("cannot inspect directory '" + dir.string() +
                                     "': " + ec.message());
        return;
    }
    for (fs::recursive_directory_iterator it(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot iterate directory '" + dir.string() +
                                     "': " + ec.message());
        const auto status = it->symlink_status(ec);
        if (ec)
            throw std::runtime_error("cannot stat directory entry '" + it->path().string() +
                                     "': " + ec.message());
        if (!fs::is_regular_file(status) && !fs::is_symlink(status))
            continue;
        const fs::path rel = it->path().lexically_relative(dir);
        paths.push_back(sanitizePackageRelativePath(rel.generic_string(), "macOS manpage path"));
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

/// @brief Compute the relative symlink target from a system man path back into
///        the Zanna install tree's `share/man`.
/// @details Builds the right number of `../` hops (accounting for the
///          `share/man/` prefix plus any subdirectories) followed by
///          `zanna/share/man/<manRelPath>`, so the installed man page is a stable
///          relative link regardless of install root.
/// @param manRelPath Validated path relative to `share/man/`.
/// @return Relative link target from the corresponding system man directory.
fs::path macOSManSymlinkTarget(const std::string &manRelPath) {
    const fs::path relPath = fs::path(manRelPath);
    const fs::path parentPath = relPath.parent_path();
    size_t upLevels = 2; // share/man
    for (auto it = parentPath.begin(); it != parentPath.end(); ++it)
        ++upLevels;
    fs::path target;
    for (size_t i = 0; i < upLevels; ++i)
        target /= "..";
    target /= "zanna";
    target /= "share";
    target /= "man";
    target /= relPath;
    return target;
}

/// @brief Return the association's file extension with any leading dot(s) removed.
/// @param assoc File association to inspect.
/// @return Extension without leading dots.
std::string fileAssociationExtensionWithoutDot(const FileAssoc &assoc) {
    std::string ext = assoc.extension;
    while (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return ext;
}

/// @brief Map a file association to its Uniform Type Identifier (UTI).
/// @details Uses well-known UTIs for the built-in zia/bas/il types and a
///          generic "org.zanna.<ext>" for anything else.
/// @param assoc File association to map.
/// @return Validated exported Uniform Type Identifier.
std::string macOSFileAssociationUTI(const FileAssoc &assoc) {
    std::string ext = fileAssociationExtensionWithoutDot(assoc);
    for (char &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "zia")
        return "org.zanna.zia-source";
    if (ext == "bas")
        return "org.zanna.basic-source";
    if (ext == "il")
        return "org.zanna.il-module";
    for (char &c : ext) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-')
            c = '-';
    }
    const std::string uti = "org.zanna." + ext;
    validateMacOSBundleIdentifier(uti, "macOS file association UTI");
    return uti;
}

/// @brief Locate the staged `libexec/zanna/zanna-file-handler` entry, if present.
/// @param manifest Manifest to search.
/// @return Pointer to the manifest entry, or nullptr when no handler is staged.
const ToolchainFileEntry *findMacOSFileHandler(const ToolchainInstallManifest &manifest) {
    /// @brief Identify the staged macOS file-handler executable.
    /// @param file Manifest entry to inspect.
    /// @return `true` when `file` is the non-symlink file-handler payload entry.
    /// @throws std::runtime_error If the staged path is invalid.
    auto it = std::find_if(manifest.files.begin(), manifest.files.end(), [](const auto &file) {
        return !file.symlink &&
               sanitizePackageRelativePath(file.stagedRelativePath, "macOS file handler path") ==
                   "libexec/zanna/zanna-file-handler";
    });
    return it == manifest.files.end() ? nullptr : &*it;
}

/// @brief Build the Info.plist for the file-handler helper .app bundle.
/// @details Declares the CFBundleDocumentTypes / UTI exports that let macOS route
///          .zia/.bas/.il file opens to the bundled handler executable.
/// @param params Toolchain build parameters (identifier, file associations, etc.).
/// @param pkgVersion Version string recorded in the plist.
/// @return The Info.plist XML text.
std::string generateMacOSFileHandlerInfoPlist(const MacOSToolchainBuildParams &params,
                                              const std::string &pkgVersion) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    xml << "<plist version=\"1.0\">\n";
    xml << "<dict>\n";
    xml << "  <key>CFBundleDevelopmentRegion</key><string>en</string>\n";
    xml << "  <key>CFBundleDisplayName</key><string>Zanna Toolchain</string>\n";
    xml << "  <key>CFBundleExecutable</key><string>zanna-file-handler</string>\n";
    xml << "  <key>CFBundleIconFile</key><string>Zanna.icns</string>\n";
    xml << "  <key>CFBundleIdentifier</key><string>"
        << xmlEscape(params.identifier + ".filehandler") << "</string>\n";
    xml << "  <key>CFBundleName</key><string>Zanna Toolchain</string>\n";
    xml << "  <key>CFBundlePackageType</key><string>APPL</string>\n";
    xml << "  <key>CFBundleShortVersionString</key><string>" << xmlEscape(pkgVersion)
        << "</string>\n";
    xml << "  <key>CFBundleVersion</key><string>" << xmlEscape(pkgVersion) << "</string>\n";
    const std::string minimumVersion = params.minimumMacOSVersion.empty()
                                           ? (params.manifest.arch == "x64" ? "10.15" : "11.0")
                                           : params.minimumMacOSVersion;
    xml << "  <key>LSMinimumSystemVersion</key><string>" << xmlEscape(minimumVersion)
        << "</string>\n";
    xml << "  <key>LSUIElement</key><true/>\n";
    xml << "  <key>CFBundleDocumentTypes</key>\n";
    xml << "  <array>\n";
    for (const auto &assoc : params.manifest.fileAssociations) {
        const std::string ext = fileAssociationExtensionWithoutDot(assoc);
        const std::string uti = macOSFileAssociationUTI(assoc);
        const bool sourceType = ext == "zia" || ext == "bas";
        xml << "    <dict>\n";
        xml << "      <key>CFBundleTypeName</key><string>" << xmlEscape(assoc.description)
            << "</string>\n";
        xml << "      <key>CFBundleTypeRole</key><string>" << (sourceType ? "Editor" : "Viewer")
            << "</string>\n";
        xml << "      <key>LSHandlerRank</key><string>Alternate</string>\n";
        xml << "      <key>LSItemContentTypes</key><array><string>" << xmlEscape(uti)
            << "</string></array>\n";
        xml << "    </dict>\n";
    }
    xml << "  </array>\n";
    xml << "  <key>UTExportedTypeDeclarations</key>\n";
    xml << "  <array>\n";
    for (const auto &assoc : params.manifest.fileAssociations) {
        const std::string ext = fileAssociationExtensionWithoutDot(assoc);
        const std::string uti = macOSFileAssociationUTI(assoc);
        const bool sourceType = ext == "zia" || ext == "bas";
        xml << "    <dict>\n";
        xml << "      <key>UTTypeIdentifier</key><string>" << xmlEscape(uti) << "</string>\n";
        xml << "      <key>UTTypeDescription</key><string>" << xmlEscape(assoc.description)
            << "</string>\n";
        xml << "      <key>UTTypeConformsTo</key><array><string>"
            << (sourceType ? "public.source-code" : "public.data") << "</string></array>\n";
        xml << "      <key>UTTypeTagSpecification</key>\n";
        xml << "      <dict>\n";
        xml << "        <key>public.filename-extension</key><array><string>" << xmlEscape(ext)
            << "</string></array>\n";
        if (!assoc.mimeType.empty()) {
            xml << "        <key>public.mime-type</key><array><string>" << xmlEscape(assoc.mimeType)
                << "</string></array>\n";
        }
        xml << "      </dict>\n";
        xml << "    </dict>\n";
    }
    xml << "  </array>\n";
    xml << "</dict>\n";
    xml << "</plist>\n";
    return xml.str();
}

/// @brief Render the newline-separated installed-file manifest embedded in the package.
/// @details Lists every staged file's install-relative path so the uninstall
///          script knows exactly what to remove.
/// @param manifest Staged toolchain manifest whose paths are recorded.
/// @return Sorted, unique install-relative paths separated by newlines.
std::string macOSInstallManifestText(const ToolchainInstallManifest &manifest) {
    std::vector<std::string> paths;
    paths.reserve(manifest.files.size() + 2);
    for (const auto &file : manifest.files)
        paths.push_back(
            sanitizePackageRelativePath(file.stagedRelativePath, "macOS manifest path"));
    paths.push_back("share/zanna/install_manifest.txt");
    paths.push_back("share/zanna/uninstall.sh");
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::ostringstream out;
    for (const auto &path : paths)
        out << path << "\n";
    return out.str();
}

/// @brief Generate the `uninstall.sh` shell script shipped with the toolchain pkg.
/// @details Removes the installed CMake config files and the files recorded in the
///          install manifest for the given package identifier.
/// @param packageIdentifier Installer receipt identifier passed to `pkgutil --forget`.
/// @return POSIX shell script text.
std::string generateMacOSUninstallScript(const std::string &packageIdentifier) {
    std::ostringstream sh;
    sh << "#!/bin/sh\n";
    sh << "set -eu\n";
    sh << "ROOT=" << shQuote(std::string(kMacOSToolchainInstallRoot)) << "\n";
    sh << "MANIFEST=\"$ROOT/share/zanna/install_manifest.txt\"\n";
    sh << "APP=" << shQuote(std::string(kMacOSToolchainAppPath)) << "\n";
    sh << "LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/"
          "LaunchServices.framework/Support/lsregister\n";
    sh << "if [ \"$(id -u)\" != \"0\" ]; then echo \"Run with sudo to remove Zanna Toolchain\" "
          ">&2; exit 1; fi\n";
    sh << "if [ -x \"$LSREGISTER\" ] && [ -d \"$APP\" ]; then \"$LSREGISTER\" -u \"$APP\" "
          ">/dev/null 2>&1 || true; fi\n";
    sh << "if [ -f \"$MANIFEST\" ]; then\n";
    sh << "  while IFS= read -r rel || [ -n \"$rel\" ]; do\n";
    sh << "    [ -n \"$rel\" ] || continue\n";
    sh << "    case \"$rel\" in /*|..|../*|*/../*|*/..) echo \"Unsafe manifest path: $rel\" >&2; "
          "exit 2 ;; esac\n";
    sh << "    case \"$rel\" in\n";
    sh << "      bin/*) link=" << shQuote(std::string(kMacOSLocalBinDir))
       << "/${rel#bin/}; if [ -L \"$link\" ]; then "
          "target=$(readlink \"$link\" || true); case \"$target\" in "
          "../zanna/bin/*|";
    sh << std::string(kMacOSToolchainInstallRoot) << "/bin/*) rm -f \"$link\" ;; esac; fi ;;\n";
    sh << "      share/man/*) link=" << shQuote(std::string(kMacOSLocalManDir))
       << "/${rel#share/man/}; if [ -L \"$link\" "
          "]; then target=$(readlink \"$link\" || true); case \"$target\" in "
          "*../zanna/share/man/*|";
    sh << std::string(kMacOSToolchainInstallRoot)
       << "/share/man/*) rm -f \"$link\" ;; esac; fi ;;\n";
    sh << "    esac\n";
    sh << "    rm -f \"$ROOT/$rel\"\n";
    sh << "  done < \"$MANIFEST\"\n";
    sh << "fi\n";
    sh << "rm -rf \"$APP\"\n";
    sh << "rm -f " << shQuote(std::string(kMacOSLocalCMakeZannaDir) + "/ZannaConfig.cmake") << " "
       << shQuote(std::string(kMacOSLocalCMakeZannaDir) + "/ZannaConfigVersion.cmake") << "\n";
    sh << "if [ -d \"$ROOT\" ]; then find \"$ROOT\" -depth -type d -empty -delete 2>/dev/null || "
          "true; fi\n";
    sh << "for dir in " << shQuote(std::string(kMacOSLocalCMakeZannaDir)) << " "
       << shQuote(std::string(kMacOSLocalManDir) + "/man1") << " "
       << shQuote(std::string(kMacOSLocalManDir) + "/man7") << " "
       << shQuote(std::string(kMacOSToolchainInstallRoot))
       << "; do rmdir \"$dir\" 2>/dev/null || true; done\n";
    sh << "pkgutil --forget " << shQuote(packageIdentifier) << " >/dev/null 2>&1 || true\n";
    sh << "if command -v mandb >/dev/null 2>&1; then mandb -q "
       << shQuote(std::string(kMacOSLocalManDir)) << " >/dev/null 2>&1 || true; fi\n";
    sh << "if command -v makewhatis >/dev/null 2>&1; then makewhatis "
       << shQuote(std::string(kMacOSLocalManDir)) << " >/dev/null 2>&1 || true; fi\n";
    sh << "exit 0\n";
    return sh.str();
}

/// @brief Generate the installer `preinstall` script.
/// @details Removes any prior CLI symlinks for @p toolNames before the payload is
///          laid down, so a reinstall/upgrade starts from a clean state.
/// @param toolNames Installed executable leaf names whose owned links may be replaced.
/// @return POSIX preinstall script text.
std::string generateMacOSPreinstallScript(const std::vector<std::string> &toolNames) {
    std::ostringstream sh;
    sh << "#!/bin/sh\n";
    sh << "set -eu\n";
    sh << "ROOT=" << shQuote(std::string(kMacOSToolchainInstallRoot)) << "\n";
    sh << "OLD=\"$ROOT/share/zanna/install_manifest.txt\"\n";
    sh << "SCRIPT_DIR=$(cd \"$(dirname \"$0\")\" && pwd)\n";
    sh << "NEW=\"$SCRIPT_DIR/install_manifest.txt\"\n";
    sh << "if [ -f \"$OLD\" ] && [ -f \"$NEW\" ]; then\n";
    sh << "  while IFS= read -r rel; do\n";
    sh << "    [ -n \"$rel\" ] || continue\n";
    sh << "    case \"$rel\" in /*|..|../*|*/../*|*/..) echo \"Unsafe old manifest path: $rel\" "
          ">&2; exit 2 ;; esac\n";
    sh << "    if ! grep -F -x -- \"$rel\" \"$NEW\" >/dev/null 2>&1; then\n";
    sh << "      case \"$rel\" in\n";
    sh << "        bin/*)\n";
    sh << "          link=" << shQuote(std::string(kMacOSLocalBinDir)) << "/${rel#bin/}\n";
    sh << "          if [ -L \"$link\" ]; then\n";
    sh << "            target=$(readlink \"$link\" || true)\n";
    sh << "            case \"$target\" in ../zanna/bin/*|"
       << std::string(kMacOSToolchainInstallRoot) << "/bin/*) rm -f \"$link\" ;; esac\n";
    sh << "          fi\n";
    sh << "          ;;\n";
    sh << "        share/man/*)\n";
    sh << "          link=" << shQuote(std::string(kMacOSLocalManDir)) << "/${rel#share/man/}\n";
    sh << "          if [ -L \"$link\" ]; then\n";
    sh << "            target=$(readlink \"$link\" || true)\n";
    sh << "            case \"$target\" in *../zanna/share/man/*|"
       << std::string(kMacOSToolchainInstallRoot) << "/share/man/*) rm -f \"$link\" ;; esac\n";
    sh << "          fi\n";
    sh << "          ;;\n";
    sh << "      esac\n";
    sh << "      rm -f \"$ROOT/$rel\"\n";
    sh << "    fi\n";
    sh << "  done < \"$OLD\"\n";
    sh << "fi\n";
    for (const auto &name : toolNames) {
        sh << "link=" << shQuote(std::string(kMacOSLocalBinDir) + "/" + name) << "\n";
        sh << "if [ -L \"$link\" ]; then\n";
        sh << "  target=$(readlink \"$link\" || true)\n";
        sh << "  case \"$target\" in ../zanna/bin/*|" << std::string(kMacOSToolchainInstallRoot)
           << "/bin/*) rm -f \"$link\" ;; esac\n";
        sh << "fi\n";
    }
    sh << "if [ -L " << shQuote(std::string(kMacOSLocalCMakeZannaDir)) << " ]; then rm -f "
       << shQuote(std::string(kMacOSLocalCMakeZannaDir)) << "; fi\n";
    sh << "exit 0\n";
    return sh.str();
}

/// @brief Generate the installer `postinstall` script.
/// @details Creates `/usr/local/bin` CLI symlinks for @p toolNames and man-page
///          symlinks for @p manPagePaths, and registers the file-handler app with
///          Launch Services when @p registerFileAssociationApp is true.
/// @param toolNames Executable leaf names linked under `/usr/local/bin`.
/// @param manPagePaths Paths relative to `share/man/` linked into the system tree.
/// @param registerFileAssociationApp Whether to register the staged handler app.
/// @return POSIX postinstall script text.
std::string generateMacOSPostinstallScript(const std::vector<std::string> &toolNames,
                                           const std::vector<std::string> &manPagePaths,
                                           bool registerFileAssociationApp) {
    std::ostringstream sh;
    sh << "#!/bin/sh\n";
    sh << "set -eu\n";
    sh << "mkdir -p " << shQuote(std::string(kMacOSLocalBinDir)) << " "
       << shQuote(std::string(kMacOSLocalCMakeZannaDir)) << " "
       << shQuote(std::string(kMacOSLocalManDir)) << "\n";
    for (const auto &name : toolNames) {
        sh << "if [ -e " << shQuote(std::string(kMacOSToolchainInstallRoot) + "/bin/" + name)
           << " ]; then\n";
        sh << "  link=" << shQuote(std::string(kMacOSLocalBinDir) + "/" + name) << "\n";
        sh << "  if [ ! -e \"$link\" ] || [ -L \"$link\" ]; then\n";
        sh << "    ln -sfn ../zanna/bin/" << shQuote(name) << " \"$link\"\n";
        sh << "  fi\n";
        sh << "fi\n";
    }
    for (const auto &page : manPagePaths) {
        const std::string source = std::string(kMacOSToolchainInstallRoot) + "/share/man/" + page;
        const std::string link = std::string(kMacOSLocalManDir) + "/" + page;
        const std::string parent = fs::path(link).parent_path().generic_string();
        sh << "if [ -e " << shQuote(source) << " ]; then\n";
        sh << "  mkdir -p " << shQuote(parent) << "\n";
        sh << "  if [ ! -e " << shQuote(link) << " ] || [ -L " << shQuote(link) << " ]; then\n";
        sh << "    ln -sfn " << shQuote(macOSManSymlinkTarget(page).generic_string()) << " "
           << shQuote(link) << "\n";
        sh << "  fi\n";
        sh << "fi\n";
    }
    if (registerFileAssociationApp) {
        sh << "APP=" << shQuote(std::string(kMacOSToolchainAppPath)) << "\n";
        sh << "LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/"
              "LaunchServices.framework/Support/lsregister\n";
        sh << "if [ -x \"$LSREGISTER\" ] && [ -d \"$APP\" ]; then \"$LSREGISTER\" -f \"$APP\" "
              ">/dev/null 2>&1 || true; fi\n";
    }
    sh << "if command -v mandb >/dev/null 2>&1; then mandb -q "
       << shQuote(std::string(kMacOSLocalManDir)) << " >/dev/null 2>&1 || true; fi\n";
    sh << "if command -v makewhatis >/dev/null 2>&1; then makewhatis "
       << shQuote(std::string(kMacOSLocalManDir)) << " >/dev/null 2>&1 || true; fi\n";
    sh << "find " << shQuote(std::string(kMacOSToolchainInstallRoot))
       << " -type d -empty -delete 2>/dev/null || true\n";
    sh << "exit 0\n";
    return sh.str();
}

/// @brief Build the component package's `PackageInfo` XML.
/// @details Declares the package identifier/version, payload file count and size,
///          and the preinstall/postinstall script hooks for a flat component pkg.
/// @param params Toolchain build parameters (identifier, manifest).
/// @param pkgVersion Dotted-numeric package version string.
/// @param payloadEntryCount Number of files in the CPIO payload.
/// @return The PackageInfo XML text.
std::string generateMacOSToolchainPackageInfo(const MacOSToolchainBuildParams &params,
                                              const std::string &pkgVersion,
                                              size_t payloadEntryCount) {
    std::ostringstream xml;
    const uint64_t kbytes = (params.manifest.totalSizeBytes() + 1023u) / 1024u;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml << "<pkg-info overwrite-permissions=\"true\" relocatable=\"false\" identifier=\""
        << xmlEscape(params.identifier) << "\" postinstall-action=\"none\" version=\""
        << xmlEscape(pkgVersion)
        << "\" format-version=\"2\" auth=\"root\" install-location=\"/\">\n";
    xml << "    <payload numberOfFiles=\"" << payloadEntryCount << "\" installKBytes=\"" << kbytes
        << "\"/>\n";
    xml << "    <bundle-version/>\n";
    xml << "    <upgrade-bundle/>\n";
    xml << "    <update-bundle/>\n";
    xml << "    <atomic-update-bundle/>\n";
    xml << "    <strict-identifier/>\n";
    xml << "    <relocate/>\n";
    xml << "    <scripts>\n";
    xml << "        <preinstall file=\"./preinstall\" timeout=\"600\"/>\n";
    xml << "        <postinstall file=\"./postinstall\" timeout=\"600\"/>\n";
    xml << "    </scripts>\n";
    xml << "</pkg-info>\n";
    return xml.str();
}

/// @brief Map a payload arch to the Distribution `hostArchitectures` attribute.
/// @param arch Portable manifest architecture.
/// @return "arm64", "x86_64", or "x86_64,arm64" (universal) for unknown/empty.
std::string macOSHostArchitectures(const std::string &arch) {
    if (arch == "arm64")
        return "arm64";
    if (arch == "x64" || arch == "x86_64")
        return "x86_64";
    return "x86_64,arm64";
}

/// @brief Resolve and validate the minimum supported macOS version for the payload architecture.
/// @param params Toolchain settings and manifest architecture.
/// @return Explicit minimum version or the architecture-specific default.
/// @throws std::runtime_error If the version is not dotted numeric text.
std::string minimumMacOSVersion(const MacOSToolchainBuildParams &params) {
    const std::string version = params.minimumMacOSVersion.empty()
                                    ? (params.manifest.arch == "x64" ? "10.15" : "11.0")
                                    : params.minimumMacOSVersion;
    validateDottedNumericVersion(version, "minimum macOS version");
    return version;
}

/// @brief Build the product archive `Distribution` XML wrapping the component pkg.
/// @details Declares the title, host-architecture options, install size, and the
///          single choice referencing the embedded ZannaToolchain.pkg component.
/// @param params Toolchain build parameters (display name, identifier, arch).
/// @param pkgVersion Dotted-numeric package version string.
/// @param installKBytes Estimated installed size in KiB.
/// @param hasBackground Whether a light installer background resource is present.
/// @param hasDarkBackground Whether a dark-appearance background resource is present.
/// @return The Distribution XML text.
std::string generateMacOSToolchainDistribution(const MacOSToolchainBuildParams &params,
                                               const std::string &pkgVersion,
                                               uint64_t installKBytes,
                                               bool hasBackground,
                                               bool hasDarkBackground) {
    std::ostringstream xml;
    const std::string escapedId = xmlEscape(params.identifier);
    const std::string escapedProductId = xmlEscape(params.identifier + ".product");
    const std::string escapedName = xmlEscape(params.displayName);
    const std::string escapedVersion = xmlEscape(pkgVersion);
    const std::string escapedMinimumVersion = xmlEscape(minimumMacOSVersion(params));
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml << "<installer-gui-script minSpecVersion=\"1\">\n";
    xml << "  <title>" << escapedName << "</title>\n";
    xml << "  <welcome file=\"welcome.html\" mime-type=\"text/html\"/>\n";
    xml << "  <readme file=\"readme.html\" mime-type=\"text/html\"/>\n";
    xml << "  <license file=\"license.txt\" mime-type=\"text/plain\"/>\n";
    xml << "  <conclusion file=\"conclusion.html\" mime-type=\"text/html\"/>\n";
    if (hasBackground)
        xml << "  <background file=\"background.png\" alignment=\"bottomleft\" "
               "scaling=\"proportional\"/>\n";
    if (hasDarkBackground)
        xml << "  <background-darkAqua file=\"background-dark.png\" "
               "alignment=\"bottomleft\" scaling=\"proportional\"/>\n";
    xml << "  <domains enable_anywhere=\"false\" enable_currentUserHome=\"false\" "
           "enable_localSystem=\"true\"/>\n";
    xml << "  <volume-check>\n";
    xml << "    <allowed-os-versions><os-version min=\"" << escapedMinimumVersion
        << "\"/></allowed-os-versions>\n";
    xml << "  </volume-check>\n";
    xml << "  <pkg-ref id=\"" << escapedId << "\">\n";
    xml << "    <bundle-version/>\n";
    xml << "  </pkg-ref>\n";
    xml << "  <options customize=\"never\" require-scripts=\"false\" rootVolumeOnly=\"true\" "
           "hostArchitectures=\""
        << xmlEscape(macOSHostArchitectures(params.manifest.arch)) << "\"/>\n";
    xml << "  <choices-outline>\n";
    xml << "    <line choice=\"default\">\n";
    xml << "      <line choice=\"" << escapedId << "\"/>\n";
    xml << "    </line>\n";
    xml << "  </choices-outline>\n";
    xml << "  <choice id=\"default\" title=\"" << escapedName << "\"/>\n";
    xml << "  <choice id=\"" << escapedId << "\" title=\"" << escapedName
        << "\" visible=\"false\">\n";
    xml << "    <pkg-ref id=\"" << escapedId << "\"/>\n";
    xml << "  </choice>\n";
    xml << "  <pkg-ref id=\"" << escapedId << "\" version=\"" << escapedVersion
        << "\" onConclusion=\"none\" installKBytes=\"" << installKBytes
        << "\" updateKBytes=\"0\" auth=\"Root\">#ZannaToolchain.pkg</pkg-ref>\n";
    xml << "  <product id=\"" << escapedProductId << "\" version=\"" << escapedVersion << "\"/>\n";
    xml << "</installer-gui-script>\n";
    return xml.str();
}

/// @brief Generate a dependency-free branded Installer.app background image.
/// @param dark Select the dark-appearance palette when true.
/// @return RGBA image sized for the installer presentation pane.
PkgImage defaultMacOSInstallerBackground(bool dark) {
    PkgImage image;
    image.width = 620;
    image.height = 418;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4u);
    const std::array<uint8_t, 3> top =
        dark ? std::array<uint8_t, 3>{31, 40, 54} : std::array<uint8_t, 3>{248, 250, 253};
    const std::array<uint8_t, 3> bottom =
        dark ? std::array<uint8_t, 3>{15, 21, 31} : std::array<uint8_t, 3>{226, 235, 247};
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint32_t denom = image.height > 1 ? image.height - 1 : 1;
        for (uint32_t x = 0; x < image.width; ++x) {
            uint8_t *pixel = image.at(x, y);
            for (size_t channel = 0; channel < 3; ++channel) {
                pixel[channel] =
                    static_cast<uint8_t>((static_cast<uint32_t>(top[channel]) * (denom - y) +
                                          static_cast<uint32_t>(bottom[channel]) * y) /
                                         denom);
            }
            pixel[3] = 255;
        }
    }
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < 6; ++x) {
            uint8_t *pixel = image.at(x, y);
            pixel[0] = 45;
            pixel[1] = 119;
            pixel[2] = 210;
        }
    }

    const PkgImage icon = imageResize(defaultZannaToolchainIconImage(), 160, 160);
    constexpr uint32_t iconX = 430;
    constexpr uint32_t iconY = 126;
    for (uint32_t y = 0; y < icon.height; ++y) {
        for (uint32_t x = 0; x < icon.width; ++x) {
            const uint8_t *source = icon.at(x, y);
            uint8_t *destination = image.at(iconX + x, iconY + y);
            const uint32_t alpha = static_cast<uint32_t>(source[3]) * (dark ? 92u : 72u) / 255u;
            for (size_t channel = 0; channel < 3; ++channel) {
                destination[channel] = static_cast<uint8_t>(
                    (static_cast<uint32_t>(source[channel]) * alpha +
                     static_cast<uint32_t>(destination[channel]) * (255u - alpha)) /
                    255u);
            }
        }
    }
    return image;
}

/// @brief Generate the HTML welcome pane shown first in the macOS toolchain installer.
/// @param displayName User-visible toolchain package name.
/// @param version Installer version displayed in the heading.
/// @return Complete HTML document.
std::string generateMacOSToolchainWelcomeHtml(const std::string &displayName,
                                              const std::string &version) {
    std::ostringstream html;
    html << "<!DOCTYPE html>\n<html><body "
            "style=\"font-family:-apple-system,Helvetica,Arial,sans-serif;\">\n";
    html << "<h2>" << xmlEscape(displayName) << " " << xmlEscape(version) << "</h2>\n";
    html
        << "<p>This installer sets up the Zanna compiler toolchain &mdash; the <code>zanna</code>, "
           "<code>zbasic</code>, <code>zia</code>, language servers, IL utilities, and "
           "<code>zannastudio</code> together with the runtime libraries, headers, CMake package "
           "files, "
           "and manual pages.</p>\n";
    html << "<p>The toolchain installs under <code>/usr/local/zanna</code> with convenience "
            "symlinks in <code>/usr/local/bin</code>. Click Continue to proceed.</p>\n";
    html << "<p>An uninstall helper is installed at "
            "<code>/usr/local/zanna/share/zanna/uninstall.sh</code>. Source and IL file opens are "
            "handled by a small helper app in <code>/Applications</code>.</p>\n";
    html << "</body></html>\n";
    return html.str();
}

/// @brief Generate the pre-install summary pane for the macOS toolchain installer.
/// @param minimumVersion Minimum supported macOS version shown to the user.
/// @return Complete HTML document.
std::string generateMacOSToolchainReadmeHtml(const std::string &minimumVersion) {
    std::ostringstream html;
    html << "<!DOCTYPE html>\n<html><body "
            "style=\"font-family:-apple-system,Helvetica,Arial,sans-serif;\">\n";
    html << "<h2>What will be installed</h2>\n";
    html << "<ul><li>The toolchain under <code>/usr/local/zanna</code>.</li>"
            "<li>Command links under <code>/usr/local/bin</code> and manual-page links under "
            "<code>/usr/local/share/man</code>.</li>"
            "<li>A lightweight file-opening helper in <code>/Applications</code>.</li></ul>\n";
    html << "<p>Administrator approval is required. Existing Zanna-owned files are upgraded in "
            "place; unrelated files are preserved. Requires macOS "
         << xmlEscape(minimumVersion) << " or newer.</p>\n";
    html << "</body></html>\n";
    return html.str();
}

/// @brief Generate the completion pane with verification and uninstall guidance.
std::string generateMacOSToolchainConclusionHtml() {
    return "<!DOCTYPE html>\n<html><body "
           "style=\"font-family:-apple-system,Helvetica,Arial,sans-serif;\">\n"
           "<h2>Zanna Toolchain is ready</h2>\n"
           "<p>Open a new Terminal window and run <code>zanna --version</code> to verify the "
           "installation.</p>\n"
           "<p>To remove Zanna later, run "
           "<code>sudo /usr/local/zanna/share/zanna/uninstall.sh</code>.</p>\n"
           "</body></html>\n";
}

/// @brief Generate the plain-text license pane for the macOS toolchain installer.
/// @param spdx Declared license identifier; empty defaults to GPL-3.0-only.
/// @return Plain-text license summary.
std::string generateMacOSToolchainLicenseText(const std::string &spdx) {
    const std::string id = spdx.empty() ? std::string("GPL-3.0-only") : spdx;
    std::ostringstream txt;
    txt << "Zanna Compiler Toolchain\n\n";
    txt << "This software is distributed under the " << id << " license.\n\n";
    txt << "The complete license text ships with the Zanna source distribution and is available "
           "online at https://www.gnu.org/licenses/. By choosing Agree you accept the terms of the "
        << id << " license.\n";
    return txt.str();
}

/// @brief Find the staged Zanna license file for the macOS installer license pane.
/// @details Prefers a manifest entry whose leaf name is exactly `LICENSE`, which
///          matches the CMake install layout under share/doc/zanna. Returns
///          nullptr when the staged tree does not contain a suitable file.
/// @param manifest Staged toolchain manifest.
/// @return Pointer to the license entry, or nullptr.
const ToolchainFileEntry *findMacOSToolchainLicenseFile(const ToolchainInstallManifest &manifest) {
    for (const auto &file : manifest.files) {
        std::string leaf = fs::path(file.stagedRelativePath).filename().generic_string();
        for (char &c : leaf)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!file.symlink && leaf == "license")
            return &file;
    }
    return nullptr;
}

/// @brief Walk the staged .app bundle and write all entries to a ZIP at `outputPath`.
/// The file at `execPath` gets Unix mode 0100755; all other files get 0100644.
/// @param stageRoot Root used to derive archive-relative paths.
/// @param appPath Staged `.app` directory to traverse.
/// @param execPath Main executable, which must retain executable permissions.
/// @param outputPath Destination ZIP path.
/// @throws std::runtime_error If traversal, path mapping, reading, or output fails.
void addStagedAppToZip(const fs::path &stageRoot,
                       const fs::path &appPath,
                       const fs::path &execPath,
                       const std::string &outputPath) {
    ZipWriter zip;
    const std::string appEntry = fs::relative(appPath, stageRoot).generic_string();
    zip.addDirectory(appEntry);

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        appPath, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        throw std::runtime_error("cannot iterate staged macOS app bundle: " + ec.message());
    const auto end = fs::recursive_directory_iterator();
    while (it != end) {
        const fs::path entryPath = it->path();
        const std::string rel = fs::relative(entryPath, stageRoot, ec).generic_string();
        if (ec)
            throw std::runtime_error("cannot compute macOS app ZIP path: " + ec.message());
        ec.clear();
        const fs::file_status symlinkStatus = it->symlink_status(ec);
        if (ec)
            throw std::runtime_error("cannot stat staged macOS app entry: " + ec.message());
        if (fs::is_symlink(symlinkStatus)) {
            const fs::path target = fs::read_symlink(entryPath, ec);
            if (ec)
                throw std::runtime_error("cannot read staged macOS app symlink: " + ec.message());
            zip.addSymlink(rel, target.generic_string());
        } else if (it->is_directory(ec)) {
            if (ec)
                throw std::runtime_error("cannot stat staged macOS app directory: " + ec.message());
            zip.addDirectory(rel);
        } else if (it->is_regular_file(ec)) {
            if (ec)
                throw std::runtime_error("cannot stat staged macOS app file: " + ec.message());
            const auto data = readFile(entryPath.string());
            const bool isMainExec = fs::equivalent(entryPath, execPath, ec);
            if (ec)
                throw std::runtime_error("cannot compare staged macOS executable path: " +
                                         ec.message());
            const fs::perms sourcePerms = fs::status(entryPath, ec).permissions();
            if (ec)
                throw std::runtime_error("cannot inspect staged macOS file mode: " + ec.message());
            const fs::perms executableBits =
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
            const bool isExec = isMainExec || (sourcePerms & executableBits) != fs::perms::none;
            const uint32_t mode = isExec ? 0100755 : 0100644;
            zip.addFile(rel, data.data(), data.size(), mode);
        }
        it.increment(ec);
        if (ec)
            throw std::runtime_error("cannot advance staged macOS app iterator: " + ec.message());
    }
    zip.finish(outputPath);
}

/// @brief Verify the codesign signature of `appPath` using `codesign --verify --deep --strict`.
/// Only runs on Apple hosts; no-op on all other platforms.
/// @param appPath Application bundle to verify.
/// @throws std::runtime_error On Apple hosts when codesign verification fails.
void verifyMacOSBundleSignatureIfAvailable(const fs::path &appPath) {
#if defined(__APPLE__)
    runChecked({"codesign", "--verify", "--deep", "--strict", "--verbose=2", appPath.string()},
               "macOS code signature verification");
#else
    (void)appPath;
#endif
}

/// @brief Sign the .app bundle using `codesign` with the mode resolved from `pkg`.
/// Supports Developer ID (with optional notarization/stapling), ad-hoc, and no-op modes.
/// "ad-hoc" and "developer-id" modes throw on non-Apple hosts.
/// @param stageRoot Temporary staging root used for notarization ZIP output.
/// @param appPath Application bundle to sign.
/// @param execPath Main executable used while constructing notarization ZIPs.
/// @param projectRoot Trusted root for resolving optional entitlements.
/// @param pkg Signing, notarization, and entitlement configuration.
/// @throws std::runtime_error If configuration, signing, notarization, or verification fails.
void signMacOSBundle(const fs::path &stageRoot,
                     const fs::path &appPath,
                     const fs::path &execPath,
                     const fs::path &projectRoot,
                     const PackageConfig &pkg) {
    validateMacOSSigningConfig(pkg);
    const std::string mode = resolveMacOSSignModeForHost(pkg);
    if (mode == "none" || mode == "preserve")
        return;

#if !defined(__APPLE__)
    (void)stageRoot;
    (void)appPath;
    (void)execPath;
    (void)projectRoot;
    (void)pkg;
    throw std::runtime_error("macOS signing mode '" + mode + "' requires running on macOS");
#else
    const bool developerId = mode == "developer-id";
    if (developerId && pkg.macosSignIdentity.empty())
        throw std::runtime_error("macOS Developer ID signing requires macos-sign-identity");
    if (!pkg.macosNotaryProfile.empty() && !developerId)
        throw std::runtime_error("macOS notarization requires macos-sign-mode developer-id");

    std::vector<std::string> args = {
        "codesign", "--force", "--sign", developerId ? pkg.macosSignIdentity : std::string("-")};
    if (developerId)
        args.push_back("--timestamp");
    // Hardened runtime is required for notarization and is the safe default for any real
    // (Developer ID) signing; enable it unless the package explicitly opts out. Ad-hoc/local
    // signing leaves it off unless requested.
    bool hardenedRuntime =
        pkg.macosHardenedRuntime || !pkg.macosNotaryProfile.empty() || developerId;
    if (pkg.macosDisableHardenedRuntime)
        hardenedRuntime = false;
    if (hardenedRuntime) {
        args.push_back("--options");
        args.push_back("runtime");
    }
    if (!pkg.macosEntitlements.empty()) {
        const fs::path entitlements =
            resolvePackageSourcePath(projectRoot, pkg.macosEntitlements, "macOS entitlements");
        args.push_back("--entitlements");
        args.push_back(entitlements.string());
    }
    args.push_back(appPath.string());
    runChecked(args, "macOS code signing");
    verifyMacOSBundleSignatureIfAvailable(appPath);

    if (!pkg.macosNotaryProfile.empty()) {
        const fs::path notaryZip = stageRoot / "notary-submit.zip";
        addStagedAppToZip(stageRoot, appPath, execPath, notaryZip.string());
        std::vector<std::string> notaryArgs = {"xcrun",
                                               "notarytool",
                                               "submit",
                                               notaryZip.string(),
                                               "--keychain-profile",
                                               pkg.macosNotaryProfile,
                                               "--wait"};
        // Bound the wait so a stalled Apple service cannot hang the build indefinitely.
        const int notaryTimeoutSeconds =
            pkg.macosNotaryTimeoutSeconds > 0 ? pkg.macosNotaryTimeoutSeconds : 1800;
        notaryArgs.push_back("--timeout");
        notaryArgs.push_back(std::to_string(notaryTimeoutSeconds) + "s");
        // Retry once on a transient submit/network failure before giving up.
        for (int attempt = 1;; ++attempt) {
            try {
                runChecked(notaryArgs, "macOS notarization");
                break;
            } catch (const std::exception &) {
                if (attempt >= 2)
                    throw;
            }
        }
        if (pkg.macosStaple) {
            runChecked({"xcrun", "stapler", "staple", appPath.string()},
                       "macOS notarization stapling");
            verifyMacOSBundleSignatureIfAvailable(appPath);
        }
    }
#endif
}

} // namespace

//=============================================================================
// MacOS Package Builder
//=============================================================================

/// @brief Result of staging a macOS .app bundle on disk.
struct StagedMacOSApp {
    fs::path appPath;    ///< Absolute path to the staged <name>.app bundle.
    fs::path stagedExec; ///< Absolute path to the bundle's Contents/MacOS/<exe>.
};

/// @brief Stage (and code-sign) a macOS .app bundle into @p stageRoot.
/// @details Shared by the .app-in-.zip and .app-in-.dmg builders so both emit an
///          identical, signed bundle. The caller owns @p stageRoot and its
///          TempDirGuard; this only populates it.
/// @param params Application metadata, executable, assets, and signing configuration.
/// @param stageRoot Existing temporary directory to populate.
/// @return Absolute staged app and primary-executable paths.
/// @throws std::runtime_error If validation, staging, asset copying, or signing fails.
static StagedMacOSApp stageMacOSAppBundle(const MacOSBuildParams &params,
                                          const fs::path &stageRoot) {
    const auto &pkg = params.pkgConfig;
    std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    validateBundleDisplayName(displayName);
    validateMacOSBundleIdentifier(pkg.identifier, "macOS bundle identifier");
    validateDottedNumericVersion(version, "macOS package version");
    if (!pkg.minOsMacos.empty())
        validateDottedNumericVersion(pkg.minOsMacos, "minimum macOS version");
    validatePackageFileAssociations(pkg.fileAssociations);
    validateMacOSSigningConfig(pkg);
    if (!fs::is_regular_file(params.executablePath))
        throw std::runtime_error("macOS package executable is not a regular file: " +
                                 params.executablePath);

    // Determine executable name (lowercase, no spaces)
    const std::string execName = normalizeExecName(params.projectName);

    const std::string appName = displayName + ".app";
    const fs::path appPath = stageRoot / appName;
    const fs::path contentsDir = appPath / "Contents";
    const fs::path macosDir = contentsDir / "MacOS";
    const fs::path resourcesDir = contentsDir / "Resources";
    fs::create_directories(macosDir);
    fs::create_directories(resourcesDir);

    writeFileString(contentsDir / "PkgInfo",
                    generatePkgInfo(),
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);

    const fs::path stagedExec = macosDir / execName;
    writeFileBytes(stagedExec,
                   readFile(params.executablePath),
                   fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                       fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                       fs::perms::others_exec);

    std::string iconFileName;
    if (!pkg.iconPath.empty()) {
        fs::path iconSrc =
            resolvePackageSourcePath(params.projectRoot, pkg.iconPath, "package icon");
        if (!fs::is_regular_file(iconSrc))
            throw std::runtime_error("package icon not found: " + pkg.iconPath);
        auto srcImage = pngRead(iconSrc.string());
        auto icnsData = generateIcns(srcImage);
        iconFileName = execName;
        writeFileBytes(resourcesDir / (execName + ".icns"),
                       icnsData,
                       fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                           fs::perms::others_read);
    }

    PlistParams plistParams;
    plistParams.executableName = execName;
    plistParams.bundleId =
        pkg.identifier.empty() ? defaultMacOSBundleIdentifier(params.projectName) : pkg.identifier;
    plistParams.bundleName = displayName;
    plistParams.version = version;
    plistParams.iconFile = iconFileName;
    plistParams.minOsVersion = pkg.minOsMacos;
    plistParams.appCategory = macOSApplicationCategory(pkg.category);
    plistParams.fileAssociations = pkg.fileAssociations;
    writeFileString(contentsDir / "Info.plist",
                    generatePlist(plistParams),
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);

    for (const auto &asset : pkg.assets) {
        fs::path srcPath =
            resolvePackageSourcePath(params.projectRoot, asset.sourcePath, "asset source path");
        std::string targetDir = sanitizePackageRelativePath(asset.targetPath, "asset target path");
        copyPackageAssetToResources(
            srcPath, params.projectRoot, resourcesDir, targetDir, asset.sourcePath);
    }

    signMacOSBundle(stageRoot, appPath, stagedExec, params.projectRoot, pkg);
    return {appPath, stagedExec};
}

/// @brief Wrap a staged .app bundle in a compressed drag-to-install .dmg.
/// @details Stages the app, an `/Applications` symlink, and optional visual
///          assets from package metadata. The image is created read-write,
///          styled best-effort through Finder when a macOS desktop session is
///          available, then compressed to the final UDZO output. If styling
///          commands fail in headless CI, the package remains valid.
/// @param params Original app build parameters carrying project root and DMG metadata.
/// @param appPath Path to the already-staged signed `.app` bundle.
/// @param volumeName Mounted volume name and Finder window title.
/// @param outputPath Final `.dmg` output path.
/// @throws std::runtime_error on staging, hdiutil, or required visual asset failures.
static void addStagedAppToDmg(const MacOSBuildParams &params,
                              const fs::path &appPath,
                              const std::string &volumeName,
                              const std::string &outputPath) {
    // hdiutil is macOS-only; off-platform it is simply absent and runChecked surfaces a
    // clear failure, so this helper needs no host macro of its own.
    if (volumeName.empty() || volumeName.find('/') != std::string::npos ||
        volumeName.find(':') != std::string::npos)
        throw std::runtime_error(
            "macOS .dmg volume name must be non-empty and free of '/' and ':'");

    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-app-dmg");
    TempDirGuard cleanup(tmpRoot);
    const fs::path stage = tmpRoot / "stage";
    fs::create_directories(stage);
    fs::copy(appPath,
             stage / appPath.filename(),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);

    std::error_code symEc;
    fs::create_directory_symlink("/Applications", stage / "Applications", symEc);
    if (symEc)
        throw std::runtime_error("cannot create /Applications symlink for .dmg staging: " +
                                 symEc.message());

    bool haveBackground = false;
    if (!params.pkgConfig.macosDmgBackground.empty()) {
        const fs::path bgSrc = resolvePackageSourcePath(
            params.projectRoot, params.pkgConfig.macosDmgBackground, "macOS DMG background");
        if (!fs::is_regular_file(bgSrc))
            throw std::runtime_error("macOS DMG background is not a regular file: " +
                                     params.pkgConfig.macosDmgBackground);
        const fs::path bgDir = stage / ".background";
        fs::create_directories(bgDir);
        fs::copy_file(bgSrc, bgDir / "background.png", fs::copy_options::overwrite_existing);
        haveBackground = true;
    }

    bool haveVolumeIcon = false;
    if (!params.pkgConfig.macosDmgIcon.empty()) {
        const fs::path iconSrc = resolvePackageSourcePath(
            params.projectRoot, params.pkgConfig.macosDmgIcon, "macOS DMG icon");
        if (!fs::is_regular_file(iconSrc))
            throw std::runtime_error("macOS DMG icon is not a regular file: " +
                                     params.pkgConfig.macosDmgIcon);
        fs::copy_file(iconSrc, stage / ".VolumeIcon.icns", fs::copy_options::overwrite_existing);
        haveVolumeIcon = true;
    }

    std::error_code rmEc;
    fs::remove(outputPath, rmEc);
    const fs::path rwDmg = tmpRoot / "rw.dmg";
    runChecked({"hdiutil",
                "create",
                "-srcfolder",
                stage.string(),
                "-volname",
                volumeName,
                "-fs",
                "HFS+",
                "-format",
                "UDRW",
                "-ov",
                rwDmg.string()},
               "macOS app .dmg read-write image creation");

    const fs::path mountPoint =
        attachMacOSDmgForStyling(rwDmg, "macOS app .dmg Finder-visible attach");

    {
        const std::string appFileName =
            validateDmgItemName(appPath.filename().string(), "macOS app DMG item name");
        const std::string volumeLiteral = appleScriptStringLiteral(
            mountPoint.filename().string(), "macOS app DMG mounted volume name");
        const std::string appLiteral =
            appleScriptStringLiteral(appFileName, "macOS app DMG item name");
        std::ostringstream s;
        s << "with timeout of 15 seconds\n"
          << "tell application \"Finder\"\n"
          << "  tell disk " << volumeLiteral << "\n"
          << "    open\n"
          << "    set current view of container window to icon view\n"
          << "    set toolbar visible of container window to false\n"
          << "    set statusbar visible of container window to false\n"
          << "    set the bounds of container window to {200, 120, 760, 500}\n"
          << "    set vopts to the icon view options of container window\n"
          << "    set arrangement of vopts to not arranged\n"
          << "    set icon size of vopts to 128\n";
        if (haveBackground)
            s << "    set background picture of vopts to file \".background:background.png\"\n";
        s << "    set position of item " << appLiteral << " of container window to {180, 240}\n"
          << "    set position of item \"Applications\" of container window to {420, 240}\n"
          << "    update without registering applications\n"
          << "    delay 1\n"
          << "    close\n"
          << "  end tell\n"
          << "end tell\n"
          << "end timeout\n";
        runBestEffortMacOSStyling({"osascript", "-e", s.str()}, "macOS app DMG Finder styling");
    }
    if (haveVolumeIcon)
        runBestEffortMacOSStyling({"SetFile", "-a", "C", mountPoint.string()},
                                  "macOS app DMG volume icon styling");
    runChecked({"sync"}, "macOS app DMG filesystem flush");

    if (run_process({"hdiutil", "detach", mountPoint.string()}).exit_code != 0)
        runChecked({"hdiutil", "detach", "-force", mountPoint.string()}, "macOS app .dmg detach");

    runChecked({"hdiutil",
                "convert",
                rwDmg.string(),
                "-format",
                "UDZO",
                "-imagekey",
                "zlib-level=9",
                "-ov",
                "-o",
                outputPath},
               "macOS app .dmg compression");
    runChecked({"hdiutil", "verify", outputPath}, "macOS app .dmg verification");
    verifyMountedMacOSDmgContents(
        outputPath, {}, {appPath.filename().string()}, {"Applications"}, "macOS app .dmg");
}

/// @brief Build a macOS .app bundle inside a ZIP archive from the given build parameters.
/// Stages the bundle in a temp directory (exec, Resources, PkgInfo, Info.plist, ICNS),
/// optionally signs it with codesign, then packs the result into a ZIP at `params.outputPath`.
/// @param params Application metadata, input paths, signing options, and output ZIP.
/// @throws std::runtime_error If staging, signing, or ZIP creation fails.
void buildMacOSPackage(const MacOSBuildParams &params) {
    const fs::path stageRoot =
        uniqueTempPackagingDir("zanna-macos-app-" + normalizeExecName(params.projectName));
    TempDirGuard cleanup(stageRoot);
    const StagedMacOSApp staged = stageMacOSAppBundle(params, stageRoot);
    addStagedAppToZip(stageRoot, staged.appPath, staged.stagedExec, params.outputPath);
}

/// @brief Build a macOS .app bundle wrapped in a drag-to-install .dmg.
/// @details Stages and signs the bundle exactly like buildMacOSPackage, then wraps it
///          (with an /Applications symlink) into a compressed .dmg instead of a .zip.
///          macOS-only (hdiutil).
/// @param params Application metadata, DMG presentation settings, and output path.
/// @throws std::runtime_error If staging, signing, disk-image creation, or verification fails.
void buildMacOSAppDmg(const MacOSBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    const std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const fs::path stageRoot =
        uniqueTempPackagingDir("zanna-macos-app-dmg-" + normalizeExecName(params.projectName));
    TempDirGuard cleanup(stageRoot);
    const StagedMacOSApp staged = stageMacOSAppBundle(params, stageRoot);
    addStagedAppToDmg(params, staged.appPath, displayName, params.outputPath);
}

/// @brief Build a macOS `.pkg` installer for the staged toolchain.
/// Stages files under `/usr/local/zanna/`, creates receipt-owned command and
/// manpage symlinks, emits native CPIO/XAR archives, and wraps the component in
/// a product distribution package.
/// @param params Staged manifest, package identity, presentation, signing, and output settings.
/// @throws std::runtime_error If validation, staging, signing, or archive construction fails.
void buildMacOSToolchainPackage(const MacOSToolchainBuildParams &params) {
    namespace fs = std::filesystem;
    validateToolchainInstallManifest(params.manifest);
    if (params.manifest.platform != "macos") {
        throw std::runtime_error("macOS toolchain package requires a macOS staged toolchain "
                                 "manifest, got '" +
                                 params.manifest.platform + "'");
    }
    if (params.manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    const std::string version = params.manifest.version;
    const std::string pkgVersion =
        resolveMacOSToolchainPackageVersion(version, params.packageVersion);
    if (params.identifier.empty())
        throw std::runtime_error("macOS toolchain package identifier is required");
    validateMacOSBundleIdentifier(params.identifier, "macOS package identifier");
    validateDebVersion(version, "macOS toolchain manifest version");
    const std::string minimumVersion = minimumMacOSVersion(params);

    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-macos-toolchain-" + version);
    TempDirGuard cleanup(tmpRoot);

    const fs::path payloadRoot = tmpRoot / "root";
    const fs::path installRoot = payloadRoot / "usr" / "local" / "zanna";
    fs::create_directories(installRoot);
    fs::create_directories(payloadRoot / "usr" / "local" / "bin");

    for (const auto &file : params.manifest.files) {
        const fs::path cleanRel =
            fs::path(sanitizePackageRelativePath(file.stagedRelativePath, "macOS staged path"));
        const fs::path dst = (installRoot / cleanRel).lexically_normal();
        if (!isPathWithin(installRoot, dst)) {
            throw std::runtime_error("macOS staged path escapes install root: " +
                                     file.stagedRelativePath);
        }
        fs::create_directories(dst.parent_path());
        if (file.symlink) {
            std::error_code ec;
            fs::remove(dst, ec);
            ec.clear();
            fs::create_symlink(fs::path(file.symlinkTarget), dst, ec);
            if (ec)
                throw std::runtime_error("cannot create package symlink '" + dst.string() +
                                         "' -> '" + file.symlinkTarget + "': " + ec.message());
            continue;
        }
        fs::copy_file(file.stagedAbsolutePath, dst, fs::copy_options::overwrite_existing);
        if (file.executable) {
            fs::permissions(dst,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                                fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                            fs::perm_options::replace);
        } else {
            fs::permissions(dst,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                fs::perms::others_read,
                            fs::perm_options::replace);
        }
    }

    std::vector<std::string> toolNames = macOSToolNames(params.manifest);
    appendLeafNamesFromDirectory(toolNames, installRoot / "bin");
    for (const auto &name : toolNames)
        createPackageSymlink(payloadRoot / "usr" / "local" / "bin" / name,
                             fs::path("../zanna/bin") / name);

    writeFileString(payloadRoot / "usr" / "local" / "lib" / "cmake" / "Zanna" / "ZannaConfig.cmake",
                    "include(\"" + std::string(kMacOSToolchainInstallRoot) +
                        "/lib/cmake/Zanna/ZannaConfig.cmake\")\n",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);
    writeFileString(payloadRoot / "usr" / "local" / "lib" / "cmake" / "Zanna" /
                        "ZannaConfigVersion.cmake",
                    "include(\"" + std::string(kMacOSToolchainInstallRoot) +
                        "/lib/cmake/Zanna/ZannaConfigVersion.cmake\")\n",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);

    std::vector<std::string> manPagePaths = macOSManPagePaths(params.manifest);
    appendRelativeFilePathsFromDirectory(manPagePaths, installRoot / "share" / "man");
    for (const auto &page : manPagePaths)
        createPackageSymlink(payloadRoot / "usr" / "local" / "share" / "man" / page,
                             macOSManSymlinkTarget(page));

    const bool registerFileAssociationApp = !params.manifest.fileAssociations.empty();
    if (registerFileAssociationApp) {
        const ToolchainFileEntry *handler = findMacOSFileHandler(params.manifest);
        if (handler == nullptr) {
            throw std::runtime_error("macOS toolchain file associations require staged "
                                     "libexec/zanna/zanna-file-handler");
        }
        const fs::path appContents = payloadRoot /
                                     fs::path(std::string(kMacOSToolchainAppPath)).relative_path() /
                                     "Contents";
        const fs::path appMacOS = appContents / "MacOS";
        const fs::path appResources = appContents / "Resources";
        fs::create_directories(appMacOS);
        fs::create_directories(appResources);
        writeFileString(appContents / "Info.plist",
                        generateMacOSFileHandlerInfoPlist(params, pkgVersion),
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read);
        writeFileString(appContents / "PkgInfo",
                        generatePkgInfo(),
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read);
        const fs::path appHandler = appMacOS / "zanna-file-handler";
        fs::copy_file(
            handler->stagedAbsolutePath, appHandler, fs::copy_options::overwrite_existing);
        fs::permissions(appHandler,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                            fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace);
        const auto icns = generateIcns(defaultZannaToolchainIconImage());
        writeFileBytes(appResources / "Zanna.icns",
                       icns,
                       fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                           fs::perms::others_read);
    }

    writeExecutableScript(installRoot / "share" / "zanna" / "uninstall.sh",
                          generateMacOSUninstallScript(params.identifier));
    const std::string installManifest = macOSInstallManifestText(params.manifest);
    writeFileString(installRoot / "share" / "zanna" / "install_manifest.txt",
                    installManifest,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);

    const fs::path scriptsRoot = tmpRoot / "scripts";
    fs::create_directories(scriptsRoot);
    writeExecutableScript(scriptsRoot / "preinstall", generateMacOSPreinstallScript(toolNames));
    writeExecutableScript(
        scriptsRoot / "postinstall",
        generateMacOSPostinstallScript(toolNames, manPagePaths, registerFileAssociationApp));
    writeFileString(scriptsRoot / "install_manifest.txt",
                    installManifest,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read);

    signMacOSToolchainPayload(payloadRoot, params.applicationSignIdentity);

    const fs::path bomPath = tmpRoot / "Bom";
    runChecked({"mkbom", "-s", payloadRoot.string(), bomPath.string()},
               "macOS bill-of-materials generation");

    CpioWriter payloadCpio;
    addFilesystemTreeToCpio(payloadCpio, payloadRoot);
    const auto payloadArchive = payloadCpio.finish();
    const auto payloadGzip = gzip(payloadArchive.data(), payloadArchive.size());

    CpioWriter scriptsCpio;
    addFilesystemTreeToCpio(scriptsCpio, scriptsRoot);
    const auto scriptsArchive = scriptsCpio.finish();
    const auto scriptsGzip = gzip(scriptsArchive.data(), scriptsArchive.size());

    const size_t payloadEntryCount = sortedTreeEntries(payloadRoot).size();
    const uint64_t installKBytes =
        roundedKiB(params.manifest.totalSizeBytes(), "macOS toolchain package");
    const std::string packageInfo =
        generateMacOSToolchainPackageInfo(params, pkgVersion, payloadEntryCount);
    // Installer branding resources: welcome, license, and accessible light/dark backgrounds are
    // always present. A caller-provided background overrides the generated light appearance.
    const std::string welcomeHtml =
        generateMacOSToolchainWelcomeHtml(params.displayName, pkgVersion);
    const std::string readmeHtml = generateMacOSToolchainReadmeHtml(minimumVersion);
    const std::string conclusionHtml = generateMacOSToolchainConclusionHtml();
    std::string licenseText;
    if (!params.licenseFilePath.empty()) {
        if (!fs::is_regular_file(params.licenseFilePath))
            throw std::runtime_error("macOS package license file not found: " +
                                     params.licenseFilePath);
        const auto bytes = readFile(params.licenseFilePath);
        licenseText.assign(bytes.begin(), bytes.end());
    } else if (const ToolchainFileEntry *license = findMacOSToolchainLicenseFile(params.manifest)) {
        const auto bytes = readFile(license->stagedAbsolutePath.string());
        licenseText.assign(bytes.begin(), bytes.end());
    } else {
        licenseText = generateMacOSToolchainLicenseText(params.manifest.license);
    }
    std::vector<uint8_t> backgroundBytes;
    if (!params.backgroundImagePath.empty()) {
        if (!fs::is_regular_file(params.backgroundImagePath))
            throw std::runtime_error("macOS package background image not found: " +
                                     params.backgroundImagePath);
        backgroundBytes = readFile(params.backgroundImagePath);
    } else {
        backgroundBytes = pngEncode(defaultMacOSInstallerBackground(false));
    }
    const std::vector<uint8_t> darkBackgroundBytes =
        pngEncode(defaultMacOSInstallerBackground(true));

    const std::string distribution = generateMacOSToolchainDistribution(
        params, pkgVersion, installKBytes, !backgroundBytes.empty(), !darkBackgroundBytes.empty());

    const fs::path output = fs::path(params.outputPath);
    if (!output.parent_path().empty())
        fs::create_directories(output.parent_path());
    XarWriter product;
    product.addDirectory("ZannaToolchain.pkg", 0700);
    product.addFileVec("ZannaToolchain.pkg/Bom", readFile(bomPath.string()), false);
    product.addFileVec("ZannaToolchain.pkg/Payload", payloadGzip, false);
    product.addFileVec("ZannaToolchain.pkg/Scripts", scriptsGzip, false);
    product.addFileString("ZannaToolchain.pkg/PackageInfo", packageInfo, true);
    product.addFileString("Distribution", distribution, true);
    product.addFileString("welcome.html", welcomeHtml, true);
    product.addFileString("readme.html", readmeHtml, true);
    product.addFileString("license.txt", licenseText, true);
    product.addFileString("conclusion.html", conclusionHtml, true);
    if (!backgroundBytes.empty())
        product.addFileVec("background.png", backgroundBytes, false);
    if (!darkBackgroundBytes.empty())
        product.addFileVec("background-dark.png", darkBackgroundBytes, false);
    product.finishToFile(params.outputPath);
}

//=============================================================================
// MacOS Toolchain DMG Builder
//=============================================================================

/// @brief Wrap a completed toolchain installer package in a styled compressed DMG.
/// @param params Input PKG, output path, volume naming, and optional presentation assets.
/// @throws std::runtime_error If invoked off macOS or if staging, hdiutil, styling,
///         conversion, or mounted-content verification fails.
void buildMacOSToolchainDmg(const MacOSToolchainDmgParams &params) {
#if !defined(__APPLE__)
    (void)params;
    throw std::runtime_error("building a macOS .dmg requires running on macOS (hdiutil)");
#else
    if (!fs::is_regular_file(params.pkgPath))
        throw std::runtime_error("macOS .dmg source package is not a regular file: " +
                                 params.pkgPath);
    if (params.outputPath.empty())
        throw std::runtime_error("macOS .dmg output path is required");
    std::error_code pathEc;
    const fs::path inputCanonical = fs::weakly_canonical(params.pkgPath, pathEc);
    if (pathEc)
        throw std::runtime_error("cannot resolve macOS .dmg source package: " + pathEc.message());
    pathEc.clear();
    const fs::path outputCanonical = fs::weakly_canonical(params.outputPath, pathEc);
    if (pathEc)
        throw std::runtime_error("cannot resolve macOS .dmg output path: " + pathEc.message());
    if (inputCanonical == outputCanonical)
        throw std::runtime_error("macOS .dmg output path must differ from its source .pkg");
    validateSingleLineField(params.volumeName, "macOS .dmg volume name");
    if (params.volumeName.empty() || params.volumeName.find('/') != std::string::npos ||
        params.volumeName.find(':') != std::string::npos)
        throw std::runtime_error(
            "macOS .dmg volume name must be non-empty and free of '/' and ':'");

    std::error_code ec;
    const fs::path tmpRoot = uniqueTempPackagingDir("zanna-dmg");
    TempDirGuard cleanup(tmpRoot);
    const fs::path stage = tmpRoot / "stage";
    fs::create_directories(stage);

    const std::string pkgName = validateDmgItemName(
        params.pkgDisplayName.empty() ? std::string("Zanna Toolchain.pkg") : params.pkgDisplayName,
        "macOS DMG package display name");
    fs::copy_file(params.pkgPath, stage / pkgName, fs::copy_options::overwrite_existing);

    bool haveBackground = true;
    const fs::path bgDir = stage / ".background";
    fs::create_directories(bgDir);
    if (!params.backgroundPng.empty()) {
        if (!fs::is_regular_file(params.backgroundPng))
            throw std::runtime_error("macOS .dmg background image not found: " +
                                     params.backgroundPng);
        fs::copy_file(
            params.backgroundPng, bgDir / "background.png", fs::copy_options::overwrite_existing);
    } else {
        const std::vector<uint8_t> background = pngEncode(defaultMacOSInstallerBackground(false));
        writeFileBytes(bgDir / "background.png",
                       background,
                       fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                           fs::perms::others_read);
    }

    bool haveVolumeIcon = true;
    if (!params.volumeIcns.empty()) {
        if (!fs::is_regular_file(params.volumeIcns))
            throw std::runtime_error("macOS .dmg volume icon not found: " + params.volumeIcns);
        fs::copy_file(
            params.volumeIcns, stage / ".VolumeIcon.icns", fs::copy_options::overwrite_existing);
    } else {
        const std::vector<uint8_t> icon = generateIcns(defaultZannaToolchainIconImage());
        writeFileBytes(stage / ".VolumeIcon.icns",
                       icon,
                       fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                           fs::perms::others_read);
    }

    // 1) Read-write image sized to the staged contents.
    const fs::path rwDmg = tmpRoot / "rw.dmg";
    runChecked({"hdiutil",
                "create",
                "-srcfolder",
                stage.string(),
                "-volname",
                params.volumeName,
                "-fs",
                "HFS+",
                "-format",
                "UDRW",
                "-ov",
                rwDmg.string()},
               "macOS .dmg read-write image creation");

    // 2) Attach for styling.
    const fs::path mountPoint =
        attachMacOSDmgForStyling(rwDmg, "macOS toolchain .dmg Finder-visible attach");

    // 3) Best-effort Finder styling — the image stays valid even if this fails (headless/CI).
    {
        const std::string volumeLiteral = appleScriptStringLiteral(mountPoint.filename().string(),
                                                                   "macOS DMG mounted volume name");
        const std::string packageLiteral =
            appleScriptStringLiteral(pkgName, "macOS DMG package display name");
        std::ostringstream s;
        s << "with timeout of 15 seconds\n"
          << "tell application \"Finder\"\n"
          << "  tell disk " << volumeLiteral << "\n"
          << "    open\n"
          << "    set current view of container window to icon view\n"
          << "    set toolbar visible of container window to false\n"
          << "    set statusbar visible of container window to false\n"
          << "    set the bounds of container window to {200, 120, 720, 480}\n"
          << "    set vopts to the icon view options of container window\n"
          << "    set arrangement of vopts to not arranged\n"
          << "    set icon size of vopts to 128\n";
        if (haveBackground)
            s << "    set background picture of vopts to file \".background:background.png\"\n";
        s << "    set position of item " << packageLiteral << " of container window to {260, 240}\n"
          << "    update without registering applications\n"
          << "    delay 1\n"
          << "    close\n"
          << "  end tell\n"
          << "end tell\n"
          << "end timeout\n";
        runBestEffortMacOSStyling({"osascript", "-e", s.str()},
                                  "macOS toolchain DMG Finder styling");
    }
    if (haveVolumeIcon)
        runBestEffortMacOSStyling({"SetFile", "-a", "C", mountPoint.string()},
                                  "macOS toolchain DMG volume icon styling");
    runChecked({"sync"}, "macOS toolchain DMG filesystem flush");

    // 4) Detach (force-retry once if the volume is briefly busy).
    if (run_process({"hdiutil", "detach", mountPoint.string()}).exit_code != 0)
        runChecked({"hdiutil", "detach", "-force", mountPoint.string()}, "macOS .dmg detach");

    // 5) Compress to a read-only UDZO image at the output path.
    const fs::path outputPath(params.outputPath);
    if (!outputPath.parent_path().empty())
        fs::create_directories(outputPath.parent_path());
    fs::remove(outputPath, ec);
    runChecked({"hdiutil",
                "convert",
                rwDmg.string(),
                "-format",
                "UDZO",
                "-imagekey",
                "zlib-level=9",
                "-ov",
                "-o",
                params.outputPath},
               "macOS .dmg compression");

    runChecked({"hdiutil", "verify", params.outputPath}, "macOS .dmg verification");
    verifyMountedMacOSDmgContents(params.outputPath, {pkgName}, {}, {}, "macOS toolchain .dmg");
#endif
}

} // namespace zanna::pkg
