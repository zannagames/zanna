//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerWizard.hpp
/// @brief Declares accessible native setup configuration, progress, and completion surfaces.
///
/// Quiet mode creates no window, passive mode presents progress without questions, and full mode
/// returns every user choice through HostOptions before mutation. Native controls honor system
/// colors, fonts, keyboard navigation, and per-monitor DPI. Dialog functions retain no references
/// after returning.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "WindowsInstallerHost.hpp"

#include <functional>
#include <set>

namespace zanna::installer {

/// @brief Collect full-UI install or maintenance choices.
/// @param instance Current module instance used for dialogs and resources.
/// @param package Verified package supplying display, component, and integration metadata.
/// @param initialDestination Initial installation root shown to the user.
/// @param initialScope Initial user or machine scope.
/// @param initialComponents Initially selected normalized component IDs.
/// @param installationPresent Whether maintenance must preserve the registered scope and root.
/// @param options Mutable host options populated with accepted user choices.
/// @return @c false when the user cancels before mutation; otherwise @c true.
bool configureInstallerWizard(HINSTANCE instance,
                              const HostPackage &package,
                              const std::filesystem::path &initialDestination,
                              InstallScope initialScope,
                              const std::set<std::string> &initialComponents,
                              bool installationPresent,
                              HostOptions &options);

/// @brief Execute a lifecycle operation behind a cooperatively cancellable native progress dialog.
/// @param instance Current module instance used to create the progress dialog.
/// @param package Verified package supplying display metadata.
/// @param operation Lifecycle operation being presented.
/// @param uiLevel Quiet, passive, or full presentation policy.
/// @param logger Installer logger and cooperative cancellation source.
/// @param work Lifecycle callback executed exactly once.
/// @return Callback exit code after completion or translated cancellation.
int runInstallerProgress(HINSTANCE instance,
                         const HostPackage &package,
                         Operation operation,
                         UiLevel uiLevel,
                         Logger &logger,
                         const std::function<int()> &work);

/// @brief Show the successful completion surface and collect one launch action.
/// @param instance Current module instance used for dialog resources.
/// @param package Verified package supplying product and component metadata.
/// @param installRoot Completed installation root used to validate available actions.
/// @param selectedComponents Normalized component selection used to gate launch choices.
/// @param options Mutable host options updated with the selected post-install action.
void showInstallerFinish(HINSTANCE instance,
                         const HostPackage &package,
                         const std::filesystem::path &installRoot,
                         const std::set<std::string> &selectedComponents,
                         HostOptions &options);

} // namespace zanna::installer
