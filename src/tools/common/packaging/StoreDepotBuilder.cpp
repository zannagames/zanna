//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/StoreDepotBuilder.cpp
// Purpose: Implement store depot planning, staging, verification, manifests,
//          and the Steam profile with its SteamPipe app build script.
// Key invariants:
//   - Content is staged in a private sibling of content/<platform>/ and
//     renamed into place only after verification succeeds; the previous
//     content is restored if the final rename fails.
//   - Content paths are unique case-insensitively, because Windows and macOS
//     install depots onto case-insensitive file systems.
//   - Redistributables keep the vendor's bytes; only Zanna-produced code and
//     project DLLs pass through a signer.
// Ownership/Lifetime:
//   - Temporary staging directories are removed on every failure path.
// Links: StoreDepotBuilder.hpp, docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements store depot packaging.

#include "StoreDepotBuilder.hpp"

#include "MacOSPackageBuilder.hpp"
#include "NativeBinaryInspector.hpp"
#include "PkgHash.hpp"
#include "PkgUtils.hpp"
#include "common/Filesystem.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace zanna::pkg {
namespace {

/// @brief Largest Steam app or depot id.
constexpr uint64_t kMaxSteamId = 4294967295ull;

/// @brief Steam depot platform keys in build-script order.
constexpr std::array<std::string_view, 4> kSteamPlatformOrder = {
    "windows", "macos", "linux", "linux-arm64"};

/// @brief Mode of executables and redistributables staged on POSIX hosts (0755).
constexpr fs::perms kExecutableMode =
    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec | fs::perms::group_read |
    fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec;

/// @brief Mode of data files staged on POSIX hosts (0644).
constexpr fs::perms kDataMode =
    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;

/// @brief Convert a path to UTF-8 text.
/// @param path Native path.
/// @return UTF-8 spelling with native separators.
std::string utf8(const fs::path &path) {
    return zanna::filesystem::pathToUtf8(path);
}

/// @brief Lowercase ASCII letters for case-insensitive comparisons.
/// @param text Source text.
/// @return Lowercased copy.
std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// @brief Escape text for a JSON string literal body.
/// @param text UTF-8 text.
/// @return Escaped text without surrounding quotes.
std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

/// @brief Remove a trailing empty path element so component-wise prefix checks work.
/// @param path Path that may end with a separator.
/// @return Equivalent path without a trailing separator.
fs::path withoutTrailingSeparator(fs::path path) {
    while (!path.empty() && path.filename().empty() && path.has_relative_path())
        path = path.parent_path();
    return path;
}

/// @brief Resolve symlinks in the existing prefix of a path, keeping the rest lexical.
/// @param path Absolute path.
/// @return Weakly canonical path, or the lexically normal path when resolution fails.
fs::path comparablePath(const fs::path &path) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec)
        resolved = path.lexically_normal();
    return withoutTrailingSeparator(resolved);
}

//===----------------------------------------------------------------------===//
// Steam profile hooks
//===----------------------------------------------------------------------===//

/// @brief Canonical Steam app id from the configuration, or empty.
/// @param pkg Package configuration.
/// @return Canonical app id.
std::string steamAppIdFor(const PackageConfig &pkg) {
    const auto id = canonicalSteamId(pkg.steamAppId);
    return id ? *id : std::string{};
}

/// @brief Steam depot id mapped to a platform key, or empty.
/// @param pkg Package configuration.
/// @param platformKey Depot platform key.
/// @return Canonical depot id.
std::string steamDepotIdFor(const PackageConfig &pkg, const std::string &platformKey) {
    for (const auto &mapping : pkg.steamDepots) {
        if (mapping.platform == platformKey) {
            const auto id = canonicalSteamId(mapping.depotId);
            return id ? *id : std::string{};
        }
    }
    return {};
}

/// @brief Validate every steam-* setting a Steam depot build consumes.
/// @param pkg Package configuration.
/// @throws std::runtime_error with the ADR 0354 diagnostic.
void validateSteamConfig(const PackageConfig &pkg) {
    if (pkg.steamAppId.empty())
        throw std::runtime_error("Steam depot packaging requires steam-app-id in zanna.project");
    if (!canonicalSteamId(pkg.steamAppId)) {
        throw std::runtime_error("invalid steam-app-id '" + pkg.steamAppId +
                                 "'; expected an integer in 1..4294967295");
    }
    std::map<std::string, std::string> platformByDepot;
    std::set<std::string> platforms;
    for (const auto &mapping : pkg.steamDepots) {
        if (!isSteamDepotPlatform(mapping.platform)) {
            throw std::runtime_error("invalid steam-depot platform '" + mapping.platform +
                                     "'; expected windows, macos, linux, or linux-arm64");
        }
        const auto id = canonicalSteamId(mapping.depotId);
        if (!id) {
            throw std::runtime_error("invalid steam-depot id '" + mapping.depotId +
                                     "'; expected an integer in 1..4294967295");
        }
        if (!platforms.insert(mapping.platform).second)
            throw std::runtime_error("duplicate steam-depot platform '" + mapping.platform + "'");
        const auto [it, inserted] = platformByDepot.emplace(*id, mapping.platform);
        if (!inserted) {
            throw std::runtime_error("steam-depot id " + *id + " is already mapped to " +
                                     it->second);
        }
    }
    if (!pkg.steamBuildDescription.empty())
        validateSteamBuildDescription(pkg.steamBuildDescription);
    if (!pkg.steamSetLive.empty())
        validateSteamSetLiveBranch(pkg.steamSetLive);
}

/// @brief Relative path of the SteamPipe app build script.
/// @param pkg Package configuration.
/// @return `scripts/app_build_<app-id>.vdf`.
std::string steamBuildScriptRelativePath(const PackageConfig &pkg) {
    return "scripts/app_build_" + steamAppIdFor(pkg) + ".vdf";
}

/// @brief Effective SteamPipe build description.
/// @details The default `<project> <version>` replaces the characters a VDF string cannot
///          carry safely with `_`; an explicit description is validated instead.
/// @param pkg Package configuration.
/// @param projectName Project name.
/// @param version Effective version.
/// @return Description text.
std::string steamBuildDescriptionFor(const PackageConfig &pkg,
                                     const std::string &projectName,
                                     const std::string &version) {
    if (!pkg.steamBuildDescription.empty())
        return pkg.steamBuildDescription;
    std::string text = projectName + " " + version;
    for (char &c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\' || uc < 0x20 || uc == 0x7F)
            c = '_';
    }
    return text;
}

/// @brief Branch the Steam build script sets live.
/// @param pkg Package configuration.
/// @return steam-set-live value, or empty.
std::string steamLiveBranchFor(const PackageConfig &pkg) {
    return pkg.steamSetLive;
}

/// @brief Rewrite `scripts/app_build_<app-id>.vdf` from the content directories present.
/// @param input Build root, configuration, and the platform packaged this run.
/// @return Written script and warnings.
/// @throws std::runtime_error when the script cannot be written.
StoreBuildScriptOutcome writeSteamBuildScripts(const StoreBuildScriptInput &input) {
    const PackageConfig &pkg = *input.pkg;
    StoreBuildScriptOutcome outcome;
    const std::string scriptRelative = steamBuildScriptRelativePath(pkg);
    if (steamDepotIdFor(pkg, input.packagedPlatformKey).empty()) {
        outcome.warnings.push_back("no steam-depot is configured for " + input.packagedPlatformKey +
                                   "; " + scriptRelative + " does not upload content/" +
                                   input.packagedPlatformKey);
    }
    std::vector<SteamDepotMapping> depots;
    for (const std::string_view key : kSteamPlatformOrder) {
        const std::string platform(key);
        std::error_code ec;
        if (!fs::is_directory(input.outputRoot / "content" / platform, ec))
            continue;
        const std::string depotId = steamDepotIdFor(pkg, platform);
        if (!depotId.empty())
            depots.push_back({platform, depotId});
    }
    if (depots.empty())
        return outcome;
    const fs::path scriptPath = input.outputRoot / zanna::filesystem::pathFromUtf8(scriptRelative);
    writeTextFileAtomic(
        scriptPath,
        renderSteamAppBuildScript(steamAppIdFor(pkg),
                                  steamBuildDescriptionFor(pkg, input.projectName, input.version),
                                  pkg.steamSetLive,
                                  depots));
    outcome.writtenPaths.push_back(utf8(scriptPath));
    return outcome;
}

/// @brief steamcmd command line that uploads an app build script.
/// @param scriptPath Absolute script path.
/// @return Command with an `<account>` placeholder.
std::string steamUploadCommand(const std::string &scriptPath) {
    return "steamcmd +login <account> +run_app_build \"" + scriptPath + "\" +quit";
}

/// @brief Build the Steam profile row.
/// @return Steam profile.
StoreProfile makeSteamProfile() {
    StoreProfile p;
    p.kind = StoreKind::Steam;
    p.id = "steam";
    p.displayName = "Steam";
    p.sdkName = "Steamworks";
    p.supportedSdk = "Steamworks SDK 1.61-1.65";
    p.redistDirective = "steam-redist";
    p.redistOption = "--steam-redist";
    p.redistRootSubdirectories = {"redistributable_bin", "sdk/redistributable_bin"};
    p.redistributables = {
        {"windows", DepotOs::Windows, "x64", "win64/steam_api64.dll", "steam_api64.dll"},
        {"macos", DepotOs::MacOS, "", "osx/libsteam_api.dylib", "libsteam_api.dylib"},
        {"linux", DepotOs::Linux, "x64", "linux64/libsteam_api.so", "libsteam_api.so"},
        {"linux-arm64", DepotOs::Linux, "arm64", "linuxarm64/libsteam_api.so", "libsteam_api.so"},
    };
    p.unsupportedPlatforms = {
        {DepotOs::Windows,
         "arm64",
         "Steam Windows depots are x64-only: Valve ships no Windows arm64 Steamworks "
         "redistributable"},
    };
    // The core export table ADR 0352 resolves all-or-nothing at run time.
    p.requiredExports = {"SteamAPI_InitFlat",
                         "SteamAPI_Shutdown",
                         "SteamAPI_RestartAppIfNecessary",
                         "SteamAPI_IsSteamRunning",
                         "SteamAPI_GetHSteamPipe",
                         "SteamAPI_ManualDispatch_Init",
                         "SteamAPI_ManualDispatch_RunFrame",
                         "SteamAPI_ManualDispatch_GetNextCallback",
                         "SteamAPI_ManualDispatch_FreeLastCallback",
                         "SteamAPI_ManualDispatch_GetAPICallResult"};
    p.providerMarker = "SteamAPI_InitFlat";
    p.providerDescription = "the Zanna.Services Steam provider";
    p.providerRuntimeName = "Zanna.Services";
    p.requiredMacOSEntitlements = {"com.apple.security.cs.disable-library-validation",
                                   "com.apple.security.cs.allow-dyld-environment-variables"};
    p.forbiddenMacOSEntitlements = {"com.apple.security.app-sandbox"};
    p.forbiddenContentFiles = {
        {"steam_appid.txt", "Zanna.Services.Platform.Init sets SteamAppId itself"}};
    p.outputSuffix = "steam";
    p.validateConfig = validateSteamConfig;
    p.appId = steamAppIdFor;
    p.depotId = steamDepotIdFor;
    p.buildScriptRelativePath = steamBuildScriptRelativePath;
    p.buildDescription = steamBuildDescriptionFor;
    p.liveBranch = steamLiveBranchFor;
    p.writeBuildScripts = writeSteamBuildScripts;
    p.uploadCommand = steamUploadCommand;
    return p;
}

//===----------------------------------------------------------------------===//
// Planning helpers
//===----------------------------------------------------------------------===//

/// @brief Find the redistributable slot for a platform key.
/// @param profile Store profile.
/// @param platformKey Depot platform key.
/// @return Slot row.
/// @throws std::runtime_error when the profile has no such key.
const StoreRedistributableSlot &slotFor(const StoreProfile &profile,
                                        const std::string &platformKey) {
    for (const auto &slot : profile.redistributables) {
        if (slot.platformKey == platformKey)
            return slot;
    }
    throw std::runtime_error(profile.displayName + " has no depot platform '" + platformKey + "'");
}

/// @brief Container format a payload of @p os must use.
/// @param os Payload operating system.
/// @return Native format.
NativeBinaryFormat formatFor(DepotOs os) {
    switch (os) {
        case DepotOs::Windows:
            return NativeBinaryFormat::PE;
        case DepotOs::MacOS:
            return NativeBinaryFormat::MachO;
        case DepotOs::Linux:
            break;
    }
    return NativeBinaryFormat::ELF;
}

/// @brief Diagnostic name, with its article, of the shared-library kind a slot requires.
/// @param os Payload operating system.
/// @return For example `a Mach-O dynamic library`.
std::string libraryKindName(DepotOs os) {
    switch (os) {
        case DepotOs::Windows:
            return "a PE32+ DLL";
        case DepotOs::MacOS:
            return "a Mach-O dynamic library";
        case DepotOs::Linux:
            break;
    }
    return "an ELF shared object";
}

/// @brief Reject an output root that lies inside a directory the build reads from.
/// @param profile Store profile.
/// @param params Depot inputs.
/// @param root Absolute output root.
/// @throws std::runtime_error when the output would copy or pack itself.
void rejectOutputInsideSources(const StoreProfile &profile,
                               const StoreDepotParams &params,
                               const fs::path &root) {
    const fs::path comparableRoot = comparablePath(root);
    const fs::path projectRoot = zanna::filesystem::pathFromUtf8(params.projectRoot);
    /// @brief Check one project-relative source.
    /// @param source Source text as written in the manifest.
    auto check = [&](const std::string &source) {
        const fs::path resolved =
            resolvePackageSourcePath(projectRoot, source, "asset source path");
        std::error_code ec;
        if (!fs::is_directory(resolved, ec))
            return;
        if (isPathWithin(comparablePath(resolved), comparableRoot)) {
            throw std::runtime_error(profile.displayName + " depot output directory '" +
                                     utf8(root) + "' is inside asset source '" + source + "'");
        }
    };
    for (const auto &asset : params.pkgConfig.assets)
        check(asset.sourcePath);
    for (const auto &source : params.extraSourcePaths)
        check(source);
}

/// @brief Locate the SDK redistributable for a slot.
/// @param profile Store profile.
/// @param slot Redistributable slot.
/// @param params Depot inputs carrying the configured directory.
/// @return Absolute path of the redistributable file.
/// @throws std::runtime_error when the file does not exist.
fs::path resolveRedistributable(const StoreProfile &profile,
                                const StoreRedistributableSlot &slot,
                                const StoreDepotParams &params) {
    fs::path dir = zanna::filesystem::pathFromUtf8(params.redistSource);
    if (!dir.is_absolute()) {
        fs::path base = params.redistBaseDir.empty()
                            ? fs::current_path()
                            : zanna::filesystem::pathFromUtf8(params.redistBaseDir);
        dir = base / dir;
    }
    dir = withoutTrailingSeparator(dir.lexically_normal());
    fs::path redistRoot = dir;
    for (const auto &subdirectory : profile.redistRootSubdirectories) {
        std::error_code ec;
        const fs::path candidate = dir / zanna::filesystem::pathFromUtf8(subdirectory);
        if (fs::is_directory(candidate, ec)) {
            redistRoot = candidate;
            break;
        }
    }
    const fs::path file =
        (redistRoot / zanna::filesystem::pathFromUtf8(slot.sdkRelativePath)).lexically_normal();
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        const std::string origin =
            params.redistOrigin.empty() ? profile.redistDirective : params.redistOrigin;
        throw std::runtime_error(profile.displayName + " redistributable not found: expected " +
                                 utf8(file) + " (from " + origin + " '" + params.redistSource +
                                 "')");
    }
    return file;
}

/// @brief Check a redistributable's format, architectures, and export names.
/// @param profile Store profile.
/// @param slot Redistributable slot.
/// @param file Redistributable file.
/// @param requiredArchitectures Architectures the executable needs.
/// @throws std::runtime_error with the ADR 0354 diagnostic.
void validateRedistributable(const StoreProfile &profile,
                             const StoreRedistributableSlot &slot,
                             const fs::path &file,
                             const std::vector<std::string> &requiredArchitectures) {
    const std::vector<uint8_t> bytes = readFile(file);
    const std::string label = utf8(file);
    const auto info = inspectNativeBinary(bytes);
    const bool formatOk = info && info->format == formatFor(slot.os) &&
                          info->kind == NativeBinaryKind::SharedLibrary &&
                          (slot.os != DepotOs::Windows || info->pe32Plus);
    if (!formatOk) {
        throw std::runtime_error(profile.displayName + " redistributable '" + label + "' is not " +
                                 libraryKindName(slot.os));
    }
    const std::vector<std::string> architectures =
        slot.arch.empty() ? requiredArchitectures : std::vector<std::string>{slot.arch};
    for (const auto &arch : architectures) {
        if (std::find(info->architectures.begin(), info->architectures.end(), arch) ==
            info->architectures.end()) {
            throw std::runtime_error(profile.displayName + " redistributable '" + label +
                                     "' does not contain " + arch + " code");
        }
    }
    for (const auto &name : profile.requiredExports) {
        if (!binaryContainsSymbolName(bytes, name)) {
            throw std::runtime_error(profile.displayName + " redistributable '" + label +
                                     "' is not a " + profile.supportedSdk +
                                     " redistributable: export name '" + name + "' not found");
        }
    }
}

/// @brief Throw when a content path names a forbidden file.
/// @param profile Store profile.
/// @param contentRelativePath Path relative to the content directory.
/// @throws std::runtime_error with the ADR 0354 diagnostic.
void rejectForbiddenContentPath(const StoreProfile &profile,
                                const std::string &contentRelativePath) {
    const size_t slash = contentRelativePath.find_last_of('/');
    const std::string leaf = lowerAscii(
        slash == std::string::npos ? std::string_view(contentRelativePath)
                                   : std::string_view(contentRelativePath).substr(slash + 1));
    for (const auto &forbidden : profile.forbiddenContentFiles) {
        if (leaf == lowerAscii(forbidden.fileName)) {
            throw std::runtime_error(forbidden.fileName + " must not ship in a " +
                                     profile.displayName + " depot (found " + contentRelativePath +
                                     "); " + forbidden.reason);
        }
    }
}

//===----------------------------------------------------------------------===//
// Staging
//===----------------------------------------------------------------------===//

/// @brief Removes a directory tree on destruction unless released.
class ScopedTreeRemoval {
  public:
    /// @brief Own a directory tree.
    /// @param path Tree removed at destruction.
    explicit ScopedTreeRemoval(fs::path path) : path_(std::move(path)) {}

    /// @brief Remove the tree unless released.
    ~ScopedTreeRemoval() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

    ScopedTreeRemoval(const ScopedTreeRemoval &) = delete;
    ScopedTreeRemoval &operator=(const ScopedTreeRemoval &) = delete;

    /// @brief Keep the tree.
    void release() {
        path_.clear();
    }

  private:
    fs::path path_;
};

/// @brief Writes files into a staging directory and rejects colliding content paths.
class DepotContentWriter {
  public:
    /// @brief Bind the writer to a staging directory.
    /// @param profile Store profile for diagnostics.
    /// @param os Payload operating system (Windows paths get file-name checks).
    /// @param root Staging directory.
    DepotContentWriter(const StoreProfile &profile, DepotOs os, fs::path root)
        : profile_(profile), os_(os), root_(std::move(root)) {}

    /// @brief Write bytes at a content-relative path.
    /// @param relativePath Destination relative to the content directory.
    /// @param data File bytes.
    /// @param mode POSIX permissions.
    void writeBytes(const std::string &relativePath,
                    const std::vector<uint8_t> &data,
                    fs::perms mode) {
        const fs::path target = reserve(relativePath);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write depot file: " + utf8(target));
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out)
            throw std::runtime_error("failed while writing depot file: " + utf8(target));
        applyMode(target, mode);
    }

    /// @brief Copy a file to a content-relative path.
    /// @param relativePath Destination relative to the content directory.
    /// @param source Source file.
    /// @param mode POSIX permissions.
    void copyFile(const std::string &relativePath, const fs::path &source, fs::perms mode) {
        const fs::path target = reserve(relativePath);
        std::error_code ec;
        fs::copy_file(source, target, fs::copy_options::none, ec);
        if (ec) {
            throw std::runtime_error("cannot copy '" + utf8(source) +
                                     "' into depot content: " + ec.message());
        }
        applyMode(target, mode);
    }

  private:
    /// @brief Validate and claim a content path, creating its parent directory.
    /// @param relativePath Candidate content-relative path.
    /// @return Absolute destination path.
    fs::path reserve(const std::string &relativePath) {
        const std::string clean = sanitizePackageRelativePath(relativePath, "depot content path");
        if (clean.empty())
            throw std::runtime_error("depot content path must not be empty");
        if (os_ == DepotOs::Windows) {
            size_t start = 0;
            while (start <= clean.size()) {
                const size_t slash = clean.find('/', start);
                const std::string segment = clean.substr(
                    start, slash == std::string::npos ? std::string::npos : slash - start);
                validateWindowsFileName(segment, "Windows depot content path");
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }
        if (!claimed_.insert(lowerAscii(clean)).second) {
            throw std::runtime_error(profile_.displayName +
                                     " depot content path collision: " + clean);
        }
        const fs::path target = root_ / zanna::filesystem::pathFromUtf8(clean);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("cannot create depot directory '" +
                                     utf8(target.parent_path()) + "': " + ec.message());
        }
        return target;
    }

    /// @brief Apply POSIX permissions (a no-op beyond read-only bits on Windows hosts).
    /// @param target File to update.
    /// @param mode Permissions.
    static void applyMode(const fs::path &target, fs::perms mode) {
        std::error_code ec;
        fs::permissions(target, mode, fs::perm_options::replace, ec);
        if (ec) {
            throw std::runtime_error("cannot set depot file permissions: " + utf8(target) + ": " +
                                     ec.message());
        }
    }

    const StoreProfile &profile_;
    DepotOs os_;
    fs::path root_;
    std::set<std::string> claimed_;
};

/// @brief Mode that preserves whether a source file is executable.
/// @param source Source file.
/// @return 0755 for executable sources, otherwise 0644.
fs::perms normalizedMode(const fs::path &source) {
    std::error_code ec;
    const fs::perms perms = fs::status(source, ec).permissions();
    if (ec)
        return kDataMode;
    const fs::perms execBits =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    return (perms & execBits) != fs::perms::none ? kExecutableMode : kDataMode;
}

/// @brief Enumerate the content paths `asset` directives produce under @p prefix.
/// @param params Depot inputs.
/// @param prefix Content-relative directory the assets are placed under ("" for beside the exe).
/// @param visit Receives each content-relative path and its resolved source file.
void forEachAssetFile(const StoreDepotParams &params,
                      const std::string &prefix,
                      const std::function<void(const std::string &, const fs::path &)> &visit) {
    const fs::path projectRoot = zanna::filesystem::pathFromUtf8(params.projectRoot);
    for (const auto &asset : params.pkgConfig.assets) {
        const fs::path source =
            resolvePackageSourcePath(projectRoot, asset.sourcePath, "asset source path");
        const std::string targetDir =
            joinPackageRelativePath(prefix, asset.targetPath, "asset target path");
        std::error_code ec;
        if (fs::is_directory(source, ec)) {
            /// @brief Visit one regular file of a directory asset.
            /// @param entry Safely resolved directory entry.
            safeDirectoryIterateResolved(source, projectRoot, [&](const SafeDirectoryEntry &entry) {
                if (!entry.regularFile)
                    return;
                const std::string relative =
                    sanitizePackageRelativePath(zanna::filesystem::genericPathToUtf8(
                                                    entry.logicalPath.lexically_relative(source)),
                                                "asset path");
                visit(joinPackageRelativePath(targetDir, relative, "asset path"),
                      entry.resolvedPath);
            });
            continue;
        }
        ec.clear();
        if (!fs::is_regular_file(source, ec))
            throw std::runtime_error("asset is not a regular file or directory: " +
                                     asset.sourcePath);
        // Name the file as the manifest does, not after a symlink target, like the installers.
        const std::string sourceRel =
            sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string leaf = zanna::filesystem::genericPathToUtf8(
            zanna::filesystem::pathFromUtf8(sourceRel).filename());
        visit(joinPackageRelativePath(targetDir, leaf, "asset path"), source);
    }
}

/// @brief Reject forbidden files among the inputs before any signing or notarization.
/// @param profile Store profile.
/// @param params Depot inputs.
/// @param plan Resolved plan.
void rejectForbiddenInputs(const StoreProfile &profile,
                           const StoreDepotParams &params,
                           const StoreDepotPlan &plan) {
    std::string assetPrefix;
    if (params.os == DepotOs::MacOS) {
        const size_t appEnd = plan.executableRelativePath.find(".app/");
        assetPrefix = plan.executableRelativePath.substr(0, appEnd + 4) + "/Contents/Resources";
    }
    /// @brief Check one staged asset path.
    /// @param relativePath Content-relative destination.
    forEachAssetFile(params, assetPrefix, [&](const std::string &relativePath, const fs::path &) {
        rejectForbiddenContentPath(profile, relativePath);
    });
    for (const auto &pack : params.packFiles) {
        const std::string leaf =
            zanna::filesystem::genericPathToUtf8(zanna::filesystem::pathFromUtf8(pack).filename());
        rejectForbiddenContentPath(profile, assetPrefix.empty() ? leaf : assetPrefix + "/" + leaf);
    }
    for (const auto &dll : params.pkgConfig.windowsDlls) {
        if (params.os == DepotOs::Windows)
            rejectForbiddenContentPath(profile, dll);
    }
}

/// @brief Stage a Windows or Linux depot: the executable with its libraries, assets, and packs.
/// @param profile Store profile.
/// @param params Depot inputs.
/// @param plan Resolved plan.
/// @param staging Staging directory.
void stageFlatDepot(const StoreProfile &profile,
                    const StoreDepotParams &params,
                    const StoreDepotPlan &plan,
                    const fs::path &staging) {
    DepotContentWriter writer(profile, params.os, staging);
    const bool windows = params.os == DepotOs::Windows;
    const fs::path executable = zanna::filesystem::pathFromUtf8(params.executablePath);
    if (windows) {
        writer.writeBytes(plan.executableRelativePath,
                          signWindowsApplicationPe(params.windowsSigner,
                                                   plan.executableRelativePath,
                                                   readFile(executable),
                                                   params.arch),
                          kExecutableMode);
        for (const auto &dll : collectWindowsAppLocalDlls(params.executablePath,
                                                          params.projectRoot,
                                                          params.pkgConfig,
                                                          params.windowsCompilerRuntimeDir)) {
            writer.writeBytes(dll.installRelativePath,
                              signWindowsApplicationPe(params.windowsSigner,
                                                       dll.installRelativePath,
                                                       readFile(dll.sourcePath),
                                                       params.arch),
                              kDataMode);
        }
    } else {
        writer.copyFile(plan.executableRelativePath, executable, kExecutableMode);
    }
    if (!plan.redistSource.empty()) {
        // Vendor bytes and signature are kept; the library is loaded at run time, not imported.
        writer.copyFile(plan.redistStagedPath,
                        zanna::filesystem::pathFromUtf8(plan.redistSource),
                        windows ? kDataMode : kExecutableMode);
    }
    /// @brief Copy one asset file.
    /// @param relativePath Content-relative destination.
    /// @param source Resolved source.
    forEachAssetFile(params, "", [&](const std::string &relativePath, const fs::path &source) {
        writer.copyFile(relativePath, source, normalizedMode(source));
    });
    for (const auto &pack : params.packFiles) {
        const fs::path source = zanna::filesystem::pathFromUtf8(pack);
        writer.copyFile(zanna::filesystem::genericPathToUtf8(source.filename()), source, kDataMode);
    }
}

/// @brief Stage a macOS depot: one signed application bundle.
/// @param profile Store profile.
/// @param params Depot inputs.
/// @param plan Resolved plan.
/// @param staging Staging directory.
void stageMacOSDepot(const StoreProfile &profile,
                     const StoreDepotParams &params,
                     const StoreDepotPlan &plan,
                     const fs::path &staging) {
    MacOSAppDirectoryParams app;
    app.app.projectName = params.projectName;
    app.app.version = params.version;
    app.app.executablePath = params.executablePath;
    app.app.projectRoot = params.projectRoot;
    app.app.pkgConfig = params.pkgConfig;
    app.destinationDir = utf8(staging);
    if (!plan.redistSource.empty()) {
        const size_t appEnd = plan.redistStagedPath.find(".app/");
        app.extraFiles.push_back(
            {plan.redistSource, plan.redistStagedPath.substr(appEnd + 5), true});
    }
    app.app.packFiles = params.packFiles;
    app.requiredEntitlements = profile.requiredMacOSEntitlements;
    app.forbiddenEntitlements = profile.forbiddenMacOSEntitlements;
    app.entitlementsOwner = profile.displayName;
    const MacOSAppDirectoryResult staged = buildMacOSAppDirectory(app);
    if (staged.executableRelativePath != plan.executableRelativePath) {
        throw std::runtime_error("staged macOS executable '" + staged.executableRelativePath +
                                 "' does not match the planned launch path '" +
                                 plan.executableRelativePath + "'");
    }
}

/// @brief Walk staged content, reject forbidden files, and record sizes and hashes.
/// @param profile Store profile.
/// @param plan Resolved plan.
/// @param staging Staging directory.
/// @return Files sorted by path.
std::vector<StoreDepotFile> verifyDepotContent(const StoreProfile &profile,
                                               const StoreDepotPlan &plan,
                                               const fs::path &staging) {
    std::vector<StoreDepotFile> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(staging, ec);
    if (ec)
        throw std::runtime_error("cannot read staged depot content: " + ec.message());
    const fs::recursive_directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot read staged depot content: " + ec.message());
        const fs::path path = it->path();
        const std::string relative =
            zanna::filesystem::genericPathToUtf8(path.lexically_relative(staging));
        const fs::file_status status = it->symlink_status(ec);
        if (ec)
            throw std::runtime_error("cannot inspect staged depot file " + relative + ": " +
                                     ec.message());
        if (fs::is_symlink(status)) {
            throw std::runtime_error(profile.displayName +
                                     " depot content must not contain symbolic links: " + relative);
        }
        if (fs::is_directory(status))
            continue;
        if (!fs::is_regular_file(status)) {
            throw std::runtime_error(
                profile.displayName +
                " depot content must contain only files and directories: " + relative);
        }
        rejectForbiddenContentPath(profile, relative);
        const std::vector<uint8_t> bytes = readFile(path);
        files.push_back({relative, bytes.size(), sha256Hex(bytes.data(), bytes.size())});
    }
    // A failed final increment also ends the loop, so check once more.
    if (ec)
        throw std::runtime_error("cannot read staged depot content: " + ec.message());
    std::sort(files.begin(), files.end(), [](const StoreDepotFile &a, const StoreDepotFile &b) {
        return a.path < b.path;
    });
    /// @brief Require one planned file in the staged list.
    /// @param relativePath Planned content-relative path.
    auto require = [&](const std::string &relativePath) {
        const bool found = std::any_of(files.begin(), files.end(), [&](const StoreDepotFile &file) {
            return file.path == relativePath;
        });
        if (!found) {
            throw std::runtime_error(profile.displayName + " depot content is missing " +
                                     relativePath);
        }
    };
    require(plan.executableRelativePath);
    if (!plan.redistStagedPath.empty())
        require(plan.redistStagedPath);
    return files;
}

/// @brief Move verified staging content into place, restoring the previous tree on failure.
/// @param staging Verified staging directory.
/// @param target Final `content/<platform>` directory.
void installStagedContent(const fs::path &staging, const fs::path &target) {
    std::error_code ec;
    const bool hadPrevious = fs::exists(fs::symlink_status(target, ec));
    if (!hadPrevious) {
        fs::rename(staging, target, ec);
        if (ec) {
            throw std::runtime_error("cannot move depot content into place at '" + utf8(target) +
                                     "': " + ec.message());
        }
        return;
    }
    fs::path previous;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const fs::path candidate =
            target.parent_path() /
            zanna::filesystem::pathFromUtf8("." + utf8(target.filename()) + "-previous-" +
                                            uniqueTempSuffix(attempt));
        std::error_code probe;
        if (!fs::exists(fs::symlink_status(candidate, probe))) {
            previous = candidate;
            break;
        }
    }
    if (previous.empty())
        throw std::runtime_error("cannot choose a name for the previous depot content");
    fs::rename(target, previous, ec);
    if (ec) {
        throw std::runtime_error("cannot move previous depot content aside at '" + utf8(target) +
                                 "': " + ec.message());
    }
    fs::rename(staging, target, ec);
    if (ec) {
        std::error_code restore;
        fs::rename(previous, target, restore);
        throw std::runtime_error("cannot move depot content into place at '" + utf8(target) +
                                 "': " + ec.message());
    }
    fs::remove_all(previous, ec);
}

/// @brief Trust label recorded in the depot manifest.
/// @param params Depot inputs.
/// @return `authenticode`, `unsigned`, or the macOS signing label.
std::string depotTrustLabel(const StoreDepotParams &params) {
    switch (params.os) {
        case DepotOs::Windows:
            return params.windowsSigner ? "authenticode" : "unsigned";
        case DepotOs::MacOS: {
            const std::string mode = resolveMacOSSignModeForHost(params.pkgConfig);
            if (mode == "developer-id")
                return params.pkgConfig.macosNotaryProfile.empty() ? "developer-id"
                                                                   : "developer-id+notarized";
            return mode;
        }
        case DepotOs::Linux:
            break;
    }
    return "unsigned";
}

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// @brief Look up a store profile.
/// @param store Store identity.
/// @return Immutable profile.
const StoreProfile &storeProfile(StoreKind store) {
    switch (store) {
        case StoreKind::Steam:
            break;
    }
    static const StoreProfile steam = makeSteamProfile();
    return steam;
}

/// @brief Human-readable operating system name.
/// @param os Payload operating system.
/// @return `Windows`, `macOS`, or `Linux`.
std::string depotOsDisplayName(DepotOs os) {
    switch (os) {
        case DepotOs::Windows:
            return "Windows";
        case DepotOs::MacOS:
            return "macOS";
        case DepotOs::Linux:
            break;
    }
    return "Linux";
}

/// @brief Select the depot platform key for an operating system and architecture.
/// @param profile Store profile.
/// @param os Payload operating system.
/// @param arch Requested architecture.
/// @return Platform key.
/// @throws std::runtime_error when unsupported.
std::string storeDepotPlatformKey(const StoreProfile &profile,
                                  DepotOs os,
                                  const std::string &arch) {
    if (arch != "x64" && arch != "arm64") {
        throw std::runtime_error(profile.displayName +
                                 " depot architecture must be x64 or arm64: " + arch);
    }
    for (const auto &unsupported : profile.unsupportedPlatforms) {
        if (unsupported.os == os && unsupported.arch == arch)
            throw std::runtime_error(unsupported.message);
    }
    for (const auto &slot : profile.redistributables) {
        if (slot.os == os && (slot.arch.empty() || slot.arch == arch))
            return slot.platformKey;
    }
    throw std::runtime_error(profile.displayName + " depots do not support " +
                             depotOsDisplayName(os) + " " + arch);
}

/// @brief Canonicalize a Steam app or depot id.
/// @param text Decimal digits.
/// @return Canonical value, or nullopt when outside 1..4294967295.
std::optional<std::string> canonicalSteamId(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    for (char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
    }
    size_t first = text.find_first_not_of('0');
    if (first == std::string_view::npos)
        return std::nullopt;
    const std::string_view digits = text.substr(first);
    if (digits.size() > 10)
        return std::nullopt;
    uint64_t value = 0;
    for (char c : digits)
        value = value * 10u + static_cast<uint64_t>(c - '0');
    if (value == 0 || value > kMaxSteamId)
        return std::nullopt;
    return std::string(digits);
}

/// @brief Report whether @p key is a Steam depot platform key.
/// @param key Candidate key.
/// @return True for a known key.
bool isSteamDepotPlatform(std::string_view key) {
    return std::find(kSteamPlatformOrder.begin(), kSteamPlatformOrder.end(), key) !=
           kSteamPlatformOrder.end();
}

/// @brief Validate a steam-build-description value.
/// @param description Candidate description.
/// @throws std::runtime_error on a quote, backslash, or control character.
void validateSteamBuildDescription(const std::string &description) {
    if (description.find('"') != std::string::npos || description.find('\\') != std::string::npos)
        throw std::runtime_error("steam-build-description must not contain '\"' or '\\'");
    validateSingleLineField(description, "steam-build-description");
}

/// @brief Validate a steam-set-live branch name.
/// @param branch Candidate branch.
/// @throws std::runtime_error on invalid characters or the default branch.
void validateSteamSetLiveBranch(const std::string &branch) {
    const bool valid = !branch.empty() && std::all_of(branch.begin(), branch.end(), [](char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_' || c == '.' || c == '-';
    });
    if (!valid) {
        throw std::runtime_error("invalid steam-set-live branch '" + branch +
                                 "'; use letters, digits, '_', '.', or '-'");
    }
    if (lowerAscii(branch) == "default") {
        throw std::runtime_error("steam-set-live cannot be 'default': Steam sets the default "
                                 "branch live only through the App Admin panel");
    }
}

/// @brief Render a SteamPipe app build script.
/// @param appId Canonical app id.
/// @param description Build description.
/// @param setLive Branch to set live, or empty.
/// @param depots Depots in script order.
/// @return VDF text.
/// @throws std::runtime_error when a value cannot be written safely.
std::string renderSteamAppBuildScript(const std::string &appId,
                                      const std::string &description,
                                      const std::string &setLive,
                                      const std::vector<SteamDepotMapping> &depots) {
    if (canonicalSteamId(appId) != std::optional<std::string>(appId))
        throw std::runtime_error("invalid steam-app-id '" + appId +
                                 "'; expected an integer in 1..4294967295");
    validateSteamBuildDescription(description);
    if (!setLive.empty())
        validateSteamSetLiveBranch(setLive);
    std::ostringstream out;
    out << "\"AppBuild\"\n"
        << "{\n"
        << "\t\"AppID\" \"" << appId << "\"\n"
        << "\t\"Desc\" \"" << description << "\"\n"
        << "\t\"ContentRoot\" \"../content/\"\n"
        << "\t\"BuildOutput\" \"../output/\"\n";
    if (!setLive.empty())
        out << "\t\"SetLive\" \"" << setLive << "\"\n";
    out << "\t\"Depots\"\n"
        << "\t{\n";
    for (const auto &depot : depots) {
        if (!isSteamDepotPlatform(depot.platform) ||
            canonicalSteamId(depot.depotId) != std::optional<std::string>(depot.depotId)) {
            throw std::runtime_error("invalid steam-depot mapping '" + depot.platform + " " +
                                     depot.depotId + "'");
        }
        out << "\t\t\"" << depot.depotId << "\"\n"
            << "\t\t{\n"
            << "\t\t\t\"FileMapping\"\n"
            << "\t\t\t{\n"
            << "\t\t\t\t\"LocalPath\" \"" << depot.platform << "/*\"\n"
            << "\t\t\t\t\"DepotPath\" \".\"\n"
            << "\t\t\t\t\"recursive\" \"1\"\n"
            << "\t\t\t}\n"
            << "\t\t}\n";
    }
    out << "\t}\n"
        << "}\n";
    return out.str();
}

/// @brief Resolve and validate a depot without writing anything.
/// @param params Depot inputs.
/// @return Plan facts.
/// @throws std::runtime_error on any violation.
StoreDepotPlan planStoreDepot(const StoreDepotParams &params) {
    const StoreProfile &profile = storeProfile(params.store);
    const PackageConfig &pkg = params.pkgConfig;
    profile.validateConfig(pkg);

    StoreDepotPlan plan;
    plan.store = profile.id;
    plan.storeName = profile.displayName;
    plan.arch = params.arch;
    plan.platformKey = storeDepotPlatformKey(profile, params.os, params.arch);
    const StoreRedistributableSlot &slot = slotFor(profile, plan.platformKey);
    plan.appId = profile.appId(pkg);
    plan.depotId = profile.depotId(pkg, plan.platformKey);
    plan.buildDescription = profile.buildDescription(pkg, params.projectName, params.version);
    plan.setLive = profile.liveBranch(pkg);

    if (params.outputRoot.empty())
        throw std::runtime_error(profile.displayName + " depot output directory must not be empty");
    fs::path root = zanna::filesystem::pathFromUtf8(params.outputRoot);
    if (!root.is_absolute())
        root = fs::current_path() / root;
    root = withoutTrailingSeparator(root.lexically_normal());
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_directory(root, ec)) {
        throw std::runtime_error(profile.displayName + " depot output '" + utf8(root) +
                                 "' exists and is not a directory");
    }
    rejectOutputInsideSources(profile, params, root);
    plan.outputRoot = utf8(root);
    plan.contentDir = utf8(root / "content" / zanna::filesystem::pathFromUtf8(plan.platformKey));
    plan.manifestPath =
        utf8(root / "manifests" / zanna::filesystem::pathFromUtf8(plan.platformKey + ".json"));
    plan.buildScriptPath =
        utf8(root / zanna::filesystem::pathFromUtf8(profile.buildScriptRelativePath(pkg)));

    const std::string exec = normalizeExecName(params.projectName);
    switch (params.os) {
        case DepotOs::Windows:
            plan.executableRelativePath = exec + ".exe";
            plan.launchPath = plan.executableRelativePath;
            plan.redistStagedPath = slot.stagedName;
            break;
        case DepotOs::MacOS: {
            const std::string bundle =
                (pkg.displayName.empty() ? params.projectName : pkg.displayName) + ".app";
            plan.executableRelativePath = bundle + "/Contents/MacOS/" + exec;
            plan.launchPath = bundle;
            plan.redistStagedPath = bundle + "/Contents/MacOS/" + slot.stagedName;
            plan.requiredEntitlements = profile.requiredMacOSEntitlements;
            plan.forbiddenEntitlements = profile.forbiddenMacOSEntitlements;
            plan.macosSignMode = resolveMacOSSignModeForHost(pkg);
            break;
        }
        case DepotOs::Linux:
            plan.executableRelativePath = exec;
            plan.launchPath = exec;
            plan.redistStagedPath = slot.stagedName;
            break;
    }

    std::vector<std::string> requiredArchitectures = {params.arch};
    std::vector<uint8_t> executableBytes;
    if (!params.executablePath.empty()) {
        executableBytes = readFile(params.executablePath);
        const auto info = inspectNativeBinary(executableBytes);
        const NativeBinaryFormat expected = formatFor(params.os);
        if (!info || info->format != expected) {
            throw std::runtime_error("executable '" + params.executablePath + "' is not a " +
                                     nativeBinaryFormatName(expected) + " file");
        }
        if (std::find(info->architectures.begin(), info->architectures.end(), params.arch) ==
            info->architectures.end()) {
            throw std::runtime_error("executable '" + params.executablePath +
                                     "' does not contain " + params.arch + " code");
        }
        plan.executableArchitectures = info->architectures;
        plan.executableInspected = true;
        plan.usesProvider = binaryContainsSymbolName(executableBytes, profile.providerMarker);
        if (params.os == DepotOs::MacOS)
            requiredArchitectures = info->architectures;
    }

    if (params.redistSource.empty()) {
        plan.redistStagedPath.clear();
    } else {
        const fs::path redist = resolveRedistributable(profile, slot, params);
        validateRedistributable(profile, slot, redist, requiredArchitectures);
        plan.redistSource = utf8(redist);
    }

    if (plan.executableInspected) {
        const size_t slash = plan.executableRelativePath.find_last_of('/');
        const std::string exeName = slash == std::string::npos
                                        ? plan.executableRelativePath
                                        : plan.executableRelativePath.substr(slash + 1);
        if (plan.usesProvider && plan.redistSource.empty()) {
            throw std::runtime_error("the executable uses " + profile.providerDescription +
                                     ", but no " + profile.sdkName +
                                     " redistributable is configured; set " +
                                     profile.redistDirective + " or pass " + profile.redistOption);
        }
        if (!plan.usesProvider && !plan.redistSource.empty()) {
            plan.warnings.push_back(exeName + " does not use " + profile.providerRuntimeName +
                                    "; the staged " + profile.sdkName +
                                    " redistributable is not loaded");
        }
    }
    return plan;
}

/// @brief Stage, verify, and install one platform depot and refresh build scripts.
/// @param params Depot inputs.
/// @return Build facts.
/// @throws std::runtime_error on failure; installed content is left untouched.
StoreDepotResult buildStoreDepot(const StoreDepotParams &params) {
    const StoreProfile &profile = storeProfile(params.store);
    if (params.executablePath.empty())
        throw std::runtime_error(profile.displayName + " depot packaging requires an executable");

    StoreDepotResult result;
    result.plan = planStoreDepot(params);
    const StoreDepotPlan &plan = result.plan;
    result.warnings = plan.warnings;
    rejectForbiddenInputs(profile, params, plan);

    const fs::path root = zanna::filesystem::pathFromUtf8(plan.outputRoot);
    const fs::path target = zanna::filesystem::pathFromUtf8(plan.contentDir);
    const fs::path staging =
        createUniqueTempDirectory(root / "content", "." + plan.platformKey + "-staging");
    ScopedTreeRemoval stagingCleanup(staging);

    if (params.os == DepotOs::MacOS)
        stageMacOSDepot(profile, params, plan, staging);
    else
        stageFlatDepot(profile, params, plan, staging);
    result.files = verifyDepotContent(profile, plan, staging);
    result.trust = depotTrustLabel(params);

    installStagedContent(staging, target);
    stagingCleanup.release();

    writeTextFileAtomic(zanna::filesystem::pathFromUtf8(plan.manifestPath),
                        renderStoreDepotManifest(result, params.version));
    StoreBuildScriptInput scriptInput;
    scriptInput.pkg = &params.pkgConfig;
    scriptInput.projectName = params.projectName;
    scriptInput.version = params.version;
    scriptInput.outputRoot = root;
    scriptInput.packagedPlatformKey = plan.platformKey;
    StoreBuildScriptOutcome scripts = profile.writeBuildScripts(scriptInput);
    result.buildScripts = std::move(scripts.writtenPaths);
    result.warnings.insert(result.warnings.end(), scripts.warnings.begin(), scripts.warnings.end());
    return result;
}

/// @brief Render `manifests/<platform>.json` for a built depot.
/// @param result Build result.
/// @param version Effective package version.
/// @return JSON text.
std::string renderStoreDepotManifest(const StoreDepotResult &result, const std::string &version) {
    const StoreDepotPlan &plan = result.plan;
    const auto &archs = plan.executableArchitectures;
    const std::string arch =
        archs.size() > 1 ? "universal" : (archs.empty() ? plan.arch : archs[0]);
    std::ostringstream out;
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"store\": \"" << jsonEscape(plan.store) << "\",\n"
        << "  \"platform\": \"" << jsonEscape(plan.platformKey) << "\",\n"
        << "  \"arch\": \"" << jsonEscape(arch) << "\",\n"
        << "  \"architectures\": [";
    for (size_t i = 0; i < archs.size(); ++i)
        out << (i == 0 ? "" : ", ") << "\"" << jsonEscape(archs[i]) << "\"";
    out << "],\n"
        << "  \"version\": \"" << jsonEscape(version) << "\",\n"
        << "  \"app_id\": \"" << jsonEscape(plan.appId) << "\",\n"
        << "  \"depot_id\": ";
    if (plan.depotId.empty())
        out << "null";
    else
        out << "\"" << jsonEscape(plan.depotId) << "\"";
    out << ",\n"
        << "  \"launch\": \"" << jsonEscape(plan.launchPath) << "\",\n"
        << "  \"executable\": \"" << jsonEscape(plan.executableRelativePath) << "\",\n"
        << "  \"redistributable\": ";
    if (plan.redistStagedPath.empty())
        out << "null";
    else
        out << "\"" << jsonEscape(plan.redistStagedPath) << "\"";
    out << ",\n"
        << "  \"trust\": \"" << jsonEscape(result.trust) << "\",\n"
        << "  \"files\": [";
    for (size_t i = 0; i < result.files.size(); ++i) {
        const auto &file = result.files[i];
        out << (i == 0 ? "\n" : ",\n") << "    {\"path\": \"" << jsonEscape(file.path)
            << "\", \"size\": " << file.size << ", \"sha256\": \"" << file.sha256 << "\"}";
    }
    out << (result.files.empty() ? "]\n" : "\n  ]\n") << "}\n";
    return out.str();
}

} // namespace zanna::pkg
