//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/LinkerSupport.hpp
// Purpose: Shared linker utilities used by both x86_64 and AArch64 backends.
// Key invariants: Archive paths are validated via fileExists() before use;
//                 missing archives trigger cmake rebuild before link failure.
// Ownership/Lifetime: All functions are stateless utilities except
//                     prepareLinkContext() which populates a LinkContext by ref.
// Links: codegen/x86_64/CodegenPipeline.hpp, codegen/aarch64/CodegenPipeline.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/RuntimeComponents.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Declares archive discovery, link-context preparation, and tool launching.

namespace zanna::codegen::common {

// =========================================================================
// Pure utility functions
// =========================================================================

/// @brief Encode a filesystem path as UTF-8 without consulting the active code page.
/// @details Process-launch and linker option strings use UTF-8 on every host.
/// @param path Native filesystem path to encode.
/// @return UTF-8 representation suitable for Zanna process and linker APIs.
std::string pathToUtf8(const std::filesystem::path &path);

/// @brief Check if a regular file exists at the given path, suppressing filesystem exceptions.
/// @param path The filesystem path to check.
/// @return True if the path names an accessible regular file, false otherwise.
bool fileExists(const std::filesystem::path &path);

/// @brief Read the entire contents of a file into a string.
/// @param path The filesystem path to read from.
/// @param[out] dst Receives binary-preserving file contents on success.
/// @return `true` if the file was opened and consumed; `false` on open failure.
bool readFileToString(const std::filesystem::path &path, std::string &dst);

/// @brief Write a string to disk, replacing any existing contents.
/// @param path The filesystem path to write to.
/// @param text File contents to persist.
/// @param err Output stream for human-readable error messages.
/// @return True on success, false when the file could not be written.
bool writeTextFile(const std::filesystem::path &path, std::string_view text, std::ostream &err);

/// @brief Search for the CMake build directory by walking parent directories.
/// @details Checks `ZANNA_BUILD_DIR`, ancestors of the running executable, a
///          `build` child of the current directory, and then current-directory
///          ancestors. Each probe requires a regular `CMakeCache.txt`.
/// @return The path to the build directory, or std::nullopt if not found.
std::optional<std::filesystem::path> findBuildDir();

/// @brief Return the canonical path to the current executable when available.
/// @return Host-native absolute executable path, or `std::nullopt` when the
///         platform query or canonicalization fails.
std::optional<std::filesystem::path> currentExecutablePath();

/// @brief Resolve the installed library directory for the current Zanna executable.
/// @details Searches explicit environment overrides, executable-relative layouts,
///          and platform standard locations before falling back to build-tree logic.
/// @return First directory containing the base runtime archive, or
///         `std::nullopt` when no installed layout is detected.
std::optional<std::filesystem::path> findInstalledLibDir();

/// @brief Scan assembly text for referenced runtime symbols (rt_* / _rt_*).
/// @details Parses the assembly source for call/reference instructions targeting
///          symbols matching the Zanna runtime naming convention.
/// @param text The assembly source text to scan.
/// @return A set of unique runtime symbol names found in the text.
std::unordered_set<std::string> parseRuntimeSymbols(std::string_view text);

/// @brief Compute the filesystem path to a runtime library archive.
/// @details Prefers a discovered installed layout when the archive exists there,
///          then falls back to build-tree locations and final fallback probes.
/// @param buildDir The CMake build directory containing compiled libraries.
/// @param libBaseName Base name of the library (e.g., "zanna_rt_core").
/// @return The full path to the archive file (.a or .lib).
std::filesystem::path runtimeArchivePath(const std::filesystem::path &buildDir,
                                         std::string_view libBaseName);

/// @brief Compute the filesystem path to a non-runtime support library archive.
/// @details Used for companion graphics/audio libraries such as zannagfx,
///          zannagui, and zannaaud. Prefers discovered installed layouts before
///          build-tree fallback paths.
/// @param buildDir Optional CMake build root used for development-tree probes.
/// @param libBaseName Library name without platform prefix or extension.
/// @return Preferred installed, build-tree, or fallback archive path. The
///         fallback is not guaranteed to exist.
std::filesystem::path supportLibraryPath(const std::filesystem::path &buildDir,
                                         std::string_view libBaseName);

/// @brief Return MSVC C++ runtime archives needed by Windows native links.
/// @details The dynamic MSVC runtime exports many STL entry points, but some
///          `__std_*` helpers are object-code members of msvcprt.lib/msvcprtd.lib.
///          Those helper objects can depend on C runtime support objects from
///          msvcrt.lib/msvcrtd.lib, such as CPU-dispatch state.
///          Zanna's in-process linker must search that archive before it
///          generates import thunks, otherwise binaries can import non-exported
///          helper names and fail during Windows loader startup.
/// @param buildDir CMake build root used to inspect generator/toolset metadata.
/// @param arch Requested MSVC library architecture; `arm64` selects ARM64 and
///             other values select x64.
/// @param debugRuntime Whether to search for debug rather than release CRT archives.
/// @return Existing C++ and C runtime archive paths found in environment or
///         CMake-discovered toolsets; empty on non-Windows hosts.
std::vector<std::filesystem::path> windowsMsvcCxxRuntimeArchives(
    const std::filesystem::path &buildDir, std::string_view arch, bool debugRuntime);

/// @brief Heuristically detect whether Windows archive paths use debug CRTs.
/// @param archivePaths UTF-8 archive paths or names to inspect case-insensitively.
/// @return `true` when a debug directory marker or known debug CRT filename appears.
bool windowsArchivePathsUseDebugRuntime(const std::vector<std::string> &archivePaths);

/// @brief Static-archive closure pulled in when a codegen'd binary embeds the
///        Zia editor-service bridge. These are demand-driven (added to the
///        regular archive list, not force-loaded): only the members the
///        force-loaded zia_editor_services objects actually reference get
///        extracted. Names are both the CMake target names and
///        `supportLibraryPath` base names.
/// @return Process-lifetime ordered dependency-name vector.
const std::vector<std::string> &ziaFrontendClosureLibs();

/// @brief Static-archive closure pulled in with the BASIC language-service bridge.
/// @details fe_basic itself is force-loaded so its strong runtime entry points
///          override weak stubs; these dependency archives remain demand-driven.
/// @return Process-lifetime ordered dependency-name vector.
const std::vector<std::string> &basicFrontendClosureLibs();

// =========================================================================
// Link context — shared linker preamble
// =========================================================================

/// @brief Holds resolved linker state after symbol scanning and archive discovery.
/// @details Populated by prepareLinkContext(). Contains the build directory,
///          the set of required runtime components, and the resolved paths to
///          their archive files.
struct LinkContext {
    std::filesystem::path buildDir;              ///< Resolved CMake build directory.
    std::vector<RtComponent> requiredComponents; ///< Runtime components needed by the program.
    std::vector<std::pair<std::string, std::filesystem::path>>
        requiredArchives; ///< (lib name, archive path) pairs.
    /// True when the program references the Zia completion bridge
    /// (rt_zia_* / Zanna.Zia.*). The editor-service archive must then be
    /// force-loaded so its strong symbols override the weak runtime stubs.
    bool needsZiaFrontend = false;
    /// True when the program references the BASIC language-service bridge.
    /// fe_basic must be force-loaded so its strong bridge wins over weak stubs.
    bool needsBasicFrontend = false;
};

/// @brief Check if a specific runtime component is required by the link context.
/// @param ctx The link context to query.
/// @param c The runtime component to check for.
/// @return True if @p c is in the required components list.
bool hasComponent(const LinkContext &ctx, RtComponent c);

/// @brief Prepare a complete link context by scanning assembly for runtime symbols.
/// @details Reads the assembly file at @p asmPath, scans for runtime symbols,
///          resolves them to runtime components, locates the build directory,
///          computes archive paths, expands the component set by following
///          runtime-to-runtime archive references, and triggers cmake rebuilds
///          for any missing library targets.
/// @param asmPath Path to the assembly source file to scan for symbols.
/// @param ctx Output link context to populate with resolved state.
/// @param out Standard output stream for progress messages.
/// @param err Standard error stream for error messages.
/// @return 0 on success, non-zero on failure (missing build dir, build failure, etc.).
int prepareLinkContext(const std::string &asmPath,
                       LinkContext &ctx,
                       std::ostream &out,
                       std::ostream &err);

/// @brief Prepare a link context from a pre-resolved set of external symbol names.
/// @details Used by the native assembler path, which already has the symbol table
///          from the CodeSection. Skips the assembly-scanning step while still
///          expanding runtime component closure from the selected archives.
/// @param symbols Set of external symbol names (e.g., "rt_print_i64").
/// @param ctx Output link context to populate.
/// @param out Standard output stream for progress messages.
/// @param err Standard error stream for error messages.
/// @return Zero on successful resolution/build, or nonzero when archive
///         inspection or a required target build fails.
int prepareLinkContextFromSymbols(const std::unordered_set<std::string> &symbols,
                                  LinkContext &ctx,
                                  std::ostream &out,
                                  std::ostream &err);

// =========================================================================
// Tool invocation
// =========================================================================

/// @brief Invoke the system assembler to compile an assembly file to an object file.
/// @param ccArgs Base compiler command and flags (e.g., {"cc", "-arch", "arm64"}).
/// @param asmPath Path to the input assembly source file.
/// @param objPath Path for the output object file.
/// @param out Standard output stream for assembler messages.
/// @param err Standard error stream for assembler error messages.
/// @return The assembler's exit code, or `-1` when the process could not launch.
int invokeAssembler(const std::vector<std::string> &ccArgs,
                    const std::string &asmPath,
                    const std::string &objPath,
                    std::ostream &out,
                    std::ostream &err);

/// @brief Execute a linked native binary and forward its stdout/stderr.
/// @param exePath Path to the executable to run.
/// @param out Standard output stream to forward the executable's stdout to.
/// @param err Standard error stream to forward the executable's stderr to.
/// @return The executable's exit code, or `-1` when process launch fails.
int runExecutable(const std::string &exePath, std::ostream &out, std::ostream &err);

} // namespace zanna::codegen::common
