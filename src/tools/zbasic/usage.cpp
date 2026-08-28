//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements usage and version output for the `zbasic` CLI tool.

#include "usage.hpp"
#include "tools/common/CommonUsage.hpp"
#include "zanna/version.hpp"
#include <iostream>

namespace zbasic {

/// @brief Print the zbasic compiler and supported IL versions.
/// @details Writes the informational banner to standard output.
void printVersion() {
    std::cout << "zbasic v" << ZANNA_VERSION_STR << "\n";
    std::cout << "Zanna BASIC Compiler\n";
    std::cout << "IL version: " << ZANNA_IL_VERSION_STR << "\n";
}

/// @brief Print zbasic synopsis, modes, shared options, and examples.
/// @details Writes help text to standard error so it accompanies usage failures
///          without contaminating normal program output.
void printUsage() {
    std::cerr << "zbasic v" << ZANNA_VERSION_STR << " - Zanna BASIC Compiler\n"
              << "\n"
              << "Usage: zbasic [options] <file.bas>\n"
              << "\n"
              << "Usage Modes:\n"
              << "  zbasic script.bas              Run program (default)\n"
              << "  zbasic script.bas --emit-il    Emit IL to stdout\n"
              << "  zbasic script.bas -o OUTPUT    Write IL or a native binary\n"
              << "\n"
              << "Options:\n";
    zanna::tools::printSharedOptions(std::cerr, zanna::tools::FrontendHelpDetail::BasicAdvanced);
    std::cerr << "\n"
              << "Examples:\n"
              << "  zbasic game.bas                           Run program\n"
              << "  zbasic game.bas --emit-il                 Show generated IL\n"
              << "  zbasic game.bas -o game.il                Save IL to file\n"
              << "  zbasic game.bas -o game                   Compile a native binary\n"
              << "  zbasic game.bas --trace --bounds-checks   Debug mode\n"
              << "  zbasic game.bas -- arg1 arg2              Pass args to program\n"
              << "  zbasic game.bas --stdin-from input.txt    Redirect input\n"
              << "\n"
              << "Documentation: docs/languages/basic-reference.md\n";
}

} // namespace zbasic
