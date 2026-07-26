//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the interactive `zanna repl` command.
/// @details Parses the optional source-language selection, constructs the
///          corresponding Zia or BASIC adapter, and runs one REPL session.
// Key invariants:
//   - Default language is Zia.
//   - Supports both Zia and BASIC adapters.
//   - Exits with the session's exit code.
// Ownership/Lifetime:
//   - ReplSession owns the adapter and runs for the lifetime of the command.
// Links: src/repl/ReplSession.hpp, src/repl/ZiaReplAdapter.hpp,
//        src/repl/BasicReplAdapter.hpp
//
//===----------------------------------------------------------------------===//

#include "repl/BasicReplAdapter.hpp"
#include "repl/ReplSession.hpp"
#include "repl/ZiaReplAdapter.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

/// @brief Launch an interactive Zia or BASIC read-evaluate-print loop.
/// @details Zia is selected by default. The command accepts at most one language
///          name, handles help locally, and transfers adapter ownership to the
///          session for the duration of the interactive run.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and `repl` subcommand.
/// @return Zero after help, one for invalid arguments, or the session's exit code.
int cmdRepl(int argc, char **argv) {
    std::string lang = "zia"; // Default language
    bool languageSpecified = false;

    // Parse optional language argument
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "zia") == 0) {
            if (languageSpecified) {
                std::cerr << "error: specify at most one REPL language\n";
                std::cerr << "Usage: zanna repl [zia|basic]\n";
                return 1;
            }
            lang = "zia";
            languageSpecified = true;
        } else if (std::strcmp(argv[i], "basic") == 0) {
            if (languageSpecified) {
                std::cerr << "error: specify at most one REPL language\n";
                std::cerr << "Usage: zanna repl [zia|basic]\n";
                return 1;
            }
            lang = "basic";
            languageSpecified = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: zanna repl [zia|basic]\n"
                      << "  Launch an interactive REPL session.\n"
                      << "  Default language: zia\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            std::cerr << "Usage: zanna repl [zia|basic]\n";
            return 1;
        }
    }

    std::unique_ptr<zanna::repl::ReplAdapter> adapter;

    if (lang == "basic") {
        adapter = std::make_unique<zanna::repl::BasicReplAdapter>();
    } else {
        adapter = std::make_unique<zanna::repl::ZiaReplAdapter>();
    }

    zanna::repl::ReplSession session(std::move(adapter));
    return session.run();
}
