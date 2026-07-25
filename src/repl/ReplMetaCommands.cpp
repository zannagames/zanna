//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements registration, dispatch, and help output for REPL meta commands.
/// @details Command names are normalized to lowercase when registered and when
///          parsed, while handler arguments preserve their original spelling.
///          Dot-prefixed unknown commands are considered handled after an
///          explanatory diagnostic so they are not passed to a language
///          compiler.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplMetaCommands.cpp
// Purpose: Implementation of meta-command registry and dispatch.
// Key invariants:
//   - Input must start with '.' to be considered a meta-command.
//   - Command names are matched case-insensitively.
// Ownership/Lifetime:
//   - Commands_ vector is owned by ReplMetaCommands.
// Links: src/repl/ReplMetaCommands.hpp
//
//===----------------------------------------------------------------------===//

#include "ReplMetaCommands.hpp"
#include "ReplColorScheme.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace zanna::repl {

/// @brief Append a meta command to the dispatch registry.
/// @details The command name is normalized case-insensitively; help text and
///          handler are stored by value in registration order.
/// @param name Command spelling without the leading dot.
/// @param help Human-readable description used by `printHelp()`.
/// @param handler Callable invoked with the session and unparsed argument tail.
void ReplMetaCommands::registerCommand(
    const std::string &name,
    const std::string &help,
    std::function<void(ReplSession &, const std::string &)> handler) {
    std::string normalizedName = name;
    std::transform(normalizedName.begin(),
                   normalizedName.end(),
                   normalizedName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    commands_.push_back({normalizedName, help, std::move(handler)});
}

/// @brief Parse and dispatch a possible dot-prefixed meta command.
/// @details Leading whitespace is accepted. The first whitespace-delimited
///          token after the dot is matched case-insensitively, and remaining
///          text after leading argument whitespace is passed unchanged to the
///          handler. Unknown dot commands print guidance.
/// @param input Complete input line.
/// @param session Session supplied to a matched command handler.
/// @return `false` when @p input is not a meta command; `true` for either a
///         dispatched or unknown dot command.
bool ReplMetaCommands::tryHandle(const std::string &input, ReplSession &session) {
    size_t firstNonSpace = 0;
    while (firstNonSpace < input.size() &&
           std::isspace(static_cast<unsigned char>(input[firstNonSpace])))
        ++firstNonSpace;

    if (firstNonSpace >= input.size() || input[firstNonSpace] != '.')
        return false;

    // Parse: ".command args..."
    size_t cmdStart = firstNonSpace + 1;
    size_t cmdEnd = cmdStart;
    while (cmdEnd < input.size() && !std::isspace(static_cast<unsigned char>(input[cmdEnd])))
        ++cmdEnd;

    std::string cmdName = input.substr(cmdStart, cmdEnd - cmdStart);

    // Convert to lowercase for case-insensitive matching
    std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // Extract args (trim leading whitespace)
    std::string args;
    size_t argsStart = cmdEnd;
    while (argsStart < input.size() && std::isspace(static_cast<unsigned char>(input[argsStart])))
        ++argsStart;
    if (argsStart < input.size())
        args = input.substr(argsStart);

    // Find and execute matching command
    for (const auto &cmd : commands_) {
        if (cmd.name == cmdName) {
            cmd.handler(session, args);
            return true;
        }
    }

    // Unknown meta-command
    std::cout << colors::error() << "Unknown command: ." << cmdName << colors::reset() << "\n";
    std::cout << "Type " << colors::bold() << ".help" << colors::reset()
              << " for available commands.\n";
    return true;
}

/// @brief Print all registered commands and descriptions.
/// @details Command names use prompt color and descriptions are padded to align
///          after the longest registered name.
void ReplMetaCommands::printHelp() const {
    std::cout << colors::bold() << "Available commands:" << colors::reset() << "\n";

    // Find max command name length for alignment
    size_t maxLen = 0;
    for (const auto &cmd : commands_)
        maxLen = std::max(maxLen, cmd.name.size());

    for (const auto &cmd : commands_) {
        std::cout << "  " << colors::prompt() << "." << cmd.name << colors::reset();
        // Pad to align descriptions
        for (size_t i = cmd.name.size(); i < maxLen + 2; ++i)
            std::cout << ' ';
        std::cout << cmd.help << "\n";
    }
}

} // namespace zanna::repl
