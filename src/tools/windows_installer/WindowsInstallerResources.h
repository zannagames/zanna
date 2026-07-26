//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerResources.h
/// @brief Defines native resource identifiers shared by the installer host and resource compiler.
///
/// Resource identifiers remain stable across host architectures so the compiled resource script
/// and C++ consumers address the same assets. The constants have compile-time lifetime and are
/// consumed by WindowsInstallerHost.rc.in and WindowsInstallerWizard.cpp.
//
//===----------------------------------------------------------------------===//

#pragma once

/// @brief Resource identifier for the Zanna installer application icon.
#define IDI_ZANNA_INSTALLER 101
