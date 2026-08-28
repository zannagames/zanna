//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the zbasic command-line help and version presenters.
///
/// Usage text is written to standard error, while version information is
/// written to standard output.
//
//===----------------------------------------------------------------------===//

#pragma once

namespace zbasic {

/// @brief Print usage information for the zbasic command-line tool.
///
/// @details Displays synopsis, common usage patterns, and available options.
///          Designed to be user-friendly for newcomers to Zanna BASIC.
void printUsage();

/// @brief Print version information for zbasic.
/// @details Includes the executable, product, and supported IL versions.
void printVersion();

} // namespace zbasic
