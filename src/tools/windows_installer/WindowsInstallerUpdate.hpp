//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerUpdate.hpp
/// @brief Declares authenticated, opt-in Windows installer update discovery.
///
/// Update manifests are HTTPS-only, same-origin, bounded, and RSA-SHA256 signed. Discovery never
/// downloads or executes an installer; the user explicitly chooses whether to open an
/// authenticated release URL. Results own every parsed string and retain no network handles.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "WindowsInstallerHost.hpp"

#include <string>
#include <string_view>

namespace zanna::installer {

/// @brief Outcome of authenticated update discovery.
enum class UpdateStatus { Unconfigured, Current, Available };

/// @brief Owned, authenticated update metadata suitable for UI or automation output.
struct UpdateCheckResult {
    /// @brief Discovery outcome.
    UpdateStatus status{UpdateStatus::Unconfigured};
    /// @brief Version of the package performing the check.
    std::string currentVersion;
    /// @brief Version advertised by the matching manifest entry.
    std::string availableVersion;
    /// @brief Release channel matched by the manifest.
    std::string channel;
    /// @brief Target architecture matched by the manifest.
    std::string architecture;
    /// @brief Authenticated installer download URL.
    std::string downloadUrl;
    /// @brief Authenticated lowercase SHA-256 digest of the installer.
    std::string downloadSha256;
    /// @brief Authenticated release-notes URL offered to the user.
    std::string releaseNotesUrl;
};

/// @brief Parse and authenticate an already downloaded canonical update manifest.
/// @param package Verified package providing version, channel, architecture, URL, and pinned key.
/// @param manifestText Complete bounded manifest text.
/// @return Authenticated status and matching release metadata.
/// @throws std::runtime_error If schema, identity, URLs, digest, ordering, or signature is invalid.
UpdateCheckResult verifyUpdateManifest(const HostPackage &package, std::string_view manifestText);

/// @brief Download, bound, parse, and authenticate the configured update manifest.
/// @param package Verified package providing update configuration and current identity.
/// @return Unconfigured status or authenticated update discovery result.
/// @throws std::runtime_error If secure networking or manifest verification fails.
UpdateCheckResult checkForUpdates(const HostPackage &package);

/// @brief Return deterministic UTF-16 JSON for CLI/automation output.
/// @param result Authenticated update result to serialize.
/// @return Single-line JSON object with escaped owned fields.
std::wstring updateResultJson(const UpdateCheckResult &result);

/// @brief Show the update result and optionally open its authenticated HTTPS URL.
/// @param instance Current module instance supplying dialog icon resources.
/// @param package Verified package supplying display metadata.
/// @param result Authenticated result to present.
void showUpdateResult(HINSTANCE instance,
                      const HostPackage &package,
                      const UpdateCheckResult &result);

} // namespace zanna::installer
