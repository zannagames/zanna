//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerHost.hpp
// Purpose: Declare native Windows installer package, command-line, logging, and lifecycle APIs.
// Key invariants:
//   - Package loading verifies the complete executable overlay before lifecycle mutation.
//   - Public lifecycle results use Windows Installer-compatible process codes.
// Ownership/Lifetime:
//   - HostPackage owns every extracted package buffer for its complete lifetime.
//   - Logger exclusively owns its file handle and is movable but not copyable.
// Links: WindowsInstallerHost.cpp, WindowsInstallerLifecycle.cpp, WindowsInstallerWizard.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "WindowsInstallerMetadata.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zanna::installer {

inline constexpr int kExitSuccess = 0;                  ///< Lifecycle completed successfully.
inline constexpr int kExitInvalidCommandLine = 87;      ///< Invalid argument contract.
inline constexpr int kExitUserCancelled = 1602;         ///< User cancelled the operation.
inline constexpr int kExitFatalError = 1603;            ///< Unrecoverable lifecycle failure.
inline constexpr int kExitAnotherInstallRunning = 1618; ///< Lifecycle mutex conflict.
inline constexpr int kExitNewerVersionInstalled = 1638; ///< Downgrade was rejected.
inline constexpr int kExitRebootRequired = 3010;        ///< Success requiring restart.

/// @brief Lifecycle failure carrying a Windows Installer-compatible process code.
class InstallerError final : public std::runtime_error {
  public:
    /// @brief Construct a lifecycle error with a process exit code.
    /// @param exitCode Windows Installer-compatible code.
    /// @param message UTF-8 diagnostic text owned by std::runtime_error.
    InstallerError(int exitCode, std::string message)
        : std::runtime_error(std::move(message)), exitCode_(exitCode) {}

    /// @brief Return the process code associated with the failure.
    /// @return Windows Installer-compatible exit code.
    int exitCode() const noexcept {
        return exitCode_;
    }

  private:
    int exitCode_;
};

/// @brief Requested installer lifecycle or nonmutating utility operation.
enum class Operation {
    Auto,         ///< Infer install/modify choice from installed state.
    Install,      ///< Install the verified package.
    Modify,       ///< Change the selected installed components.
    Repair,       ///< Restore the current installation.
    Uninstall,    ///< Remove the owned installation.
    Inspect,      ///< Emit verified package metadata.
    CheckUpdates, ///< Query signed update discovery.
    Help,         ///< Display command-line help.
    SelfTest,     ///< Validate launchability without mutation.
};

/// @brief Amount of interactive installer UI.
enum class UiLevel { Full, Passive, Quiet };

/// @brief Registry/filesystem ownership scope.
enum class InstallScope { User, Machine };

/// @brief Named component-selection profile.
enum class ComponentPreset { Unspecified, Minimal, Typical, SDK, Complete };

/// @brief Normalized, cross-validated command-line request for one host session.
struct HostOptions {
    Operation operation{Operation::Auto};     ///< Lifecycle or utility operation.
    UiLevel uiLevel{UiLevel::Full};           ///< Requested interaction level.
    std::optional<InstallScope> scope;        ///< Explicit scope, or metadata default.
    std::filesystem::path destination;        ///< Optional custom install root.
    std::filesystem::path logPath;            ///< Optional explicit session-log path.
    std::filesystem::path outputPath;         ///< Optional JSON output path.
    std::set<std::string> selectedComponents; ///< Explicit component IDs.
    ComponentPreset componentPreset{ComponentPreset::Unspecified}; ///< Named preset.
    bool componentsSpecified{false};          ///< Whether explicit IDs were supplied.
    std::optional<bool> addToPath;            ///< Explicit PATH integration choice.
    std::optional<bool> registerAssociations; ///< Explicit association choice.
    std::optional<bool> createShortcuts;      ///< Explicit shortcut choice.
    bool allowDowngrade{false};               ///< Permit older-version installation.
    bool noRestart{false};                    ///< Suppress application/system restart.
    bool closeApplications{false};            ///< Permit Restart Manager closure.
    bool elevatedWorker{false};               ///< Internal elevated-worker mode.
    bool uninstallWorker{false};              ///< Internal maintenance-handoff mode.
    DWORD handoffParentId{0};                 ///< Parent PID paired with uninstall worker.
    bool launchIDE{false};                    ///< Launch the packaged IDE afterward.
    bool launchPrompt{false};                 ///< Launch a configured prompt afterward.
    bool openQuickstart{false};               ///< Open quick-start documentation afterward.
    bool openSamples{false};                  ///< Open installed samples afterward.
};

/// @brief Fully owned and verified contents of a native installer executable.
struct HostPackage {
    std::filesystem::path executablePath;          ///< Original executable path.
    std::vector<uint8_t> executableBytes;          ///< Complete stable executable image.
    std::string executableSha256;                  ///< Lowercase package digest.
    size_t overlayOffset{0};                       ///< Verified ZIP offset in executableBytes.
    size_t overlayLength{0};                       ///< Verified ZIP byte length.
    zanna::pkg::WindowsInstallerMetadata metadata; ///< Parsed package contract.
    std::vector<uint8_t> payloadZip;               ///< Verified install payload archive.
    std::vector<uint8_t> cleanupBytes;             ///< Verified detached cleanup executable.
    std::string licenseText;                       ///< Optional UTF-8 license text.
    std::string readmeText;                        ///< Optional UTF-8 readme text.
    /// @brief Verified auxiliary outer entries keyed by overlay path.
    std::map<std::string, std::vector<uint8_t>, std::less<>> outerFileBytes;
};

/// @brief Movable owner of a durable UTF-8 installer log and UI callbacks.
class Logger {
  public:
    /// @brief Construct a closed logger with no callbacks.
    Logger() = default;

    /// @brief Close the owned log handle.
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    /// @brief Move-construct and transfer the handle and callbacks.
    /// @param other Logger left closed.
    Logger(Logger &&other) noexcept;

    /// @brief Move-assign and replace existing resources.
    /// @param other Logger whose resources are transferred.
    /// @return Reference to this logger.
    Logger &operator=(Logger &&other) noexcept;

    /// @brief Open or create an append-only UTF-8 log.
    /// @param path Destination file.
    /// @param requireNew Require exclusive creation instead of appending an existing file.
    /// @throws std::runtime_error On filesystem, handle, BOM, or flush failure.
    void open(const std::filesystem::path &path, bool requireNew = false);

    /// @brief Append an informational record and publish progress.
    /// @param message Raw UTF-16 text.
    void info(std::wstring_view message);

    /// @brief Append a warning record and publish progress.
    /// @param message Raw UTF-16 text.
    void warning(std::wstring_view message);

    /// @brief Append an error record and publish progress.
    /// @param message Raw UTF-16 text.
    void error(std::wstring_view message);

    /// @brief Install or clear the progress presentation callback.
    /// @param callback Callable receiving message text after each record.
    void setProgressCallback(std::function<void(std::wstring_view)> callback);

    /// @brief Install or clear the cooperative cancellation predicate.
    /// @param callback Predicate invoked by cancellationRequested().
    void setCancellationCallback(std::function<bool()> callback);

    /// @brief Query cooperative cancellation.
    /// @return Callback result; false when absent and true when it throws.
    bool cancellationRequested() const;

    /// @brief Return the configured log path.
    /// @return Borrowed path reference valid while this logger lives.
    const std::filesystem::path &path() const {
        return path_;
    }

  private:
    /// @brief Append one timestamped record and invoke the progress callback.
    /// @param level Severity label.
    /// @param message Raw message.
    void write(std::wstring_view level, std::wstring_view message);
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::filesystem::path path_;
    std::function<void(std::wstring_view)> progressCallback_;
    std::function<bool()> cancellationCallback_;
};

/// @brief Strictly decode UTF-8 as UTF-16.
/// @param text UTF-8 bytes.
/// @return Decoded Windows text.
std::wstring utf8ToWide(std::string_view text);

/// @brief Strictly encode UTF-16 as UTF-8.
/// @param text Windows text.
/// @return Encoded UTF-8 bytes.
std::string wideToUtf8(std::wstring_view text);

/// @brief Format a compact system error message.
/// @param error Win32 error code.
/// @return System text or numeric fallback.
std::wstring formatWindowsError(DWORD error);

/// @brief Quote an argument using CommandLineToArgvW rules.
/// @param argument Raw argument.
/// @return Safely escaped command-line representation.
std::wstring quoteCommandLineArgument(std::wstring_view argument);

/// @brief Resolve the running executable path.
/// @return Absolute module path.
std::filesystem::path currentExecutablePath();

/// @brief Build a unique log path under the temporary directory.
/// @param identifier Package identifier embedded in the filename.
/// @return Timestamped, process- and tick-specific path.
std::filesystem::path defaultLogPath(std::string_view identifier);

/// @brief Parse and cross-validate the Unicode installer command line.
/// @param argc Argument count.
/// @param argv Unicode argument vector.
/// @return Normalized host options.
HostOptions parseCommandLine(int argc, wchar_t **argv);

/// @brief Return command-line help text.
/// @return Usage, option, and exit-code reference.
std::wstring commandLineHelp();

/// @brief Load and verify every owned installer package artifact.
/// @param executablePath Installer executable.
/// @return Fully owned verified package.
HostPackage loadHostPackage(const std::filesystem::path &executablePath);

/// @brief Render deterministic JSON for a verified package.
/// @param package Verified host package.
/// @return UTF-16 JSON document.
std::wstring inspectPackageJson(const HostPackage &package);

/// @brief Compare supported dotted semantic versions using SemVer prerelease precedence.
/// @param left First version.
/// @param right Second version.
/// @return Negative, zero, or positive according to version precedence.
int compareInstallerVersions(std::string_view left, std::string_view right);

/// @brief Execute the selected installer lifecycle operation.
/// @param instance Module instance.
/// @param package Verified package.
/// @param options Normalized request.
/// @param logger Open session logger.
/// @return Windows Installer-compatible process code.
int runLifecycle(HINSTANCE instance,
                 const HostPackage &package,
                 const HostOptions &options,
                 Logger &logger);

} // namespace zanna::installer
