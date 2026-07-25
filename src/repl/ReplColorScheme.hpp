//===----------------------------------------------------------------------===//
/// @file
/// @brief Defines the header-only ANSI color palette used by the REPL.
/// @details Escape sequences are exposed as stable string literals and collapse
///          to empty strings when standard output is not attached to a terminal.
///          Terminal capability is detected once per process on first use, so
///          callers can compose the helpers directly into output streams
///          without branching or managing storage.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplColorScheme.hpp
// Purpose: ANSI color constants for the Zanna REPL. Colors are applied via
//          escape codes when stdout is a terminal; all codes resolve to empty
//          strings when output is piped or redirected.
// Key invariants:
//   - isColorEnabled() checks isatty(STDOUT_FILENO) once at startup.
//   - All color() calls return "" when color is disabled.
// Ownership/Lifetime:
//   - Header-only; no heap allocations.
// Links: src/repl/ReplSession.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#include <stdio.h>
#endif

namespace zanna::repl {

/// @brief ANSI color scheme for the REPL.
/// @details All methods return empty strings when stdout is not a terminal.
namespace colors {

/// @brief Check whether ANSI color output is supported and enabled.
/// @details The platform terminal probe is evaluated once and cached in a
///          function-local static.
/// @return `true` if standard output is connected to a terminal.
inline bool isColorEnabled() {
    static const bool enabled = [] {
#if defined(__unix__) || defined(__APPLE__)
        return isatty(STDOUT_FILENO) != 0;
#elif defined(_WIN32)
        return _isatty(_fileno(stdout)) != 0;
#else
        return false;
#endif
    }();
    return enabled;
}

// --- Reset ---
/// @brief Restore default terminal attributes.
/// @return ANSI reset sequence, or an empty string when color is disabled.
inline const char *reset() {
    return isColorEnabled() ? "\033[0m" : "";
}

/// @brief Enable bold terminal text.
/// @return ANSI bold sequence, or an empty string when color is disabled.
inline const char *bold() {
    return isColorEnabled() ? "\033[1m" : "";
}

/// @brief Enable dim terminal text.
/// @return ANSI dim sequence, or an empty string when color is disabled.
inline const char *dim() {
    return isColorEnabled() ? "\033[2m" : "";
}

// --- Prompt ---
/// @brief Select the emphasized cyan primary-prompt style.
/// @return ANSI prompt sequence, or an empty string when color is disabled.
inline const char *prompt() {
    return isColorEnabled() ? "\033[1;36m" : "";
}

/// @brief Select the cyan continuation-prompt style.
/// @return ANSI continuation sequence, or an empty string when color is
///         disabled.
inline const char *contPrompt() {
    return isColorEnabled() ? "\033[36m" : "";
}

// --- Output types ---
/// @brief Select the emphasized green result style.
/// @return ANSI result sequence, or an empty string when color is disabled.
inline const char *result() {
    return isColorEnabled() ? "\033[1;32m" : "";
}

/// @brief Select the yellow string-value style.
/// @return ANSI string sequence, or an empty string when color is disabled.
inline const char *string() {
    return isColorEnabled() ? "\033[33m" : "";
}

/// @brief Select the blue numeric-value style.
/// @return ANSI number sequence, or an empty string when color is disabled.
inline const char *number() {
    return isColorEnabled() ? "\033[34m" : "";
}

/// @brief Select the magenta Boolean-value style.
/// @return ANSI Boolean sequence, or an empty string when color is disabled.
inline const char *boolean() {
    return isColorEnabled() ? "\033[35m" : "";
}

/// @brief Select the dim gray null-value style.
/// @return ANSI null sequence, or an empty string when color is disabled.
inline const char *null() {
    return isColorEnabled() ? "\033[2;37m" : "";
}

/// @brief Select the cyan type-name style.
/// @return ANSI type sequence, or an empty string when color is disabled.
inline const char *type() {
    return isColorEnabled() ? "\033[36m" : "";
}

// --- Diagnostics ---
/// @brief Select the emphasized red error style.
/// @return ANSI error sequence, or an empty string when color is disabled.
inline const char *error() {
    return isColorEnabled() ? "\033[1;31m" : "";
}

/// @brief Select the emphasized yellow warning style.
/// @return ANSI warning sequence, or an empty string when color is disabled.
inline const char *warning() {
    return isColorEnabled() ? "\033[1;33m" : "";
}

/// @brief Select the emphasized blue note style.
/// @return ANSI note sequence, or an empty string when color is disabled.
inline const char *note() {
    return isColorEnabled() ? "\033[1;34m" : "";
}

/// @brief Select the emphasized green success style.
/// @return ANSI success sequence, or an empty string when color is disabled.
inline const char *success() {
    return isColorEnabled() ? "\033[1;32m" : "";
}

} // namespace colors

} // namespace zanna::repl
