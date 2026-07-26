//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

/// @file cmd_install_package.cpp
/// @brief Builds, verifies, signs, and inventories native Zanna installation artifacts.
///
/// Exactly one staged-tree, build-tree, or verification-only input mode is active. Trusted Windows
/// output signs nested Zanna-owned PE files before hashing and signs the recursively verified outer
/// setup executable last. Release mode requires reproducible metadata and refuses weakened checks.
///
/// Temporary staging and signing workspaces are RAII-owned and retained only when explicitly
/// requested for diagnostics.
//
//===----------------------------------------------------------------------===//

#include "cli.hpp"

#include "common/Environment.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/RunProcess.hpp"
#include "tools/common/packaging/LinuxPackageBuilder.hpp"
#include "tools/common/packaging/LinuxRuntimeStubGen.hpp"
#include "tools/common/packaging/MacOSPackageBuilder.hpp"
#include "tools/common/packaging/PkgHash.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "tools/common/packaging/PkgVerify.hpp"
#include "tools/common/packaging/ToolchainInstallManifest.hpp"
#include "tools/common/packaging/WindowsPackageBuilder.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

enum class InstallPackageTarget {
    Windows,
    MacOS,
    MacOSDmg,
    LinuxDeb,
    LinuxRpm,
    LinuxBundle,
    Tarball,
    All,
    AllAvailable
};

/// @brief Parsed command-line arguments for the `zanna install-package` subcommand.
/// @details Selects the output format(s) and source (staged tree or build dir),
///          plus the macOS/Windows signing and Windows installer behavior options.
struct InstallPackageArgs {
    InstallPackageTarget target{InstallPackageTarget::All};
    std::string archOverride;
    fs::path stageDir;
    fs::path buildDir;
    std::string buildConfig;
    fs::path outputPath;
    fs::path artifactManifestPath;
    fs::path verifyOnlyPath;
    std::string macosPackageVersion;
    std::string macosMinimumVersion;
    std::string macosSignIdentity;
    std::string macosApplicationSignIdentity;
    std::string macosNotaryProfile;
    bool macosStaple{false};
    int macosNotaryTimeoutSeconds{0};
    bool macosDmg{false};
    std::string macosDmgBackground;
    std::string macosDmgIcon;
    std::string macosPkgLicense;
    std::string macosPkgBackground;
    std::string toolchainLicense;
    std::string toolchainMaintainer;
    std::string toolchainMaintainerEmail;
    std::string toolchainHomepage;
    bool windowsSign{false};
    std::string windowsSignPfx;
    std::string windowsSignThumbprint;
    std::string windowsTimestampUrl;
    std::string windowsSigntoolPath;
    bool windowsSignNoVerify{false};
    std::string linuxSignKey;
    std::string windowsInstallScope{"user"};
    std::string windowsInstallDir{"Zanna"};
    std::string windowsIdentifier{"org.zanna.toolchain"};
    bool windowsInstallDirSpecified{false};
    bool windowsIdentifierSpecified{false};
    std::string windowsChannel;
    std::string sourceCommitOverride;
    std::string windowsDocumentationUrl;
    std::string windowsUpdateManifestUrl;
    std::string windowsUpdateRsaModulus;
    std::string windowsUpdateRsaExponent;
    bool windowsAddToPath{true};
    bool windowsFileAssociations{true};
    bool windowsShortcuts{true};
    bool allowDebugToolchain{false};
    bool noVerify{false};
    bool requireChecksum{false};
    bool releaseMode{false};
    bool outputAsDirectory{false};
    bool outputAsFile{false};
    bool skipBuild{false};
    bool verbose{false};
    bool keepStageDir{false};
    bool stageOnly{false};
};

/// @brief Collision-safe Windows product identity derived from channel and explicit overrides.
struct WindowsToolchainIdentity {
    std::string channel;
    std::string installDir;
    std::string identifier;
    std::string displayName{"Zanna Toolchain"};
};

/// @brief Validate the stable lowercase syntax used in Windows product identities.
/// @param channel Candidate release-channel identifier.
/// @throws std::runtime_error If length, boundary, casing, or characters are invalid.
void validateWindowsReleaseChannel(std::string_view channel) {
    /// @brief Test one release-channel byte for allowed lowercase syntax.
    /// @param ch Byte to inspect.
    /// @return `true` for lowercase ASCII letters, digits, or hyphen.
    if (channel.empty() || channel.size() > 24U ||
        !std::isalnum(static_cast<unsigned char>(channel.front())) ||
        !std::isalnum(static_cast<unsigned char>(channel.back())) ||
        !std::all_of(channel.begin(), channel.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        })) {
        throw std::runtime_error(
            "Windows release channel must be 1-24 lowercase letters, digits, or internal hyphens");
    }
}

/// @brief Derive collision-safe Windows install identity from channel and explicit overrides.
/// @param args Parsed release, channel, directory, and identifier options.
/// @return Effective channel, install directory, product identifier, and display name.
WindowsToolchainIdentity windowsToolchainIdentity(const InstallPackageArgs &args) {
    WindowsToolchainIdentity identity;
    identity.channel = args.windowsChannel.empty() ? (args.releaseMode ? "stable" : "development")
                                                   : args.windowsChannel;
    identity.installDir = args.windowsInstallDir;
    identity.identifier = args.windowsIdentifier;
    if (identity.channel != "stable") {
        if (!args.windowsInstallDirSpecified)
            identity.installDir = "Zanna " + identity.channel;
        if (!args.windowsIdentifierSpecified)
            identity.identifier = "org.zanna.toolchain." + identity.channel;
        identity.displayName += " (" + identity.channel + ")";
    }
    return identity;
}

/// @brief Platform/architecture detected from a staged native executable.
struct NativeExecutableInfo {
    std::string platform; ///< "windows"/"macos"/"linux".
    std::string arch;     ///< "x64"/"arm64".
};

/// @brief Print usage for the `zanna install-package` subcommand to stderr.
void installPackageUsage() {
    std::cerr
        << "Usage: zanna install-package [options]\n"
        << "\n"
        << "  Package the Zanna toolchain from a staged install tree.\n"
        << "\n"
        << "Options:\n"
        << "  --target <fmt>        windows | macos | linux-deb | linux-rpm | linux-bundle | "
           "tarball | all | all-available | macos-dmg\n"
        << "  --arch <arch>         x64 | arm64 (default: manifest/host)\n"
        << "  --stage-dir <dir>     Existing staged install tree\n"
        << "  --build-dir <dir>     Build tree; runs cmake --build then cmake --install\n"
        << "  --config <cfg>        Build configuration for cmake --install from a build tree\n"
        << "  --verify-only <path>  Verify an existing artifact and exit\n"
        << "  --macos-pkg-version <v> Dotted numeric package version override\n"
        << "  --macos-min-version <v> Minimum supported macOS version (default: arch-based)\n"
        << "  --macos-sign-identity <id> Developer ID Installer identity for macOS .pkg signing\n"
        << "  --macos-app-sign-identity <id> Developer ID Application identity for nested code\n"
        << "  --macos-notary-profile <profile> notarytool keychain profile for macOS .pkg "
           "notarization\n"
        << "  --macos-staple       Staple the notarization ticket after successful submission\n"
        << "  --macos-notary-timeout <seconds> Bound the notarytool wait (default: 1800)\n"
        << "  --license <spdx>      License id for package metadata (default: GPL-3.0-only)\n"
        << "  --maintainer <name>   Maintainer/packager name (default: Zanna Project)\n"
        << "  --maintainer-email <email> Maintainer email for the deb Maintainer field\n"
        << "  --homepage <url>      Project homepage URL for deb/rpm metadata\n"
        << "  --macos-dmg          Also wrap the macOS .pkg in a styled .dmg disk image\n"
        << "  --macos-dmg-background <path> Background image (PNG) for the .dmg window\n"
        << "  --macos-dmg-icon <path> Volume icon (.icns) for the .dmg\n"
        << "  --macos-pkg-license <path> License text shown in the .pkg installer\n"
        << "  --macos-pkg-background <path> Background image for the .pkg installer pane\n"
        << "  --windows-sign        Authenticode-sign generated Windows installer\n"
        << "  --windows-sign-pfx <path> PFX certificate for Windows signing\n"
        << "  --windows-sign-thumbprint <sha1> Certificate store SHA-1 thumbprint\n"
        << "  --windows-timestamp-url <url> RFC3161 timestamp URL for Windows signing\n"
        << "  --windows-signtool <path> signtool.exe path override\n"
        << "  --windows-sign-no-verify Skip signtool verify after signing\n"
        << "  --linux-sign-key <id> GPG key id/name to sign generated .deb/.rpm packages\n"
        << "  --windows-install-scope <scope> user | machine (default: user)\n"
        << "  --windows-install-dir <name> Directory name (stable default: Zanna; otherwise "
           "channel-derived)\n"
        << "  --windows-identifier <id> Unique Apps & Features product id "
           "(stable default: org.zanna.toolchain)\n"
        << "  --windows-channel <id> Update channel (auto-derived for local builds)\n"
        << "  --source-commit <hex> Source revision override for archive/release builds\n"
        << "  --windows-documentation-url <url> HTTPS documentation link shown by setup\n"
        << "  --windows-update-manifest-url <url> HTTPS signed update manifest\n"
        << "  --windows-update-rsa-modulus <hex> Update-signing RSA public modulus\n"
        << "  --windows-update-rsa-exponent <hex> RSA exponent (for example 010001)\n"
        << "  --windows-no-path    Do not add bin/ to PATH\n"
        << "  --windows-file-associations on|off Add safe .zia/.bas/.il Open With entries "
           "(default: on)\n"
        << "  --windows-shortcuts on|off Create Start Menu developer shortcuts (default: on)\n"
        << "  --allow-debug-toolchain Allow Windows packages that reference MSVC debug CRTs\n"
        << "  --stage-only          Validate/gather the staged install tree and stop\n"
        << "  --skip-build          With --build-dir, run cmake --install without rebuilding "
           "first\n"
        << "  --keep-stage-dir      Preserve auto-generated stage directories\n"
        << "  -o <path>             Output file, or an existing directory for multiple artifacts\n"
        << "  --output-file <path>  Explicit single-artifact output path\n"
        << "  --output-dir <dir>    Explicit artifact output directory\n"
        << "  --artifact-manifest <path> JSON artifact inventory output override\n"
        << "  --release             Require clean reproducible source and native trust gates\n"
        << "  --require-checksum    Require <artifact>.sha256 with --verify-only\n"
        << "  --no-verify           Skip post-build verification\n"
        << "  --verbose, -v         Verbose output\n"
        << "  --help, -h            Show this help\n";
}

/// @brief Return the host platform name ("macos"/"windows"/"linux").
/// @return Stable lowercase host-platform token.
std::string hostPlatformName() {
#if ZANNA_HOST_MACOS
    return "macos";
#elif ZANNA_HOST_WINDOWS
    return "windows";
#else
    return "linux";
#endif
}

/// @brief Map a Zanna arch ("x64"/"arm64") to its Debian architecture name.
/// @param arch Validated or candidate Zanna architecture.
/// @return @c arm64 or @c amd64 package architecture.
/// @throws std::runtime_error If the Zanna architecture is unsupported.
std::string debArchFor(const std::string &arch) {
    zanna::pkg::validateToolchainArchitecture(arch);
    return arch == "arm64" ? "arm64" : "amd64";
}

/// @brief Map a Zanna arch ("x64"/"arm64") to its RPM architecture name.
/// @param arch Validated or candidate Zanna architecture.
/// @return @c aarch64 or @c x86_64 package architecture.
/// @throws std::runtime_error If the Zanna architecture is unsupported.
std::string rpmArchFor(const std::string &arch) {
    zanna::pkg::validateToolchainArchitecture(arch);
    return arch == "arm64" ? "aarch64" : "x86_64";
}

/// @brief Return true if the `rpmbuild` tool is available on PATH.
/// @return Cached availability result for the current process.
bool rpmbuildAvailable() {
    static std::optional<bool> cached;
    if (cached)
        return *cached;
    const RunResult rr = run_process({"rpmbuild", "--version"});
    cached = rr.exit_code == 0;
    return *cached;
}

/// @brief Return a boolean CMake cache value when @p cachePath contains @p key.
/// @details Parses `KEY:TYPE=VALUE` entries from CMakeCache.txt and recognizes
///          the usual CMake truth values. Unknown or absent keys return
///          std::nullopt so callers can preserve legacy behavior for older
///          build trees.
/// @param cachePath Path to CMakeCache.txt.
/// @param key Cache variable name to inspect.
/// @return Parsed boolean value, or std::nullopt when unavailable.
std::optional<bool> readCMakeCacheBool(const fs::path &cachePath, std::string_view key) {
    std::ifstream in(cachePath);
    if (!in)
        return std::nullopt;
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return std::nullopt;
        std::string value = line.substr(eq + 1);
        for (char &c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (value == "on" || value == "true" || value == "1" || value == "yes")
            return true;
        if (value == "off" || value == "false" || value == "0" || value == "no")
            return false;
        return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Read an environment variable, returning "" when it is unset.
/// @param name Variable name, or null.
/// @return UTF-8 value or an empty string when unavailable.
std::string getenvOrEmpty(const char *name) {
    const auto value =
        zanna::environment::getUtf8(name ? std::string_view(name) : std::string_view{});
    return value.value_or(std::string{});
}

/// @brief Return true if the args request Windows Authenticode signing.
/// @param args Parsed signing flags and explicit credentials.
/// @return @c true when flags or supported environment variables request signing.
bool windowsSigningRequested(const InstallPackageArgs &args) {
    return args.windowsSign || !args.windowsSignPfx.empty() ||
           !args.windowsSignThumbprint.empty() ||
           !getenvOrEmpty("ZANNA_WINDOWS_SIGN_PFX").empty() ||
           !getenvOrEmpty("ZANNA_WINDOWS_SIGN_THUMBPRINT").empty();
}

/// @brief Return true if the args request macOS package signing/notarization.
/// @param args Parsed signing, notarization, and stapling options.
/// @return @c true when flags or supported environment variables request a trust operation.
bool macOSPackageSigningRequested(const InstallPackageArgs &args) {
    return !args.macosSignIdentity.empty() || !args.macosApplicationSignIdentity.empty() ||
           !args.macosNotaryProfile.empty() || args.macosStaple ||
           !getenvOrEmpty("ZANNA_MACOS_SIGN_IDENTITY").empty() ||
           !getenvOrEmpty("ZANNA_MACOS_APP_SIGN_IDENTITY").empty() ||
           !getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE").empty();
}

/// @brief Locate the repository-provided Windows signing helper script.
/// @details `zanna install-package` may run outside the repository root. This
///          helper first honors ZANNA_WINDOWS_SIGN_SCRIPT, then probes the
///          current directory and likely repository roots derived from
///          --build-dir/--stage-dir before falling back to the conventional
///          relative path for diagnostics.
/// @param args Parsed install-package arguments.
/// @return Existing script path when found, otherwise `scripts/sign-windows-installer.ps1`.
fs::path defaultWindowsSigningScriptPath(const InstallPackageArgs &args) {
    const std::string envScript = getenvOrEmpty("ZANNA_WINDOWS_SIGN_SCRIPT");
    if (!envScript.empty())
        return zanna::filesystem::pathFromUtf8(envScript);
    const fs::path rel = fs::path("scripts") / "sign-windows-installer.ps1";
    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    if (!args.buildDir.empty()) {
        roots.push_back(args.buildDir.parent_path());
        roots.push_back(args.buildDir);
    }
    if (!args.stageDir.empty()) {
        roots.push_back(args.stageDir.parent_path());
        roots.push_back(args.stageDir.parent_path().parent_path());
    }
    for (const auto &root : roots) {
        if (root.empty())
            continue;
        fs::path candidate = root / rel;
        if (fs::is_regular_file(candidate))
            return candidate;
    }
    return rel;
}

/// @brief Authenticode-sign a Windows installer artifact when signing is requested.
/// @details Resolves the PFX/thumbprint (falling back to ZANNA_WINDOWS_SIGN_*
///          env vars), invokes signtool, and optionally verifies the signature.
/// @param args Parsed signing configuration.
/// @param artifactPath Installer executable to sign in place.
/// @param err Destination diagnostic stream.
/// @return true on success or when no signing was requested; false on failure.
bool signWindowsInstallerArtifact(const InstallPackageArgs &args,
                                  const fs::path &artifactPath,
                                  std::ostream &err) {
    if (!windowsSigningRequested(args))
        return true;

    std::string pfxPath =
        args.windowsSignPfx.empty() ? getenvOrEmpty("ZANNA_WINDOWS_SIGN_PFX") : args.windowsSignPfx;
    std::string thumbprint = args.windowsSignThumbprint.empty()
                                 ? getenvOrEmpty("ZANNA_WINDOWS_SIGN_THUMBPRINT")
                                 : args.windowsSignThumbprint;
    try {
        thumbprint = zanna::pkg::normalizeWindowsCertificateThumbprint(
            thumbprint, "Windows signing thumbprint");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }
    if (pfxPath.empty() && thumbprint.empty()) {
        err << "error: Windows signing requested but no PFX or certificate thumbprint was "
               "provided\n";
        return false;
    }

    std::string signtool = args.windowsSigntoolPath.empty()
                               ? getenvOrEmpty("ZANNA_WINDOWS_SIGNTOOL")
                               : args.windowsSigntoolPath;
    if (signtool.empty())
        signtool = "signtool.exe";
    std::string timestampUrl = args.windowsTimestampUrl.empty()
                                   ? getenvOrEmpty("ZANNA_WINDOWS_TIMESTAMP_URL")
                                   : args.windowsTimestampUrl;
    if (timestampUrl.empty())
        timestampUrl = "https://timestamp.digicert.com";
    try {
        zanna::pkg::validateHttpsPackageUrl(timestampUrl, "Windows timestamp URL");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }

    const fs::path signScript = defaultWindowsSigningScriptPath(args);
    if (!fs::is_regular_file(signScript)) {
        err << "error: Windows signing script not found: "
            << zanna::filesystem::pathToUtf8(signScript) << "\n";
        return false;
    }

    const std::string signScriptUtf8 = zanna::filesystem::pathToUtf8(signScript);
    const std::string artifactPathUtf8 = zanna::filesystem::pathToUtf8(artifactPath);
    std::vector<std::string> signCmd = {"powershell.exe",
                                        "-NoProfile",
                                        "-ExecutionPolicy",
                                        "Bypass",
                                        "-File",
                                        signScriptUtf8,
                                        "-InputPath",
                                        artifactPathUtf8,
                                        "-TimestampUrl",
                                        timestampUrl,
                                        "-SignToolPath",
                                        signtool};
    if (!thumbprint.empty()) {
        signCmd.push_back("-Thumbprint");
        signCmd.push_back(thumbprint);
    } else {
        if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(pfxPath))) {
            err << "error: Windows signing PFX not found: " << pfxPath << "\n";
            return false;
        }
        const std::string password = getenvOrEmpty("ZANNA_WINDOWS_SIGN_PASSWORD");
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
        signCmd.push_back("-PfxPath");
        signCmd.push_back(pfxPath);
        signCmd.push_back("-PfxPassword");
        signCmd.push_back(password);
    }
    if (args.windowsSignNoVerify)
        signCmd.push_back("-NoVerify");
    const RunResult signResult = run_process(signCmd);
    if (signResult.exit_code != 0) {
        err << "error: Windows signing script failed with exit code " << signResult.exit_code
            << "\n"
            << signResult.out << signResult.err;
        return false;
    }
    return true;
}

/// @brief Authenticode-sign one in-memory nested PE through the release signing path.
/// @details The package builder never mutates the caller's staged tree. Instead, this
///          helper writes one PE into a private temporary directory, signs and verifies
///          it using the same policy as the outer installer, then returns the signed
///          bytes. All signing sidecars and partial files are removed on every exit.
/// @param args Parsed Windows signing policy.
/// @param logicalName Payload name used for diagnostics and temporary-file naming.
/// @param unsignedPe Original PE bytes.
/// @return Signed and verified PE bytes.
/// @throws std::runtime_error If staging, signing, verification, or reading fails.
std::vector<uint8_t> signWindowsPeBytes(const InstallPackageArgs &args,
                                        std::string_view logicalName,
                                        const std::vector<uint8_t> &unsignedPe) {
    const fs::path tempDir =
        zanna::pkg::createUniqueTempDirectory(fs::temp_directory_path(), "zanna-pe-sign");
    try {
        fs::path leaf = zanna::filesystem::pathFromUtf8(logicalName).filename();
        if (leaf.empty())
            leaf = "payload.exe";
        const fs::path tempPe = tempDir / leaf;
        zanna::pkg::writeFileAtomic(tempPe, unsignedPe);

        std::ostringstream signingError;
        if (!signWindowsInstallerArtifact(args, tempPe, signingError)) {
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

/// @brief Sign (and optionally notarize/staple) a macOS .pkg when requested.
/// @details Resolves the Developer ID Installer identity and notary profile
///          (falling back to ZANNA_MACOS_* env vars), runs productsign, verifies
///          with pkgutil, and — when a notary profile is set — submits via
///          notarytool and optionally staples. Only available on macOS hosts.
/// @param args Parsed signing, notarization, timeout, and stapling policy.
/// @param artifactPath Package artifact to replace with signed output.
/// @param err Destination diagnostic stream.
/// @return true on success or when no signing was requested; false on failure.
bool signMacOSPackageArtifact(const InstallPackageArgs &args,
                              const fs::path &artifactPath,
                              std::ostream &err) {
    if (!macOSPackageSigningRequested(args))
        return true;

    std::string identity = args.macosSignIdentity.empty()
                               ? getenvOrEmpty("ZANNA_MACOS_SIGN_IDENTITY")
                               : args.macosSignIdentity;
    std::string notaryProfile = args.macosNotaryProfile.empty()
                                    ? getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE")
                                    : args.macosNotaryProfile;
    try {
        zanna::pkg::validateSingleLineField(identity, "macOS package signing identity");
        zanna::pkg::validateSingleLineField(notaryProfile, "macOS notary profile");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }
    if (identity.empty()) {
        err << "error: macOS package signing requested but no Developer ID Installer identity "
               "was provided\n";
        return false;
    }
    if (args.macosStaple && notaryProfile.empty()) {
        err << "error: --macos-staple requires --macos-notary-profile or "
               "ZANNA_MACOS_NOTARY_PROFILE\n";
        return false;
    }

#if !ZANNA_HOST_MACOS
    err << "error: macOS package signing requires running on macOS\n";
    (void)artifactPath;
    return false;
#else
    const fs::path signedParent =
        artifactPath.parent_path().empty() ? fs::current_path() : artifactPath.parent_path();
    const fs::path signedDir = zanna::pkg::createUniqueTempDirectory(signedParent, "signed-pkg");
    const fs::path signedPath = signedDir / artifactPath.filename();
    std::error_code ec;
    const RunResult signResult = run_process({"productsign",
                                              "--sign",
                                              identity,
                                              zanna::filesystem::pathToUtf8(artifactPath),
                                              zanna::filesystem::pathToUtf8(signedPath)});
    if (signResult.exit_code != 0) {
        err << "error: productsign failed with exit code " << signResult.exit_code << "\n"
            << signResult.out << signResult.err;
        fs::remove_all(signedDir, ec);
        return false;
    }
    fs::rename(signedPath, artifactPath, ec);
    if (ec) {
        err << "error: cannot replace unsigned macOS package with signed artifact: " << ec.message()
            << "\n";
        fs::remove_all(signedDir, ec);
        return false;
    }
    fs::remove_all(signedDir, ec);

    const RunResult verifyResult =
        run_process({"pkgutil", "--check-signature", zanna::filesystem::pathToUtf8(artifactPath)});
    if (verifyResult.exit_code != 0) {
        err << "error: pkgutil --check-signature failed for signed macOS package\n"
            << verifyResult.out << verifyResult.err;
        return false;
    }

    if (!notaryProfile.empty()) {
        const int notaryTimeoutSeconds =
            args.macosNotaryTimeoutSeconds > 0 ? args.macosNotaryTimeoutSeconds : 1800;
        const std::string notaryTimeoutArg = std::to_string(notaryTimeoutSeconds) + "s";
        // Bound the wait and retry once on a transient submit/network failure.
        RunResult notaryResult{};
        for (int attempt = 1; attempt <= 2; ++attempt) {
            notaryResult = run_process({"xcrun",
                                        "notarytool",
                                        "submit",
                                        zanna::filesystem::pathToUtf8(artifactPath),
                                        "--keychain-profile",
                                        notaryProfile,
                                        "--wait",
                                        "--timeout",
                                        notaryTimeoutArg});
            if (notaryResult.exit_code == 0)
                break;
        }
        if (notaryResult.exit_code != 0) {
            err << "error: notarytool submit failed with exit code " << notaryResult.exit_code
                << "\n"
                << notaryResult.out << notaryResult.err;
            return false;
        }
        if (args.macosStaple) {
            const RunResult stapleResult = run_process(
                {"xcrun", "stapler", "staple", zanna::filesystem::pathToUtf8(artifactPath)});
            if (stapleResult.exit_code != 0) {
                err << "error: stapler failed with exit code " << stapleResult.exit_code << "\n"
                    << stapleResult.out << stapleResult.err;
                return false;
            }
            const RunResult stapleValidateResult = run_process(
                {"xcrun", "stapler", "validate", zanna::filesystem::pathToUtf8(artifactPath)});
            if (stapleValidateResult.exit_code != 0) {
                err << "error: stapler validation failed with exit code "
                    << stapleValidateResult.exit_code << "\n"
                    << stapleValidateResult.out << stapleValidateResult.err;
                return false;
            }
        }
        const RunResult gatekeeperResult =
            run_process({"spctl",
                         "--assess",
                         "--type",
                         "install",
                         "--verbose=2",
                         zanna::filesystem::pathToUtf8(artifactPath)});
        if (gatekeeperResult.exit_code != 0) {
            err << "error: Gatekeeper assessment failed for notarized macOS package\n"
                << gatekeeperResult.out << gatekeeperResult.err;
            return false;
        }
    }
    return true;
#endif
}

/// @brief Notarize and optionally staple a macOS `.dmg` when a notary profile is configured.
/// @details The contained `.pkg` carries installer signing; this function treats
///          the disk image as a release artifact by submitting it separately to
///          notarytool and stapling the ticket when requested.
/// @param args Parsed notarization, timeout, and stapling policy.
/// @param dmgPath Disk image to submit and assess.
/// @param err Destination diagnostic stream.
/// @return true on success or when no DMG notarization was requested.
bool notarizeMacOSDmgArtifact(const InstallPackageArgs &args,
                              const fs::path &dmgPath,
                              std::ostream &err) {
    std::string notaryProfile = args.macosNotaryProfile.empty()
                                    ? getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE")
                                    : args.macosNotaryProfile;
    try {
        zanna::pkg::validateSingleLineField(notaryProfile, "macOS notary profile");
    } catch (const std::exception &ex) {
        err << "error: " << ex.what() << "\n";
        return false;
    }
    if (notaryProfile.empty()) {
        if (args.macosStaple) {
            err << "error: --macos-staple requires --macos-notary-profile or "
                   "ZANNA_MACOS_NOTARY_PROFILE for generated .dmg artifacts\n";
            return false;
        }
        return true;
    }
#if !ZANNA_HOST_MACOS
    err << "error: macOS .dmg notarization requires running on macOS\n";
    (void)dmgPath;
    return false;
#else
    const int notaryTimeoutSeconds =
        args.macosNotaryTimeoutSeconds > 0 ? args.macosNotaryTimeoutSeconds : 1800;
    const std::string notaryTimeoutArg = std::to_string(notaryTimeoutSeconds) + "s";
    RunResult notaryResult{};
    for (int attempt = 1; attempt <= 2; ++attempt) {
        notaryResult = run_process({"xcrun",
                                    "notarytool",
                                    "submit",
                                    zanna::filesystem::pathToUtf8(dmgPath),
                                    "--keychain-profile",
                                    notaryProfile,
                                    "--wait",
                                    "--timeout",
                                    notaryTimeoutArg});
        if (notaryResult.exit_code == 0)
            break;
    }
    if (notaryResult.exit_code != 0) {
        err << "error: notarytool submit failed for .dmg with exit code " << notaryResult.exit_code
            << "\n"
            << notaryResult.out << notaryResult.err;
        return false;
    }
    if (args.macosStaple) {
        const RunResult stapleResult =
            run_process({"xcrun", "stapler", "staple", zanna::filesystem::pathToUtf8(dmgPath)});
        if (stapleResult.exit_code != 0) {
            err << "error: stapler failed for .dmg with exit code " << stapleResult.exit_code
                << "\n"
                << stapleResult.out << stapleResult.err;
            return false;
        }
        const RunResult stapleValidateResult =
            run_process({"xcrun", "stapler", "validate", zanna::filesystem::pathToUtf8(dmgPath)});
        if (stapleValidateResult.exit_code != 0) {
            err << "error: stapler validation failed for .dmg with exit code "
                << stapleValidateResult.exit_code << "\n"
                << stapleValidateResult.out << stapleValidateResult.err;
            return false;
        }
    }
    const RunResult gatekeeperResult = run_process({"spctl",
                                                    "--assess",
                                                    "--type",
                                                    "open",
                                                    "--context",
                                                    "context:primary-signature",
                                                    "--verbose=2",
                                                    zanna::filesystem::pathToUtf8(dmgPath)});
    if (gatekeeperResult.exit_code != 0) {
        err << "error: Gatekeeper assessment failed for notarized macOS .dmg\n"
            << gatekeeperResult.out << gatekeeperResult.err;
        return false;
    }
    return true;
#endif
}

/// @brief Read a big-endian uint16 at @p off (caller must bounds-check).
/// @param data Input byte buffer.
/// @param off Offset of the first byte.
/// @return Decoded unsigned value.
uint16_t readBE16(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
}

/// @brief Read a little-endian uint16 at @p off (caller must bounds-check).
/// @param data Input byte buffer.
/// @param off Offset of the first byte.
/// @return Decoded unsigned value.
uint16_t readLE16(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
}

/// @brief Read a little-endian uint32 at @p off (caller must bounds-check).
/// @param data Input byte buffer.
/// @param off Offset of the first byte.
/// @return Decoded unsigned value.
uint32_t readLE32(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
           (static_cast<uint32_t>(data[off + 2]) << 16) |
           (static_cast<uint32_t>(data[off + 3]) << 24);
}

/// @brief Read a big-endian uint32 at @p off (caller must bounds-check).
/// @param data Input byte buffer.
/// @param off Offset of the first byte.
/// @return Decoded unsigned value.
uint32_t readBE32(const std::vector<uint8_t> &data, size_t off) {
    return (static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
           (static_cast<uint32_t>(data[off + 2]) << 8) | static_cast<uint32_t>(data[off + 3]);
}

/// @brief Return an ASCII-lowercased copy of @p text.
/// @param text Text to transform.
/// @return Lowercased copy.
std::string lowerAscii(std::string text) {
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// @brief Return the lowercased filename with any trailing ".exe" stripped.
/// @param filename Filename to normalize.
/// @return Lowercase executable base name.
std::string binaryBaseName(std::string filename) {
    filename = lowerAscii(std::move(filename));
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".exe")
        filename.resize(filename.size() - 4);
    return filename;
}

/// @brief Sanitize a version string into a portable filename component.
/// @details Keeps alphanumerics and `.+~-`, replaces anything else with `_`, and
///          falls back to "0.0.0" when the result would be empty.
/// @param version Package version text.
/// @return Portable nonempty filename component.
std::string portableArchiveVersionComponent(const std::string &version) {
    std::string out;
    out.reserve(version.size());
    for (char c : version) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '+' || c == '~' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "0.0.0" : out;
}

/// @brief Detect the platform/arch of a native executable from its header magic.
/// @details Recognises ELF, PE (MZ/PE), and Mach-O (thin and fat/universal),
///          decoding the machine/cputype field into platform+arch.
/// @param path Native executable to read.
/// @return The detected info, or std::nullopt if the format is unrecognized.
std::optional<NativeExecutableInfo> detectNativeExecutableInfo(const fs::path &path) {
    const auto data = zanna::pkg::readFile(path);
    if (data.size() < 20)
        return std::nullopt;

    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        if (data[4] != 2 || data[5] != 1)
            return std::nullopt;
        const uint16_t machine = readLE16(data, 18);
        if (machine == 62)
            return NativeExecutableInfo{"linux", "x64"};
        if (machine == 183)
            return NativeExecutableInfo{"linux", "arm64"};
        return std::nullopt;
    }

    if (data[0] == 'M' && data[1] == 'Z') {
        if (data.size() < 64)
            return std::nullopt;
        const size_t peOff = readLE32(data, 60);
        if (peOff >= data.size() || peOff > data.size() - 24 || data[peOff] != 'P' ||
            data[peOff + 1] != 'E' || data[peOff + 2] != 0 || data[peOff + 3] != 0)
            return std::nullopt;
        const uint16_t machine = readLE16(data, peOff + 4);
        if (machine == 0x8664)
            return NativeExecutableInfo{"windows", "x64"};
        if (machine == 0xAA64)
            return NativeExecutableInfo{"windows", "arm64"};
        return std::nullopt;
    }

    const uint32_t magicBE = readBE32(data, 0);
    const uint32_t magicLE = readLE32(data, 0);
    if (magicLE == 0xFEEDFACF || magicBE == 0xFEEDFACF) {
        const bool little = magicLE == 0xFEEDFACF;
        const uint32_t cputype = little ? readLE32(data, 4) : readBE32(data, 4);
        if (cputype == 0x01000007u)
            return NativeExecutableInfo{"macos", "x64"};
        if (cputype == 0x0100000Cu)
            return NativeExecutableInfo{"macos", "arm64"};
        return std::nullopt;
    }
    if ((magicBE == 0xCAFEBABEu || magicBE == 0xCAFEBABFu) && data.size() >= 8) {
        const uint32_t count = readBE32(data, 4);
        const size_t archSize = magicBE == 0xCAFEBABFu ? 32u : 20u;
        if (count == 0 || count > 64 || data.size() < 8 + static_cast<size_t>(count) * archSize)
            return std::nullopt;
        bool hasX64 = false;
        bool hasArm64 = false;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t off = 8 + static_cast<size_t>(i) * archSize;
            const uint32_t cputype = readBE32(data, off);
            hasX64 = hasX64 || cputype == 0x01000007u;
            hasArm64 = hasArm64 || cputype == 0x0100000Cu;
        }
        if (hasX64 && hasArm64)
            return NativeExecutableInfo{"macos", "universal"};
        if (hasX64)
            return NativeExecutableInfo{"macos", "x64"};
        if (hasArm64)
            return NativeExecutableInfo{"macos", "arm64"};
    }
    return std::nullopt;
}

/// @brief Detect the platform/arch from the manifest's staged `zanna` binary.
/// @param manifest Validated staged-toolchain manifest.
/// @return The detected info, or std::nullopt when no usable binary is found.
std::optional<NativeExecutableInfo> detectManifestToolchainExecutableInfo(
    const zanna::pkg::ToolchainInstallManifest &manifest) {
    for (const auto &file : manifest.files) {
        if (file.kind != zanna::pkg::ToolchainFileKind::Binary)
            continue;
        if (binaryBaseName(zanna::filesystem::pathToUtf8(file.stagedAbsolutePath.filename())) !=
            "zanna")
            continue;
        return detectNativeExecutableInfo(file.stagedAbsolutePath);
    }
    return std::nullopt;
}

/// @brief Find the first staged PE that imports an MSVC debug CRT, if any.
/// @details Scans each staged .exe/.dll's import table for the known debug-runtime
///          DLLs; used to reject (or warn about) non-redistributable debug builds.
/// @param manifest Validated staged-toolchain manifest.
/// @return A "<path> imports <dll>" description, or std::nullopt when none found.
std::optional<std::string> firstWindowsDebugRuntimeReference(
    const zanna::pkg::ToolchainInstallManifest &manifest) {
    static const char *debugDlls[] = {
        "ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll"};
    for (const auto &file : manifest.files) {
        if (file.symlink)
            continue;
        const std::string ext =
            lowerAscii(zanna::filesystem::pathToUtf8(file.stagedAbsolutePath.extension()));
        if (ext != ".exe" && ext != ".dll")
            continue;
        const auto data = zanna::pkg::readFile(file.stagedAbsolutePath);
        const auto imports = zanna::pkg::importedDllNamesFromPe(data);
        for (const char *dll : debugDlls) {
            if (std::find(imports.begin(), imports.end(), dll) != imports.end())
                return file.stagedRelativePath + " imports " + dll;
        }
    }
    return std::nullopt;
}

/// @brief Parse a --target value into an InstallPackageTarget.
/// @param text Target format token.
/// @param out Receives the parsed target on success.
/// @return true on a recognized target name; false otherwise.
bool parseTarget(const std::string &text, InstallPackageTarget &out) {
    if (text == "windows")
        out = InstallPackageTarget::Windows;
    else if (text == "macos")
        out = InstallPackageTarget::MacOS;
    else if (text == "macos-dmg")
        out = InstallPackageTarget::MacOSDmg;
    else if (text == "linux-deb")
        out = InstallPackageTarget::LinuxDeb;
    else if (text == "linux-rpm")
        out = InstallPackageTarget::LinuxRpm;
    else if (text == "linux-bundle")
        out = InstallPackageTarget::LinuxBundle;
    else if (text == "tarball")
        out = InstallPackageTarget::Tarball;
    else if (text == "all")
        out = InstallPackageTarget::All;
    else if (text == "all-available")
        out = InstallPackageTarget::AllAvailable;
    else
        return false;
    return true;
}

/// @brief Parse an on/off-style boolean option value.
/// @param text Boolean token.
/// @param out Receives the parsed Boolean on success.
/// @return true on a recognized on/off/true/false/1/0/yes/no token; false otherwise.
bool parseOnOff(const std::string &text, bool &out) {
    if (text == "on" || text == "true" || text == "1" || text == "yes") {
        out = true;
        return true;
    }
    if (text == "off" || text == "false" || text == "0" || text == "no") {
        out = false;
        return true;
    }
    return false;
}

/// @brief Parse a strictly positive decimal integer option value.
/// @details Requires full consumption of the input token, unlike std::stoi
///          which accepts numeric prefixes such as `30abc`.
/// @param text Raw command-line token to parse.
/// @param out Receives the parsed integer on success.
/// @return True when @p text is a positive base-10 int.
bool parsePositiveIntOption(std::string_view text, int &out) {
    if (text.empty())
        return false;
    int value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0)
        return false;
    out = value;
    return true;
}

/// @brief Return true if @p arg is an install-package option that takes a value.
/// @details Used during argument parsing to know when to consume the next token.
/// @param arg Option spelling to classify.
/// @return @c true when the parser must consume a following value.
bool installPackageOptionRequiresValue(const std::string &arg) {
    return arg == "--target" || arg == "--arch" || arg == "--stage-dir" || arg == "--build-dir" ||
           arg == "--config" || arg == "--verify-only" || arg == "--macos-pkg-version" ||
           arg == "--macos-min-version" || arg == "--macos-sign-identity" ||
           arg == "--macos-app-sign-identity" || arg == "--macos-notary-profile" ||
           arg == "--windows-sign-pfx" || arg == "--windows-sign-thumbprint" ||
           arg == "--windows-timestamp-url" || arg == "--windows-signtool" ||
           arg == "--windows-install-scope" || arg == "--windows-install-dir" ||
           arg == "--windows-identifier" || arg == "--windows-channel" ||
           arg == "--source-commit" || arg == "--windows-documentation-url" ||
           arg == "--windows-update-manifest-url" || arg == "--windows-update-rsa-modulus" ||
           arg == "--windows-update-rsa-exponent" || arg == "--windows-file-associations" ||
           arg == "--windows-shortcuts" || arg == "--macos-notary-timeout" || arg == "--license" ||
           arg == "--maintainer" || arg == "--maintainer-email" || arg == "--homepage" ||
           arg == "--macos-dmg-background" || arg == "--macos-dmg-icon" ||
           arg == "--macos-pkg-license" || arg == "--macos-pkg-background" ||
           arg == "--linux-sign-key" || arg == "--output-file" || arg == "--output-dir" ||
           arg == "--artifact-manifest" || arg == "-o";
}

/// @brief Parse the `zanna install-package` command line into @p args.
/// @details Handles the target/source options and the macOS/Windows signing and
///          installer options; prints usage and returns false on a malformed or
///          missing-value argument.
/// @param argc Number of install-package arguments.
/// @param argv Argument vector.
/// @param args Output configuration.
/// @return true on a successful parse.
bool parseInstallPackageArgs(int argc, char **argv, InstallPackageArgs &args) {
    std::vector<std::string> expandedArgs;
    expandedArgs.reserve(static_cast<size_t>(argc) * 2u);
    for (int rawIndex = 0; rawIndex < argc; ++rawIndex) {
        std::string raw = argv[rawIndex];
        const size_t eq = raw.find('=');
        const std::string optionName = eq == std::string::npos ? raw : raw.substr(0, eq);
        if (eq != std::string::npos && raw.rfind("--", 0) == 0 &&
            installPackageOptionRequiresValue(optionName)) {
            expandedArgs.push_back(optionName);
            expandedArgs.push_back(raw.substr(eq + 1));
        } else {
            expandedArgs.push_back(std::move(raw));
        }
    }

    for (size_t i = 0; i < expandedArgs.size(); ++i) {
        const std::string arg = expandedArgs[i];
        if ((arg == "--help" || arg == "-h")) {
            installPackageUsage();
            return false;
        }
        if (installPackageOptionRequiresValue(arg) && i + 1 >= expandedArgs.size()) {
            std::cerr << "error: " << arg << " requires a value\n";
            return false;
        } else if (arg == "--target" && i + 1 < expandedArgs.size()) {
            const std::string targetName = expandedArgs[++i];
            if (!parseTarget(targetName, args.target)) {
                std::cerr << "error: unknown install-package target '" << expandedArgs[i] << "'\n";
                return false;
            }
        } else if (arg == "--arch" && i + 1 < expandedArgs.size()) {
            args.archOverride = expandedArgs[++i];
            if (args.archOverride != "x64" && args.archOverride != "arm64") {
                std::cerr << "error: unknown arch '" << args.archOverride
                          << "'; expected x64 or arm64\n";
                return false;
            }
        } else if (arg == "--stage-dir" && i + 1 < expandedArgs.size()) {
            args.stageDir = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
        } else if (arg == "--build-dir" && i + 1 < expandedArgs.size()) {
            args.buildDir = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
        } else if (arg == "--config" && i + 1 < expandedArgs.size()) {
            args.buildConfig = expandedArgs[++i];
        } else if (arg == "--verify-only" && i + 1 < expandedArgs.size()) {
            args.verifyOnlyPath = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
        } else if (arg == "--macos-pkg-version" && i + 1 < expandedArgs.size()) {
            args.macosPackageVersion = expandedArgs[++i];
            try {
                zanna::pkg::validateDottedNumericVersion(args.macosPackageVersion,
                                                         "macOS package version override");
            } catch (const std::exception &ex) {
                std::cerr << "error: " << ex.what() << "\n";
                return false;
            }
        } else if (arg == "--macos-min-version" && i + 1 < expandedArgs.size()) {
            args.macosMinimumVersion = expandedArgs[++i];
            try {
                zanna::pkg::validateDottedNumericVersion(args.macosMinimumVersion,
                                                         "minimum macOS version");
            } catch (const std::exception &ex) {
                std::cerr << "error: " << ex.what() << "\n";
                return false;
            }
        } else if (arg == "--macos-sign-identity" && i + 1 < expandedArgs.size()) {
            args.macosSignIdentity = expandedArgs[++i];
        } else if (arg == "--macos-app-sign-identity" && i + 1 < expandedArgs.size()) {
            args.macosApplicationSignIdentity = expandedArgs[++i];
        } else if (arg == "--macos-notary-profile" && i + 1 < expandedArgs.size()) {
            args.macosNotaryProfile = expandedArgs[++i];
        } else if (arg == "--macos-staple") {
            args.macosStaple = true;
        } else if (arg == "--macos-notary-timeout" && i + 1 < expandedArgs.size()) {
            const std::string_view timeoutValue = expandedArgs[++i];
            if (!parsePositiveIntOption(timeoutValue, args.macosNotaryTimeoutSeconds)) {
                std::cerr << "error: --macos-notary-timeout expects a positive number of seconds\n";
                return false;
            }
        } else if (arg == "--license" && i + 1 < expandedArgs.size()) {
            args.toolchainLicense = expandedArgs[++i];
        } else if (arg == "--maintainer" && i + 1 < expandedArgs.size()) {
            args.toolchainMaintainer = expandedArgs[++i];
        } else if (arg == "--maintainer-email" && i + 1 < expandedArgs.size()) {
            args.toolchainMaintainerEmail = expandedArgs[++i];
        } else if (arg == "--homepage" && i + 1 < expandedArgs.size()) {
            args.toolchainHomepage = expandedArgs[++i];
        } else if (arg == "--macos-dmg") {
            args.macosDmg = true;
        } else if (arg == "--macos-dmg-background" && i + 1 < expandedArgs.size()) {
            args.macosDmgBackground = expandedArgs[++i];
            if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(args.macosDmgBackground))) {
                std::cerr << "error: --macos-dmg-background not found: " << args.macosDmgBackground
                          << "\n";
                return false;
            }
        } else if (arg == "--macos-dmg-icon" && i + 1 < expandedArgs.size()) {
            args.macosDmgIcon = expandedArgs[++i];
            if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(args.macosDmgIcon))) {
                std::cerr << "error: --macos-dmg-icon not found: " << args.macosDmgIcon << "\n";
                return false;
            }
        } else if (arg == "--macos-pkg-license" && i + 1 < expandedArgs.size()) {
            args.macosPkgLicense = expandedArgs[++i];
            if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(args.macosPkgLicense))) {
                std::cerr << "error: --macos-pkg-license not found: " << args.macosPkgLicense
                          << "\n";
                return false;
            }
        } else if (arg == "--macos-pkg-background" && i + 1 < expandedArgs.size()) {
            args.macosPkgBackground = expandedArgs[++i];
            if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(args.macosPkgBackground))) {
                std::cerr << "error: --macos-pkg-background not found: " << args.macosPkgBackground
                          << "\n";
                return false;
            }
        } else if (arg == "--windows-sign") {
            args.windowsSign = true;
        } else if (arg == "--windows-sign-pfx" && i + 1 < expandedArgs.size()) {
            args.windowsSignPfx = expandedArgs[++i];
        } else if (arg == "--windows-sign-thumbprint" && i + 1 < expandedArgs.size()) {
            args.windowsSignThumbprint = expandedArgs[++i];
        } else if (arg == "--windows-timestamp-url" && i + 1 < expandedArgs.size()) {
            args.windowsTimestampUrl = expandedArgs[++i];
        } else if (arg == "--windows-signtool" && i + 1 < expandedArgs.size()) {
            args.windowsSigntoolPath = expandedArgs[++i];
        } else if (arg == "--windows-sign-no-verify") {
            args.windowsSignNoVerify = true;
        } else if (arg == "--linux-sign-key" && i + 1 < expandedArgs.size()) {
            args.linuxSignKey = expandedArgs[++i];
        } else if (arg == "--windows-install-scope" && i + 1 < expandedArgs.size()) {
            args.windowsInstallScope = expandedArgs[++i];
            if (args.windowsInstallScope != "user" && args.windowsInstallScope != "machine") {
                std::cerr << "error: --windows-install-scope expects user or machine\n";
                return false;
            }
        } else if (arg == "--windows-install-dir" && i + 1 < expandedArgs.size()) {
            args.windowsInstallDir = expandedArgs[++i];
            args.windowsInstallDirSpecified = true;
        } else if (arg == "--windows-identifier" && i + 1 < expandedArgs.size()) {
            args.windowsIdentifier = expandedArgs[++i];
            args.windowsIdentifierSpecified = true;
        } else if (arg == "--windows-channel" && i + 1 < expandedArgs.size()) {
            args.windowsChannel = lowerAscii(expandedArgs[++i]);
        } else if (arg == "--source-commit" && i + 1 < expandedArgs.size()) {
            args.sourceCommitOverride = lowerAscii(expandedArgs[++i]);
        } else if (arg == "--windows-documentation-url" && i + 1 < expandedArgs.size()) {
            args.windowsDocumentationUrl = expandedArgs[++i];
        } else if (arg == "--windows-update-manifest-url" && i + 1 < expandedArgs.size()) {
            args.windowsUpdateManifestUrl = expandedArgs[++i];
        } else if (arg == "--windows-update-rsa-modulus" && i + 1 < expandedArgs.size()) {
            args.windowsUpdateRsaModulus = lowerAscii(expandedArgs[++i]);
        } else if (arg == "--windows-update-rsa-exponent" && i + 1 < expandedArgs.size()) {
            args.windowsUpdateRsaExponent = lowerAscii(expandedArgs[++i]);
        } else if (arg == "--windows-no-path") {
            args.windowsAddToPath = false;
        } else if (arg == "--windows-file-associations" && i + 1 < expandedArgs.size()) {
            if (!parseOnOff(lowerAscii(expandedArgs[++i]), args.windowsFileAssociations)) {
                std::cerr << "error: --windows-file-associations expects on or off\n";
                return false;
            }
        } else if (arg == "--windows-shortcuts" && i + 1 < expandedArgs.size()) {
            if (!parseOnOff(lowerAscii(expandedArgs[++i]), args.windowsShortcuts)) {
                std::cerr << "error: --windows-shortcuts expects on or off\n";
                return false;
            }
        } else if (arg == "--allow-debug-toolchain") {
            args.allowDebugToolchain = true;
        } else if (arg == "-o" && i + 1 < expandedArgs.size()) {
            args.outputPath = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
        } else if (arg == "--output-file" && i + 1 < expandedArgs.size()) {
            args.outputPath = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
            args.outputAsFile = true;
        } else if (arg == "--output-dir" && i + 1 < expandedArgs.size()) {
            args.outputPath = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
            args.outputAsDirectory = true;
        } else if (arg == "--artifact-manifest" && i + 1 < expandedArgs.size()) {
            args.artifactManifestPath = zanna::filesystem::pathFromUtf8(expandedArgs[++i]);
        } else if (arg == "--release") {
            args.releaseMode = true;
        } else if (arg == "--require-checksum") {
            args.requireChecksum = true;
        } else if (arg == "--no-verify") {
            args.noVerify = true;
        } else if (arg == "--skip-build") {
            args.skipBuild = true;
        } else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        } else if (arg == "--keep-stage-dir") {
            args.keepStageDir = true;
        } else if (arg == "--stage-only") {
            args.stageOnly = true;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return false;
        }
    }

    int modeCount = 0;
    if (!args.stageDir.empty())
        ++modeCount;
    if (!args.buildDir.empty())
        ++modeCount;
    if (!args.verifyOnlyPath.empty())
        ++modeCount;
    if (modeCount != 1) {
        std::cerr << "error: require exactly one of --stage-dir, --build-dir, or --verify-only\n";
        return false;
    }
    if (args.outputAsFile && args.outputAsDirectory) {
        std::cerr << "error: --output-file and --output-dir are mutually exclusive\n";
        return false;
    }
    if (!args.verifyOnlyPath.empty() && args.releaseMode) {
        std::cerr << "error: --release is a generation mode and cannot be combined with "
                     "--verify-only; use --require-checksum plus the platform trust verifier\n";
        return false;
    }
    if (args.verifyOnlyPath.empty() && args.requireChecksum) {
        std::cerr << "error: --require-checksum requires --verify-only\n";
        return false;
    }
    if (args.releaseMode &&
        (args.noVerify || args.windowsSignNoVerify || args.allowDebugToolchain)) {
        std::cerr << "error: --release forbids --no-verify, --windows-sign-no-verify, and "
                     "--allow-debug-toolchain\n";
        return false;
    }
    if (args.releaseMode) {
        const std::string epoch = getenvOrEmpty("SOURCE_DATE_EPOCH");
        /// @brief Test one source-date epoch byte for decimal syntax.
        /// @param c Byte to inspect.
        /// @return `true` for `0` through `9`.
        if (epoch.empty() ||
            !std::all_of(epoch.begin(), epoch.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            std::cerr << "error: --release requires numeric SOURCE_DATE_EPOCH for reproducible "
                         "metadata\n";
            return false;
        }
    }
    if (!args.releaseMode && args.windowsChannel == "stable") {
        std::cerr << "error: the stable Windows channel is reserved for --release packages\n";
        return false;
    }
#if ZANNA_HOST_WINDOWS
    if (!args.buildDir.empty() && args.buildConfig.empty())
        args.buildConfig = "Release";
#endif
    try {
        zanna::pkg::validateHttpsPackageUrl(args.windowsTimestampUrl, "Windows timestamp URL");
        zanna::pkg::validateSingleLineField(args.windowsSigntoolPath, "Windows signtool path");
        zanna::pkg::validateWindowsFileName(args.windowsInstallDir, "Windows install directory");
        zanna::pkg::validateWindowsProgIdBase(args.windowsIdentifier, "Windows package identifier");
        zanna::pkg::validateSingleLineField(args.windowsChannel, "Windows release channel");
        const WindowsToolchainIdentity identity = windowsToolchainIdentity(args);
        validateWindowsReleaseChannel(identity.channel);
        zanna::pkg::validateWindowsFileName(identity.installDir, "Windows install directory");
        zanna::pkg::validateWindowsProgIdBase(identity.identifier, "Windows package identifier");
        zanna::pkg::validateWindowsFileName(identity.displayName, "Windows display name");
        zanna::pkg::validateSingleLineField(args.sourceCommitOverride, "source commit override");
        zanna::pkg::validateHttpsPackageUrl(args.windowsDocumentationUrl,
                                            "Windows documentation URL");
        zanna::pkg::validateHttpsPackageUrl(args.windowsUpdateManifestUrl,
                                            "Windows update manifest URL");
        zanna::pkg::validateSingleLineField(args.windowsUpdateRsaModulus,
                                            "Windows update RSA modulus");
        zanna::pkg::validateSingleLineField(args.windowsUpdateRsaExponent,
                                            "Windows update RSA exponent");
        zanna::pkg::validateWindowsCertificateThumbprint(args.windowsSignThumbprint,
                                                         "Windows signing thumbprint");
        zanna::pkg::validateSingleLineField(args.macosSignIdentity,
                                            "macOS package signing identity");
        zanna::pkg::validateSingleLineField(args.macosApplicationSignIdentity,
                                            "macOS application signing identity");
        zanna::pkg::validateSingleLineField(args.macosNotaryProfile, "macOS notary profile");
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return false;
    }
    return true;
}

/// @brief Build the conventional output filename for a target from the manifest.
/// @details Encodes the version/arch/platform per platform naming convention
///          (e.g. `zanna_<v>_<arch>.deb`); returns "" for the All meta-target.
/// @param target Concrete artifact format.
/// @param manifest Toolchain metadata supplying version, architecture, and platform.
/// @return Conventional filename, or empty for meta-targets.
std::string targetFileName(InstallPackageTarget target,
                           const zanna::pkg::ToolchainInstallManifest &manifest) {
    const std::string version = manifest.version.empty() ? "0.0.0" : manifest.version;
    const std::string arch = manifest.arch.empty() ? "x64" : manifest.arch;
    const std::string platform = manifest.platform.empty() ? hostPlatformName() : manifest.platform;
    switch (target) {
        case InstallPackageTarget::Windows:
            return "zanna-" + version + "-win-" + arch + ".exe";
        case InstallPackageTarget::MacOS:
            return "zanna-" + version + "-macos-" + arch + ".pkg";
        case InstallPackageTarget::MacOSDmg:
            return "zanna-" + version + "-macos-" + arch + ".dmg";
        case InstallPackageTarget::LinuxDeb:
            return "zanna_" + version + "_" + debArchFor(arch) + ".deb";
        case InstallPackageTarget::LinuxRpm:
            return "zanna-" + version + "-1." + rpmArchFor(arch) + ".rpm";
        case InstallPackageTarget::LinuxBundle:
            return "zanna-" + version + "-linux-" + arch + ".run";
        case InstallPackageTarget::Tarball:
            return "zanna-" + version + "-" + platform + "-" + arch + ".tar.gz";
        case InstallPackageTarget::All:
        case InstallPackageTarget::AllAvailable:
        default:
            return {};
    }
}

/// @brief Stable machine-readable format name for an installer artifact target.
/// @param target Concrete or meta artifact target.
/// @return Stable format token, or @c unknown for meta-targets.
std::string artifactFormatName(InstallPackageTarget target) {
    switch (target) {
        case InstallPackageTarget::Windows:
            return "windows-exe";
        case InstallPackageTarget::MacOS:
            return "macos-pkg";
        case InstallPackageTarget::MacOSDmg:
            return "macos-dmg";
        case InstallPackageTarget::LinuxDeb:
            return "debian";
        case InstallPackageTarget::LinuxRpm:
            return "rpm";
        case InstallPackageTarget::LinuxBundle:
            return "linux-bundle";
        case InstallPackageTarget::Tarball:
            return "tar-gzip";
        default:
            return "unknown";
    }
}

/// @brief One verified release artifact and its immutable inventory metadata.
struct ArtifactRecord {
    fs::path path;
    std::string format;
    std::string platform;
    std::string arch;
    std::string version;
    std::string sha256;
    uint64_t sizeBytes{0};
    bool verified{false};
    std::string trust;
};

/// @brief Escape UTF-8 bytes for JSON strings without adding a dependency.
/// @param text Unquoted bytes to encode.
/// @return Escaped bytes without surrounding quotation marks.
std::string artifactJsonEscape(std::string_view text) {
    std::ostringstream out;
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
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
                if (c < 0x20u) {
                    out << "\\u00" << hex[(c >> 4u) & 0x0Fu] << hex[c & 0x0Fu];
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

/// @brief Hash an artifact and return its immutable inventory record.
/// @param path Artifact file to read.
/// @param target Concrete artifact format.
/// @param manifest Manifest supplying platform, architecture, and version.
/// @param verified Whether post-build verification succeeded.
/// @param trust Stable trust-policy description to own.
/// @return Complete inventory record including SHA-256 and byte size.
ArtifactRecord inventoryArtifact(const fs::path &path,
                                 InstallPackageTarget target,
                                 const zanna::pkg::ToolchainInstallManifest &manifest,
                                 bool verified,
                                 std::string trust) {
    const std::vector<uint8_t> bytes = zanna::pkg::readFile(path);
    ArtifactRecord record;
    record.path = path;
    record.format = artifactFormatName(target);
    record.platform = manifest.platform;
    record.arch = manifest.arch;
    record.version = manifest.version;
    record.sha256 = zanna::pkg::sha256Hex(bytes.data(), bytes.size());
    record.sizeBytes = bytes.size();
    record.verified = verified;
    record.trust = std::move(trust);
    return record;
}

/// @brief Validate an adjacent SHA-256 sidecar when present or required.
/// @param artifact Artifact whose @c .sha256 sidecar is checked.
/// @param required Whether absence is an error.
/// @param err Destination diagnostic stream.
/// @return @c true when absent-but-optional or fully valid.
bool verifyArtifactChecksum(const fs::path &artifact, bool required, std::ostream &err) {
    fs::path sidecar = artifact;
    sidecar += ".sha256";
    std::error_code ec;
    if (!fs::is_regular_file(sidecar, ec)) {
        if (required)
            err << "checksum: required sidecar is missing: "
                << zanna::filesystem::pathToUtf8(sidecar) << "\n";
        return !required;
    }
    const std::vector<uint8_t> sidecarBytes = zanna::pkg::readFile(sidecar);
    const std::string text(sidecarBytes.begin(), sidecarBytes.end());
    /// @brief Test one checksum byte for lowercase hexadecimal syntax.
    /// @param c Byte to inspect.
    /// @return `true` for `0` through `9` or `a` through `f`.
    if (text.size() < 66u || text[64] != ' ' || text[65] != ' ' ||
        !std::all_of(text.begin(), text.begin() + 64, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        err << "checksum: invalid SHA-256 sidecar: " << zanna::filesystem::pathToUtf8(sidecar)
            << "\n";
        return false;
    }
    if (text.size() > 66u) {
        const size_t newline = text.find_first_of("\r\n", 66u);
        const std::string listedName = text.substr(66u, newline - 66u);
        const std::string artifactName = zanna::filesystem::pathToUtf8(artifact.filename());
        if (!listedName.empty() && listedName != artifactName) {
            err << "checksum: sidecar names '" << listedName << "' instead of '" << artifactName
                << "'\n";
            return false;
        }
    }
    const std::vector<uint8_t> artifactBytes = zanna::pkg::readFile(artifact);
    const std::string actual = zanna::pkg::sha256Hex(artifactBytes.data(), artifactBytes.size());
    if (actual != text.substr(0, 64u)) {
        err << "checksum: SHA-256 mismatch for " << zanna::filesystem::pathToUtf8(artifact) << "\n";
        return false;
    }
    return true;
}

/// @brief Write consolidated checksums and the release artifact JSON inventory.
/// @param records Artifact records to sort and publish.
/// @param directoryOutput Whether @p outputBase denotes the output directory.
/// @param outputBase Selected output file or directory.
/// @param manifestOverride Optional explicit inventory path.
/// @return Written inventory path.
/// @throws std::runtime_error On empty records, path collision, or atomic-write failure.
fs::path writeArtifactInventory(std::vector<ArtifactRecord> records,
                                bool directoryOutput,
                                const fs::path &outputBase,
                                const fs::path &manifestOverride) {
    /// @brief Order artifact records by their output leaf names.
    /// @param lhs Left-hand artifact.
    /// @param rhs Right-hand artifact.
    /// @return `true` when the UTF-8 filename of `lhs` precedes that of `rhs`.
    std::sort(records.begin(), records.end(), [](const auto &lhs, const auto &rhs) {
        return zanna::filesystem::pathToUtf8(lhs.path.filename()) <
               zanna::filesystem::pathToUtf8(rhs.path.filename());
    });
    const fs::path outputDirectory = directoryOutput ? outputBase
                                                     : (records.front().path.parent_path().empty()
                                                            ? fs::current_path()
                                                            : records.front().path.parent_path());
    fs::path defaultManifest = records.front().path;
    defaultManifest += ".manifest.json";
    const fs::path manifestPath =
        !manifestOverride.empty()
            ? manifestOverride
            : (directoryOutput || records.size() > 1u ? outputDirectory / "zanna-artifacts.json"
                                                      : defaultManifest);
    const fs::path normalizedManifest = fs::absolute(manifestPath).lexically_normal();
    const fs::path checksumPath = outputDirectory / "SHA256SUMS";
    if ((directoryOutput || records.size() > 1u) &&
        normalizedManifest == fs::absolute(checksumPath).lexically_normal()) {
        throw std::runtime_error("artifact manifest path collides with SHA256SUMS: " +
                                 zanna::filesystem::pathToUtf8(manifestPath));
    }
    for (const auto &record : records) {
        const fs::path normalizedArtifact = fs::absolute(record.path).lexically_normal();
        fs::path sidecarPath = record.path;
        sidecarPath += ".sha256";
        const fs::path normalizedSidecar = fs::absolute(sidecarPath).lexically_normal();
        if (normalizedManifest == normalizedArtifact || normalizedManifest == normalizedSidecar) {
            throw std::runtime_error("artifact manifest path collides with artifact output: " +
                                     zanna::filesystem::pathToUtf8(manifestPath));
        }
        zanna::pkg::writeTextFileAtomic(
            sidecarPath,
            record.sha256 + "  " + zanna::filesystem::pathToUtf8(record.path.filename()) + "\n");
    }
    if (directoryOutput || records.size() > 1u) {
        std::ostringstream checksums;
        for (const auto &record : records)
            checksums << record.sha256 << "  "
                      << zanna::filesystem::pathToUtf8(record.path.filename()) << "\n";
        zanna::pkg::writeTextFileAtomic(checksumPath, checksums.str());
    }
    std::ostringstream json;
    json << "{\n  \"schema_version\": 1,\n  \"source_date_epoch\": ";
    const std::string epoch = getenvOrEmpty("SOURCE_DATE_EPOCH");
    if (epoch.empty())
        json << "null";
    else
        json << "\"" << artifactJsonEscape(epoch) << "\"";
    json << ",\n  \"artifacts\": [\n";
    for (size_t index = 0; index < records.size(); ++index) {
        const ArtifactRecord &record = records[index];
        json << "    {\"file\": \""
             << artifactJsonEscape(zanna::filesystem::pathToUtf8(record.path.filename()))
             << "\", \"format\": \"" << artifactJsonEscape(record.format) << "\", \"platform\": \""
             << artifactJsonEscape(record.platform) << "\", \"arch\": \""
             << artifactJsonEscape(record.arch) << "\", \"version\": \""
             << artifactJsonEscape(record.version) << "\", \"size\": " << record.sizeBytes
             << ", \"sha256\": \"" << record.sha256
             << "\", \"verified\": " << (record.verified ? "true" : "false") << ", \"trust\": \""
             << artifactJsonEscape(record.trust) << "\"}";
        if (index + 1u != records.size())
            json << ',';
        json << '\n';
    }
    json << "  ]\n}\n";
    zanna::pkg::writeTextFileAtomic(manifestPath, json.str());
    return manifestPath;
}

/// @brief Compute the payload paths a built package of @p target must contain.
/// @details Derives the expected install-relative paths from the manifest files,
///          adding the platform-specific layout prefixes and any file-association
///          metadata (.desktop/MIME entries). Used by post-build verification.
/// @param target Concrete package format.
/// @param manifest Validated staged-toolchain manifest.
/// @return The list of required payload paths for @p target.
std::vector<std::string> requiredPayloadPaths(
    InstallPackageTarget target, const zanna::pkg::ToolchainInstallManifest &manifest) {
    std::vector<std::string> paths;
    paths.reserve(manifest.files.size() + 1);
    /// @brief Append Linux desktop and MIME metadata required by file associations.
    /// @param prefix Package-layout prefix prepended to emitted paths.
    /// @param portable Whether paths use the portable `share/` layout.
    auto appendLinuxAssociationMetadata = [&](const std::string &prefix, bool portable) {
        if (manifest.fileAssociations.empty())
            return;
        bool hasSource = false;
        bool hasIl = false;
        for (const auto &assoc : manifest.fileAssociations) {
            std::string ext = assoc.extension;
            for (char &c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".il")
                hasIl = true;
            else
                hasSource = true;
        }
        const std::string appDir = portable ? "share/applications/" : "usr/share/applications/";
        const std::string mimeDir = portable ? "share/mime/packages/" : "usr/share/mime/packages/";
        if (hasSource)
            paths.push_back(prefix + appDir + "zanna-source.desktop");
        if (hasIl)
            paths.push_back(prefix + appDir + "zanna-il.desktop");
        paths.push_back(prefix + mimeDir + "zanna.xml");
    };
    switch (target) {
        case InstallPackageTarget::Windows:
            for (const auto &file : manifest.files) {
                const std::string relative = zanna::pkg::sanitizePackageRelativePath(
                    file.stagedRelativePath, "windows toolchain path");
                const std::string lowerRelative = lowerAscii(relative);
                if (lowerRelative == "bin/zanna-installer-host.exe" ||
                    lowerRelative == "bin/zanna-installer-cleanup.exe") {
                    continue;
                }
                paths.push_back(relative);
            }
            paths.push_back("bin/zanna-dev.cmd");
            paths.push_back("share/zanna/README.windows-prerequisites.txt");
            paths.push_back("share/zanna/zanna.ico");
            break;
        case InstallPackageTarget::LinuxDeb:
        case InstallPackageTarget::LinuxRpm:
            for (const auto &file : manifest.files) {
                const std::string installPath =
                    zanna::pkg::mapInstallPath(file, zanna::pkg::InstallPathPolicy::LinuxUsrRoot);
                paths.push_back(zanna::pkg::sanitizePackageRelativePath(
                    installPath.size() > 1 ? installPath.substr(1) : installPath,
                    "linux install path"));
            }
            paths.push_back("usr/share/applications/zannastudio.desktop");
            paths.push_back("usr/share/icons/hicolor/256x256/apps/zanna.png");
            appendLinuxAssociationMetadata("", false);
            break;
        case InstallPackageTarget::LinuxBundle:
            for (const auto &file : manifest.files) {
                paths.push_back(zanna::pkg::mapInstallPath(
                    file, zanna::pkg::InstallPathPolicy::PortableArchive));
            }
            paths.push_back("AppRun");
            paths.push_back(".DirIcon");
            paths.push_back("zanna.desktop");
            paths.push_back("zanna.png");
            paths.push_back("share/applications/zannastudio.desktop");
            paths.push_back("share/icons/hicolor/256x256/apps/zanna.png");
            appendLinuxAssociationMetadata("", true);
            break;
        case InstallPackageTarget::Tarball: {
            const std::string version = manifest.version.empty() ? "0.0.0" : manifest.version;
            const std::string packageName = "zanna";
            const std::string topDir = zanna::pkg::sanitizePackageRelativePath(
                packageName + "-" + portableArchiveVersionComponent(version) + "-" +
                    manifest.platform + "-" + manifest.arch,
                "toolchain tarball top-level directory");
            for (const auto &file : manifest.files) {
                paths.push_back(topDir + "/" +
                                zanna::pkg::mapInstallPath(
                                    file, zanna::pkg::InstallPathPolicy::PortableArchive));
            }
            if (manifest.platform == "linux") {
                appendLinuxAssociationMetadata(topDir + "/", true);
                paths.push_back(topDir + "/share/applications/zannastudio.desktop");
                paths.push_back(topDir + "/share/icons/hicolor/256x256/apps/zanna.png");
                paths.push_back(topDir + "/share/zanna/install_manifest.txt");
                paths.push_back(topDir + "/install.sh");
                paths.push_back(topDir + "/uninstall.sh");
                paths.push_back(topDir + "/README.install");
            }
            break;
        }
        case InstallPackageTarget::MacOS:
            for (const auto &file : manifest.files) {
                const std::string rel = zanna::pkg::sanitizePackageRelativePath(
                    file.stagedRelativePath, "macOS toolchain path");
                paths.push_back("usr/local/zanna/" + rel);
                if (file.kind == zanna::pkg::ToolchainFileKind::Binary ||
                    rel.rfind("bin/", 0) == 0) {
                    paths.push_back("usr/local/bin/" +
                                    zanna::filesystem::genericPathToUtf8(
                                        zanna::filesystem::pathFromUtf8(rel).filename()));
                } else if (file.kind == zanna::pkg::ToolchainFileKind::ManPage ||
                           rel.rfind("share/man/", 0) == 0) {
                    static constexpr std::string_view kManPrefix = "share/man/";
                    if (rel.rfind(kManPrefix, 0) == 0) {
                        paths.push_back("usr/local/share/man/" + rel.substr(kManPrefix.size()));
                    }
                }
            }
            paths.push_back("usr/local/zanna/share/zanna/install_manifest.txt");
            paths.push_back("usr/local/zanna/share/zanna/uninstall.sh");
            paths.push_back("usr/local/lib/cmake/Zanna/ZannaConfig.cmake");
            paths.push_back("usr/local/lib/cmake/Zanna/ZannaConfigVersion.cmake");
            if (!manifest.fileAssociations.empty()) {
                paths.push_back("Applications/Zanna Toolchain.app/Contents/Info.plist");
                paths.push_back("Applications/Zanna Toolchain.app/Contents/PkgInfo");
                paths.push_back("Applications/Zanna Toolchain.app/Contents/Resources/Zanna.icns");
                paths.push_back(
                    "Applications/Zanna Toolchain.app/Contents/MacOS/zanna-file-handler");
            }
            break;
        case InstallPackageTarget::MacOSDmg:
            break;
        case InstallPackageTarget::All:
        case InstallPackageTarget::AllAvailable:
            break;
    }
    return paths;
}

/// @brief Verify every @p required path (leading slashes stripped) is in @p actual.
/// @param actual Set of payload paths actually present in the built artifact.
/// @param required Paths that must be present.
/// @param kind Artifact-kind label for diagnostics.
/// @param err Stream for error messages.
/// @return true when all required paths are present.
bool requireListedPayloadPaths(const std::set<std::string> &actual,
                               const std::vector<std::string> &required,
                               const char *kind,
                               std::ostream &err) {
    for (const auto &path : required) {
        std::string normalized = path;
        while (!normalized.empty() && normalized.front() == '/')
            normalized.erase(normalized.begin());
        if (actual.find(normalized) == actual.end()) {
            err << kind << ": missing required payload path " << normalized << "\n";
            return false;
        }
    }
    return true;
}

/// @brief Return the gzip tar payload appended to a Zanna Linux bundle.
/// @details Generated bundles are shell stubs followed by
///          kLinuxRuntimePayloadMarker and a gzip-compressed tar tree.
/// @param data Complete Linux bundle bytes.
/// @param err Stream that receives a diagnostic when the marker or payload is missing.
/// @return The bytes following the payload marker, or an empty vector on failure.
std::vector<uint8_t> linuxBundlePayloadBytes(const std::vector<uint8_t> &data, std::ostream &err) {
    const std::string marker = std::string(zanna::pkg::kLinuxRuntimePayloadMarker) + "\n";
    const auto markerIt =
        std::search(data.begin(),
                    data.end(),
                    reinterpret_cast<const uint8_t *>(marker.data()),
                    reinterpret_cast<const uint8_t *>(marker.data()) + marker.size());
    if (markerIt == data.end()) {
        err << "Linux bundle: missing payload marker\n";
        return {};
    }
    const auto payloadIt = markerIt + static_cast<std::ptrdiff_t>(marker.size());
    if (payloadIt == data.end()) {
        err << "Linux bundle: missing appended payload\n";
        return {};
    }
    return std::vector<uint8_t>(payloadIt, data.end());
}

/// @brief One index entry in an RPM header section.
struct RpmHeaderEntry {
    uint32_t tag{0};    ///< RPM tag id.
    uint32_t type{0};   ///< Value type code.
    uint32_t offset{0}; ///< Offset into the header's data store.
    uint32_t count{0};  ///< Number of values.
};

/// @brief A parsed RPM header: its index entries and data-store bounds.
struct RpmHeaderView {
    size_t storeOffset{0};               ///< File offset of the data store.
    size_t storeSize{0};                 ///< Size of the data store in bytes.
    size_t endOffset{0};                 ///< File offset just past this header.
    std::vector<RpmHeaderEntry> entries; ///< Index entries describing the values.
};

/// @brief Parse an RPM header section at @p offset into @p header.
/// @details Validates the header magic and index/store sizes; used to read the
///          file listing out of a generated .rpm for verification.
/// @param data Whole .rpm file bytes.
/// @param offset File offset where the header begins.
/// @param name Header name for diagnostics (e.g. "signature", "main").
/// @param header Output parsed header view.
/// @param err Stream for error messages.
/// @return true when the header parses successfully.
bool parseRpmHeaderAt(const std::vector<uint8_t> &data,
                      size_t offset,
                      const char *name,
                      RpmHeaderView &header,
                      std::ostream &err) {
    if (offset + 16 > data.size()) {
        err << "rpm: missing " << name << " header\n";
        return false;
    }
    if (data[offset] != 0x8E || data[offset + 1] != 0xAD || data[offset + 2] != 0xE8 ||
        data[offset + 3] != 0x01) {
        err << "rpm: missing " << name << " header magic\n";
        return false;
    }
    const uint32_t indexCount = readBE32(data, offset + 8);
    const uint32_t storeSize = readBE32(data, offset + 12);
    const size_t entriesOffset = offset + 16;
    if (indexCount > (data.size() - entriesOffset) / 16u) {
        err << "rpm: " << name << " header index extends past end of file\n";
        return false;
    }
    const size_t storeOffset = entriesOffset + static_cast<size_t>(indexCount) * 16u;
    if (storeSize > data.size() - storeOffset) {
        err << "rpm: " << name << " header store extends past end of file\n";
        return false;
    }

    header.storeOffset = storeOffset;
    header.storeSize = storeSize;
    header.endOffset = storeOffset + storeSize;
    header.entries.clear();
    header.entries.reserve(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        const size_t entryOffset = entriesOffset + static_cast<size_t>(i) * 16u;
        RpmHeaderEntry entry;
        entry.tag = readBE32(data, entryOffset);
        entry.type = readBE32(data, entryOffset + 4);
        entry.offset = readBE32(data, entryOffset + 8);
        entry.count = readBE32(data, entryOffset + 12);
        if (entry.offset > storeSize) {
            err << "rpm: " << name << " header tag " << entry.tag
                << " points past the header store\n";
            return false;
        }
        header.entries.push_back(entry);
    }
    return true;
}

/// @brief Find the header index entry with the given RPM @p tag.
/// @param header Parsed RPM header whose index is searched.
/// @param tag Numeric RPM tag identifier to locate.
/// @return Pointer to the entry, or nullptr if the tag is absent.
const RpmHeaderEntry *findRpmHeaderEntry(const RpmHeaderView &header, uint32_t tag) {
    /// @brief Match one RPM header entry by numeric tag.
    /// @param entry Candidate header entry.
    /// @return `true` when its tag equals the requested tag.
    auto it = std::find_if(header.entries.begin(), header.entries.end(), [&](const auto &entry) {
        return entry.tag == tag;
    });
    return it == header.entries.end() ? nullptr : &*it;
}

/// @brief Read a STRING/STRING_ARRAY/I18NSTRING RPM tag value into @p values.
/// @details Validates the entry type and reads NUL-terminated strings from the
///          header data store, bounds-checking against truncation.
/// @param data Complete RPM file bytes containing the header store.
/// @param header Parsed header that owns @p entry.
/// @param entry String-valued header entry to decode.
/// @param values Output vector replaced with the decoded strings.
/// @param err Stream that receives type and bounds diagnostics.
/// @return true on success; false (with @p err set) on a type/bounds error.
bool readRpmStringArray(const std::vector<uint8_t> &data,
                        const RpmHeaderView &header,
                        const RpmHeaderEntry &entry,
                        std::vector<std::string> &values,
                        std::ostream &err) {
    static constexpr uint32_t kRpmString = 6;
    static constexpr uint32_t kRpmStringArray = 8;
    static constexpr uint32_t kRpmI18NString = 9;
    if (entry.type != kRpmString && entry.type != kRpmStringArray && entry.type != kRpmI18NString) {
        err << "rpm: header tag " << entry.tag << " is not a string array\n";
        return false;
    }
    const uint32_t expected = entry.type == kRpmString ? 1u : entry.count;
    size_t pos = header.storeOffset + entry.offset;
    const size_t end = header.storeOffset + header.storeSize;
    values.clear();
    values.reserve(expected);
    for (uint32_t i = 0; i < expected; ++i) {
        if (pos >= end) {
            err << "rpm: string array tag " << entry.tag << " is truncated\n";
            return false;
        }
        size_t nul = pos;
        while (nul < end && data[nul] != 0)
            ++nul;
        if (nul >= end) {
            err << "rpm: string array tag " << entry.tag << " is missing a terminator\n";
            return false;
        }
        values.emplace_back(reinterpret_cast<const char *>(data.data() + pos), nul - pos);
        pos = nul + 1;
    }
    return true;
}

/// @brief Read an INT32 RPM tag value array into @p values (big-endian).
/// @param data Complete RPM file bytes containing the header store.
/// @param header Parsed header that owns @p entry.
/// @param entry INT32-valued header entry to decode.
/// @param values Output vector replaced with the decoded integers.
/// @param err Stream that receives type and bounds diagnostics.
/// @return true on success; false (with @p err set) on a type/bounds error.
bool readRpmInt32Array(const std::vector<uint8_t> &data,
                       const RpmHeaderView &header,
                       const RpmHeaderEntry &entry,
                       std::vector<uint32_t> &values,
                       std::ostream &err) {
    static constexpr uint32_t kRpmInt32 = 4;
    if (entry.type != kRpmInt32) {
        err << "rpm: header tag " << entry.tag << " is not an int32 array\n";
        return false;
    }
    if (entry.count > (header.storeSize - entry.offset) / 4u) {
        err << "rpm: int32 array tag " << entry.tag << " is truncated\n";
        return false;
    }
    values.clear();
    values.reserve(entry.count);
    size_t pos = header.storeOffset + entry.offset;
    for (uint32_t i = 0; i < entry.count; ++i) {
        values.push_back(readBE32(data, pos));
        pos += 4;
    }
    return true;
}

/// @brief Normalize an RPM-listed path by stripping leading slashes and a leading "./".
/// @param path Path spelling read from an RPM filename tag.
/// @return The normalized package-relative path.
std::string normalizeRpmListedPath(std::string path) {
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    if (path.rfind("./", 0) == 0)
        path.erase(0, 2);
    return path;
}

/// @brief Reconstruct the installed file paths recorded in an .rpm header.
/// @details Combines the BASENAMES, DIRINDEXES, and DIRNAMES tags into full
///          normalized paths for post-build payload verification.
/// @param data Complete RPM file bytes.
/// @param payloadPaths Optional output set replaced with the reconstructed paths;
///        when null, only structural header and payload validation is performed.
/// @param err Stream that receives RPM validation diagnostics.
/// @return true when the path tags were read and assembled successfully.
bool readRpmPayloadPaths(const std::vector<uint8_t> &data,
                         std::set<std::string> *payloadPaths,
                         std::ostream &err) {
    if (data.size() < 112 || data[0] != 0xED || data[1] != 0xAB || data[2] != 0xEE ||
        data[3] != 0xDB) {
        err << "rpm: missing lead magic\n";
        return false;
    }
    if (data[4] != 3) {
        err << "rpm: unsupported lead major version " << static_cast<int>(data[4]) << "\n";
        return false;
    }
    if (readBE16(data, 78) != 5) {
        err << "rpm: unsupported signature type\n";
        return false;
    }

    RpmHeaderView signature;
    if (!parseRpmHeaderAt(data, 96, "signature", signature, err))
        return false;
    const size_t mainOff = (signature.endOffset + 7ull) & ~7ull;
    RpmHeaderView mainHeader;
    if (!parseRpmHeaderAt(data, mainOff, "main", mainHeader, err))
        return false;
    if (mainHeader.endOffset == data.size()) {
        err << "rpm: package has no payload after main header\n";
        return false;
    }
    bool sawPayloadByte = false;
    for (size_t i = mainHeader.endOffset; i < data.size(); ++i) {
        if (data[i] != 0) {
            sawPayloadByte = true;
            break;
        }
    }
    if (!sawPayloadByte) {
        err << "rpm: package payload is empty\n";
        return false;
    }
    if (payloadPaths == nullptr)
        return true;

    static constexpr uint32_t kRpmTagOldFileNames = 1027;
    static constexpr uint32_t kRpmTagDirIndexes = 1116;
    static constexpr uint32_t kRpmTagBaseNames = 1117;
    static constexpr uint32_t kRpmTagDirNames = 1118;
    static constexpr uint32_t kRpmTagFileNames = 5000;

    payloadPaths->clear();
    std::vector<std::string> fullNames;
    if (const auto *fileNamesEntry = findRpmHeaderEntry(mainHeader, kRpmTagFileNames)) {
        if (!readRpmStringArray(data, mainHeader, *fileNamesEntry, fullNames, err))
            return false;
    } else if (const auto *oldFileNamesEntry =
                   findRpmHeaderEntry(mainHeader, kRpmTagOldFileNames)) {
        if (!readRpmStringArray(data, mainHeader, *oldFileNamesEntry, fullNames, err))
            return false;
    }
    if (!fullNames.empty()) {
        for (auto &path : fullNames) {
            path = normalizeRpmListedPath(std::move(path));
            if (!path.empty())
                payloadPaths->insert(path);
        }
        return true;
    }

    const auto *baseEntry = findRpmHeaderEntry(mainHeader, kRpmTagBaseNames);
    const auto *dirEntry = findRpmHeaderEntry(mainHeader, kRpmTagDirNames);
    const auto *indexEntry = findRpmHeaderEntry(mainHeader, kRpmTagDirIndexes);
    if (baseEntry == nullptr || dirEntry == nullptr || indexEntry == nullptr) {
        err << "rpm: main header does not contain file name tags\n";
        return false;
    }

    std::vector<std::string> basenames;
    std::vector<std::string> dirnames;
    std::vector<uint32_t> dirindexes;
    if (!readRpmStringArray(data, mainHeader, *baseEntry, basenames, err) ||
        !readRpmStringArray(data, mainHeader, *dirEntry, dirnames, err) ||
        !readRpmInt32Array(data, mainHeader, *indexEntry, dirindexes, err)) {
        return false;
    }
    if (basenames.size() != dirindexes.size()) {
        err << "rpm: basename and directory index counts differ\n";
        return false;
    }
    for (size_t i = 0; i < basenames.size(); ++i) {
        if (dirindexes[i] >= dirnames.size()) {
            err << "rpm: file directory index points past directory table\n";
            return false;
        }
        std::string path = normalizeRpmListedPath(dirnames[dirindexes[i]] + basenames[i]);
        if (!path.empty())
            payloadPaths->insert(path);
    }
    return true;
}

/// @brief Structurally verify a built package artifact for @p target.
/// @details Dispatches to the format-specific PkgVerify routine; when @p manifest
///          is provided, additionally asserts the required payload paths are
///          present (using the RPM header reader for .rpm).
/// @param artifact Package artifact to read and validate.
/// @param target Concrete package format expected for @p artifact.
/// @param err Stream that receives format-specific validation diagnostics.
/// @param manifest Optional staged-toolchain manifest used to verify payload completeness.
/// @return true when the artifact is valid (and complete, if a manifest is given).
bool verifyArtifact(const fs::path &artifact,
                    InstallPackageTarget target,
                    std::ostream &err,
                    const zanna::pkg::ToolchainInstallManifest *manifest = nullptr) {
    const auto data = zanna::pkg::readFile(artifact);
    switch (target) {
        case InstallPackageTarget::Windows:
            if (!zanna::pkg::verifyWindowsNativeInstaller(data, err))
                return false;
            if (manifest) {
                return zanna::pkg::verifyPEZipOverlayNestedPayload(
                    data,
                    {"meta/payload.zip",
                     "meta/installer-v2.txt",
                     "meta/cleanup.exe",
                     "meta/uninstall.exe"},
                    "meta/payload.zip",
                    requiredPayloadPaths(target, *manifest),
                    err);
            }
            return true;
        case InstallPackageTarget::LinuxDeb:
            if (manifest) {
                return zanna::pkg::verifyDebPayload(
                    data, requiredPayloadPaths(target, *manifest), err);
            }
            return zanna::pkg::verifyDeb(data, err);
        case InstallPackageTarget::Tarball:
            if (manifest) {
                return zanna::pkg::verifyTarGzPayload(
                    data, requiredPayloadPaths(target, *manifest), err);
            }
            return zanna::pkg::verifyTarGz(data, err);
        case InstallPackageTarget::MacOS: {
            const bool structurallyValid =
                manifest ? zanna::pkg::verifyMacOSPkgPayload(
                               data, requiredPayloadPaths(target, *manifest), err)
                         : zanna::pkg::verifyMacOSPkg(data, err);
            if (!structurallyValid)
                return false;
#if ZANNA_HOST_MACOS
            const RunResult nativeResult = run_process({"/usr/sbin/installer",
                                                        "-pkginfo",
                                                        "-pkg",
                                                        zanna::filesystem::pathToUtf8(artifact)});
            if (nativeResult.exit_code != 0) {
                err << "macOS pkg: Installer.app rejected package metadata\n"
                    << nativeResult.out << nativeResult.err;
                return false;
            }
#endif
            return true;
        }
        case InstallPackageTarget::MacOSDmg: {
            if (!zanna::pkg::verifyMacOSDmg(data, err))
                return false;
#if ZANNA_HOST_MACOS
            const RunResult nativeResult =
                run_process({"hdiutil", "verify", zanna::filesystem::pathToUtf8(artifact)});
            if (nativeResult.exit_code != 0) {
                err << "dmg: hdiutil verification failed\n" << nativeResult.out << nativeResult.err;
                return false;
            }
#endif
            return true;
        }
        case InstallPackageTarget::LinuxRpm: {
            if (manifest) {
                std::set<std::string> payloadPaths;
                if (!readRpmPayloadPaths(data, &payloadPaths, err))
                    return false;
                return requireListedPayloadPaths(
                    payloadPaths, requiredPayloadPaths(target, *manifest), "rpm", err);
            }
            return zanna::pkg::verifyRpm(data, err);
        }
        case InstallPackageTarget::LinuxBundle: {
            std::string bundleErr;
            if (!zanna::pkg::verifyLinuxAppImage(data, &bundleErr)) {
                err << bundleErr;
                return false;
            }
            if (manifest) {
                std::ostringstream payloadErr;
                const auto payload = linuxBundlePayloadBytes(data, payloadErr);
                if (payload.empty()) {
                    err << payloadErr.str();
                    return false;
                }
                if (!zanna::pkg::verifyTarGzPayload(
                        payload, requiredPayloadPaths(target, *manifest), payloadErr)) {
                    err << payloadErr.str();
                    return false;
                }
            }
            return true;
        }
        case InstallPackageTarget::All:
        case InstallPackageTarget::AllAvailable:
        default:
            return false;
    }
}

/// @brief Infer the package target from an artifact's filename extension.
/// @details Maps .exe/.pkg/.dmg/.deb/.rpm/.run/.tar.gz/.tgz to their targets for
///          `--verify-only`.
/// @param path Artifact path whose filename extension is inspected.
/// @param target Output target set when the extension is recognized.
/// @return true on a recognized extension; false when no supported suffix matches.
bool inferVerifyTargetFromPath(const fs::path &path, InstallPackageTarget &target) {
    const std::string name = lowerAscii(zanna::filesystem::pathToUtf8(path.filename()));
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".exe")
        target = InstallPackageTarget::Windows;
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".pkg")
        target = InstallPackageTarget::MacOS;
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".dmg")
        target = InstallPackageTarget::MacOSDmg;
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".deb")
        target = InstallPackageTarget::LinuxDeb;
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".rpm")
        target = InstallPackageTarget::LinuxRpm;
    else if (name.size() >= 4 && name.substr(name.size() - 4) == ".run")
        target = InstallPackageTarget::LinuxBundle;
    else if ((name.size() >= 7 && name.substr(name.size() - 7) == ".tar.gz") ||
             (name.size() >= 4 && name.substr(name.size() - 4) == ".tgz"))
        target = InstallPackageTarget::Tarball;
    else
        return false;
    return true;
}

/// @brief RAII guard that removes an auto-created staging directory on scope exit.
/// @details Call dismiss() to keep the directory once staging succeeds; otherwise
///          the destructor recursively deletes it (used for auto-generated stages).
class AutoStageCleanup {
  public:
    /// @brief Construct a guard for a potentially temporary staging directory.
    /// @param path Directory to remove if the guard remains enabled.
    /// @param enabled Whether scope-exit cleanup is active initially.
    AutoStageCleanup(fs::path path, bool enabled) : path_(std::move(path)), enabled_(enabled) {}

    /// @brief Remove the staging directory unless the guard was dismissed.
    ~AutoStageCleanup() {
        if (enabled_ && !path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }

    /// @brief Cancel the pending cleanup so the directory is preserved.
    void dismiss() {
        enabled_ = false;
    }

  private:
    fs::path path_;       ///< Staging directory to remove.
    bool enabled_{false}; ///< Whether the destructor should delete @c path_.
};

/// @brief Remove a partially generated release artifact set unless explicitly committed.
class ReleaseArtifactCleanup {
  public:
    /// @brief Construct a release-output cleanup guard.
    /// @param enabled Whether tracked outputs should be removed on scope exit.
    explicit ReleaseArtifactCleanup(bool enabled) : enabled_(enabled) {}

    /// @brief Remove tracked artifacts, sidecars, and auxiliary files when enabled.
    ~ReleaseArtifactCleanup() {
        if (!enabled_)
            return;
        std::error_code ec;
        for (const fs::path &path : paths_) {
            fs::remove(path, ec);
            ec.clear();
            fs::path sidecar = path;
            sidecar += ".sha256";
            fs::remove(sidecar, ec);
            ec.clear();
            sidecar = path;
            sidecar += ".sha256.txt";
            fs::remove(sidecar, ec);
            ec.clear();
            sidecar = path;
            sidecar += ".manifest.json";
            fs::remove(sidecar, ec);
            ec.clear();
        }
        for (const fs::path &path : auxiliaryPaths_) {
            fs::remove(path, ec);
            ec.clear();
        }
    }

    /// @brief Register an artifact and its conventional sidecars for possible cleanup.
    /// @param path Primary artifact path to track.
    void trackArtifact(const fs::path &path) {
        if (enabled_)
            paths_.push_back(path);
    }

    /// @brief Register a standalone output such as an aggregate manifest for cleanup.
    /// @param path Auxiliary output path to track.
    void trackAuxiliary(const fs::path &path) {
        if (enabled_)
            auxiliaryPaths_.push_back(path);
    }

    /// @brief Commit the tracked output set by disabling scope-exit cleanup.
    void dismiss() {
        enabled_ = false;
    }

  private:
    bool enabled_{false};
    std::vector<fs::path> paths_;
    std::vector<fs::path> auxiliaryPaths_;
};

/// @brief Serialize release writers targeting the same output directory.
class ReleaseOutputLock {
  public:
    /// @brief Acquire a directory-scoped release-output lock when requested.
    /// @param directory Output directory in which the lock directory is created.
    /// @param enabled Whether locking is required for this operation.
    /// @throws std::runtime_error if the lock directory cannot be acquired.
    ReleaseOutputLock(const fs::path &directory, bool enabled) {
        if (!enabled)
            return;
        path_ = directory / ".zanna-release.lock";
        std::error_code ec;
        if (!fs::create_directory(path_, ec)) {
            throw std::runtime_error("cannot acquire release output lock '" +
                                     zanna::filesystem::pathToUtf8(path_) +
                                     "' (another release may be running; remove a stale lock only "
                                     "after confirming no writer is active)" +
                                     (ec ? ": " + ec.message() : std::string{}));
        }
    }

    /// @brief Release an acquired output lock.
    ~ReleaseOutputLock() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }

  private:
    fs::path path_;
};

/// @brief Return an existing staged install tree, or build/stage one on demand.
/// @details If --stage-dir was given, returns it directly. Otherwise creates a
///          unique directory under --build-dir, optionally runs `cmake --build`,
///          then `cmake --install --prefix <stage>`; the directory is removed on
///          failure (and on success unless --keep-stage-dir was set).
/// @param args Parsed command options controlling the input build and staging directory.
/// @return Path to the caller-supplied or newly populated staging directory.
/// @throws std::runtime_error on directory creation or cmake failure.
fs::path ensureStageDir(const InstallPackageArgs &args) {
    if (!args.stageDir.empty())
        return args.stageDir;

    fs::path stageDir =
        zanna::pkg::createUniqueTempDirectory(args.buildDir, "install-toolchain-stage");
    AutoStageCleanup cleanup(stageDir, !args.keepStageDir);

    if (const auto installIDE =
            readCMakeCacheBool(args.buildDir / "CMakeCache.txt", "ZANNA_INSTALL_ZANNASTUDIO");
        installIDE && !*installIDE) {
        throw std::runtime_error("toolchain installers require ZANNA_INSTALL_ZANNASTUDIO=ON; "
                                 "reconfigure the build tree before running install-package");
    }

    if (!args.skipBuild) {
        std::vector<std::string> buildCmd = {
            "cmake", "--build", zanna::filesystem::pathToUtf8(args.buildDir)};
        if (!args.buildConfig.empty()) {
            buildCmd.push_back("--config");
            buildCmd.push_back(args.buildConfig);
        }
        const RunResult buildResult = run_process(buildCmd);
        if (buildResult.exit_code != 0) {
            throw std::runtime_error("cmake --build failed while preparing toolchain payload:\n" +
                                     buildResult.out + buildResult.err);
        }
    }

    std::vector<std::string> installCmd = {"cmake",
                                           "--install",
                                           zanna::filesystem::pathToUtf8(args.buildDir),
                                           "--prefix",
                                           zanna::filesystem::pathToUtf8(stageDir)};
    if (!args.buildConfig.empty()) {
        installCmd.push_back("--config");
        installCmd.push_back(args.buildConfig);
    }

    const RunResult rr = run_process(installCmd);
    if (rr.exit_code != 0) {
        throw std::runtime_error("cmake --install failed while staging toolchain:\n" + rr.out +
                                 rr.err);
    }
    cleanup.dismiss();
    return stageDir;
}

/// @brief Return true if @p target is buildable for the staged @p platform.
/// @details Tarball matches any platform; native formats require the matching
///          platform string.
/// @param target Concrete package format to compare.
/// @param platform Normalized platform name recorded in the staged manifest.
/// @return true when the target can package that staged platform.
bool targetMatchesStagedPlatform(InstallPackageTarget target, const std::string &platform) {
    switch (target) {
        case InstallPackageTarget::Windows:
            return platform == "windows";
        case InstallPackageTarget::MacOS:
        case InstallPackageTarget::MacOSDmg:
            return platform == "macos";
        case InstallPackageTarget::LinuxDeb:
        case InstallPackageTarget::LinuxRpm:
        case InstallPackageTarget::LinuxBundle:
            return platform == "linux";
        case InstallPackageTarget::Tarball:
            return true;
        case InstallPackageTarget::All:
        case InstallPackageTarget::AllAvailable:
        default:
            return false;
    }
}

/// @brief Expand the requested target into the concrete formats to build.
/// @details A specific target maps to itself; the All meta-target expands to the
///          native format(s) for @p platform (deb+rpm on Linux) plus a tarball.
/// @param target Requested concrete target or aggregate target.
/// @param platform Normalized platform name recorded in the staged manifest.
/// @return Ordered concrete package targets to generate.
std::vector<InstallPackageTarget> selectedTargets(InstallPackageTarget target,
                                                  const std::string &platform) {
    if (target != InstallPackageTarget::All && target != InstallPackageTarget::AllAvailable)
        return {target};

    std::vector<InstallPackageTarget> result;
    if (platform == "windows") {
        result.push_back(InstallPackageTarget::Windows);
    } else if (platform == "macos") {
        result.push_back(InstallPackageTarget::MacOS);
    } else if (platform == "linux") {
        result.push_back(InstallPackageTarget::LinuxDeb);
        if (target == InstallPackageTarget::All || rpmbuildAvailable())
            result.push_back(InstallPackageTarget::LinuxRpm);
        result.push_back(InstallPackageTarget::LinuxBundle);
    }
    result.push_back(InstallPackageTarget::Tarball);
    return result;
}

} // namespace

/// @brief Build or verify distributable toolchain installation packages.
/// @details Parses install-package options, prepares or consumes a staged toolchain,
///          validates platform and release policy, builds each selected package,
///          performs requested signing and verification, and writes artifact
///          inventory/checksum metadata.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the top-level executable and subcommand.
/// @return Zero on success; one for invalid options, policy failures, build/signing
///         failures, or artifact-verification errors.
int cmdInstallPackage(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            installPackageUsage();
            return 0;
        }
    }

    InstallPackageArgs args;
    if (!parseInstallPackageArgs(argc, argv, args))
        return 1;
    if (args.linuxSignKey.empty())
        args.linuxSignKey = getenvOrEmpty("ZANNA_LINUX_SIGN_KEY");
    if (args.target == InstallPackageTarget::MacOSDmg && args.verifyOnlyPath.empty()) {
        args.target = InstallPackageTarget::MacOS;
        args.macosDmg = true;
    }

    try {
        if (!args.verifyOnlyPath.empty()) {
            std::ostringstream err;
            InstallPackageTarget target = args.target;
            if ((target == InstallPackageTarget::All ||
                 target == InstallPackageTarget::AllAvailable) &&
                !inferVerifyTargetFromPath(args.verifyOnlyPath, target)) {
                std::cerr << "error: cannot infer install-package artifact type from extension: "
                          << zanna::filesystem::pathToUtf8(args.verifyOnlyPath)
                          << " (use --target for supported artifact formats)\n";
                return 1;
            }
            if (!verifyArtifact(args.verifyOnlyPath, target, err)) {
                std::cerr << err.str();
                return 1;
            }
            if (!verifyArtifactChecksum(
                    args.verifyOnlyPath, args.requireChecksum || args.releaseMode, err)) {
                std::cerr << err.str();
                return 1;
            }
            return 0;
        }

        fs::path stageDir = ensureStageDir(args);
        AutoStageCleanup stageCleanup(stageDir, args.stageDir.empty() && !args.keepStageDir);
        std::optional<fs::path> installManifestPath;
        if (!args.buildDir.empty())
            installManifestPath = args.buildDir / "install_manifest.txt";
        zanna::pkg::ToolchainInstallManifest manifest =
            zanna::pkg::gatherToolchainInstallManifest(stageDir, installManifestPath);
        if (!args.sourceCommitOverride.empty())
            manifest.sourceCommit = args.sourceCommitOverride;
        if (!args.toolchainLicense.empty())
            manifest.license = args.toolchainLicense;
        if (!args.toolchainMaintainer.empty())
            manifest.maintainer = args.toolchainMaintainer;
        if (!args.toolchainMaintainerEmail.empty())
            manifest.maintainerEmail = args.toolchainMaintainerEmail;
        if (!args.toolchainHomepage.empty())
            manifest.homepage = args.toolchainHomepage;
        const auto detectedInfo = detectManifestToolchainExecutableInfo(manifest);
        if (detectedInfo) {
            manifest.platform = detectedInfo->platform;
        }
        if (!args.archOverride.empty()) {
            if (detectedInfo && detectedInfo->arch != "universal" &&
                detectedInfo->arch != args.archOverride) {
                std::cerr << "error: --arch " << args.archOverride
                          << " does not match staged zanna binary architecture "
                          << detectedInfo->arch << "\n";
                return 1;
            }
            manifest.arch = args.archOverride;
        } else if (detectedInfo && detectedInfo->arch == "universal") {
            manifest.arch = "universal";
        } else if (detectedInfo) {
            manifest.arch = detectedInfo->arch;
        }
        zanna::pkg::validateToolchainInstallManifest(manifest);
        if (args.releaseMode) {
            if (manifest.sourceState != "clean") {
                std::cerr << "error: --release requires a staged build from a clean source tree; "
                             "reported state is "
                          << manifest.sourceState << "\n";
                return 1;
            }
            if (manifest.sourceCommit.empty()) {
                std::cerr
                    << "error: --release requires immutable source commit metadata; configure "
                       "with ZANNA_SOURCE_COMMIT or pass --source-commit\n";
                return 1;
            }
        }

        if (args.verbose) {
            std::cout << "Stage: " << zanna::filesystem::pathToUtf8(stageDir) << "\n";
            std::cout << "Version: " << manifest.version << "\n";
            std::cout << "Source: "
                      << (manifest.sourceCommit.empty() ? "unknown" : manifest.sourceCommit) << " ("
                      << manifest.sourceState << ")\n";
            std::cout << "Platform: " << manifest.platform << "\n";
            std::cout << "Arch: " << manifest.arch << "\n";
            std::cout << "Files: " << manifest.files.size() << "\n";
            if (args.target == InstallPackageTarget::Windows ||
                ((args.target == InstallPackageTarget::All ||
                  args.target == InstallPackageTarget::AllAvailable) &&
                 manifest.platform == "windows")) {
                const WindowsToolchainIdentity identity = windowsToolchainIdentity(args);
                std::cout << "Windows channel: " << identity.channel << "\n";
                std::cout << "Windows identifier: " << identity.identifier << "\n";
                std::cout << "Windows install directory: " << identity.installDir << "\n";
                std::cout << "Windows display name: " << identity.displayName << "\n";
            }
        }

        if (args.stageOnly) {
            stageCleanup.dismiss();
            return 0;
        }

        fs::path outBase = args.outputPath;
        if (outBase.empty())
            outBase = stageDir.parent_path() / "installers";
        const auto targets = selectedTargets(args.target, manifest.platform);
        std::error_code outEc;
        const bool outPathExistsAsDirectory = !outBase.empty() && fs::is_directory(outBase, outEc);
        if (args.outputAsFile && (targets.size() > 1u || args.macosDmg)) {
            std::cerr << "error: --output-file cannot be used when the selected target emits "
                         "multiple artifacts\n";
            return 1;
        }
        if (args.outputAsFile && outPathExistsAsDirectory) {
            std::cerr << "error: --output-file names an existing directory: "
                      << zanna::filesystem::pathToUtf8(outBase) << "\n";
            return 1;
        }
        if (args.outputAsDirectory && fs::exists(outBase, outEc) && !outPathExistsAsDirectory) {
            std::cerr << "error: --output-dir names an existing non-directory: "
                      << zanna::filesystem::pathToUtf8(outBase) << "\n";
            return 1;
        }
        const bool outIsDirectoryLike = args.outputAsDirectory || args.outputPath.empty() ||
                                        outPathExistsAsDirectory || targets.size() > 1u;
        if (outIsDirectoryLike)
            fs::create_directories(outBase);
        const fs::path releaseOutputDirectory =
            outIsDirectoryLike
                ? outBase
                : (outBase.parent_path().empty() ? fs::current_path() : outBase.parent_path());
        if (args.releaseMode)
            fs::create_directories(releaseOutputDirectory);
        ReleaseOutputLock releaseOutputLock(releaseOutputDirectory, args.releaseMode);

        if (args.releaseMode) {
            const bool buildsWindows =
                std::find(targets.begin(), targets.end(), InstallPackageTarget::Windows) !=
                targets.end();
            const bool buildsMacOS =
                std::find(targets.begin(), targets.end(), InstallPackageTarget::MacOS) !=
                targets.end();
            const bool buildsSignedLinux =
                std::find(targets.begin(), targets.end(), InstallPackageTarget::LinuxDeb) !=
                    targets.end() ||
                std::find(targets.begin(), targets.end(), InstallPackageTarget::LinuxRpm) !=
                    targets.end();
            if (buildsWindows && !windowsSigningRequested(args)) {
                std::cerr << "error: --release requires Windows Authenticode signing\n";
                return 1;
            }
            if (buildsSignedLinux && args.linuxSignKey.empty()) {
                std::cerr << "error: --release requires --linux-sign-key or "
                             "ZANNA_LINUX_SIGN_KEY for Debian/RPM artifacts\n";
                return 1;
            }
            if (buildsMacOS) {
                const std::string installerIdentity =
                    args.macosSignIdentity.empty() ? getenvOrEmpty("ZANNA_MACOS_SIGN_IDENTITY")
                                                   : args.macosSignIdentity;
                const std::string appIdentity = args.macosApplicationSignIdentity.empty()
                                                    ? getenvOrEmpty("ZANNA_MACOS_APP_SIGN_IDENTITY")
                                                    : args.macosApplicationSignIdentity;
                const std::string notaryProfile = args.macosNotaryProfile.empty()
                                                      ? getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE")
                                                      : args.macosNotaryProfile;
                if (installerIdentity.empty() || appIdentity.empty() || notaryProfile.empty() ||
                    !args.macosStaple) {
                    std::cerr << "error: --release macOS packages require Installer and "
                                 "Application identities, a notary profile, and --macos-staple\n";
                    return 1;
                }
            }
        }

        const bool buildsWindows =
            std::find(targets.begin(), targets.end(), InstallPackageTarget::Windows) !=
            targets.end();
        if (buildsWindows && manifest.platform == "windows" && !args.allowDebugToolchain) {
            if (const auto debugRuntime = firstWindowsDebugRuntimeReference(manifest)) {
                std::cerr
                    << "error: refusing to build a Windows installer from a Debug CRT payload: "
                    << *debugRuntime << "\n"
                    << "       rebuild with --config Release or RelWithDebInfo, or pass "
                       "--allow-debug-toolchain for local-only diagnostics\n";
                return 1;
            }
        }

        std::vector<ArtifactRecord> artifactRecords;
        ReleaseArtifactCleanup releaseCleanup(args.releaseMode);
        for (InstallPackageTarget target : targets) {
            if (!targetMatchesStagedPlatform(target, manifest.platform)) {
                std::cerr << "error: target does not match staged zanna binary platform "
                          << manifest.platform << "\n";
                return 1;
            }
            if (target == InstallPackageTarget::LinuxRpm && !rpmbuildAvailable()) {
                if (args.target == InstallPackageTarget::All) {
                    std::cerr << "error: --target all for a Linux toolchain includes linux-rpm "
                                 "and requires rpmbuild; install rpm-build or choose "
                                 "--target linux-deb/tarball explicitly\n";
                } else {
                    std::cerr << "error: --target linux-rpm requires rpmbuild; install rpm-build "
                                 "or use --target linux-deb/tarball\n";
                }
                return 1;
            }
            fs::path artifactPath =
                outIsDirectoryLike ? (outBase / targetFileName(target, manifest)) : outBase;
            fs::path explicitMacOSDmgPath;
            if (target == InstallPackageTarget::MacOS && args.macosDmg && !outIsDirectoryLike &&
                lowerAscii(zanna::filesystem::pathToUtf8(artifactPath.extension())) == ".dmg") {
                explicitMacOSDmgPath = artifactPath;
                artifactPath.replace_extension(".pkg");
            }
            if (!outIsDirectoryLike && !artifactPath.parent_path().empty())
                fs::create_directories(artifactPath.parent_path());
            fs::path artifactSha256 = artifactPath;
            artifactSha256 += ".sha256";
            fs::path artifactSigningMetadata = artifactPath;
            artifactSigningMetadata += ".sha256.txt";
            fs::path artifactManifest = artifactPath;
            artifactManifest += ".manifest.json";
            if (args.releaseMode &&
                (fs::exists(artifactPath) || fs::exists(artifactSha256) ||
                 fs::exists(artifactSigningMetadata) || fs::exists(artifactManifest))) {
                std::cerr << "error: --release refuses to overwrite an existing artifact set: "
                          << zanna::filesystem::pathToUtf8(artifactPath) << "\n";
                return 1;
            }
            releaseCleanup.trackArtifact(artifactPath);

            switch (target) {
                case InstallPackageTarget::Windows: {
                    zanna::pkg::WindowsToolchainBuildParams params;
                    const WindowsToolchainIdentity identity = windowsToolchainIdentity(args);
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    params.archStr = manifest.arch;
                    params.installScope = args.windowsInstallScope;
                    params.installDirName = identity.installDir;
                    params.identifier = identity.identifier;
                    params.displayName = identity.displayName;
                    params.publisher = manifest.maintainer;
                    if (!manifest.homepage.empty())
                        params.homepage = manifest.homepage;
                    params.documentationUrl = args.windowsDocumentationUrl;
                    params.updateManifestUrl = args.windowsUpdateManifestUrl;
                    params.updateRsaModulus = args.windowsUpdateRsaModulus;
                    params.updateRsaExponent = args.windowsUpdateRsaExponent;
                    params.releaseChannel = identity.channel;
                    params.sourceCommit = manifest.sourceCommit;
                    params.addToPath = args.windowsAddToPath;
                    params.registerFileAssociations = args.windowsFileAssociations;
                    params.createStartMenuShortcuts = args.windowsShortcuts;
                    for (const auto &file : manifest.files) {
                        std::string relative = file.stagedRelativePath;
                        std::replace(relative.begin(), relative.end(), '\\', '/');
                        std::transform(
                            relative.begin(),
                            relative.end(),
                            relative.begin(),
                            /// @brief Fold one staged path byte to lowercase.
                            /// @param ch Byte to normalize.
                            /// @return Lowercase representation converted back to `char`.
                            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (relative == "bin/zanna-installer-host.exe") {
                            params.installerHostPath =
                                zanna::filesystem::pathToUtf8(file.stagedAbsolutePath);
                        } else if (relative == "bin/zanna-installer-cleanup.exe") {
                            params.installerCleanupPath =
                                zanna::filesystem::pathToUtf8(file.stagedAbsolutePath);
                        }
                    }
                    if (params.installerHostPath.empty()) {
                        throw std::runtime_error(
                            "Windows toolchain stage is missing bin/zanna-installer-host.exe; "
                            "reinstall the staged build with the native installer host enabled");
                    }
                    if (params.installerCleanupPath.empty()) {
                        throw std::runtime_error(
                            "Windows toolchain stage is missing "
                            "bin/zanna-installer-cleanup.exe; reinstall the staged build");
                    }
                    if (windowsSigningRequested(args)) {
                        /// @brief Sign one generated Windows toolchain PE payload.
                        /// @param logicalName Logical payload name used in signing diagnostics.
                        /// @param unsignedPe Unsigned PE bytes.
                        /// @return Signed PE bytes.
                        /// @throws std::runtime_error If the configured signing command fails.
                        params.peSigner = [&](std::string_view logicalName,
                                              const std::vector<uint8_t> &unsignedPe) {
                            return signWindowsPeBytes(args, logicalName, unsignedPe);
                        };
                    }
                    zanna::pkg::buildWindowsToolchainInstaller(params);
                    break;
                }
                case InstallPackageTarget::MacOS: {
                    zanna::pkg::MacOSToolchainBuildParams params;
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    params.packageVersion = args.macosPackageVersion;
                    params.minimumMacOSVersion = args.macosMinimumVersion.empty()
                                                     ? getenvOrEmpty("ZANNA_MACOS_MIN_VERSION")
                                                     : args.macosMinimumVersion;
                    params.licenseFilePath = args.macosPkgLicense;
                    params.backgroundImagePath = args.macosPkgBackground;
                    params.applicationSignIdentity =
                        args.macosApplicationSignIdentity.empty()
                            ? getenvOrEmpty("ZANNA_MACOS_APP_SIGN_IDENTITY")
                            : args.macosApplicationSignIdentity;
                    if (macOSPackageSigningRequested(args) &&
                        params.applicationSignIdentity.empty()) {
                        throw std::runtime_error(
                            "macOS package signing/notarization requires "
                            "--macos-app-sign-identity or ZANNA_MACOS_APP_SIGN_IDENTITY so nested "
                            "executables can be hardened-signed");
                    }
                    zanna::pkg::buildMacOSToolchainPackage(params);
                    break;
                }
                case InstallPackageTarget::LinuxDeb: {
                    zanna::pkg::LinuxToolchainBuildParams params;
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    zanna::pkg::buildToolchainDebPackage(params);
                    break;
                }
                case InstallPackageTarget::LinuxRpm: {
                    zanna::pkg::LinuxToolchainBuildParams params;
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    zanna::pkg::buildToolchainRpmPackage(params);
                    break;
                }
                case InstallPackageTarget::LinuxBundle: {
                    zanna::pkg::LinuxToolchainBuildParams params;
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    zanna::pkg::buildToolchainBundle(params);
                    break;
                }
                case InstallPackageTarget::Tarball: {
                    zanna::pkg::LinuxToolchainBuildParams params;
                    params.manifest = manifest;
                    params.outputPath = zanna::filesystem::pathToUtf8(artifactPath);
                    zanna::pkg::buildToolchainTarball(params);
                    break;
                }
                case InstallPackageTarget::MacOSDmg:
                    std::cerr << "error: --target macos-dmg is only supported with --verify-only; "
                                 "use --target macos --macos-dmg to build a DMG\n";
                    return 1;
                case InstallPackageTarget::All:
                case InstallPackageTarget::AllAvailable:
                    break;
            }

            if (target == InstallPackageTarget::Windows &&
                !signWindowsInstallerArtifact(args, artifactPath, std::cerr)) {
                std::error_code removeEc;
                fs::remove(artifactPath, removeEc);
                return 1;
            }
            if (target == InstallPackageTarget::MacOS &&
                !signMacOSPackageArtifact(args, artifactPath, std::cerr)) {
                std::error_code removeEc;
                fs::remove(artifactPath, removeEc);
                return 1;
            }
            if ((target == InstallPackageTarget::LinuxDeb ||
                 target == InstallPackageTarget::LinuxRpm) &&
                !args.linuxSignKey.empty()) {
                try {
                    zanna::pkg::signLinuxPackage(zanna::filesystem::pathToUtf8(artifactPath),
                                                 args.linuxSignKey,
                                                 target == InstallPackageTarget::LinuxRpm);
                } catch (const std::exception &ex) {
                    std::cerr << "error: " << ex.what() << "\n";
                    std::error_code removeEc;
                    fs::remove(artifactPath, removeEc);
                    return 1;
                }
            }

            if (!args.noVerify) {
                std::ostringstream err;
                if (!verifyArtifact(artifactPath, target, err, &manifest)) {
                    std::cerr << "error: verification failed for "
                              << zanna::filesystem::pathToUtf8(artifactPath) << "\n"
                              << err.str();
                    std::error_code removeEc;
                    fs::remove(artifactPath, removeEc);
                    return 1;
                }
            }

            std::string artifactTrust = "checksum-only";
            if (target == InstallPackageTarget::Windows)
                artifactTrust = windowsSigningRequested(args) ? "authenticode" : "unsigned";
            else if (target == InstallPackageTarget::MacOS) {
                const std::string notaryProfile = args.macosNotaryProfile.empty()
                                                      ? getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE")
                                                      : args.macosNotaryProfile;
                artifactTrust =
                    !notaryProfile.empty()
                        ? "developer-id+notarized"
                        : (macOSPackageSigningRequested(args) ? "developer-id" : "unsigned");
            } else if (target == InstallPackageTarget::LinuxDeb ||
                       target == InstallPackageTarget::LinuxRpm) {
                artifactTrust = args.linuxSignKey.empty() ? "unsigned" : "openpgp";
            }
            artifactRecords.push_back(inventoryArtifact(
                artifactPath, target, manifest, !args.noVerify, std::move(artifactTrust)));

            std::cout << zanna::filesystem::pathToUtf8(artifactPath) << "\n";

            if (target == InstallPackageTarget::MacOS && args.macosDmg) {
                zanna::pkg::MacOSToolchainDmgParams dmgParams;
                dmgParams.pkgPath = zanna::filesystem::pathToUtf8(artifactPath);
                fs::path defaultDmgPath = artifactPath;
                defaultDmgPath.replace_extension(".dmg");
                dmgParams.outputPath = zanna::filesystem::pathToUtf8(
                    explicitMacOSDmgPath.empty() ? defaultDmgPath : explicitMacOSDmgPath);
                dmgParams.backgroundPng = args.macosDmgBackground;
                dmgParams.volumeIcns = args.macosDmgIcon;
                const fs::path dmgOutput = zanna::filesystem::pathFromUtf8(dmgParams.outputPath);
                fs::path dmgSha256 = dmgOutput;
                dmgSha256 += ".sha256";
                fs::path dmgSigningMetadata = dmgOutput;
                dmgSigningMetadata += ".sha256.txt";
                fs::path dmgManifest = dmgOutput;
                dmgManifest += ".manifest.json";
                if (args.releaseMode &&
                    (fs::exists(dmgOutput) || fs::exists(dmgSha256) ||
                     fs::exists(dmgSigningMetadata) || fs::exists(dmgManifest))) {
                    std::cerr << "error: --release refuses to overwrite an existing artifact set: "
                              << dmgParams.outputPath << "\n";
                    return 1;
                }
                releaseCleanup.trackArtifact(dmgOutput);
                zanna::pkg::buildMacOSToolchainDmg(dmgParams);
                std::error_code dmgEc;
                if (!fs::exists(dmgOutput, dmgEc)) {
                    std::cerr << "error: macOS .dmg builder did not create output file: "
                              << dmgParams.outputPath << "\n";
                    return 1;
                }
                const auto dmgSize = fs::file_size(dmgOutput, dmgEc);
                if (dmgEc || dmgSize == 0) {
                    std::cerr << "error: macOS .dmg output is not readable or is empty: "
                              << dmgParams.outputPath;
                    if (dmgEc)
                        std::cerr << ": " << dmgEc.message();
                    std::cerr << "\n";
                    return 1;
                }
                if (!notarizeMacOSDmgArtifact(args, dmgOutput, std::cerr)) {
                    std::error_code removeEc;
                    fs::remove(dmgOutput, removeEc);
                    return 1;
                }
                if (!args.noVerify) {
                    std::ostringstream dmgErr;
                    if (!verifyArtifact(
                            dmgOutput, InstallPackageTarget::MacOSDmg, dmgErr, nullptr)) {
                        std::cerr << "error: verification failed for " << dmgParams.outputPath
                                  << "\n"
                                  << dmgErr.str();
                        std::error_code removeEc;
                        fs::remove(dmgOutput, removeEc);
                        return 1;
                    }
                }
                const std::string notaryProfile = args.macosNotaryProfile.empty()
                                                      ? getenvOrEmpty("ZANNA_MACOS_NOTARY_PROFILE")
                                                      : args.macosNotaryProfile;
                artifactRecords.push_back(
                    inventoryArtifact(dmgOutput,
                                      InstallPackageTarget::MacOSDmg,
                                      manifest,
                                      !args.noVerify,
                                      notaryProfile.empty() ? "checksum-only" : "notarized"));
                std::cout << dmgParams.outputPath << "\n";
            }
        }

        if (!artifactRecords.empty()) {
            const fs::path inventoryDirectory =
                outIsDirectoryLike ? outBase
                                   : (artifactRecords.front().path.parent_path().empty()
                                          ? fs::current_path()
                                          : artifactRecords.front().path.parent_path());
            fs::path singleArtifactInventory = artifactRecords.front().path;
            singleArtifactInventory += ".manifest.json";
            const fs::path expectedInventory =
                !args.artifactManifestPath.empty()
                    ? args.artifactManifestPath
                    : (outIsDirectoryLike || artifactRecords.size() > 1u
                           ? inventoryDirectory / "zanna-artifacts.json"
                           : singleArtifactInventory);
            if (args.releaseMode && fs::exists(expectedInventory)) {
                std::cerr << "error: --release refuses to overwrite artifact manifest: "
                          << zanna::filesystem::pathToUtf8(expectedInventory) << "\n";
                return 1;
            }
            if (args.releaseMode && (outIsDirectoryLike || artifactRecords.size() > 1u) &&
                fs::exists(inventoryDirectory / "SHA256SUMS")) {
                std::cerr << "error: --release refuses to overwrite existing SHA256SUMS in "
                          << zanna::filesystem::pathToUtf8(inventoryDirectory) << "\n";
                return 1;
            }
            releaseCleanup.trackAuxiliary(expectedInventory);
            if (outIsDirectoryLike || artifactRecords.size() > 1u)
                releaseCleanup.trackAuxiliary(inventoryDirectory / "SHA256SUMS");
            const fs::path inventoryPath = writeArtifactInventory(
                artifactRecords, outIsDirectoryLike, outBase, args.artifactManifestPath);
            std::cerr << "Artifact manifest: " << zanna::filesystem::pathToUtf8(inventoryPath)
                      << "\n";
        }

        if (!args.keepStageDir && args.stageDir.empty())
            fs::remove_all(stageDir);
        stageCleanup.dismiss();
        releaseCleanup.dismiss();

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
