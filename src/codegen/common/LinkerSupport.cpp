//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/LinkerSupport.cpp
// Purpose: Implementation of shared linker utilities used by both x86_64 and
//          AArch64 codegen pipelines: runtime symbol scanning, archive
//          discovery, missing-target rebuild, and system linker invocation.
// Key invariants:
//   - Archive paths are always validated via fileExists() before being added
//     to the link command; missing archives trigger a cmake rebuild instead of
//     a silent link failure.
//   - LinkContext cache keys include both the build directory and the sorted
//     symbol set so cache hits cannot collide across distinct programs.
// Ownership/Lifetime:
//   - All exported functions are stateless; per-call state lives in the caller-
//     provided LinkContext.
//   - The link-context cache is process-wide, guarded by a static mutex.
// Cross-platform touchpoints: runtime archive naming (.a/.lib), system-library
//                             discovery, tool invocation, and host linker flags.
// Links: codegen/common/LinkerSupport.hpp,
//        codegen/x86_64/CodegenPipeline.hpp,
//        codegen/aarch64/CodegenPipeline.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/LinkerSupport.hpp"

#include "codegen/common/linker/ArchiveReader.hpp"
#include "codegen/common/linker/NameMangling.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "common/Environment.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/RunProcess.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <windows.h>
#elif ZANNA_HOST_MACOS
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#else
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif

/// @file
/// @brief Implements cross-platform archive discovery and native tool invocation.

namespace zanna::codegen::common {
namespace {

/// @brief Build the platform-correct archive filename for a given library base.
/// @details Produces `<base>.lib` on Windows and `lib<base>.a` elsewhere; the
///          base name should not include any prefix or extension.
/// @param libBaseName Library target/base name.
/// @return Host-specific static archive filename.
std::string archiveFileName(std::string_view libBaseName) {
    if constexpr (zanna::platform::kHostWindows)
        return std::string(libBaseName) + ".lib";
    return "lib" + std::string(libBaseName) + ".a";
}

/// @brief Probe whether @p dir looks like an installed Zanna lib directory.
/// @details Tests for the presence of libzanna_rt_base, the always-required
///          base archive, as a low-cost fingerprint for an installed layout.
/// @param dir Directory candidate to inspect.
/// @return `true` when the platform-named base archive is a regular file.
bool dirHasArchiveProbe(const std::filesystem::path &dir) {
    return fileExists(dir / archiveFileName("zanna_rt_base"));
}

/// @brief Resolve an installed library directory supplied through ZANNA_LIB_PATH.
/// @details ZANNA_LIB_PATH may point either at an archive file or at the directory
///          containing the installed runtime/support archives. The probe normalizes
///          file inputs to their parent directory and only accepts directories that
///          contain the always-required zanna_rt_base archive.
/// @return The normalized installed library directory, or std::nullopt when the
///         environment variable is absent or does not identify a Zanna install.
std::optional<std::filesystem::path> configuredInstalledLibDir() {
    if (const auto env = zanna::environment::getUtf8("ZANNA_LIB_PATH")) {
        std::filesystem::path candidate = zanna::filesystem::pathFromUtf8(*env);
        std::error_code dirEc;
        if (fileExists(candidate) && !std::filesystem::is_directory(candidate, dirEc))
            candidate = candidate.parent_path();
        if (!candidate.empty() && dirHasArchiveProbe(candidate))
            return candidate;
    }
    return std::nullopt;
}

/// @brief Build an installed archive path from an optional installed directory.
/// @details Keeps call sites compact when installed-layout discovery is optional.
///          The returned path is not revalidated; callers still use fileExists()
///          before adding it to a link line.
/// @param dir Installed library directory returned by a discovery probe.
/// @param libBaseName Runtime/support library base name without prefix or suffix.
/// @return std::nullopt when @p dir is empty; otherwise the platform-correct
///         archive path within that directory.
std::optional<std::filesystem::path> installedLibraryPathInDir(
    const std::optional<std::filesystem::path> &dir, std::string_view libBaseName) {
    if (!dir)
        return std::nullopt;
    return *dir / archiveFileName(libBaseName);
}

/// @brief Source/build-tree sub-directory that produces a given support lib.
/// @details Most companion libs land under `lib/`, but a few live where their
///          sources are: GUI under src/lib/gui, shared text under
///          src/common/text, frontend-common under src/frontends/common, Zia
///          frontend/editor archives under src/frontends/zia, and the Zia
///          static-link closure (the IL build/verify/transform/runtime/core/
///          support archives pulled when IntelliSense is embedded in a
///          codegen'd binary) directly under src/ (zanna_support under
///          src/support).
/// @param libBaseName Support-library target/base name.
/// @return Relative build-tree directory expected to contain the archive.
std::filesystem::path supportLibBuildSubdir(std::string_view libBaseName) {
    if (libBaseName == "zannagui")
        return std::filesystem::path("src") / "lib" / "gui";
    if (libBaseName == "zanna_text_core")
        return std::filesystem::path("src") / "common" / "text";
    if (libBaseName == "fe_common")
        return std::filesystem::path("src") / "frontends" / "common";
    if (libBaseName == "fe_zia" || libBaseName == "zia_editor_services")
        return std::filesystem::path("src") / "frontends" / "zia";
    if (libBaseName == "fe_basic")
        return std::filesystem::path("src") / "frontends" / "basic";
    if (libBaseName == "zanna_support")
        return std::filesystem::path("src") / "support";
    if (libBaseName == "zanna_il_io")
        return std::filesystem::path("src") / "il" / "io";
    if (libBaseName == "il_build" || libBaseName == "il_transform" || libBaseName == "il_runtime" ||
        libBaseName == "il_analysis" || libBaseName == "il_utils" || libBaseName == "il_api" ||
        libBaseName == "zanna_il_core" || libBaseName == "zanna_il_verify" ||
        libBaseName == "zanna_pass")
        return std::filesystem::path("src");
    return std::filesystem::path("lib");
}

/// @brief Best-effort static-layout path for a support lib, used as a final fallback.
/// @param libBaseName Support-library target/base name.
/// @return Relative source/build-layout path without existence validation.
std::filesystem::path fallbackSupportLibraryPath(std::string_view libBaseName) {
    return supportLibBuildSubdir(libBaseName) / archiveFileName(libBaseName);
}

/// @brief Return multi-config build configurations in preferred probe order.
///
/// An explicit `ZANNA_BUILD_TYPE` is considered first, followed by a
/// debug- or release-biased deterministic fallback sequence.
///
/// @return Unique configuration names in search order.
[[maybe_unused]] std::vector<std::string> preferredBuildConfigs() {
    std::vector<std::string> configs;
    /// Append one nonempty configuration unless it is already present.
    auto append = [&](std::string config) {
        if (config.empty())
            return;
        if (std::find(configs.begin(), configs.end(), config) == configs.end())
            configs.push_back(std::move(config));
    };

    if (const auto env = zanna::environment::getUtf8("ZANNA_BUILD_TYPE"))
        append(*env);

    if constexpr (zanna::platform::kCompilerMSVC) {
#if defined(NDEBUG)
        append("Release");
        append("RelWithDebInfo");
        append("MinSizeRel");
        append("Debug");
#else
        append("Debug");
        append("RelWithDebInfo");
        append("Release");
        append("MinSizeRel");
#endif
    } else {
#if defined(NDEBUG)
        append("Release");
        append("RelWithDebInfo");
        append("MinSizeRel");
        append("Debug");
#else
        append("Debug");
        append("RelWithDebInfo");
        append("Release");
        append("MinSizeRel");
#endif
    }
    return configs;
}

/// @brief Locate a support-library archive inside a CMake build directory.
/// @details On Windows, probes the script-provided or current build configuration
///          before other multi-config directories; this prevents stale archives
///          from a different configuration being mixed into the link. Returns an
///          empty path when no candidate exists.
/// @param buildDir CMake build root.
/// @param libBaseName Support-library target/base name.
/// @return Existing preferred archive path, a deterministic non-Windows path,
///         or an empty path when Windows probes find no archive.
std::filesystem::path buildTreeSupportLibraryPath(const std::filesystem::path &buildDir,
                                                  std::string_view libBaseName) {
    const std::filesystem::path subdir = supportLibBuildSubdir(libBaseName);
    const std::string archive = archiveFileName(libBaseName);
    if constexpr (zanna::platform::kHostWindows) {
        std::vector<std::filesystem::path> candidates;
        for (const auto &config : preferredBuildConfigs())
            candidates.push_back(buildDir / subdir / config / archive);
        candidates.push_back(buildDir / subdir / archive);
        for (const auto &candidate : candidates) {
            if (fileExists(candidate))
                return candidate;
        }
        return std::filesystem::path{};
    }
    return buildDir / subdir / archive;
}

/// @brief Read one exact key from a CMake cache.
/// @param buildDir Directory containing `CMakeCache.txt`.
/// @param key Cache key without type or delimiter.
/// @return Text following `=` for the matching key, or an empty string when
///         the cache cannot be read or the key is absent.
static std::string cmakeCacheValue(const std::filesystem::path &buildDir, std::string_view key) {
    if (buildDir.empty())
        return {};
    std::ifstream cache(buildDir / "CMakeCache.txt");
    if (!cache.is_open())
        return {};

    const std::string prefix(key);
    std::string line;
    while (std::getline(cache, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto colon = line.find(':');
        const auto keyEnd = colon == std::string::npos ? eq : colon;
        if (keyEnd == prefix.size() && line.compare(0, keyEnd, prefix) == 0)
            return line.substr(eq + 1);
    }
    return {};
}

/// @brief Locate the newest MSVC toolset under a Visual Studio instance.
/// @param instance Visual Studio installation root.
/// @param arch Required library architecture subdirectory.
/// @return Toolset root containing the requested libraries, or `std::nullopt`.
static std::optional<std::filesystem::path> windowsMsvcToolsetFromInstance(
    const std::filesystem::path &instance, std::string_view arch) {
    if (instance.empty())
        return std::nullopt;

    const auto toolsRoot = instance / "VC" / "Tools" / "MSVC";
    std::error_code ec;
    if (!std::filesystem::is_directory(toolsRoot, ec))
        return std::nullopt;

    std::vector<std::filesystem::path> versions;
    for (const auto &entry : std::filesystem::directory_iterator(toolsRoot, ec)) {
        if (ec)
            break;
        if (!entry.is_directory(ec))
            continue;
        versions.push_back(entry.path());
    }
    std::sort(versions.begin(), versions.end());

    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        const auto libDir = *it / "lib" / std::filesystem::path(std::string(arch));
        if (std::filesystem::is_directory(libDir, ec))
            return *it;
    }
    return std::nullopt;
}

/// @brief Resolve an MSVC toolset from `VCToolsInstallDir`.
/// @param arch Required library architecture subdirectory.
/// @return Validated toolset root, or `std::nullopt`.
static std::optional<std::filesystem::path> windowsMsvcToolsetFromEnv(std::string_view arch) {
    if (const auto env = zanna::environment::getUtf8("VCToolsInstallDir")) {
        const std::filesystem::path root = zanna::filesystem::pathFromUtf8(*env);
        std::error_code ec;
        if (std::filesystem::is_directory(root / "lib" / std::filesystem::path(std::string(arch)),
                                          ec))
            return root;
    }
    return std::nullopt;
}

/// @brief Resolve an MSVC toolset from CMake generator or archiver metadata.
/// @param buildDir Directory containing the relevant CMake cache.
/// @param arch Required library architecture subdirectory.
/// @return Validated toolset root, or `std::nullopt`.
static std::optional<std::filesystem::path> windowsMsvcToolsetFromCMakeCache(
    const std::filesystem::path &buildDir, std::string_view arch) {
    const std::string instance = cmakeCacheValue(buildDir, "CMAKE_GENERATOR_INSTANCE");
    if (auto toolset =
            windowsMsvcToolsetFromInstance(zanna::filesystem::pathFromUtf8(instance), arch))
        return toolset;

    const std::filesystem::path ar =
        zanna::filesystem::pathFromUtf8(cmakeCacheValue(buildDir, "CMAKE_AR"));
    if (ar.empty())
        return std::nullopt;

    auto cur = ar.parent_path();
    while (!cur.empty()) {
        if (cur.filename() == "MSVC")
            break;
        const auto libDir = cur / "lib" / std::filesystem::path(std::string(arch));
        std::error_code ec;
        if (std::filesystem::is_directory(libDir, ec))
            return cur;
        cur = cur.parent_path();
    }
    return std::nullopt;
}

/// @brief Return the platform's standard library search dirs for installed Zanna.
/// @details macOS uses /usr/local/zanna/lib; Linux walks the usual /usr/{,local}
///          tree; Windows returns an empty list (everything is co-located).
/// @return Host-specific candidate directories in probe order.
std::vector<std::filesystem::path> standardInstalledLibDirs() {
    std::vector<std::filesystem::path> dirs;
    if constexpr (zanna::platform::kHostMacOS) {
        dirs.emplace_back("/usr/local/zanna/lib");
    } else if constexpr (!zanna::platform::kHostWindows) {
        dirs.emplace_back("/usr/lib");
        dirs.emplace_back("/usr/local/lib");
        dirs.emplace_back("/usr/lib64");
        dirs.emplace_back("/usr/local/lib64");
    }
    return dirs;
}

/// @brief Order-sensitive equality on two RtComponent lists.
/// @details Used by the LinkContext cache to detect when a recomputed closure
///          changed and the cache entry must be evicted.
/// @param lhs First component sequence.
/// @param rhs Second component sequence.
/// @return `true` when sizes and elements match in order.
bool componentsEqual(const std::vector<RtComponent> &lhs, const std::vector<RtComponent> &rhs) {
    return lhs == rhs;
}

/// @brief Recompute @p ctx.requiredArchives from its current requiredComponents.
/// @details Called whenever the component list mutates (initial discovery or
///          archive-closure expansion) so the archive list stays in sync.
/// @param[in,out] ctx Context whose archive vector is replaced.
void rebuildRequiredArchives(LinkContext &ctx) {
    ctx.requiredArchives.clear();
    for (auto comp : ctx.requiredComponents) {
        auto name = archiveNameForComponent(comp);
        ctx.requiredArchives.emplace_back(std::string(name),
                                          runtimeArchivePath(ctx.buildDir, name));
    }
}

/// @brief Check whether the build tree was configured with the zannaaud backend.
/// @details The audio support library only exists when configure found a
///          platform backend (ALSA/CoreAudio/WASAPI). `src/lib/audio` records
///          the decision as ZANNAAUD_AVAILABLE in CMakeCache.txt; when it is
///          OFF, the `zannaaud` target does not exist and must not be requested
///          from `cmake --build`. Missing cache information defaults to
///          available so configured trees keep their historical behavior.
/// @param buildDir CMake build directory backing the link context.
/// @return False only when the cache explicitly records the backend as absent.
static bool zannaaudAvailable(const std::filesystem::path &buildDir) {
    if (buildDir.empty())
        return true;
    std::ifstream cache(buildDir / "CMakeCache.txt");
    if (!cache.is_open())
        return true;
    std::string line;
    while (std::getline(cache, line)) {
        if (line.rfind("ZANNAAUD_AVAILABLE:", 0) == 0)
            return line.find("=OFF") == std::string::npos;
    }
    return true;
}

/// @brief Build any required runtime/support archives that are missing from disk.
/// @details Drives `cmake --build <dir> --target <missing...>` for every
///          required archive plus the appropriate gfx/gui/audio support libs.
///          Returns false (and writes to @p err) if the cmake invocation fails.
/// @param ctx Resolved component, frontend, and build-directory requirements.
/// @param out Stream receiving build standard output.
/// @param err Stream receiving build diagnostics and launch errors.
/// @return `true` when nothing is missing or the build succeeds.
bool ensureRequiredTargetsBuilt(const LinkContext &ctx, std::ostream &out, std::ostream &err) {
    if (ctx.buildDir.empty())
        return true;

    std::vector<std::string> missingTargets;
    for (const auto &[tgt, path] : ctx.requiredArchives) {
        if (!fileExists(path))
            missingTargets.push_back(tgt);
    }
    if (hasComponent(ctx, RtComponent::Graphics)) {
        const std::filesystem::path gfxLib = supportLibraryPath(ctx.buildDir, "zannagfx");
        if (!fileExists(gfxLib))
            missingTargets.push_back("zannagfx");
        const std::filesystem::path guiLib = supportLibraryPath(ctx.buildDir, "zannagui");
        if (!fileExists(guiLib))
            missingTargets.push_back("zannagui");
        const std::filesystem::path textCoreLib =
            supportLibraryPath(ctx.buildDir, "zanna_text_core");
        if (!fileExists(textCoreLib))
            missingTargets.push_back("zanna_text_core");
    }
    if (hasComponent(ctx, RtComponent::Audio)) {
        const std::filesystem::path audLib = supportLibraryPath(ctx.buildDir, "zannaaud");
        // Only request the target when the configure step found an audio
        // backend; otherwise the target does not exist and the runtime's
        // audio stubs satisfy the link without it.
        if (!fileExists(audLib) && zannaaudAvailable(ctx.buildDir))
            missingTargets.push_back("zannaaud");
    }
    if (ctx.needsZiaFrontend) {
        if (!fileExists(supportLibraryPath(ctx.buildDir, "zia_editor_services")))
            missingTargets.push_back("zia_editor_services");
        for (const auto &lib : ziaFrontendClosureLibs()) {
            if (!fileExists(supportLibraryPath(ctx.buildDir, lib)))
                missingTargets.push_back(lib);
        }
    }
    if (ctx.needsBasicFrontend) {
        if (!fileExists(supportLibraryPath(ctx.buildDir, "fe_basic")))
            missingTargets.push_back("fe_basic");
        for (const auto &lib : basicFrontendClosureLibs()) {
            if (!fileExists(supportLibraryPath(ctx.buildDir, lib)))
                missingTargets.push_back(lib);
        }
    }
    if (missingTargets.empty())
        return true;

    std::sort(missingTargets.begin(), missingTargets.end());
    missingTargets.erase(std::unique(missingTargets.begin(), missingTargets.end()),
                         missingTargets.end());
    std::vector<std::string> cmd = {"cmake", "--build", pathToUtf8(ctx.buildDir), "--target"};
    cmd.insert(cmd.end(), missingTargets.begin(), missingTargets.end());
    const RunResult build = run_process(cmd);
    if (!build.out.empty())
        out << build.out;
    if (!build.err.empty())
        err << build.err;
    if (build.exit_code != 0) {
        err << "error: failed to build required runtime libraries in '" << pathToUtf8(ctx.buildDir)
            << "'\n";
        return false;
    }
    return true;
}

/// @brief Walk archive members for transitive runtime symbols and union them in.
/// @details Performs a fixed-point closure: for every undefined symbol in @p
///          symbols, if a member of any required archive defines it, the
///          member's other rt_* references are added to @p symbols and a new
///          iteration is scheduled. Members are extracted at most once.
/// @param ctx Resolved link context whose archives are scanned.
/// @param symbols In/out symbol set; expanded to include the transitive closure.
/// @param err Stream for archive/object-file read errors.
/// @return true on success, false if any archive or member could not be read.
bool addArchiveClosureSymbols(const LinkContext &ctx,
                              std::unordered_set<std::string> &symbols,
                              std::ostream &err) {
    using namespace zanna::codegen::linker;

    std::vector<Archive> archives;
    archives.reserve(ctx.requiredArchives.size());
    for (const auto &[name, path] : ctx.requiredArchives) {
        if (!fileExists(path))
            continue;
        Archive ar;
        if (!readArchive(pathToUtf8(path), ar, err)) {
            err << "error: failed to inspect runtime archive '" << name << "' at '"
                << pathToUtf8(path) << "'\n";
            return false;
        }
        archives.push_back(std::move(ar));
    }

    std::unordered_set<std::string> resolved;
    std::unordered_set<std::string> unresolved = symbols;
    std::unordered_set<std::string> extractedMembers;

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &ar : archives) {
            std::vector<std::string> undefSnapshot(unresolved.begin(), unresolved.end());
            for (const auto &undef : undefSnapshot) {
                if (resolved.count(undef) != 0)
                    continue;

                auto symIt = findWithMachoFallback(ar.symbolIndex, undef);
                if (symIt == ar.symbolIndex.end())
                    continue;

                const size_t memberIdx = symIt->second;
                if (memberIdx >= ar.members.size())
                    continue;

                const std::string memberKey =
                    ar.path + "#" + std::to_string(static_cast<unsigned long long>(memberIdx));
                if (!extractedMembers.insert(memberKey).second)
                    continue;

                auto memberData = memberDataView(ar, ar.members[memberIdx]);
                if (memberData.data == nullptr || memberData.size == 0)
                    continue;

                ObjFile memberObj;
                std::ostringstream memberErr;
                if (!readObjFile(memberData.data,
                                 memberData.size,
                                 ar.path + "(" + ar.members[memberIdx].name + ")",
                                 memberObj,
                                 memberErr)) {
                    err << memberErr.str();
                    return false;
                }

                for (size_t i = 1; i < memberObj.symbols.size(); ++i) {
                    const auto &sym = memberObj.symbols[i];
                    if (sym.name.empty())
                        continue;

                    const bool isRuntimeSymbol = componentForRuntimeSymbol(sym.name).has_value();
                    if (sym.binding == ObjSymbol::Undefined) {
                        if (resolved.count(sym.name) == 0 && unresolved.insert(sym.name).second)
                            changed = true;
                        if (isRuntimeSymbol && symbols.insert(sym.name).second)
                            changed = true;
                        continue;
                    }

                    resolved.insert(sym.name);
                    unresolved.erase(sym.name);
                    if (isRuntimeSymbol && symbols.insert(sym.name).second)
                        changed = true;
                }
            }
        }
    }

    return true;
}

/// @brief Return an environment signature for process-wide link-context caching.
/// @details Runtime archive discovery depends on a small set of environment
///          variables. Including their current values in the cache key prevents
///          a long-lived process from reusing archive paths after the caller
///          switches install/build configuration between links.
std::string linkEnvironmentCacheKey() {
    std::string key;
    /// Append one environment variable name and current UTF-8 value.
    auto appendEnv = [&](const char *name) {
        key += name;
        key.push_back('=');
        if (const auto value = zanna::environment::getUtf8(name))
            key += *value;
        key.push_back('\n');
    };
    appendEnv("ZANNA_LIB_PATH");
    appendEnv("ZANNA_BUILD_TYPE");
    appendEnv("ZANNA_BUILD_DIR");
    return key;
}

/// @brief Stable, deterministic cache key for link-context reuse.
/// @details Sorts the symbol set so identical inputs produce identical keys
///          regardless of hash-table iteration order, and includes the current
///          archive-discovery environment so process-wide cache hits cannot
///          cross install/build configuration changes.
/// @param buildDir Resolved build root, possibly empty for installed layouts.
/// @param symbols Unordered external symbol set.
/// @return Deterministic normalized build/environment/symbol key.
std::string linkContextCacheKey(const std::filesystem::path &buildDir,
                                const std::unordered_set<std::string> &symbols) {
    std::vector<std::string> ordered(symbols.begin(), symbols.end());
    std::sort(ordered.begin(), ordered.end());
    std::string key = pathToUtf8(buildDir.lexically_normal());
    key.push_back('\n');
    key += linkEnvironmentCacheKey();
    for (const auto &symbol : ordered) {
        key.push_back('\n');
        key += symbol;
    }
    return key;
}

} // namespace

// =========================================================================
// Pure utilities
// =========================================================================

/// @copydoc pathToUtf8
std::string pathToUtf8(const std::filesystem::path &path) {
    return zanna::filesystem::pathToUtf8(path);
}

/// @copydoc fileExists
bool fileExists(const std::filesystem::path &path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

/// @copydoc readFileToString
bool readFileToString(const std::filesystem::path &path, std::string &dst) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    dst = ss.str();
    return true;
}

/// @copydoc writeTextFile
bool writeTextFile(const std::filesystem::path &path, std::string_view text, std::ostream &err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err << "error: unable to open '" << pathToUtf8(path) << "' for writing\n";
        return false;
    }
    out << text;
    if (!out) {
        err << "error: failed to write file '" << pathToUtf8(path) << "'\n";
        return false;
    }
    return true;
}

/// @copydoc windowsMsvcCxxRuntimeArchives
std::vector<std::filesystem::path> windowsMsvcCxxRuntimeArchives(
    const std::filesystem::path &buildDir, std::string_view arch, bool debugRuntime) {
    std::vector<std::filesystem::path> archives;
    if constexpr (!zanna::platform::kHostWindows) {
        (void)buildDir;
        (void)arch;
        (void)debugRuntime;
        return archives;
    }

    const std::string archDir = arch == "arm64" ? "arm64" : "x64";
    const std::vector<std::string> archiveNames =
        debugRuntime ? std::vector<std::string>{"msvcprtd.lib", "msvcrtd.lib"}
                     : std::vector<std::string>{"msvcprt.lib", "msvcrt.lib"};

    std::vector<std::filesystem::path> toolsets;
    if (auto envToolset = windowsMsvcToolsetFromEnv(archDir))
        toolsets.push_back(*envToolset);
    if (auto cacheToolset = windowsMsvcToolsetFromCMakeCache(buildDir, archDir))
        toolsets.push_back(*cacheToolset);

    for (const auto &archiveName : archiveNames) {
        for (const auto &toolset : toolsets) {
            const auto archive = toolset / "lib" / archDir / archiveName;
            if (fileExists(archive)) {
                archives.push_back(archive);
                break;
            }
        }
    }
    return archives;
}

/// @copydoc windowsArchivePathsUseDebugRuntime
bool windowsArchivePathsUseDebugRuntime(const std::vector<std::string> &archivePaths) {
    for (const auto &path : archivePaths) {
        std::string lower = path;
        /// Lowercase one path byte without invoking undefined signed-char behavior.
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.find("\\debug\\") != std::string::npos ||
            lower.find("/debug/") != std::string::npos ||
            lower.rfind("msvcrtd.lib") != std::string::npos ||
            lower.rfind("ucrtd.lib") != std::string::npos ||
            lower.rfind("vcruntimed.lib") != std::string::npos ||
            lower.rfind("msvcprtd.lib") != std::string::npos)
            return true;
    }
    return false;
}

/// @copydoc findBuildDir
std::optional<std::filesystem::path> findBuildDir() {
    if (const auto env = zanna::environment::getUtf8("ZANNA_BUILD_DIR")) {
        std::filesystem::path candidate = zanna::filesystem::pathFromUtf8(*env);
        if (fileExists(candidate / "CMakeCache.txt"))
            return candidate;
    }

    // A dev-tree zanna executable lives inside its own CMake build directory
    // (e.g. <root>/build/src/tools/zanna/zanna). Walking the executable's
    // ancestors finds that build tree regardless of the caller's working
    // directory — otherwise a stale installed toolchain layout can shadow the
    // freshly-built runtime archives. Installed binaries have no CMakeCache.txt
    // above them and fall through to the installed-layout discovery unchanged.
    if (const auto exePath = currentExecutablePath()) {
        std::filesystem::path cur = exePath->parent_path();
        for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
            if (fileExists(cur / "CMakeCache.txt"))
                return cur;
            if (!cur.has_parent_path())
                break;
            cur = cur.parent_path();
        }
    }

    std::error_code ec;
    std::filesystem::path cur = std::filesystem::current_path(ec);
    if (!ec) {
        const std::filesystem::path nestedBuild = cur / "build";
        if (fileExists(nestedBuild / "CMakeCache.txt"))
            return nestedBuild;
    }

    const std::filesystem::path defaultBuild = std::filesystem::path("build");
    if (fileExists(defaultBuild / "CMakeCache.txt"))
        return defaultBuild;

    if (!ec) {
        for (int depth = 0; depth < 8; ++depth) {
            if (fileExists(cur / "CMakeCache.txt"))
                return cur;
            if (!cur.has_parent_path())
                break;
            cur = cur.parent_path();
        }
    }

    return std::nullopt;
}

/// @copydoc currentExecutablePath
std::optional<std::filesystem::path> currentExecutablePath() {
#if ZANNA_HOST_WINDOWS
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    while (true) {
        len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0)
            return std::nullopt;
        if (len < buf.size() - 1)
            break;
        buf.resize(buf.size() * 2);
    }
    buf.resize(len);
    return std::filesystem::path(buf);
#elif ZANNA_HOST_MACOS
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0)
        return std::nullopt;
    std::vector<char> raw(size + 1, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0)
        return std::nullopt;
    char resolved[PATH_MAX];
    if (!realpath(raw.data(), resolved))
        return std::nullopt;
    return std::filesystem::path(resolved);
#else
    char resolved[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (len <= 0)
        return std::nullopt;
    resolved[len] = '\0';
    return std::filesystem::path(resolved);
#endif
}

/// @brief Probe the executable-relative `../lib` installed layout.
/// @return Canonical library directory containing the base archive, or
///         `std::nullopt` when the executable/path probe fails.
std::optional<std::filesystem::path> executableInstalledLibDir() {
    if (const auto exePath = currentExecutablePath()) {
        std::error_code ec;
        std::filesystem::path exeDir = exePath->parent_path();
        std::filesystem::path candidate =
            std::filesystem::weakly_canonical(exeDir / ".." / "lib", ec);
        if (!ec && dirHasArchiveProbe(candidate))
            return candidate;
    }
    return std::nullopt;
}

/// @brief Probe host-standard installed library directories.
/// @return First directory containing the base runtime archive, or `std::nullopt`.
std::optional<std::filesystem::path> systemInstalledLibDir() {
    for (const auto &candidate : standardInstalledLibDirs()) {
        if (dirHasArchiveProbe(candidate))
            return candidate;
    }
    return std::nullopt;
}

/// @copydoc findInstalledLibDir
std::optional<std::filesystem::path> findInstalledLibDir() {
    if (const auto configured = configuredInstalledLibDir())
        return configured;
    if (const auto adjacent = executableInstalledLibDir())
        return adjacent;
    if (const auto system = systemInstalledLibDir())
        return system;
    return std::nullopt;
}

/// @copydoc parseRuntimeSymbols
std::unordered_set<std::string> parseRuntimeSymbols(std::string_view text) {
    /// Test the ASCII byte policy for unqualified runtime identifiers.
    auto isIdent = [](unsigned char c) -> bool { return std::isalnum(c) || c == '_'; };
    // Namespace-qualified symbols also contain dots.
    /// Test the ASCII byte policy for namespace-qualified Zanna identifiers.
    auto isNsIdent = [](unsigned char c) -> bool {
        return std::isalnum(c) || c == '_' || c == '.';
    };

    std::unordered_set<std::string> symbols;

    // Pass 1: Scan for rt_* symbols (existing logic).
    for (std::size_t i = 0; i + 3 < text.size(); ++i) {
        std::size_t start = std::string_view::npos;
        std::size_t boundary = std::string_view::npos;
        if (text[i] == 'r' && text[i + 1] == 't' && text[i + 2] == '_') {
            start = i;
            boundary = (start == 0) ? std::string_view::npos : (start - 1);
        } else if (text[i] == '_' && text[i + 1] == 'r' && text[i + 2] == 't' &&
                   text[i + 3] == '_') {
            start = i + 1;
            boundary = (i == 0) ? std::string_view::npos : (i - 1);
        }

        if (start == std::string_view::npos)
            continue;
        if (boundary != std::string_view::npos &&
            isIdent(static_cast<unsigned char>(text[boundary])))
            continue;

        std::size_t j = start;
        while (j < text.size() && isIdent(static_cast<unsigned char>(text[j])))
            ++j;

        if (j > start)
            symbols.emplace(text.substr(start, j - start));
        i = j == 0 ? i : j - 1;
    }

    // Pass 2: Scan for Zanna.* namespace-qualified symbols (OOP-style IL).
    // These appear as _Zanna.Terminal.PrintStr or Zanna.Collections.List.Add
    // in the emitted assembly.
    static constexpr std::string_view kZannaPrefix = "Zanna.";
    for (std::size_t i = 0; i + kZannaPrefix.size() < text.size(); ++i) {
        // Match "Zanna." or "_Zanna." at a word boundary.
        std::size_t start = std::string_view::npos;
        if (text.substr(i, kZannaPrefix.size()) == kZannaPrefix) {
            if (i == 0 || !isIdent(static_cast<unsigned char>(text[i - 1])))
                start = i;
        } else if (text[i] == '_' && i + 1 + kZannaPrefix.size() <= text.size() &&
                   text.substr(i + 1, kZannaPrefix.size()) == kZannaPrefix) {
            if (i == 0 || !isIdent(static_cast<unsigned char>(text[i - 1])))
                start = i + 1; // Skip the leading underscore
        }

        if (start == std::string_view::npos)
            continue;

        // Read the full namespace-qualified identifier (alphanumeric, _, .).
        std::size_t j = start;
        while (j < text.size() && isNsIdent(static_cast<unsigned char>(text[j])))
            ++j;

        if (j > start)
            symbols.emplace(text.substr(start, j - start));
        i = j == 0 ? i : j - 1;
    }

    return symbols;
}

/// @copydoc runtimeArchivePath
std::filesystem::path runtimeArchivePath(const std::filesystem::path &buildDir,
                                         std::string_view libBaseName) {
    const std::string libName = archiveFileName(libBaseName);
    const auto configuredPath = installedLibraryPathInDir(configuredInstalledLibDir(), libBaseName);
    const auto adjacentPath = installedLibraryPathInDir(executableInstalledLibDir(), libBaseName);
    const auto systemPath = installedLibraryPathInDir(systemInstalledLibDir(), libBaseName);
    if (configuredPath && fileExists(*configuredPath))
        return *configuredPath;
    if (adjacentPath && fileExists(*adjacentPath))
        return *adjacentPath;
    if constexpr (zanna::platform::kHostWindows) {
        const std::string objLibName = std::string(libBaseName) + "_obj.lib";
        /// Choose the first existing Windows candidate, retaining the first
        /// candidate as the deterministic missing-file path for rebuild logic.
        auto pickFirstExisting =
            [](const std::vector<std::filesystem::path> &candidates) -> std::filesystem::path {
            for (const auto &candidate : candidates) {
                if (fileExists(candidate))
                    return candidate;
            }
            return candidates.empty() ? std::filesystem::path{} : candidates.front();
        };

        if (!buildDir.empty()) {
            std::vector<std::filesystem::path> candidates;
            for (const auto &config : preferredBuildConfigs()) {
                candidates.push_back(buildDir / "src/runtime" /
                                     (std::string(libBaseName) + "_obj.dir") / config / objLibName);
                candidates.push_back(buildDir / "src/runtime" / config / libName);
            }
            candidates.push_back(buildDir / "src/runtime" / libName);
            return pickFirstExisting(candidates);
        }
    } else {
        if (!buildDir.empty()) {
            const auto buildTreePath = buildDir / "src/runtime" / libName;
            if (fileExists(buildTreePath))
                return buildTreePath;
        }
    }
    if (systemPath && fileExists(*systemPath))
        return *systemPath;
    if (configuredPath)
        return *configuredPath;
    if (adjacentPath)
        return *adjacentPath;
    if (systemPath)
        return *systemPath;
    if (!buildDir.empty())
        return buildDir / "src/runtime" / libName;
    return std::filesystem::path("src/runtime") / libName;
}

/// @copydoc supportLibraryPath
std::filesystem::path supportLibraryPath(const std::filesystem::path &buildDir,
                                         std::string_view libBaseName) {
    const auto configuredPath = installedLibraryPathInDir(configuredInstalledLibDir(), libBaseName);
    const auto adjacentPath = installedLibraryPathInDir(executableInstalledLibDir(), libBaseName);
    const auto systemPath = installedLibraryPathInDir(systemInstalledLibDir(), libBaseName);
    if (configuredPath && fileExists(*configuredPath))
        return *configuredPath;
    if (adjacentPath && fileExists(*adjacentPath))
        return *adjacentPath;
    if (!buildDir.empty()) {
        const auto path = buildTreeSupportLibraryPath(buildDir, libBaseName);
        if (!path.empty() && fileExists(path))
            return path;
    }
    if (systemPath && fileExists(*systemPath))
        return *systemPath;
    if (!buildDir.empty()) {
        const auto path = buildTreeSupportLibraryPath(buildDir, libBaseName);
        if (!path.empty())
            return path;
    }
    if (configuredPath)
        return *configuredPath;
    if (adjacentPath)
        return *adjacentPath;
    if (systemPath)
        return *systemPath;
    return fallbackSupportLibraryPath(libBaseName);
}

/// @copydoc ziaFrontendClosureLibs
const std::vector<std::string> &ziaFrontendClosureLibs() {
    static const std::vector<std::string> kLibs = {
        "fe_zia",
        "fe_common",
        "il_build",
        "il_transform",
        "il_runtime",
        "il_analysis",
        "il_utils",
        "il_api",
        "zanna_pass",
        "zanna_il_core",
        "zanna_il_verify",
        "zanna_il_io",
        "zanna_support",
    };
    return kLibs;
}

/// @copydoc basicFrontendClosureLibs
const std::vector<std::string> &basicFrontendClosureLibs() {
    static const std::vector<std::string> kLibs = {
        "fe_common",
        "il_build",
        "il_transform",
        "il_runtime",
        "il_analysis",
        "il_utils",
        "il_api",
        "zanna_pass",
        "zanna_il_core",
        "zanna_il_verify",
        "zanna_il_io",
        "zanna_support",
    };
    return kLibs;
}

// =========================================================================
// Link context
// =========================================================================

/// @copydoc hasComponent
bool hasComponent(const LinkContext &ctx, RtComponent c) {
    for (auto rc : ctx.requiredComponents)
        if (rc == c)
            return true;
    return false;
}

/// @brief Common implementation: resolve components, discover archives, rebuild missing.
/// @details Shared by both prepareLinkContext (file-based) and
///          prepareLinkContextFromSymbols (symbol-set-based). Repeatedly scans
///          selected archive members until runtime-component discovery reaches
///          a fixed point, then detects frontend bridges and caches the result.
/// @param symbols Initial external symbols.
/// @param[out] ctx Fully resolved context on success; may contain partial state
///                 on failure.
/// @param out Stream receiving any target-build output.
/// @param err Stream receiving archive and target-build diagnostics.
/// @return Zero on success, one on archive inspection or build failure.
static int resolveAndBuildArchives(const std::unordered_set<std::string> &symbols,
                                   LinkContext &ctx,
                                   std::ostream &out,
                                   std::ostream &err) {
    std::unordered_set<std::string> closureSymbols = symbols;

    const std::optional<std::filesystem::path> buildDirOpt = findBuildDir();
    ctx.buildDir = buildDirOpt.value_or(std::filesystem::path{});

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, LinkContext> cache;
    const std::string cacheKey = linkContextCacheKey(ctx.buildDir, symbols);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = cache.find(cacheKey); it != cache.end()) {
            ctx = it->second;
            return ensureRequiredTargetsBuilt(ctx, out, err) ? 0 : 1;
        }
    }

    while (true) {
        const auto previousComponents = ctx.requiredComponents;
        ctx.requiredComponents = resolveRequiredComponents(closureSymbols);
        rebuildRequiredArchives(ctx);

        if (!ensureRequiredTargetsBuilt(ctx, out, err))
            return 1;
        if (!addArchiveClosureSymbols(ctx, closureSymbols, err))
            return 1;

        const auto expandedComponents = resolveRequiredComponents(closureSymbols);
        if (componentsEqual(previousComponents, expandedComponents) &&
            componentsEqual(ctx.requiredComponents, expandedComponents)) {
            ctx.requiredComponents = expandedComponents;
            rebuildRequiredArchives(ctx);
            if (!ensureRequiredTargetsBuilt(ctx, out, err))
                return 1;
            break;
        }
    }

    ctx.needsZiaFrontend =
        /// Recognize Zia bridge symbols in C and namespace-qualified forms.
        std::any_of(closureSymbols.begin(), closureSymbols.end(), [](const std::string &sym) {
            return sym.rfind("rt_zia_", 0) == 0 || sym.rfind("_rt_zia_", 0) == 0 ||
                   sym.rfind("Zanna.Zia.", 0) == 0 || sym.rfind("_Zanna.Zia.", 0) == 0;
        });
    ctx.needsBasicFrontend =
        /// Recognize BASIC bridge symbols in C and namespace-qualified forms.
        std::any_of(closureSymbols.begin(), closureSymbols.end(), [](const std::string &sym) {
            return sym.rfind("rt_basic_toolchain_", 0) == 0 ||
                   sym.rfind("_rt_basic_toolchain_", 0) == 0 ||
                   sym.rfind("rt_basic_completion_", 0) == 0 ||
                   sym.rfind("_rt_basic_completion_", 0) == 0 ||
                   sym.rfind("Zanna.Basic.LanguageService.", 0) == 0 ||
                   sym.rfind("_Zanna.Basic.LanguageService.", 0) == 0;
        });

    // Frontend bridge requirements are known only after archive-closure
    // expansion. Re-run the missing-target gate so a clean or partial build
    // cannot silently fall back to weak language-service stubs.
    if (!ensureRequiredTargetsBuilt(ctx, out, err))
        return 1;

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[cacheKey] = ctx;
    }
    return 0;
}

/// @copydoc prepareLinkContext
int prepareLinkContext(const std::string &asmPath,
                       LinkContext &ctx,
                       std::ostream &out,
                       std::ostream &err) {
    std::string asmText;
    if (!readFileToString(zanna::filesystem::pathFromUtf8(asmPath), asmText)) {
        err << "error: unable to read '" << asmPath << "' for runtime library selection\n";
        return 1;
    }

    const std::unordered_set<std::string> symbols = parseRuntimeSymbols(asmText);
    return resolveAndBuildArchives(symbols, ctx, out, err);
}

/// @copydoc prepareLinkContextFromSymbols
int prepareLinkContextFromSymbols(const std::unordered_set<std::string> &symbols,
                                  LinkContext &ctx,
                                  std::ostream &out,
                                  std::ostream &err) {
    return resolveAndBuildArchives(symbols, ctx, out, err);
}

// =========================================================================
// Tool invocation
// =========================================================================

/// @copydoc invokeAssembler
int invokeAssembler(const std::vector<std::string> &ccArgs,
                    const std::string &asmPath,
                    const std::string &objPath,
                    std::ostream &out,
                    std::ostream &err) {
    std::vector<std::string> cmd = ccArgs;
    cmd.push_back("-c");
    cmd.push_back(asmPath);
    cmd.push_back("-o");
    cmd.push_back(objPath);

    const RunResult rr = run_process(cmd);
    if (rr.exit_code == -1) {
        err << "error: failed to launch system assembler command\n";
        return -1;
    }
    if (!rr.out.empty())
        out << rr.out;
    if (!rr.err.empty())
        err << rr.err;
    return rr.exit_code;
}

/// @copydoc runExecutable
int runExecutable(const std::string &exePath, std::ostream &out, std::ostream &err) {
    /// @brief Normalize an executable path before passing it to the process runner.
    /// @details POSIX does not search the current directory for bare command
    ///          names. When a caller gives `foo` instead of `./foo`, prefix the
    ///          current-directory component so freshly linked binaries run
    ///          consistently across host platforms.
    /// @param path Caller-supplied UTF-8 executable path.
    /// @return Original absolute/qualified path, or a current-directory-relative
    ///         path on POSIX when @p path is a bare filename.
    auto commandPath = [](const std::string &path) -> std::string {
        if constexpr (zanna::platform::kHostWindows)
            return path;
        const std::filesystem::path fsPath = zanna::filesystem::pathFromUtf8(path);
        if (fsPath.is_absolute() || fsPath.has_parent_path())
            return path;
        return zanna::filesystem::pathToUtf8(std::filesystem::path(".") / fsPath);
    };

    const RunResult rr = run_process({commandPath(exePath)});
    if (rr.exit_code == -1) {
        err << "error: failed to execute '" << exePath << "'\n";
        if (!rr.err.empty())
            err << rr.err << '\n';
        return -1;
    }
    if (!rr.out.empty())
        out << rr.out;
    if (!rr.err.empty())
        err << rr.err;
    return rr.exit_code;
}

} // namespace zanna::codegen::common
