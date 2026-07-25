//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SelectModel.hpp
// Purpose: Define a normalized SELECT CASE model shared between parser and
//          lowering.
// Key invariants: Captures validated 32-bit ranges, labels, and relations,
//                 tracking CASE ELSE coverage without duplicating work across
//                 pipeline stages.
// Ownership/Lifetime: Views reference strings owned by the AST; consumers must
//                     keep the originating AST alive while using the model.
// Links: docs/internals/codemap.md, docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

#pragma once

#include "support/source_location.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace il::frontends::basic {

/// @brief Parsed explicit CASE arm consumed by @ref SelectModelBuilder.
struct CaseArm;

/// @brief Parsed SELECT CASE statement consumed by @ref SelectModelBuilder.
struct SelectCaseStmt;

/// @file SelectModel.hpp
/// @brief Declares the flattened dispatch model shared by BASIC SELECT stages.
/// @details The model preserves AST arm order while separating discrete numeric
///          labels, numeric ranges, numeric relations, and string labels into
///          lowering-friendly vectors. Numeric values are restricted to signed
///          32-bit storage; string values remain non-owning views into the AST.

/// @brief Flattened, lowering-oriented representation of `SELECT CASE` data.
/// @details Entries retain their zero-based AST arm index and source location.
///          The vectors do not imply deduplication, sorting, overlap checking,
///          or range-endpoint reordering.
struct SelectModel {
    /// @brief One discrete signed 32-bit CASE label.
    struct NumericLabel {
        /// @brief Label value after successful signed 32-bit narrowing.
        int32_t value = 0;
        /// @brief Index of the owning arm in `SelectCaseStmt::arms`.
        size_t armIndex = 0;
        /// @brief Beginning of the owning arm, used for emitted instructions
        ///        and diagnostics.
        il::support::SourceLoc loc{};
    };

    /// @brief One inclusive numeric `low TO high` CASE range.
    struct NumericRange {
        /// @brief Parsed lower endpoint after signed 32-bit narrowing.
        int32_t lo = 0;
        /// @brief Parsed upper endpoint after signed 32-bit narrowing.
        int32_t hi = 0;
        /// @brief Index of the owning arm in `SelectCaseStmt::arms`.
        size_t armIndex = 0;
        /// @brief Beginning of the owning arm.
        il::support::SourceLoc loc{};
    };

    /// @brief One numeric `CASE IS <op> rhs` predicate.
    struct NumericRelation {
        /// @brief Relational comparison applied between selector and RHS.
        enum class Op {
            /// @brief Match when selector is less than the RHS.
            LT,
            /// @brief Match when selector is less than or equal to the RHS.
            LE,
            /// @brief Match when selector equals the RHS.
            EQ,
            /// @brief Match when selector is greater than or equal to the RHS.
            GE,
            /// @brief Match when selector is greater than the RHS.
            GT,
        };

        /// @brief Predicate operation.
        Op op = Op::EQ;
        /// @brief Right-hand operand after signed 32-bit narrowing.
        int32_t rhs = 0;
        /// @brief Index of the owning arm in `SelectCaseStmt::arms`.
        size_t armIndex = 0;
        /// @brief Beginning of the owning arm.
        il::support::SourceLoc loc{};
    };

    /// @brief One string literal label and its destination arm.
    struct StringLabel {
        /// @brief Non-owning view of text stored by the parsed AST.
        std::string_view value{};
        /// @brief Index of the owning arm in `SelectCaseStmt::arms`.
        size_t armIndex = 0;
        /// @brief Beginning of the owning arm.
        il::support::SourceLoc loc{};
    };

    /// @brief Discrete numeric labels in AST traversal order.
    std::vector<NumericLabel> numericLabels;
    /// @brief Inclusive numeric ranges in AST traversal order.
    std::vector<NumericRange> numericRanges;
    /// @brief Numeric relations in AST traversal order.
    std::vector<NumericRelation> numericRelations;
    /// @brief String labels in AST traversal order.
    std::vector<StringLabel> stringLabels;
    /// @brief Whether the parsed statement supplied a non-empty else body.
    bool hasCaseElse = false;
    /// @brief Whether at least one fully representable numeric range was added.
    bool hasNumericRanges = false;
};

/// @brief Builds a flattened SELECT dispatch model from parsed AST data.
/// @details The builder owns only a diagnostic callback. Every call to
///          @ref build creates an independent model and may report signed
///          32-bit narrowing failures without aborting the remaining traversal.
class SelectModelBuilder {
  public:
    /// @brief Signature used to report a model-building diagnostic.
    /// @details Arguments are source location, highlighted length, message, and
    ///          stable diagnostic code, respectively.
    using DiagnoseFn =
        std::function<void(il::support::SourceLoc, uint32_t, std::string_view, std::string_view)>;

    /// @brief Construct a builder that reports errors through the supplied callback.
    /// @details Stores @p diagnose so out-of-range numeric operands can use the
    ///          caller's diagnostic path. An empty callback suppresses those
    ///          diagnostics while invalid entries are still omitted.
    /// @param diagnose Callback retained by value and invoked synchronously by
    ///                 later calls to @ref build.
    explicit SelectModelBuilder(DiagnoseFn diagnose) noexcept;

    /// @brief Creates a flattened model for one SELECT CASE statement.
    /// @details Reserves exact upper bounds, then copies string labels and
    ///          narrows each numeric label, endpoint, and relation RHS. A range
    ///          is omitted if either endpoint is invalid; other entries remain.
    ///          No sorting, deduplication, overlap analysis, or endpoint
    ///          canonicalization is performed.
    /// @param stmt Parsed statement borrowed for the duration of the call.
    /// @return A model whose string views remain valid only while the strings
    ///         in @p stmt remain alive and unmoved.
    [[nodiscard]] SelectModel build(const SelectCaseStmt &stmt);

  private:
    /// @brief Attempt to narrow a parsed integer literal into a 32-bit range.
    /// @details Compares @p value against @ref kCaseLabelMin and
    ///          @ref kCaseLabelMax. Failure invokes the callback, when present,
    ///          using `DiagSelectCaseLabelRange`, then returns no value.
    /// @param value Parsed operand before narrowing.
    /// @param loc Location associated with the owning arm.
    /// @return The exact signed 32-bit value, or @c std::nullopt when it lies
    ///         outside the representable interval.
    [[nodiscard]] std::optional<int32_t> narrowToI32(int64_t value,
                                                     il::support::SourceLoc loc) const;

    /// @brief Optional caller-owned diagnostic sink retained by value.
    DiagnoseFn diagnose_;
};

} // namespace il::frontends::basic
