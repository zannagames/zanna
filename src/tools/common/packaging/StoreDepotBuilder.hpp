//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/StoreDepotBuilder.hpp
// Purpose: Stage store depot directories (SteamPipe first) from a native
//          executable: platform redistributable placement and validation,
//          assets and .zpak packs, macOS bundle signing with store
//          entitlements, content verification, file manifests, and store
//          build scripts.
// Key invariants:
//   - Store knowledge lives in StoreProfile rows; staging, verification, and
//     manifests are store-neutral.
//   - A build replaces only content/<platform>/ and manifests/<platform>.json
//     under the build root, and only after the staged tree verifies.
//   - Redistributables are read from the developer's SDK and copied into the
//     output; nothing is copied into the Zanna source tree.
// Ownership/Lifetime:
//   - Free functions over caller-owned parameters; results are owned values.
//   - Profiles returned by storeProfile() are immutable process-lifetime data.
// Links: StoreDepotBuilder.cpp, MacOSPackageBuilder.hpp, WindowsPackageBuilder.hpp,
//        NativeBinaryInspector.hpp, MacOSEntitlements.hpp,
//        docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares store depot packaging (`zanna package --target steam-*`).

#pragma once

#include "PackageConfig.hpp"
#include "WindowsPackageBuilder.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::pkg {

/// @brief Stores with depot packaging support.
enum class StoreKind { Steam };

/// @brief Operating system of a depot payload.
enum class DepotOs { Windows, MacOS, Linux };

/// @brief One platform redistributable a store stages beside the executable.
struct StoreRedistributableSlot {
    std::string platformKey;    ///< Depot platform key, for example `windows` or `linux-arm64`.
    DepotOs os{DepotOs::Linux}; ///< Payload operating system served by this slot.
    std::string arch; ///< `x64` or `arm64`; empty when one library serves every architecture.
    std::string sdkRelativePath; ///< File below the SDK redistributable directory.
    std::string stagedName;      ///< File name beside the executable (Contents/MacOS on macOS).
};

/// @brief A platform and architecture a store cannot ship, with its explanation.
struct StoreUnsupportedPlatform {
    DepotOs os{DepotOs::Windows}; ///< Rejected operating system.
    std::string arch;             ///< Rejected architecture.
    std::string message;          ///< Complete diagnostic.
};

/// @brief A file name that must never appear in depot content.
struct StoreForbiddenContentFile {
    std::string fileName; ///< Leaf name, compared case-insensitively.
    std::string reason;   ///< Explanation appended to the diagnostic.
};

/// @brief Inputs handed to a store's build-script writer after content is in place.
struct StoreBuildScriptInput {
    const PackageConfig *pkg{nullptr}; ///< Effective package configuration.
    std::string projectName;           ///< Project name (default build descriptions).
    std::string version;               ///< Effective package version.
    std::filesystem::path outputRoot;  ///< Absolute build root.
    std::string packagedPlatformKey;   ///< Platform key this run staged.
};

/// @brief What a store's build-script writer produced.
struct StoreBuildScriptOutcome {
    std::vector<std::string> writtenPaths; ///< Absolute paths of scripts written this run.
    std::vector<std::string> warnings;     ///< Warnings without the `warning: ` prefix.
};

/// @brief Everything the depot packager knows about one store.
/// @details Adding a store adds a row, its manifest directives, and its targets.
struct StoreProfile {
    StoreKind kind{StoreKind::Steam}; ///< Store identity.
    std::string id;                   ///< Machine name, for example `steam`.
    std::string displayName;          ///< Diagnostic name, for example `Steam`.
    std::string sdkName;              ///< SDK family name, for example `Steamworks`.
    std::string supportedSdk;    ///< Supported SDK range, for example `Steamworks SDK 1.61-1.65`.
    std::string redistDirective; ///< Manifest directive naming the redistributable directory.
    std::string redistOption;    ///< Command-line override of @ref redistDirective.
    /// Subdirectories that hold the redistributables when the configured directory is an SDK root,
    /// checked in order before the configured directory itself.
    std::vector<std::string> redistRootSubdirectories;
    std::vector<StoreRedistributableSlot> redistributables;     ///< One row per platform key.
    std::vector<StoreUnsupportedPlatform> unsupportedPlatforms; ///< Explicitly rejected targets.
    std::vector<std::string> requiredExports; ///< Export names a supported redistributable has.
    std::string providerMarker;      ///< NUL-terminated string present in executables using it.
    std::string providerDescription; ///< For example `the Zanna.Services Steam provider`.
    std::string providerRuntimeName; ///< Runtime API family, for example `Zanna.Services`.
    std::vector<std::string> requiredMacOSEntitlements;           ///< Forced to true when signing.
    std::vector<std::string> forbiddenMacOSEntitlements;          ///< Must not be true.
    std::vector<StoreForbiddenContentFile> forbiddenContentFiles; ///< Never shipped.
    std::string outputSuffix; ///< Default build root is `<project>-<version>-<outputSuffix>`.
    /// Validate store-specific configuration; throws std::runtime_error.
    void (*validateConfig)(const PackageConfig &pkg){nullptr};
    /// Application id for manifests and scripts.
    std::string (*appId)(const PackageConfig &pkg){nullptr};
    /// Depot id mapped to a platform key, or empty.
    std::string (*depotId)(const PackageConfig &pkg, const std::string &platformKey){nullptr};
    /// Build-script path relative to the build root, for dry runs.
    std::string (*buildScriptRelativePath)(const PackageConfig &pkg){nullptr};
    /// Effective build description.
    std::string (*buildDescription)(const PackageConfig &pkg,
                                    const std::string &projectName,
                                    const std::string &version){nullptr};
    /// Branch the build script sets live, or empty.
    std::string (*liveBranch)(const PackageConfig &pkg){nullptr};
    /// Rewrite the store's build scripts from the content directories now present.
    StoreBuildScriptOutcome (*writeBuildScripts)(const StoreBuildScriptInput &input){nullptr};
    /// Command line that uploads a written build script, for progress output.
    std::string (*uploadCommand)(const std::string &scriptPath){nullptr};
};

/// @brief Look up a store profile.
/// @param store Store identity.
/// @return Immutable profile.
const StoreProfile &storeProfile(StoreKind store);

/// @brief Human-readable operating system name (`Windows`, `macOS`, `Linux`).
/// @param os Payload operating system.
/// @return Display name.
std::string depotOsDisplayName(DepotOs os);

/// @brief Select the depot platform key for an operating system and architecture.
/// @param profile Store profile.
/// @param os Payload operating system.
/// @param arch Requested architecture, `x64` or `arm64`.
/// @return Platform key, for example `linux-arm64`.
/// @throws std::runtime_error when the store cannot ship that combination.
std::string storeDepotPlatformKey(const StoreProfile &profile, DepotOs os, const std::string &arch);

//===----------------------------------------------------------------------===//
// Steam directive rules (shared by the manifest loader and the builder)
//===----------------------------------------------------------------------===//

/// @brief Canonicalize a Steam app or depot id.
/// @param text Decimal digits.
/// @return The value without leading zeros, or nullopt unless it is an integer in 1..4294967295.
std::optional<std::string> canonicalSteamId(std::string_view text);

/// @brief Report whether @p key is a Steam depot platform key.
/// @param key Candidate key.
/// @return True for `windows`, `macos`, `linux`, and `linux-arm64`.
bool isSteamDepotPlatform(std::string_view key);

/// @brief Validate a steam-build-description value.
/// @param description Candidate description.
/// @throws std::runtime_error with the ADR 0354 diagnostic.
void validateSteamBuildDescription(const std::string &description);

/// @brief Validate a steam-set-live branch name.
/// @param branch Candidate branch.
/// @throws std::runtime_error with the ADR 0354 diagnostic.
void validateSteamSetLiveBranch(const std::string &branch);

/// @brief Render a SteamPipe app build script.
/// @param appId Canonical app id.
/// @param description Build description (already validated).
/// @param setLive Branch to set live, or empty.
/// @param depots Depots to upload, in script order.
/// @return VDF text with tab indentation and a trailing newline.
std::string renderSteamAppBuildScript(const std::string &appId,
                                      const std::string &description,
                                      const std::string &setLive,
                                      const std::vector<SteamDepotMapping> &depots);

//===----------------------------------------------------------------------===//
// Depot planning and staging
//===----------------------------------------------------------------------===//

/// @brief Inputs for planning or building one platform depot.
struct StoreDepotParams {
    StoreKind store{StoreKind::Steam}; ///< Target store.
    DepotOs os{DepotOs::Linux};        ///< Payload operating system.
    std::string arch;                  ///< Requested architecture, `x64` or `arm64`.
    std::string projectName;           ///< Project name (executable and bundle names).
    std::string version;               ///< Effective package version.
    std::string projectRoot;           ///< Absolute trusted project root.
    PackageConfig pkgConfig;           ///< Effective package configuration.
    std::string outputRoot;            ///< Build root directory.
    std::string executablePath;        ///< Native executable; empty only for dry runs without one.
    std::string redistSource;  ///< Configured redistributable directory text; empty when none.
    std::string redistOrigin;  ///< Directive or option that supplied @ref redistSource.
    std::string redistBaseDir; ///< Base directory for a relative @ref redistSource.
    std::vector<std::string> packFiles; ///< Generated `.zpak` files shipped with the executable.
    /// Project source directories (pack and embed inputs) the output must not be inside,
    /// in addition to `asset` sources.
    std::vector<std::string> extraSourcePaths;
    std::string windowsCompilerRuntimeDir; ///< Trusted MSVC runtime directory, or empty.
    WindowsPeSigner windowsSigner;         ///< Authenticode signer for the executable and app DLLs.
};

/// @brief Resolved facts about one platform depot, shared by dry runs and builds.
struct StoreDepotPlan {
    std::string store;                                ///< Profile id.
    std::string storeName;                            ///< Profile display name.
    std::string platformKey;                          ///< Depot platform key.
    std::string arch;                                 ///< Requested architecture.
    std::vector<std::string> executableArchitectures; ///< From the executable when inspected.
    bool executableInspected{false};    ///< Whether an executable was available to inspect.
    bool usesProvider{false};           ///< Whether the executable contains the provider marker.
    std::string appId;                  ///< Store application id.
    std::string depotId;                ///< Depot mapped to the platform, or empty.
    std::string outputRoot;             ///< Absolute build root.
    std::string contentDir;             ///< Absolute `content/<platform>` directory.
    std::string launchPath;             ///< Launch path relative to @ref contentDir.
    std::string executableRelativePath; ///< Executable relative to @ref contentDir.
    std::string redistSource;           ///< Resolved SDK file, empty when none is configured.
    std::string redistStagedPath;       ///< Staged redistributable relative to @ref contentDir.
    std::vector<std::string> requiredEntitlements;  ///< macOS keys forced true when signing.
    std::vector<std::string> forbiddenEntitlements; ///< macOS keys that must not be true.
    std::string macosSignMode;         ///< Effective macOS sign mode (macOS depots only).
    std::string buildScriptPath;       ///< Absolute build-script path.
    std::string manifestPath;          ///< Absolute `manifests/<platform>.json` path.
    std::string buildDescription;      ///< Effective build description.
    std::string setLive;               ///< Branch set live by the script, or empty.
    std::vector<std::string> warnings; ///< Warnings without the `warning: ` prefix.
};

/// @brief One file in staged depot content.
struct StoreDepotFile {
    std::string path;   ///< Path relative to the content directory with `/` separators.
    uint64_t size{0};   ///< Size in bytes.
    std::string sha256; ///< Lowercase hexadecimal SHA-256.
};

/// @brief Outcome of building one platform depot.
struct StoreDepotResult {
    StoreDepotPlan plan;                   ///< Plan the build executed.
    std::vector<StoreDepotFile> files;     ///< Content files sorted by path.
    std::string trust;                     ///< Signing trust label recorded in the manifest.
    std::vector<std::string> buildScripts; ///< Build scripts written this run.
    std::vector<std::string> warnings;     ///< Plan and build-script warnings.
};

/// @brief Resolve and validate a depot without writing anything.
/// @details Validates the store configuration, platform, output root, redistributable, and
///          (when @ref StoreDepotParams::executablePath is set) the executable's architectures
///          and provider use.
/// @param params Depot inputs.
/// @return Plan facts.
/// @throws std::runtime_error with an ADR 0354 diagnostic on any violation.
StoreDepotPlan planStoreDepot(const StoreDepotParams &params);

/// @brief Stage, verify, and install one platform depot and refresh store build scripts.
/// @param params Depot inputs; @ref StoreDepotParams::executablePath is required.
/// @return Build facts, including the content file list.
/// @throws std::runtime_error on validation, staging, signing, verification, or I/O failure;
///         previously installed content is left untouched.
StoreDepotResult buildStoreDepot(const StoreDepotParams &params);

/// @brief Render `manifests/<platform>.json` for a built depot.
/// @param result Build result.
/// @param version Effective package version.
/// @return JSON text with a trailing newline.
std::string renderStoreDepotManifest(const StoreDepotResult &result, const std::string &version);

} // namespace zanna::pkg
