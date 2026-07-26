//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the language-neutral REPL session lifecycle and main loop.
/// @details A session owns one language adapter, terminal line editor, and
///          meta-command registry. It accumulates multiline input until the
///          adapter reports completeness, dispatches meta commands before
///          compilation, presents typed results and diagnostics, and loads/saves
///          per-language history around the session lifetime.
///
///          Interactive mode provides prompts, editing, cancellation, and
///          double-interrupt exit behavior. Piped mode reads standard input
///          directly and reports an incomplete trailing submission with a
///          nonzero exit status.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplSession.cpp
// Purpose: Implementation of the core REPL session loop.
// Key invariants:
//   - Compilation errors never destroy session state.
//   - Multi-line input accumulates until bracket depth reaches zero.
//   - Meta-commands are dispatched before any compilation attempt.
// Ownership/Lifetime:
//   - Owns the adapter, editor, and meta-command registry.
// Links: src/repl/ReplSession.hpp
//
//===----------------------------------------------------------------------===//

#include "ReplSession.hpp"
#include "ReplColorScheme.hpp"

#include "zanna/version.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace zanna::repl {

/// @brief Construct a REPL session around a language adapter.
/// @details Ownership of @p adapter transfers to the session. Default meta
///          commands and adapter-backed completion are registered, then
///          per-language persistent history is loaded when a home directory is
///          available.
/// @param adapter Non-null language adapter with session state.
ReplSession::ReplSession(std::unique_ptr<ReplAdapter> adapter) : adapter_(std::move(adapter)) {
    registerDefaultCommands();

    // Wire up tab completion
    editor_.setCompletionCallback(
        /// @brief Request language-specific completions from the owned adapter.
        /// @param input Complete editable input buffer.
        /// @param cursor Cursor byte offset within @p input.
        /// @return Candidate completion strings in adapter-defined order.
        [this](const std::string &input, size_t cursor) -> std::vector<std::string> {
            return adapter_->complete(input, cursor);
        });

    // Load persistent history
    auto histPath = historyFilePath();
    if (!histPath.empty())
        editor_.loadHistory(histPath);
}

/// @brief Resolve the per-language persistent history path.
/// @details `HOME` is used normally and `USERPROFILE` is a Windows fallback.
///          The filename includes `languageName()` to isolate adapter histories.
/// @return `$HOME/.zanna/repl_history_<language>`, or an empty path when no
///         supported home-directory environment variable is set.
std::filesystem::path ReplSession::historyFilePath() const {
    const char *home = std::getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = std::getenv("USERPROFILE");
#endif
    if (!home)
        return {};

    std::filesystem::path dir(home);
    dir /= ".zanna";
    std::string filename = "repl_history_";
    filename += adapter_->languageName();
    return dir / filename;
}

/// @brief Populate the built-in meta-command registry.
/// @details Registers help/exit aliases, adapter state queries, type and IL
///          inspection, timed evaluation, source loading, and explicit history
///          saving. Handlers either capture this session or use the session
///          reference supplied by dispatch.
void ReplSession::registerDefaultCommands() {
    metaCmds_.registerCommand(
        "help",
        "Show this help message",
        /// @brief Print the registered meta-command help table.
        /// @param session Dispatch session; the captured session is used instead.
        /// @param args Unused argument tail.
        [this](ReplSession & /*session*/, const std::string & /*args*/) { metaCmds_.printHelp(); });

    metaCmds_.registerCommand(
        /// @brief Request an orderly REPL exit through the dispatched session.
        /// @param session Session whose exit flag is set.
        /// @param args Unused argument tail.
        "quit", "Exit the REPL", [](ReplSession &session, const std::string & /*args*/) {
            session.requestExit();
        });

    metaCmds_.registerCommand(
        /// @brief Handle the `.exit` alias by requesting an orderly REPL exit.
        /// @param session Session whose exit flag is set.
        /// @param args Unused argument tail.
        "exit", "Exit the REPL", [](ReplSession &session, const std::string & /*args*/) {
            session.requestExit();
        });

    metaCmds_.registerCommand(
        /// @brief Reset adapter state and acknowledge the cleared session.
        /// @param session Session whose language adapter is reset.
        /// @param args Unused argument tail.
        "clear", "Reset session state", [](ReplSession &session, const std::string & /*args*/) {
            session.adapter().reset();
            std::cout << "Session state cleared.\n";
        });

    metaCmds_.registerCommand(
        /// @brief List persistent variables reported by the language adapter.
        /// @param session Session whose adapter supplies variable metadata.
        /// @param args Unused argument tail.
        "vars", "List session variables", [](ReplSession &session, const std::string & /*args*/) {
            auto vars = session.adapter().listVariables();
            if (vars.empty()) {
                std::cout << colors::dim() << "(no variables)" << colors::reset() << "\n";
                return;
            }
            for (const auto &v : vars) {
                std::cout << "  " << colors::bold() << v.name << colors::reset() << " : "
                          << colors::type() << v.type << colors::reset() << "\n";
            }
        });

    metaCmds_.registerCommand(
        /// @brief List persistent functions reported by the language adapter.
        /// @param session Session whose adapter supplies function metadata.
        /// @param args Unused argument tail.
        "funcs", "List defined functions", [](ReplSession &session, const std::string & /*args*/) {
            auto funcs = session.adapter().listFunctions();
            if (funcs.empty()) {
                std::cout << colors::dim() << "(no functions)" << colors::reset() << "\n";
                return;
            }
            for (const auto &f : funcs) {
                std::cout << "  " << colors::bold() << f.name << colors::reset() << " "
                          << colors::dim() << f.signature << colors::reset() << "\n";
            }
        });

    metaCmds_.registerCommand("binds",
                              "List active bind statements",
                              /// @brief List active bind statements reported by the adapter.
                              /// @param session Session whose adapter supplies bind text.
                              /// @param args Unused argument tail.
                              [](ReplSession &session, const std::string & /*args*/) {
                                  auto binds = session.adapter().listBinds();
                                  if (binds.empty()) {
                                      std::cout << colors::dim() << "(no binds)" << colors::reset()
                                                << "\n";
                                      return;
                                  }
                                  for (const auto &b : binds) {
                                      std::cout << "  " << b << "\n";
                                  }
                              });

    metaCmds_.registerCommand(
        /// @brief Print the adapter-inferred type of an expression argument.
        /// @param session Session whose adapter performs type inference.
        /// @param args Unparsed expression text.
        "type", "Show type of expression", [](ReplSession &session, const std::string &args) {
            if (args.empty()) {
                std::cout << colors::warning() << "Usage: .type <expression>" << colors::reset()
                          << "\n";
                return;
            }
            std::string typeStr = session.adapter().getExprType(args);
            std::cout << colors::type() << typeStr << colors::reset() << "\n";
        });

    metaCmds_.registerCommand("il",
                              "Show generated IL for expression",
                              /// @brief Print adapter-generated IL for an expression.
                              /// @param session Session whose adapter lowers the expression.
                              /// @param args Unparsed expression text.
                              [](ReplSession &session, const std::string &args) {
                                  if (args.empty()) {
                                      std::cout << colors::warning() << "Usage: .il <expression>"
                                                << colors::reset() << "\n";
                                      return;
                                  }
                                  std::string il = session.adapter().getIL(args);
                                  std::cout << colors::dim() << il << colors::reset();
                                  if (!il.empty() && il.back() != '\n')
                                      std::cout << "\n";
                              });

    metaCmds_.registerCommand(
        "time",
        "Evaluate and show execution time",
        /// @brief Evaluate an expression and report its wall-clock duration.
        /// @param session Session whose adapter evaluates the expression.
        /// @param args Unparsed expression text.
        [](ReplSession &session, const std::string &args) {
            if (args.empty()) {
                std::cout << colors::warning() << "Usage: .time <expression>" << colors::reset()
                          << "\n";
                return;
            }
            auto start = std::chrono::high_resolution_clock::now();
            auto result = session.adapter().eval(args);
            auto end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            if (result.success && !result.output.empty()) {
                std::cout << colors::result() << result.output << colors::reset();
                if (result.output.back() != '\n')
                    std::cout << "\n";
            } else if (!result.success) {
                std::cout << colors::error() << result.errorMessage << colors::reset();
                if (!result.errorMessage.empty() && result.errorMessage.back() != '\n')
                    std::cout << "\n";
            }

            // Format elapsed time
            if (elapsed.count() >= 1000000) {
                double secs = static_cast<double>(elapsed.count()) / 1000000.0;
                std::cout << colors::dim() << "Elapsed: " << secs << "s" << colors::reset() << "\n";
            } else if (elapsed.count() >= 1000) {
                double ms = static_cast<double>(elapsed.count()) / 1000.0;
                std::cout << colors::dim() << "Elapsed: " << ms << "ms" << colors::reset() << "\n";
            } else {
                std::cout << colors::dim() << "Elapsed: " << elapsed.count() << "us"
                          << colors::reset() << "\n";
            }
        });

    metaCmds_.registerCommand(
        "load",
        "Load and execute a source file",
        /// @brief Load and execute the source file named by the argument tail.
        /// @param session Session used to load and evaluate the file.
        /// @param args File-system path text.
        [](ReplSession &session, const std::string &args) {
            if (args.empty()) {
                std::cout << colors::warning() << "Usage: .load <filepath>" << colors::reset()
                          << "\n";
                return;
            }
            if (session.loadFile(args))
                std::cout << colors::success() << "Loaded: " << args << colors::reset() << "\n";
        });

    metaCmds_.registerCommand("save",
                              "Save session history to a file",
                              /// @brief Save the current editor history to a requested path.
                              /// @param session Dispatch session; captured editor state is used.
                              /// @param args File-system path text.
                              [this](ReplSession & /*session*/, const std::string &args) {
                                  if (args.empty()) {
                                      std::cout << colors::warning() << "Usage: .save <filepath>"
                                                << colors::reset() << "\n";
                                      return;
                                  }
                                  auto history = editor_.getHistory();
                                  if (!editor_.saveHistory(args)) {
                                      std::cout << colors::error() << "Could not write: " << args
                                                << colors::reset() << "\n";
                                      return;
                                  }
                                  std::cout << colors::success() << "Saved " << history.size()
                                            << " entries to: " << args << colors::reset() << "\n";
                              });
}

/// @brief Print the interactive REPL banner and command hints.
void ReplSession::printBanner() {
    std::cout << colors::bold() << "Zanna " << adapter_->languageName() << " REPL"
              << colors::reset() << " v" << ZANNA_VERSION_STR << "\n";
    std::cout << "Type " << colors::prompt() << ".help" << colors::reset() << " for commands, "
              << colors::prompt() << ".quit" << colors::reset() << " to exit.\n\n";
}

/// @brief Build the colored primary prompt for the active language.
/// @return Prompt bytes including ANSI style/reset sequences when enabled.
std::string ReplSession::makePrompt() const {
    std::string p;
    p += colors::prompt();
    p += adapter_->languageName();
    p += "> ";
    p += colors::reset();
    return p;
}

/// @brief Build the colored multiline continuation prompt.
/// @return Prompt bytes including ANSI style/reset sequences when enabled.
std::string ReplSession::makeContinuationPrompt() const {
    std::string p;
    p += colors::contPrompt();
    p += "...> ";
    p += colors::reset();
    return p;
}

/// @brief Load and sequentially evaluate submissions from a source file.
/// @details Physical lines are accumulated according to adapter classification.
///          Empty fragments are skipped, meta commands are dispatched, and
///          complete language fragments are evaluated in the current session.
///          Processing stops at the first evaluation failure or incomplete
///          trailing fragment.
/// @param path Source file path.
/// @return `true` when the file opens and every accumulated submission
///         succeeds; otherwise `false` after printing a diagnostic.
bool ReplSession::loadFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << colors::error() << "Could not open: " << path << colors::reset() << "\n";
        return false;
    }

    std::string accumulated;
    std::string line;
    while (std::getline(file, line)) {
        if (!accumulated.empty())
            accumulated += "\n";
        accumulated += line;

        InputKind kind = adapter_->classifyInput(accumulated);
        if (kind == InputKind::Empty) {
            accumulated.clear();
            continue;
        }
        if (kind == InputKind::Incomplete)
            continue;
        if (kind == InputKind::MetaCommand) {
            metaCmds_.tryHandle(accumulated, *this);
            accumulated.clear();
            continue;
        }

        EvalResult result = adapter_->eval(accumulated);
        if (!result.success) {
            std::cout << colors::error() << result.errorMessage << colors::reset();
            if (!result.errorMessage.empty() && result.errorMessage.back() != '\n')
                std::cout << "\n";
            return false;
        }
        if (!result.output.empty()) {
            std::cout << colors::result() << result.output << colors::reset();
            if (result.output.back() != '\n')
                std::cout << "\n";
        }
        accumulated.clear();
    }

    if (!accumulated.empty()) {
        std::cout << colors::error() << "Incomplete input at end of file: " << path
                  << colors::reset() << "\n";
        return false;
    }

    return true;
}

/// @brief Request termination after the current loop action completes.
void ReplSession::requestExit() {
    running_ = false;
}

/// @brief Run the REPL until EOF, exit command, or confirmed interrupt.
/// @details Terminal availability selects interactive line editing or piped
///          input. Complete submissions are added to history and evaluated;
///          result type chooses display color. On exit, persistent history is
///          saved when a home path exists.
/// @return Zero for normal completion, or one when piped input ends with an
///         incomplete submission.
int ReplSession::run() {
    const bool interactive = editor_.isActive();
    int exitCode = 0;

    if (interactive)
        printBanner();

    while (running_) {
        std::string line;

        if (interactive) {
            // Interactive mode with line editing
            std::string prompt =
                accumulatedInput_.empty() ? makePrompt() : makeContinuationPrompt();
            ReadResult readResult = editor_.readLine(prompt, line);

            switch (readResult) {
                case ReadResult::Eof:
                    running_ = false;
                    continue;

                case ReadResult::Interrupt:
                    if (!accumulatedInput_.empty()) {
                        accumulatedInput_.clear();
                        consecutiveInterrupts_ = 0;
                        std::cout << colors::note() << "(input cancelled)" << colors::reset()
                                  << "\n";
                    } else {
                        ++consecutiveInterrupts_;
                        if (consecutiveInterrupts_ >= 2) {
                            running_ = false;
                        } else {
                            std::cout << colors::dim() << "(press Ctrl-C again to exit)"
                                      << colors::reset() << "\n";
                        }
                    }
                    continue;

                case ReadResult::Line:
                    consecutiveInterrupts_ = 0;
                    break;
            }
        } else {
            // Non-interactive mode (piped input): read lines from stdin
            if (!std::getline(std::cin, line)) {
                if (!accumulatedInput_.empty() &&
                    adapter_->classifyInput(accumulatedInput_) == InputKind::Incomplete) {
                    std::cout << colors::error()
                              << "Incomplete input at end of stream; expected more input."
                              << colors::reset() << "\n";
                    exitCode = 1;
                }
                running_ = false;
                continue;
            }
        }

        // Accumulate input
        if (!accumulatedInput_.empty()) {
            accumulatedInput_ += "\n";
        }
        accumulatedInput_ += line;

        // Classify accumulated input
        InputKind kind = adapter_->classifyInput(accumulatedInput_);

        switch (kind) {
            case InputKind::Empty:
                accumulatedInput_.clear();
                continue;

            case InputKind::MetaCommand:
                metaCmds_.tryHandle(accumulatedInput_, *this);
                editor_.addHistory(accumulatedInput_);
                accumulatedInput_.clear();
                continue;

            case InputKind::Incomplete:
                // Need more input; loop with continuation prompt
                continue;

            case InputKind::Complete:
                break;
        }

        // Evaluate the complete input
        ++inputCounter_;
        editor_.addHistory(accumulatedInput_);

        EvalResult result = adapter_->eval(accumulatedInput_);
        accumulatedInput_.clear();

        if (result.success) {
            if (!result.output.empty()) {
                // Choose color based on expression result type
                const char *color;
                switch (result.resultType) {
                    case ResultType::Integer:
                    case ResultType::Number:
                        color = colors::number();
                        break;
                    case ResultType::String:
                        color = colors::string();
                        break;
                    case ResultType::Boolean:
                        color = colors::boolean();
                        break;
                    case ResultType::Object:
                        color = colors::type();
                        break;
                    default:
                        color = colors::result();
                        break;
                }
                std::cout << color << result.output << colors::reset();
                // Ensure trailing newline
                if (result.output.back() != '\n')
                    std::cout << "\n";
            }
        } else {
            std::cout << colors::error() << result.errorMessage << colors::reset();
            if (!result.errorMessage.empty() && result.errorMessage.back() != '\n')
                std::cout << "\n";
        }
    }

    // Save history to persistent file
    auto histPath = historyFilePath();
    if (!histPath.empty())
        editor_.saveHistory(histPath);

    if (interactive)
        std::cout << "Goodbye.\n";
    return exitCode;
}

} // namespace zanna::repl
