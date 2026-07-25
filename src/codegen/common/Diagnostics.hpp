//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/Diagnostics.hpp
// Purpose: Target-independent diagnostic sink for codegen passes.
// Key invariants: Errors trigger pipeline short-circuit; warnings do not.
// Ownership/Lifetime: Owned by caller; typically lives for the duration of a
//                     single pass-manager run.
// Links: codegen/common/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "support/diagnostics.hpp"

#include <ostream>
#include <string>
#include <vector>

/// @file
/// @brief Declares the accumulating diagnostic sink shared by codegen passes.

namespace zanna::codegen::common {

/// @brief Diagnostic sink used by passes to surface errors and warnings.
/// @details Errors are fatal and cause the pass manager to short-circuit.
///          Warnings are advisory and do not stop the pipeline.
class Diagnostics {
  public:
    /// @brief Record an error message and mark the diagnostic stream as failed.
    /// @param message Human-readable error text. The generic code
    ///                `V-CG-ERROR` and codegen stage are attached.
    void error(std::string message);

    /// @brief Record a structured error diagnostic.
    /// @param code Stable diagnostic identifier.
    /// @param message Human-readable error text.
    /// @param loc Optional source location associated with the failure.
    void error(std::string code, std::string message, il::support::SourceLoc loc = {});

    /// @brief Record a non-fatal warning message.
    /// @param message Human-readable warning text. The generic code
    ///                `V-CG-WARN` and codegen stage are attached.
    void warning(std::string message);

    /// @brief Record a structured warning diagnostic.
    /// @param code Stable diagnostic identifier.
    /// @param message Human-readable warning text.
    /// @param loc Optional source location associated with the warning.
    void warning(std::string code, std::string message, il::support::SourceLoc loc = {});

    /// @brief Query whether any error has been recorded.
    /// @return `true` when the error-message collection is nonempty.
    [[nodiscard]] bool hasErrors() const noexcept;

    /// @brief Query whether any warnings were recorded.
    /// @return `true` when the warning-message collection is nonempty.
    [[maybe_unused]] [[nodiscard]] bool hasWarnings() const noexcept;

    /// @brief Emit accumulated diagnostics to the provided streams.
    /// @param err Destination for all error-severity structured diagnostics.
    /// @param warn Optional destination for warnings. When null, warnings are
    ///             retained but not printed.
    /// @note Flushing does not clear stored diagnostics.
    void flush(std::ostream &err, std::ostream *warn = nullptr) const;

    /// @brief Obtain the stored plain error-message view.
    /// @return Reference valid until this object is mutated or destroyed.
    [[maybe_unused]] [[nodiscard]] const std::vector<std::string> &errors() const noexcept;

    /// @brief Obtain the stored plain warning-message view.
    /// @return Reference valid until this object is mutated or destroyed.
    [[maybe_unused]] [[nodiscard]] const std::vector<std::string> &warnings() const noexcept;

    /// @brief Direct read-only access to structured diagnostics.
    /// @return Source-order error and warning records.
    [[maybe_unused]] [[nodiscard]] const std::vector<il::support::Diagnostic> &
    diagnostics() const noexcept;

  private:
    /// Plain error messages retained for inexpensive failure checks and clients.
    std::vector<std::string> errors_{};

    /// Plain warning messages retained for clients.
    std::vector<std::string> warnings_{};

    /// Structured source-order diagnostics used by @ref flush.
    std::vector<il::support::Diagnostic> diagnostics_{};
};

} // namespace zanna::codegen::common
