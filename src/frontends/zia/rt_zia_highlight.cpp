//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/rt_zia_highlight.cpp
// Purpose: extern "C" bridge that lets the GUI's syntax-highlighter callback
//          (in zanna_runtime/graphics/rt_gui_codeeditor.c) consult the *real*
//          Zia keyword table from Lexer.cpp instead of maintaining its own
//          drift-prone duplicate. The runtime highlighter calls
//          rt_zia_is_keyword(name, len) for every identifier; this bridge
//          delegates to Lexer::lookupKeyword which binary-searches the
//          authoritative keyword table.
//
// Key invariants:
//   - Strong-symbol implementation. Wins over the weak fallback in
//     zanna_runtime/core/rt_zia_highlight_stub.c when zia_editor_services is
//     linked and force-loaded by the zia binary.
//   - Pure read-only lookup with no persistent state; safe to call from any
//     thread. The transient std::string used by the lexer lookup may allocate.
//
// Ownership/Lifetime:
//   - The (name, len) pair is borrowed from the caller; we copy into a
//     short-lived std::string for the lookup. No allocation crosses the
//     C boundary.
//
// Links: src/runtime/core/rt_zia_highlight_stub.c (weak fallback),
//        src/runtime/graphics/rt_gui_codeeditor.c (consumer),
//        src/frontends/zia/Lexer.hpp (lookupKeyword)
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lexer.hpp"

#include <cstdint>
#include <string>

using il::frontends::zia::Lexer;

extern "C" {

/// @brief Returns 1 if the (name, len) identifier is a Zia keyword, 0 otherwise.
/// @details Strong-symbol override of the weak stub in
///          rt_zia_highlight_stub.c. The runtime syntax highlighter calls
///          this for each identifier it tokenizes; a non-zero return paints
///          the span with the keyword color.
/// @param name Borrowed pointer to the first identifier byte; may be null.
/// @param len Number of bytes available at @p name.
/// @return `1` when the complete byte range is a reserved Zia keyword;
///         otherwise `0`, including for null or empty input.
int rt_zia_is_keyword(const char *name, int64_t len) {
    if (!name || len <= 0)
        return 0;
    std::string key(name, static_cast<size_t>(len));
    return Lexer::lookupKeyword(key).has_value() ? 1 : 0;
}

} // extern "C"
