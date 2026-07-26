//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements usage and version output for the `zia` CLI tool.

#include "usage.hpp"
#include "tools/common/CommonUsage.hpp"
#include "zanna/version.hpp"
#include <iostream>

namespace zia {

/// @brief Print the Zia compiler and supported IL versions.
/// @details Writes stable human-readable version information to standard output.
void printVersion() {
    std::cout << "zia v" << ZANNA_VERSION_STR << "\n";
    std::cout << "Zia Compiler\n";
    std::cout << "IL version: " << ZANNA_IL_VERSION_STR << "\n";
}

/// @brief Print invocation forms, shared options, and examples for the Zia CLI.
/// @details Writes user-facing help to standard error and delegates the common
///          compiler option list to the shared frontend usage formatter.
void printUsage() {
    std::cerr << "zia v" << ZANNA_VERSION_STR << " - Zia Compiler\n"
              << "\n"
              << "Usage: zia [options] <file.zia>\n"
              << "\n"
              << "Usage Modes:\n"
              << "  zia script.zia              Run program (default)\n"
              << "  zia script.zia --emit-il    Emit IL to stdout\n"
              << "  zia script.zia -o file.il   Emit IL to file\n"
              << "\n"
              << "Options:\n";
    zanna::tools::printSharedOptions(std::cerr, zanna::tools::FrontendHelpDetail::Advanced);
    std::cerr << "\n"
              << "Examples:\n"
              << "  zia hello.zia                           Run program\n"
              << "  zia hello.zia --emit-il                 Show generated IL\n"
              << "  zia hello.zia -o hello.il               Save IL to file\n"
              << "  zia hello.zia --trace --bounds-checks   Debug mode\n"
              << "  zia hello.zia -- arg1 arg2              Pass args to program\n"
              << "  zia hello.zia --stdin-from input.txt    Redirect input\n"
              << "\n"
              << "Documentation: docs/languages/zia-reference.md\n";
}

} // namespace zia
