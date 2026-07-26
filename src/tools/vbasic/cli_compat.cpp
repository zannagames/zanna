//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Defines legacy global CLI shims required by the shared BASIC frontend
///        command implementation.
//
//===----------------------------------------------------------------------===//

#include "usage.hpp"

/// @brief Print vbasic-specific help through the legacy global usage symbol.
/// @details The cmdFrontBasic implementation calls the global usage() function
///          when argument parsing fails. For vbasic, we redirect to our own
///          vbasic-specific help text.
void usage() {
    vbasic::printUsage();
}
