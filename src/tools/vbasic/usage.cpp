//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements usage and version output for the `vbasic` CLI tool.

#include "usage.hpp"
#include "tools/common/CommonUsage.hpp"
#include "zanna/version.hpp"
#include <iostream>

namespace vbasic {

/// @brief Print the vbasic, Zanna BASIC, and IL version banner.
/// @details Writes the informational banner to standard output.
void printVersion() {
    std::cout << "vbasic v" << ZANNA_VERSION_STR << "\n";
    std::cout << "Zanna BASIC Interpreter/Compiler\n";
    std::cout << "IL version: " << ZANNA_IL_VERSION_STR << "\n";
}

/// @brief Print vbasic synopsis, modes, shared options, and examples.
/// @details Writes help text to standard error so it accompanies usage failures
///          without contaminating normal program output.
void printUsage() {
    std::cerr << "vbasic v" << ZANNA_VERSION_STR << " - Zanna BASIC Interpreter\n"
              << "\n"
              << "Usage: vbasic [options] <file.bas>\n"
              << "\n"
              << "Usage Modes:\n"
              << "  vbasic script.bas              Run program (default)\n"
              << "  vbasic script.bas --emit-il    Emit IL to stdout\n"
              << "  vbasic script.bas -o file.il   Emit IL to file\n"
              << "\n"
              << "Options:\n";
    zanna::tools::printSharedOptions(std::cerr);
    std::cerr << "\n"
              << "Examples:\n"
              << "  vbasic game.bas                           Run program\n"
              << "  vbasic game.bas --emit-il                 Show generated IL\n"
              << "  vbasic game.bas -o game.il                Save IL to file\n"
              << "  vbasic game.bas --trace --bounds-checks   Debug mode\n"
              << "  vbasic game.bas -- arg1 arg2              Pass args to program\n"
              << "  vbasic game.bas --stdin-from input.txt    Redirect input\n"
              << "\n"
              << "Documentation: docs/languages/basic-reference.md\n";
}

} // namespace vbasic
