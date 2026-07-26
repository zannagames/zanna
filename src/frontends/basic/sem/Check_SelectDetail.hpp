//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/Check_SelectDetail.hpp
// Purpose: Define the state and inline validation helpers used to check BASIC
//          SELECT CASE selectors, labels, ranges, and relational arms.
// Key invariants:
//   * Numeric labels are restricted to signed 32-bit values before conversion.
//   * Exact labels, closed ranges, and relational intervals share collision
//     tracking so overlapping arms are diagnosed consistently.
//   * Numeric and string arm labels cannot be mixed, and CASE ELSE can occur at
//     most once.
// Ownership: Validation contexts borrow the analyzer's diagnostic sink; arm
//            labels and interval records are owned by the context for one
//            SELECT CASE analysis.
// References: docs/tutorials/basic-tutorial.md#select-case,
//             docs/internals/codemap/basic.md#semantic-analyzer
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Internal helpers for SELECT CASE semantic checking.
/// @details Defines context objects and routines shared between exported
///          dispatcher functions, keeping the translation unit focused on
///          orchestration.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/sem/Check_Common.hpp"

#include "frontends/basic/BasicDiagnosticMessages.hpp"
#include "frontends/basic/SelectCaseRange.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

namespace il::frontends::basic {

/// @brief Holds the classification result of a SELECT CASE selector expression.
/// @details After evaluating the selector expression's type, this struct records
///          whether the selector is string-typed or numeric-typed, and whether a
///          fatal type error was detected (e.g. selector is Bool or Unknown).
struct SemanticAnalyzer::SelectCaseSelectorInfo {
    bool selectorIsString = false;  ///< True if the selector evaluates to a String type.
    bool selectorIsNumeric = false; ///< True if the selector evaluates to an Int type.
    bool fatal = false;             ///< True if the selector type is unsupported (error emitted).
};

/// @brief Mutable context accumulated while validating CASE arms within a SELECT CASE.
/// @details Tracks which label values and ranges have been seen so far, enabling
///          detection of duplicate labels, overlapping ranges, multiple CASE ELSE
///          clauses, and mixed numeric/string label types across arms.
struct SemanticAnalyzer::SelectCaseArmContext {
    /// @brief Classification of label types seen in CASE arms.
    enum class LabelKind {
        None,    ///< No labels seen yet.
        Numeric, ///< Numeric integer labels (CASE 1, CASE 1 TO 5, CASE IS > 3).
        String,  ///< String literals (CASE "hello").
    };

    /// @brief Represents a half-open or closed interval derived from a relational CASE label.
    /// @details Used to detect overlapping relational conditions like CASE IS > 5
    ///          and CASE IS < 10 which together cover all values.
    struct RelInterval {
        bool hasLo = false; ///< True if the interval has a finite lower bound.
        int64_t lo = 0;     ///< Lower bound value (inclusive), valid only if hasLo is true.
        bool hasHi = false; ///< True if the interval has a finite upper bound.
        int64_t hi = 0;     ///< Upper bound value (inclusive), valid only if hasHi is true.
    };

    /// @brief Construct an arm context for validating CASE labels.
    /// @param diagnostics Diagnostic sink for emitting overlap and duplicate errors.
    /// @param selectorIsStringIn True if the SELECT selector is string-typed.
    /// @param selectorIsNumericIn True if the SELECT selector is numeric-typed.
    /// @param hasElseBody True if the SELECT CASE has a CASE ELSE clause in the AST.
    SelectCaseArmContext(SemanticDiagnostics &diagnostics,
                         bool selectorIsStringIn,
                         bool selectorIsNumericIn,
                         bool hasElseBody) noexcept
        : de(diagnostics), selectorIsString(selectorIsStringIn),
          selectorIsNumeric(selectorIsNumericIn), caseElseCount(hasElseBody ? 1 : 0) {}

    SemanticDiagnostics &de;                      ///< Diagnostic sink for error reporting.
    bool selectorIsString = false;                ///< True if selector type is String.
    bool selectorIsNumeric = false;               ///< True if selector type is Int.
    int caseElseCount = 0;                        ///< Number of CASE ELSE arms encountered.
    LabelKind seenArmLabelKind = LabelKind::None; ///< First label kind seen across all arms.
    bool reportedMixedLabelTypes = false;   ///< True if mixed-type diagnostic was already emitted.
    std::unordered_set<int32_t> seenLabels; ///< Set of exact integer labels seen.
    std::vector<std::pair<int32_t, int32_t>>
        seenRanges;                            ///< List of (lo,hi) ranges from CASE lo TO hi.
    std::vector<RelInterval> seenRelIntervals; ///< List of intervals from CASE IS <op> <val>.
    std::unordered_set<std::string> seenStringLabels; ///< Set of string literals seen in CASE arms.
};

} // namespace il::frontends::basic

namespace il::frontends::basic::sem::detail {
namespace {
using il::frontends::basic::kCaseLabelMax;
using il::frontends::basic::kCaseLabelMin;

using ArmContext = SemanticAnalyzer::SelectCaseArmContext;
using LabelKind = SemanticAnalyzer::SelectCaseArmContext::LabelKind;
using RelInterval = SemanticAnalyzer::SelectCaseArmContext::RelInterval;

/// @brief Determine whether a CASE arm is a CASE ELSE (no explicit labels).
/// @details A CASE ELSE arm has no integer labels, no ranges, no relational
///          comparisons, and no string labels — it matches everything not
///          covered by other arms.
/// @param arm The CASE arm to inspect.
/// @return True if the arm has no labels of any kind (i.e. it is CASE ELSE).
inline bool isCaseElseArm(const CaseArm &arm) {
    return arm.labels.empty() && arm.ranges.empty() && arm.rels.empty() && arm.str_labels.empty();
}

/// @brief Construct a closed RelInterval from explicit lower and upper bounds.
/// @param lo The inclusive lower bound of the range.
/// @param hi The inclusive upper bound of the range.
/// @return A RelInterval representing [lo, hi].
inline RelInterval makeRangeInterval(int32_t lo, int32_t hi) {
    RelInterval interval;
    interval.hasLo = true;
    interval.lo = static_cast<int64_t>(lo);
    interval.hasHi = true;
    interval.hi = static_cast<int64_t>(hi);
    return interval;
}

/// @brief Construct a RelInterval from a relational CASE IS operator and value.
/// @details Maps relational operators to half-open or closed intervals:
///          LT → (-inf, rhs-1], LE → (-inf, rhs], EQ → [rhs, rhs],
///          GE → [rhs, +inf), GT → [rhs+1, +inf).
/// @param op The relational comparison operator (LT, LE, EQ, GE, GT).
/// @param rhs The integer value on the right-hand side of the comparison.
/// @return A RelInterval representing the set of values matching the condition.
inline RelInterval makeRelInterval(CaseArm::CaseRel::Op op, int32_t rhs) {
    RelInterval interval;
    switch (op) {
        case CaseArm::CaseRel::Op::LT:
            interval.hasHi = true;
            interval.hi = static_cast<int64_t>(rhs) - 1;
            break;
        case CaseArm::CaseRel::Op::LE:
            interval.hasHi = true;
            interval.hi = static_cast<int64_t>(rhs);
            break;
        case CaseArm::CaseRel::Op::EQ:
            interval.hasLo = true;
            interval.lo = static_cast<int64_t>(rhs);
            interval.hasHi = true;
            interval.hi = static_cast<int64_t>(rhs);
            break;
        case CaseArm::CaseRel::Op::GE:
            interval.hasLo = true;
            interval.lo = static_cast<int64_t>(rhs);
            break;
        case CaseArm::CaseRel::Op::GT:
            interval.hasLo = true;
            interval.lo = static_cast<int64_t>(rhs) + 1;
            break;
    }
    return interval;
}

/// @brief Test whether two RelIntervals have any values in common.
/// @details Computes the intersection of both intervals and returns true if
///          the intersection is non-empty. Unbounded sides are treated as
///          ±infinity for the purposes of overlap testing.
/// @param lhs The first interval to test.
/// @param rhs The second interval to test.
/// @return True if the intervals share at least one integer value.
inline bool intervalsOverlap(const RelInterval &lhs, const RelInterval &rhs) {
    const int64_t lo = std::max(lhs.hasLo ? lhs.lo : std::numeric_limits<int64_t>::min(),
                                rhs.hasLo ? rhs.lo : std::numeric_limits<int64_t>::min());
    const int64_t hi = std::min(lhs.hasHi ? lhs.hi : std::numeric_limits<int64_t>::max(),
                                rhs.hasHi ? rhs.hi : std::numeric_limits<int64_t>::max());
    return lo <= hi;
}

/// @brief Test whether an interval contains a specific integer value.
/// @param interval The interval to test against.
/// @param value The integer value to check for membership.
/// @return True if the value falls within the interval's bounds.
inline bool intervalContains(const RelInterval &interval, int32_t value) {
    if (interval.hasLo && static_cast<int64_t>(value) < interval.lo)
        return false;
    if (interval.hasHi && static_cast<int64_t>(value) > interval.hi)
        return false;
    return true;
}

/// @brief Emit an overlapping-range diagnostic for a CASE arm.
/// @param ctx The arm validation context containing the diagnostic sink.
/// @param arm The CASE arm where the overlap was detected.
inline void emitOverlap(ArmContext &ctx, const CaseArm &arm) {
    std::string msg(diag::ERR_SelectCase_OverlappingRange.text);
    ctx.de.emit(il::support::Severity::Error,
                std::string(diag::ERR_SelectCase_OverlappingRange.id),
                arm.range.begin,
                1,
                std::move(msg));
}

/// @brief Check a new interval against all previously seen labels, ranges, and intervals.
/// @details Tests for overlap against the accumulated seenRanges, seenLabels, and
///          seenRelIntervals. Emits an overlap diagnostic on the first collision found.
/// @param ctx The arm validation context with accumulated label/range history.
/// @param arm The CASE arm being validated (used for source location in diagnostics).
/// @param interval The new interval to check for collisions.
/// @return True if a collision was detected (diagnostic already emitted).
inline bool checkIntervalCollision(ArmContext &ctx,
                                   const CaseArm &arm,
                                   const RelInterval &interval) {
    /// @brief Tests whether a prior explicit range overlaps the new interval.
    /// @param seen Previously recorded inclusive range.
    /// @return `true` when the ranges overlap.
    const auto collidesRange =
        std::any_of(ctx.seenRanges.begin(), ctx.seenRanges.end(), [&](const auto &seen) {
            return intervalsOverlap(interval, makeRangeInterval(seen.first, seen.second));
        });
    if (collidesRange) {
        emitOverlap(ctx, arm);
        return true;
    }

    /// @brief Tests whether a prior scalar label lies in the new interval.
    /// @param label Previously recorded label.
    /// @return `true` when `interval` contains `label`.
    const auto collidesLabel =
        std::any_of(ctx.seenLabels.begin(), ctx.seenLabels.end(), [&](int32_t label) {
            return intervalContains(interval, label);
        });
    if (collidesLabel) {
        emitOverlap(ctx, arm);
        return true;
    }

    /// @brief Tests whether a prior relational interval overlaps the new interval.
    /// @param seen Previously recorded relational interval.
    /// @return `true` when the intervals overlap.
    const auto collidesRel =
        std::any_of(ctx.seenRelIntervals.begin(),
                    ctx.seenRelIntervals.end(),
                    [&](const RelInterval &seen) { return intervalsOverlap(interval, seen); });
    if (collidesRel) {
        emitOverlap(ctx, arm);
        return true;
    }

    return false;
}

/// @brief Record a CASE ELSE arm and report whether it is valid.
/// @details Increments the CASE ELSE counter and emits a "duplicate CASE ELSE"
///          diagnostic if more than one CASE ELSE arm has been seen.
/// @param ctx The arm validation context tracking the CASE ELSE count.
/// @param arm The CASE arm identified as CASE ELSE (for source location).
/// @return True when this is the first CASE ELSE arm, false if duplicated.
inline bool noteCaseElse(ArmContext &ctx, const CaseArm &arm) {
    ++ctx.caseElseCount;
    if (ctx.caseElseCount <= 1)
        return true;

    std::string msg(diag::ERR_SelectCase_DuplicateElse.text);
    ctx.de.emit(il::support::Severity::Error,
                std::string(diag::ERR_SelectCase_DuplicateElse.id),
                arm.range.begin,
                1,
                std::move(msg));
    return false;
}

/// @brief Emit a diagnostic when CASE arms mix numeric and string labels.
/// @details Only emits the diagnostic once; subsequent calls are suppressed
///          via the reportedMixedLabelTypes flag.
/// @param ctx The arm validation context.
/// @param arm The CASE arm where the mixed types were detected.
/// @return Always false so callers can fold it into their validation state.
inline bool reportMixedLabelTypes(ArmContext &ctx, const CaseArm &arm) {
    if (ctx.reportedMixedLabelTypes)
        return false;

    std::string msg(diag::ERR_SelectCase_MixedLabelTypes.text);
    ctx.de.emit(il::support::Severity::Error,
                std::string(diag::ERR_SelectCase_MixedLabelTypes.id),
                arm.range.begin,
                1,
                std::move(msg));
    ctx.reportedMixedLabelTypes = true;
    return false;
}

/// @brief Track the label kind of the current arm and detect mixed types.
/// @details On the first arm with labels, records the kind. On subsequent arms,
///          compares against the recorded kind and reports mixed types if different.
/// @param ctx The arm validation context.
/// @param kind The label kind of the current arm (Numeric or String).
/// @param arm The CASE arm being validated (for diagnostic source location).
/// @return True when the arm kind is compatible with previous arms.
inline bool trackArmLabelKind(ArmContext &ctx, LabelKind kind, const CaseArm &arm) {
    if (kind == LabelKind::None || ctx.reportedMixedLabelTypes)
        return kind == LabelKind::None;
    if (ctx.seenArmLabelKind == LabelKind::None) {
        ctx.seenArmLabelKind = kind;
        return true;
    }
    if (ctx.seenArmLabelKind != kind)
        return reportMixedLabelTypes(ctx, arm);
    return true;
}

/// @brief Emit an out-of-32-bit-range error for a CASE range bound.
/// @param ctx The arm validation context containing the diagnostic sink.
/// @param arm The CASE arm with the out-of-range bound (for source location).
/// @param which Either "lower" or "upper", identifying which bound is invalid.
/// @param value The 64-bit value that exceeds the 32-bit signed range.
inline void emitRangeBoundError(ArmContext &ctx,
                                const CaseArm &arm,
                                const char *which,
                                int64_t value) {
    std::string msg = "CASE range ";
    msg += which;
    msg += " bound ";
    msg += std::to_string(value);
    msg += " is outside 32-bit signed range";
    ctx.de.emit(il::support::Severity::Error,
                std::string(SemanticAnalyzer::DiagSelectCaseLabelRange),
                arm.range.begin,
                1,
                std::move(msg));
}

/// @brief Validate that a CASE range's lower and upper bounds are within 32-bit range.
/// @details Also checks that lo <= hi and emits appropriate diagnostics on failure.
/// @param ctx The arm validation context containing the diagnostic sink.
/// @param arm The CASE arm being validated (for source location).
/// @param rawLo The raw 64-bit lower bound from the parser.
/// @param rawHi The raw 64-bit upper bound from the parser.
/// @return True if both bounds are valid and lo <= hi.
inline bool validateRangeBounds(ArmContext &ctx, const CaseArm &arm, int64_t rawLo, int64_t rawHi) {
    bool valid = true;
    if (rawLo < kCaseLabelMin || rawLo > kCaseLabelMax) {
        emitRangeBoundError(ctx, arm, "lower", rawLo);
        valid = false;
    }
    if (rawHi < kCaseLabelMin || rawHi > kCaseLabelMax) {
        emitRangeBoundError(ctx, arm, "upper", rawHi);
        valid = false;
    }
    if (rawLo > rawHi) {
        std::string msg(diag::ERR_SelectCase_InvalidRange.text);
        ctx.de.emit(il::support::Severity::Error,
                    std::string(diag::ERR_SelectCase_InvalidRange.id),
                    arm.range.begin,
                    1,
                    std::move(msg));
        valid = false;
    }
    return valid;
}

/// @brief Emit an out-of-range diagnostic for a CASE label value if it exceeds 32-bit range.
/// @param arm The CASE arm containing the label (for source location).
/// @param ctx The arm validation context with the diagnostic sink.
/// @param raw The raw 64-bit label value to check.
/// @return True if the value was out of range (diagnostic was emitted).
inline bool emitOutOfRangeLabel(const CaseArm &arm, ArmContext &ctx, int64_t raw) {
    if (raw >= kCaseLabelMin && raw <= kCaseLabelMax)
        return false;

    std::string msg = il::frontends::basic::makeSelectCaseLabelRangeMessage(raw);
    ctx.de.emit(il::support::Severity::Error,
                std::string(SemanticAnalyzer::DiagSelectCaseLabelRange),
                arm.range.begin,
                1,
                std::move(msg));
    return true;
}

/// @brief Emit a duplicate-label diagnostic for a CASE label.
/// @param ctx The arm validation context with the diagnostic sink.
/// @param arm The CASE arm containing the duplicate (for source location).
/// @param label Human-readable representation of the duplicate label value.
inline void emitDuplicateLabel(ArmContext &ctx, const CaseArm &arm, std::string label) {
    std::string msg(diag::ERR_SelectCase_DuplicateLabel.text);
    msg += ": ";
    msg += std::move(label);
    ctx.de.emit(il::support::Severity::Error,
                std::string(diag::ERR_SelectCase_DuplicateLabel.id),
                arm.range.begin,
                1,
                std::move(msg));
}

} // namespace

/// @brief Classify the SELECT CASE selector expression as numeric, string, or invalid.
/// @details Evaluates the selector expression type and fills out the
///          SelectCaseSelectorInfo struct. Emits a diagnostic if the selector
///          type is not Int or String (e.g. Bool or Float without implicit conversion).
/// @param context The control-flow check context providing evaluation and diagnostics.
/// @param stmt The SELECT CASE statement containing the selector expression.
/// @return A SelectCaseSelectorInfo describing the selector classification.
inline SemanticAnalyzer::SelectCaseSelectorInfo classifySelectCaseSelector(
    ControlCheckContext &context, const SelectCaseStmt &stmt) {
    SemanticAnalyzer::SelectCaseSelectorInfo info;
    if (!stmt.selector)
        return info;

    using Type = SemanticAnalyzer::Type;
    const Type selectorType = context.evaluateExpr(*stmt.selector);
    if (selectorType == Type::Int) {
        context.markImplicitConversion(*stmt.selector, Type::Int);
        info.selectorIsNumeric = true;
        return info;
    }
    if (selectorType == Type::String) {
        info.selectorIsString = true;
        return info;
    }
    if (selectorType == Type::Unknown)
        return info;

    std::string msg(diag::ERR_SelectCase_NonIntegerSelector.text);
    context.diagnostics().emit(il::support::Severity::Error,
                               std::string(diag::ERR_SelectCase_NonIntegerSelector.id),
                               stmt.selector->loc,
                               1,
                               std::move(msg));
    info.fatal = true;
    return info;
}

/// @brief Validate string labels in a CASE arm against the selector type.
/// @details Emits an error if the selector is numeric but the arm uses string
///          labels. Tracks label-kind consistency and detects duplicate string values.
/// @param arm The CASE arm containing string labels to validate.
/// @param ctx The arm validation context tracking seen labels and types.
/// @return True when no errors were emitted for this arm.
inline bool validateSelectCaseStringArm(const CaseArm &arm, ArmContext &ctx) {
    bool ok = true;
    if (ctx.selectorIsNumeric) {
        std::string msg(diag::ERR_SelectCase_StringLabelSelector.text);
        ctx.de.emit(il::support::Severity::Error,
                    std::string(diag::ERR_SelectCase_StringLabelSelector.id),
                    arm.range.begin,
                    1,
                    std::move(msg));
        ok = false;
    }

    ok = trackArmLabelKind(ctx, LabelKind::String, arm) && ok;
    for (const auto &label : arm.str_labels) {
        if (ctx.seenStringLabels.insert(label).second)
            continue;
        emitDuplicateLabel(ctx, arm, '"' + label + '"');
        ok = false;
    }
    return ok;
}

/// @brief Validate numeric labels, ranges, and relational conditions in a CASE arm.
/// @details Emits errors for: string selector with numeric labels, out-of-range
///          values, overlapping ranges, duplicate exact labels, and colliding
///          relational intervals. Updates the accumulated label/range state in ctx.
/// @param arm The CASE arm containing numeric labels to validate.
/// @param ctx The arm validation context tracking seen labels, ranges, and intervals.
/// @return True when no errors were emitted for this arm.
inline bool validateSelectCaseNumericArm(const CaseArm &arm, ArmContext &ctx) {
    bool ok = true;
    if (ctx.selectorIsString) {
        std::string msg(diag::ERR_SelectCase_StringSelectorLabels.text);
        ctx.de.emit(il::support::Severity::Error,
                    std::string(diag::ERR_SelectCase_StringSelectorLabels.id),
                    arm.range.begin,
                    1,
                    std::move(msg));
        ok = false;
    }

    ok = trackArmLabelKind(ctx, LabelKind::Numeric, arm) && ok;

    for (const auto &[rawLo, rawHi] : arm.ranges) {
        if (!validateRangeBounds(ctx, arm, rawLo, rawHi)) {
            ok = false;
            continue;
        }

        const auto interval =
            makeRangeInterval(static_cast<int32_t>(rawLo), static_cast<int32_t>(rawHi));
        if (checkIntervalCollision(ctx, arm, interval)) {
            ok = false;
            continue;
        }

        ctx.seenRanges.emplace_back(static_cast<int32_t>(rawLo), static_cast<int32_t>(rawHi));
    }

    for (int64_t rawLabel : arm.labels) {
        if (emitOutOfRangeLabel(arm, ctx, rawLabel)) {
            ok = false;
            continue;
        }

        const int32_t label = static_cast<int32_t>(rawLabel);
        if (auto [_, inserted] = ctx.seenLabels.insert(label); !inserted) {
            emitDuplicateLabel(ctx, arm, std::to_string(label));
            ok = false;
        }
    }

    for (const auto &rel : arm.rels) {
        if (emitOutOfRangeLabel(arm, ctx, rel.rhs)) {
            ok = false;
            continue;
        }

        const int32_t rhs = static_cast<int32_t>(rel.rhs);
        const auto interval = makeRelInterval(rel.op, rhs);
        if (checkIntervalCollision(ctx, arm, interval)) {
            ok = false;
            continue;
        }

        if (rel.op == CaseArm::CaseRel::Op::EQ) {
            if (auto [_, inserted] = ctx.seenLabels.insert(rhs); !inserted) {
                emitDuplicateLabel(ctx, arm, std::to_string(rel.rhs));
                ok = false;
            }
            continue;
        }

        ctx.seenRelIntervals.push_back(interval);
    }

    return ok;
}

/// @brief Validate a complete CASE arm (CASE ELSE, string, numeric, or mixed).
/// @details Dispatches to the appropriate validation function based on whether
///          the arm is a CASE ELSE, has string labels, or has numeric labels.
///          Reports mixed label types if an arm contains both string and numeric labels.
/// @param arm The CASE arm to validate.
/// @param ctx The arm validation context tracking cumulative label state.
/// @return True when no validation errors were emitted for the arm.
inline bool validateSelectCaseArm(const CaseArm &arm, ArmContext &ctx) {
    if (isCaseElseArm(arm)) {
        return noteCaseElse(ctx, arm);
    }

    const bool hasString = !arm.str_labels.empty();
    const bool hasNumeric = !arm.labels.empty() || !arm.ranges.empty() || !arm.rels.empty();
    bool ok = true;
    if (hasString && hasNumeric) {
        reportMixedLabelTypes(ctx, arm);
        ok = false;
    }

    if (hasString)
        ok = validateSelectCaseStringArm(arm, ctx) && ok;
    if (hasNumeric)
        ok = validateSelectCaseNumericArm(arm, ctx) && ok;
    return ok;
}

/// @brief Analyze the statement body of a single CASE arm within its own scope.
/// @details Opens a new lexical scope, visits each child statement for semantic
///          analysis, and closes the scope on return.
/// @param context The control-flow check context for scope and statement analysis.
/// @param body The list of statements within the CASE arm body.
inline void analyzeSelectCaseBody(ControlCheckContext &context, const std::vector<StmtPtr> &body) {
    auto scope = context.pushScope();
    for (const auto &child : body) {
        if (!child)
            continue;
        context.visitStmt(*child);
    }
}

} // namespace il::frontends::basic::sem::detail
