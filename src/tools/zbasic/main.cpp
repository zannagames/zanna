//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Main entry point for the zbasic command-line tool.
// Provides a user-friendly interface to run and compile BASIC programs.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Entry point for the `zbasic` CLI tool.
/// @details Wires the Zanna BASIC frontend into the shared frontend runner so
///          the tool mirrors the standalone `zia` compiler interface.

#include "common/Utf8CommandLine.hpp"
#include "tools/common/frontend_tool.hpp"
#include "tools/zanna/cli.hpp"
#include "usage.hpp"

/// @brief Main entry point for the Zanna BASIC compiler CLI.
/// @param argc Number of command-line arguments.
/// @param argv Array of argument strings.
/// @return Exit status: 0 on success, non-zero on error.
/// @details Configures BASIC-specific callbacks and delegates argument parsing,
///          execution, IL emission, and native compilation to the same shared
///          frontend-tool pipeline used by `zia`.
int main(int argc, char **argv) {
    zanna::tools::Utf8CommandLine commandLine(argc, argv);
    if (!commandLine.applyOrReport(argc, argv))
        return 1;

    zanna::tools::FrontendToolCallbacks callbacks{
        .fileExtension = ".bas",
        .languageName = "BASIC",
        .printUsage = zbasic::printUsage,
        .printVersion = zbasic::printVersion,
        .frontendCommand = cmdFrontBasic,
    };

    return zanna::tools::runFrontendTool(argc, argv, callbacks);
}
