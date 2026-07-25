//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the dependency-free terminal line editor used by the REPL.
/// @details The public interface hides terminal-session and input-decoder types
///          behind a private implementation. Construction acquires terminal
///          editing state, destruction restores it, and history entries and
///          completion callbacks are owned by the editor.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplLineEditor.hpp
// Purpose: Custom line editor for the Zanna REPL, built on the TUI framework's
//          TerminalSession and InputDecoder. Provides line editing, history,
//          and tab completion without any external dependencies.
// Key invariants:
//   - Enters raw terminal mode on construction; restores on destruction.
//   - History is bounded by maxHistorySize.
//   - Tab completion callback is optional (nullptr = no completion).
// Ownership/Lifetime:
//   - Owns TerminalSession (raw mode RAII) and InputDecoder.
//   - History entries are owned std::strings.
// Links: src/tui/include/tui/term/session.hpp, src/tui/include/tui/term/input.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zanna::repl {

/// @brief Result of a line editor read operation.
enum class ReadResult {
    Line,      ///< A complete line was entered (Enter pressed).
    Interrupt, ///< Ctrl-C was pressed (cancel current input).
    Eof,       ///< Ctrl-D was pressed (end of input).
};

/// @brief Completion callback signature.
/// @param input Current line buffer content.
/// @param cursor Current cursor position in the buffer.
/// @return Vector of completion strings to offer.
using CompletionCallback =
    std::function<std::vector<std::string>(const std::string &input, size_t cursor)>;

/// @brief Custom line editor built on the Zanna TUI framework.
/// @details Provides interactive line editing with cursor movement (left/right,
///          Home/End), character insertion and deletion (Backspace, Delete),
///          history navigation (Up/Down arrows), word-level movement (Ctrl+Left,
///          Ctrl+Right), line kill (Ctrl-U, Ctrl-K), tab completion, and
///          signal handling (Ctrl-C, Ctrl-D).
class ReplLineEditor {
  public:
    /// @brief Construct a line editor.
    /// @param maxHistory Maximum number of history entries to retain.
    ReplLineEditor(size_t maxHistory = 1000);

    /// @brief Destroy the editor and restore terminal session state.
    ~ReplLineEditor();

    // Non-copyable
    /// @brief Disable copying because terminal-session ownership is unique.
    ReplLineEditor(const ReplLineEditor &) = delete;
    /// @brief Disable copy assignment because terminal-session ownership is unique.
    /// @return This declaration is deleted and cannot be invoked.
    ReplLineEditor &operator=(const ReplLineEditor &) = delete;

    /// @brief Read a line of input with the given prompt.
    /// @param prompt The prompt string to display (may contain ANSI codes).
    /// @param line Output parameter; set to the entered line on ReadResult::Line.
    /// @return The result of the read operation.
    ReadResult readLine(const std::string &prompt, std::string &line);

    /// @brief Add an entry to the history ring.
    /// @param entry The line to add. Empty lines and duplicates of the last entry are skipped.
    void addHistory(const std::string &entry);

    /// @brief Set the tab completion callback.
    /// @param cb Callback function; nullptr to disable completion.
    void setCompletionCallback(CompletionCallback cb);

    /// @brief Check if the terminal is in raw mode and usable.
    /// @return `true` while the underlying terminal session is active.
    bool isActive() const;

    /// @brief Get a copy of the history entries.
    /// @return Vector of history strings (oldest first).
    std::vector<std::string> getHistory() const;

    /// @brief Load history from a file.
    /// @details Existing in-memory history is replaced and decoded entries are
    ///          trimmed to this editor's maximum retention count.
    /// @param path Path to the history file.
    /// @return Number of entries decoded before retention trimming.
    size_t loadHistory(const std::filesystem::path &path);

    /// @brief Save history to a file.
    /// @details Creates parent directories if they don't exist.
    /// @param path Path to the history file.
    /// @return True if save succeeded.
    bool saveHistory(const std::filesystem::path &path) const;

  private:
    struct Impl;
    Impl *impl_; ///< Pointer-to-implementation (hides TUI headers from callers).
};

} // namespace zanna::repl
