//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/Diagnostics.cpp
// Purpose: Implementation of the target-independent diagnostic sink.
//
//===----------------------------------------------------------------------===//

#include "codegen/common/Diagnostics.hpp"

#include "support/diag_expected.hpp"

#include <ostream>
#include <utility>

/// @file
/// @brief Implements accumulation and formatted emission of codegen diagnostics.

namespace zanna::codegen::common {

/// @copydoc Diagnostics::error(std::string)
void Diagnostics::error(std::string message) {
    error("V-CG-ERROR", std::move(message));
}

/// @copydoc Diagnostics::error(std::string, std::string, il::support::SourceLoc)
void Diagnostics::error(std::string code, std::string message, il::support::SourceLoc loc) {
    errors_.push_back(message);
    il::support::Diagnostic diag{
        il::support::Severity::Error, std::move(message), loc, std::move(code)};
    diag.stage = "codegen";
    diagnostics_.push_back(std::move(diag));
}

/// @copydoc Diagnostics::warning(std::string)
void Diagnostics::warning(std::string message) {
    warning("V-CG-WARN", std::move(message));
}

/// @copydoc Diagnostics::warning(std::string, std::string, il::support::SourceLoc)
void Diagnostics::warning(std::string code, std::string message, il::support::SourceLoc loc) {
    warnings_.push_back(message);
    il::support::Diagnostic diag{
        il::support::Severity::Warning, std::move(message), loc, std::move(code)};
    diag.stage = "codegen";
    diagnostics_.push_back(std::move(diag));
}

/// @copydoc Diagnostics::hasErrors
bool Diagnostics::hasErrors() const noexcept {
    return !errors_.empty();
}

/// @copydoc Diagnostics::hasWarnings
[[maybe_unused]] bool Diagnostics::hasWarnings() const noexcept {
    return !warnings_.empty();
}

// cppcheck-suppress unusedFunction
/// @copydoc Diagnostics::errors
[[maybe_unused]] const std::vector<std::string> &Diagnostics::errors() const noexcept {
    return errors_;
}

// cppcheck-suppress unusedFunction
/// @copydoc Diagnostics::warnings
[[maybe_unused]] const std::vector<std::string> &Diagnostics::warnings() const noexcept {
    return warnings_;
}

/// @copydoc Diagnostics::diagnostics
[[maybe_unused]] const std::vector<il::support::Diagnostic> &
Diagnostics::diagnostics() const noexcept {
    return diagnostics_;
}

/// @copydoc Diagnostics::flush
void Diagnostics::flush(std::ostream &err, std::ostream *warn) const {
    for (const auto &diag : diagnostics_) {
        if (diag.severity == il::support::Severity::Warning) {
            if (warn)
                il::support::printDiag(diag, *warn);
        } else {
            il::support::printDiag(diag, err);
        }
    }
}

} // namespace zanna::codegen::common
