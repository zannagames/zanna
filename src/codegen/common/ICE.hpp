//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ICE.hpp
// Purpose: Internal Compiler Error (ICE) reporting utility for codegen passes.
//          Provides a macro that always fires (even in release builds) and
//          prints a clean diagnostic before aborting, replacing raw assert()
//          calls that are stripped in NDEBUG builds.
//
// Key invariants:
//   - ZANNA_ICE always executes (not gated on NDEBUG).
//   - Reports file, line, and a human-readable message.
//   - Terminates via std::abort() for clean debugger attachment.
//
// Ownership/Lifetime: Header-only utility, no state.
// Links: plans/audit-05-ice-diagnostics.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

/// @file
/// @brief Provides release-active internal compiler error termination.

namespace zanna::codegen::common {

/// @brief Report an internal compiler error with file/line context, then abort.
///
/// @details This function is intentionally [[noreturn]] and always active —
///          unlike assert(), it is NOT stripped in release builds. Use this for
///          invariant checks that guard against silent miscompilation.
///
/// @param file Source file where the error was detected.
/// @param line Line number where the error was detected.
/// @param msg  Human-readable description of the internal failure.
[[noreturn]] inline void reportICE(const char *file, int line, const std::string &msg) {
    std::cerr << "internal compiler error at " << file << ":" << line << ": " << msg << "\n"
              << "This is a bug in the Zanna compiler. Please report it.\n";
    std::abort();
}

} // namespace zanna::codegen::common

/// @brief Report an internal compiler error with automatic file/line capture.
/// @details Always fires regardless of NDEBUG. Use instead of assert(false)
///          when the fallthrough would produce silently wrong code.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZANNA_ICE(msg) ::zanna::codegen::common::reportICE(__FILE__, __LINE__, (msg))
