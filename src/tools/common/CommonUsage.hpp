//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/CommonUsage.hpp
// Purpose: Shared CLI option text for frontend tools (zbasic, zia).
// Key invariants: All frontend tools share the same option descriptions.
// Ownership/Lifetime: Stateless; the helper borrows the caller's output stream
//                     for the duration of the call and retains no references.
// Links: docs/internals/architecture.md, src/tools/zbasic/usage.cpp, src/tools/zia/usage.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared help-detail selection and frontend option text.
/// @details Standalone language drivers call the inline printer to keep common
///          option spelling and explanations synchronized without duplicating
///          static strings across tool executables.

#pragma once

#include <ostream>

namespace zanna::tools {

/// @brief Selects how much of the shared option list to emit.
/// @details Controls whether @ref printSharedOptions appends the rarely used
///          "Advanced diagnostics" section after the common options block.
///          `All` currently includes the same shared groups as `Advanced` and
///          leaves room for callers to append tool-specific groups.
enum class FrontendHelpDetail {
    Common,        ///< Emit only options honored by every standalone frontend tool.
    Advanced,      ///< Emit common options plus Zia warning/diagnostic controls.
    BasicAdvanced, ///< Emit common options plus BASIC diagnostics and compatibility flags.
    All,           ///< Emit every option group.
};

/// @brief Print shared CLI option descriptions to an output stream.
/// @details Outputs the standard option descriptions used by all frontend tools.
///          This ensures consistency across zbasic and zia help text. When
///          @p detail requests an advanced mode, the applicable diagnostics
///          and warning-control block is appended after the common options.
/// @param os Output stream to write to (typically std::cerr).
/// @param detail Amount of option detail to emit; defaults to
///        @ref FrontendHelpDetail::Common.
inline void printSharedOptions(std::ostream &os,
                               FrontendHelpDetail detail = FrontendHelpDetail::Common) {
    os << "  -o, --output FILE              Write .il or compile a native binary\n"
       << "  --emit-il                      Emit IL to stdout instead of running\n"
       << "  --trace[=il|src]               Enable execution tracing\n"
       << "  --bounds-checks                Enable array bounds checking (default for source)\n"
       << "  --no-bounds-checks             Disable generated source bounds checks\n"
       << "  --stdin-from FILE              Redirect stdin from file\n"
       << "  --max-steps N                  Limit execution steps\n"
       << "  --dump-trap                    Show detailed trap diagnostics\n"
       << "  -h, --help                     Show this help message\n"
       << "  --version                      Show version information\n";
    if (detail == FrontendHelpDetail::Advanced || detail == FrontendHelpDetail::BasicAdvanced ||
        detail == FrontendHelpDetail::All) {
        os << "  --diagnostic-format text|json   Select diagnostic output format\n";
        if (detail == FrontendHelpDetail::BasicAdvanced) {
            os << "  -Wall, -Werror, -Wno-XXXX      Accepted for shared CLI compatibility\n"
               << "  --[no-]strict-diagnostics       Accepted for shared CLI compatibility\n"
               << "  --quiet-warnings               Suppress warnings on successful compiles\n";
        } else {
            os << "  -Wall                          Enable all warnings\n"
               << "  -Werror                        Treat warnings as errors\n"
               << "  --strict-diagnostics           Error on safety-critical warnings (default)\n"
               << "  --no-strict-diagnostics        Leave safety-critical warnings non-fatal\n"
               << "  --quiet-warnings               Suppress warning output on successful "
                  "compiles\n"
               << "  -Wno-XXXX                      Disable warning (code or name)\n";
        }
        os << "\n"
           << "Advanced diagnostics:\n"
           << "  --paranoid-verify              Run all frontend verifier checkpoints\n"
           << "  --pass-stats                   Print detailed optimizer pass statistics\n";
    }
}

} // namespace zanna::tools
