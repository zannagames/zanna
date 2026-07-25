//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the owned registry for dot-prefixed REPL meta commands.
/// @details Entries retain normalized command names, help text, and handlers by
///          value. Dispatch occurs before language compilation and distinguishes
///          ordinary source from any dot-prefixed command, including unknown
///          names that receive a user-facing diagnostic.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplMetaCommands.hpp
// Purpose: Registry of dot-prefixed meta-commands (.help, .quit, .vars, etc.)
//          for the Zanna REPL.
// Key invariants:
//   - Command names are stored without the leading dot.
//   - tryHandle() returns false if the input is not a meta-command.
// Ownership/Lifetime:
//   - Owns the handler map; handlers are std::function objects.
// Links: src/repl/ReplSession.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace zanna::repl {

// Forward declaration
class ReplSession;

/// @brief A single meta-command entry.
/// @details Entry storage is owned by `ReplMetaCommands`; the handler may capture
///          external state whose lifetime remains the registrant's responsibility.
struct MetaCommandEntry {
    std::string name; ///< Command name (without dot).
    std::string help; ///< Short help description.
    std::function<void(ReplSession &, const std::string &)> handler; ///< Handler receiving args.
};

/// @brief Registry and dispatcher for REPL meta-commands.
/// @details Meta-commands are dot-prefixed (e.g., ".help", ".quit") and are
///          dispatched before any language compilation. The registry supports
///          multiple aliases for the same command (e.g., .quit and .exit).
class ReplMetaCommands {
  public:
    /// @brief Register a new meta-command.
    /// @param name Command name (without leading dot).
    /// @param help Short description shown in .help output.
    /// @param handler Function called with (session, remaining_args).
    void registerCommand(const std::string &name,
                         const std::string &help,
                         std::function<void(ReplSession &, const std::string &)> handler);

    /// @brief Try to handle input as a meta-command.
    /// @details Unknown dot-prefixed commands are consumed after printing an
    ///          explanatory message.
    /// @param input Raw input, optionally preceded by whitespace.
    /// @param session The REPL session to pass to the handler.
    /// @return `false` only when the input is not dot-prefixed; otherwise `true`.
    bool tryHandle(const std::string &input, ReplSession &session);

    /// @brief Print help text listing all registered commands.
    void printHelp() const;

  private:
    std::vector<MetaCommandEntry> commands_;
};

} // namespace zanna::repl
