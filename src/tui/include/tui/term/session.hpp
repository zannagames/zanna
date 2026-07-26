//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the cross-platform raw-terminal RAII session.
/// @details Saves and restores POSIX termios or Windows console modes while
///          enabling raw input and ANSI/virtual-terminal behavior for TUI use.
//
// TerminalSession follows RAII semantics: the constructor enters raw mode
// and the destructor restores the original terminal state. This ensures
// the terminal is always left in a usable state, even if the application
// crashes or throws an exception.
//
// Key invariants:
//   - active() returns true only if raw mode was successfully entered.
//   - The destructor always restores the original terminal state.
//   - TerminalSession is non-copyable to prevent double-restore.
//
// Ownership: TerminalSession stores the original terminal configuration
// (termios on POSIX, console modes on Windows) and is responsible for
// restoring them on destruction.
//
//===----------------------------------------------------------------------===//

#pragma once
#include <cstdlib>

#if defined(__unix__) || defined(__APPLE__)
#define ZANNATUI_POSIX 1
#include <termios.h>
#include <unistd.h>
#else
#define ZANNATUI_POSIX 0
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace zanna::tui {

/// @brief RAII guard that enters raw terminal mode on construction and restores
///        the original terminal settings on destruction.
/// @details On POSIX, configures the terminal for raw input (disables canonical
///          mode, echo, and signal processing). On Windows, enables Virtual
///          Terminal Processing for ANSI escape sequence support. The guard is
///          non-copyable to prevent double-restoration of terminal state.
class TerminalSession {
  public:
    /// @brief Enter raw terminal mode, saving the original settings for later restoration.
    TerminalSession();
    /// @brief Restore the original terminal settings saved during construction.
    ~TerminalSession();

    TerminalSession(const TerminalSession &) = delete;
    TerminalSession &operator=(const TerminalSession &) = delete;

    /// @brief Check whether raw terminal mode was successfully activated.
    /// @return True if the terminal is currently in raw mode; false if setup failed.
    bool active() const;

  private:
    bool active_{false}; ///< Whether terminal acquisition succeeded and needs restoration.
#if ZANNATUI_POSIX
    termios orig_{}; ///< Original POSIX terminal attributes.
#endif
#if defined(_WIN32)
    DWORD orig_out_mode_{0}; ///< Original Windows output console mode.
    DWORD orig_in_mode_{0};  ///< Original Windows input console mode.
#endif
};

} // namespace zanna::tui
