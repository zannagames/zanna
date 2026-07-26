//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/diag_expected.hpp
// Purpose: Provides diagnostic helpers and a lightweight Expected container for CLI tools.
// Key invariants: A Diagnostic carries exactly one severity, message, and optional location; an
//                 Expected<T> holds either a value or an error Diag, never both and never neither.
// Ownership/Lifetime: Expected owns its stored value or diagnostic by value; the optional
//                     SourceManager passed to the print helpers is borrowed for the call only.
// Links: docs/internals/architecture.md, src/support/diagnostics.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares diagnostic-oriented expected values and text/JSON printers.
/// @details This header provides a compact success-or-diagnostic abstraction
///          used by tools that cannot yet rely on `std::expected`, together
///          with the canonical helpers for constructing and rendering support
///          diagnostics. Stored values and errors are owned; output streams and
///          optional source managers are borrowed only for each printing call.

#pragma once

#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

#include <cassert>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace il::support {
using Diag = Diagnostic;

/// @brief Tag type selecting the value constructor for Expected<Diag>.
struct SuccessDiagTag {
    explicit SuccessDiagTag() = default;
};

/// @brief Inline tag instance used to disambiguate success payload construction.
inline constexpr SuccessDiagTag kSuccessDiag{};

/// @brief Expected-style container pairing a value with a diagnostic on error.
/// @tparam T Stored value type when the operation succeeds.
/// @note Mirrors a subset of std::expected for tool-only usage until the
///       standard type becomes universally available on our toolchain.
template <class T> class [[nodiscard]] Expected {
  public:
    /// @brief Construct a successful result containing @p value.
    /// @param value Value produced by a successful computation.
    /// @details Enabled only when the provided value does not decay to Diag to
    ///          avoid colliding with the diagnostic constructor below.
    template <class U = T, class = std::enable_if_t<!std::is_same_v<std::decay_t<U>, Diag>>>
    Expected(U &&value) : value_(std::forward<U>(value)) {}

    /// @brief Construct an error result holding diagnostic @p diag.
    /// @param diag Diagnostic to return to the caller.
    Expected(Diag diag) : error_(std::move(diag)) {}

    /// @brief Construct a successful result containing a Diag payload.
    /// @param tag Tag used to disambiguate value-vs-error construction.
    /// @param value Diagnostic stored as the success payload.
    template <class U = T, std::enable_if_t<std::is_same_v<U, Diag>, int> = 0>
    Expected(SuccessDiagTag, Diag value) : value_(std::move(value)) {}

    /// @brief Create a successful Expected<Diag> from a diagnostic payload.
    /// @param tag Tag selecting the success overload.
    /// @param value Diagnostic stored as the success payload.
    template <class U = T, std::enable_if_t<std::is_same_v<U, Diag>, int> = 0>
    static Expected success(SuccessDiagTag tag, Diag value) {
        return Expected(tag, std::move(value));
    }

    /// @brief Check whether a value is present.
    /// @return True when the Expected stores a value.
    [[nodiscard]] bool hasValue() const noexcept {
        return value_.has_value();
    }

    /// @brief Allow use in boolean contexts to test success.
    /// @return True when a success value is present.
    explicit operator bool() const noexcept {
        return hasValue();
    }

    /// @brief Access the stored value; requires hasValue().
    /// @return Mutable reference to the stored success payload.
    T &value() & {
        assert(value_.has_value());
        return value_.value();
    }

    /// @brief Access the stored value; requires hasValue().
    /// @return Const reference to the stored success payload.
    const T &value() const & {
        assert(value_.has_value());
        return value_.value();
    }

    /// @brief Move-access the stored value; requires hasValue().
    /// @return Rvalue reference to the success payload.
    /// @details This overload lets callers consume successful Expected values
    ///          without an extra copy when the Expected itself is an rvalue.
    T &&value() && {
        assert(value_.has_value());
        return std::move(value_.value());
    }

    /// @brief Access the diagnostic describing the failure.
    /// @return Const reference to the stored failure diagnostic.
    const Diag &error() const & {
        assert(error_.has_value());
        return error_.value();
    }

    /// @brief Move-access the diagnostic describing the failure.
    /// @return Rvalue reference to the stored diagnostic payload.
    /// @details This overload supports efficient propagation from temporary
    ///          Expected values while preserving the lvalue accessor above for
    ///          ordinary inspection.
    Diag &&error() && {
        assert(error_.has_value());
        return std::move(error_.value());
    }

  private:
    std::optional<T> value_;
    std::optional<Diag> error_;
};

/// @brief Expected specialization for void success type.
template <> class [[nodiscard]] Expected<void> {
  public:
    /// @brief Construct a successful result with no payload.
    Expected() = default;

    /// @brief Construct an error result holding diagnostic @p diag.
    /// @param diag Diagnostic describing the failure.
    Expected(Diag diag);

    /// @brief Check whether the Expected represents success.
    /// @return True when no error diagnostic is stored.
    [[nodiscard]] bool hasValue() const noexcept;

    /// @brief Allow use in boolean contexts to test success.
    /// @return True when no error diagnostic is stored.
    explicit operator bool() const noexcept;

    /// @brief Access the diagnostic describing the failure.
    /// @return Const reference to the stored failure diagnostic.
    const Diag &error() const &;

    /// @brief Move-access the diagnostic describing the failure.
    /// @return Rvalue reference to the stored diagnostic payload.
    Diag &&error() &&;

  private:
    std::optional<Diag> error_;
};

namespace detail {
/// @brief Convert diagnostic severity to lowercase string.
/// @param severity Severity value to translate.
/// @return Null-terminated lowercase severity name, or `"unknown"`.
const char *diagSeverityToString(Severity severity);
} // namespace detail

/// @brief Create an error diagnostic with location and message.
/// @param loc Optional source location associated with the diagnostic.
/// @param msg Human-readable diagnostic message.
/// @return Diagnostic marked as an error severity.
Diag makeError(SourceLoc loc, std::string msg);

/// @brief Create an error diagnostic with location, code, and message.
/// @param loc Optional source location associated with the diagnostic.
/// @param code Diagnostic code (e.g., "B1001", "IL001").
/// @param msg Human-readable diagnostic message.
/// @return Diagnostic marked as an error severity with the given code.
Diag makeErrorWithCode(SourceLoc loc, std::string code, std::string msg);

/// @brief Print a single diagnostic to the provided stream.
/// @param diag Diagnostic to format.
/// @param os Output stream receiving the text.
/// @param sm Optional source manager to resolve file paths.
/// @note Follows DiagnosticEngine::printAll formatting for consistency.
void printDiag(const Diag &diag, std::ostream &os, const SourceManager *sm = nullptr);

/// @brief Print diagnostics as a compact JSON object.
/// @param diagnostics Diagnostics to encode under the "diagnostics" key.
/// @param os Output stream receiving JSON.
/// @param sm Optional source manager used to resolve file paths.
void printDiagnosticsJson(const std::vector<Diag> &diagnostics,
                          std::ostream &os,
                          const SourceManager *sm = nullptr);

/// @brief Print diagnostics as a compact JSON object from any contiguous span.
/// @param diagnostics Diagnostics to encode under the "diagnostics" key.
/// @param os Output stream receiving JSON.
/// @param sm Optional source manager used to resolve file paths.
/// @details This overload avoids forcing callers that already hold an array,
///          SmallVector-backed span, or other contiguous view to first materialize
///          a std::vector solely for serialization.
void printDiagnosticsJson(std::span<const Diag> diagnostics,
                          std::ostream &os,
                          const SourceManager *sm = nullptr);

/// @brief Print one diagnostic as a JSON diagnostics object.
/// @param diag Diagnostic to encode.
/// @param os Output stream receiving JSON.
/// @param sm Optional source manager used to resolve file paths.
void printDiagJson(const Diag &diag, std::ostream &os, const SourceManager *sm = nullptr);

/// @brief Emit @p text as a JSON-escaped, double-quoted string literal.
/// @details Shared by tools that hand-build small JSON documents (eval,
///          explain, runtime API dumps) so escaping rules stay in one place.
/// @param os Output stream receiving the quoted literal.
/// @param text Raw text to escape and quote.
void printJsonStringEscaped(std::ostream &os, std::string_view text);
} // namespace il::support
