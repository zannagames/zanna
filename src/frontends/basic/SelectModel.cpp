//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SelectModel.cpp
// Purpose: Implement the shared SELECT CASE model builder.
// Key invariants: Normalises label data into 32-bit ranges and relations while
//                 surfacing diagnostics through the supplied callback.
// Ownership/Lifetime: Operates on AST references without taking ownership.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SelectModel.cpp
/// @brief Implements conversion from parsed SELECT CASE arms to SelectModel.
/// @details Model construction is intentionally mechanical: it pre-reserves
///          storage, retains AST order and arm indices, and excludes only
///          numeric entries that cannot be represented as signed 32-bit values.

#include "frontends/basic/SelectModel.hpp"

#include "frontends/basic/SelectCaseRange.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/ast/StmtControl.hpp"

#include <utility>

namespace il::frontends::basic {

/// @brief Stores the diagnostic sink used by subsequent model builds.
/// @param diagnose Callback invoked for out-of-range numeric operands. It may
///                 be empty to suppress emission.
/// @post The callback has been moved into this builder; no AST is inspected.
SelectModelBuilder::SelectModelBuilder(DiagnoseFn diagnose) noexcept
    : diagnose_(std::move(diagnose)) {}

/// @brief Narrows one parsed numeric operand to SELECT's 32-bit model type.
/// @details Values outside the inclusive interval
///          [@ref kCaseLabelMin, @ref kCaseLabelMax] are rejected. When a
///          callback exists, rejection emits `DiagSelectCaseLabelRange` with a
///          highlight length of one and the shared range-message text.
/// @param value Parsed signed 64-bit operand.
/// @param loc Beginning of the owning CASE arm.
/// @return The exact @c int32_t conversion on success, or @c std::nullopt when
///         @p value is outside the representable interval.
std::optional<int32_t> SelectModelBuilder::narrowToI32(int64_t value,
                                                       il::support::SourceLoc loc) const {
    if (value < kCaseLabelMin || value > kCaseLabelMax) {
        if (diagnose_) {
            diagnose_(loc,
                      1,
                      makeSelectCaseLabelRangeMessage(value),
                      SemanticAnalyzer::DiagSelectCaseLabelRange);
        }
        return std::nullopt;
    }
    return static_cast<int32_t>(value);
}

/// @brief Flattens one parsed SELECT CASE statement into dispatch vectors.
/// @details Marks CASE ELSE only when `elseBody` is non-empty, computes reserve
///          capacities from all arms, and traverses arms in source order.
///          Strings become non-owning views. Discrete labels and relation RHS
///          values are added only after successful narrowing. A range is added
///          only when both endpoints narrow successfully; its original endpoint
///          order is preserved. Relation operators are translated one-for-one
///          to the model enumeration. No sorting, duplicate detection, overlap
///          checking, or range canonicalization occurs.
/// @param stmt Parsed AST borrowed for the duration of this call.
/// @return Independent numeric vectors plus string views into @p stmt. The
///         caller must keep the AST's strings alive and unmoved while using the
///         returned model.
SelectModel SelectModelBuilder::build(const SelectCaseStmt &stmt) {
    SelectModel model{};
    model.hasCaseElse = !stmt.elseBody.empty();

    size_t strCount = 0;
    size_t labelCount = 0;
    size_t rangeCount = 0;
    size_t relCount = 0;
    for (const auto &arm : stmt.arms) {
        strCount += arm.str_labels.size();
        labelCount += arm.labels.size();
        rangeCount += arm.ranges.size();
        relCount += arm.rels.size();
    }

    model.stringLabels.reserve(strCount);
    model.numericLabels.reserve(labelCount);
    model.numericRanges.reserve(rangeCount);
    model.numericRelations.reserve(relCount);

    for (size_t index = 0; index < stmt.arms.size(); ++index) {
        const CaseArm &arm = stmt.arms[index];
        const il::support::SourceLoc loc = arm.range.begin;

        for (const auto &str : arm.str_labels) {
            model.stringLabels.push_back({std::string_view(str), index, loc});
        }

        for (int64_t rawLabel : arm.labels) {
            if (auto narrowed = narrowToI32(rawLabel, loc)) {
                model.numericLabels.push_back({*narrowed, index, loc});
            }
        }

        for (const auto &[rawLo, rawHi] : arm.ranges) {
            auto narrowedLo = narrowToI32(rawLo, loc);
            auto narrowedHi = narrowToI32(rawHi, loc);
            if (!narrowedLo || !narrowedHi)
                continue;

            model.numericRanges.push_back({*narrowedLo, *narrowedHi, index, loc});
            model.hasNumericRanges = true;
        }

        for (const auto &rel : arm.rels) {
            if (auto narrowed = narrowToI32(rel.rhs, loc)) {
                SelectModel::NumericRelation entry{};
                entry.armIndex = index;
                entry.loc = loc;
                entry.rhs = *narrowed;
                switch (rel.op) {
                    case CaseArm::CaseRel::Op::LT:
                        entry.op = SelectModel::NumericRelation::Op::LT;
                        break;
                    case CaseArm::CaseRel::Op::LE:
                        entry.op = SelectModel::NumericRelation::Op::LE;
                        break;
                    case CaseArm::CaseRel::Op::EQ:
                        entry.op = SelectModel::NumericRelation::Op::EQ;
                        break;
                    case CaseArm::CaseRel::Op::GE:
                        entry.op = SelectModel::NumericRelation::Op::GE;
                        break;
                    case CaseArm::CaseRel::Op::GT:
                        entry.op = SelectModel::NumericRelation::Op::GT;
                        break;
                }
                model.numericRelations.push_back(entry);
            }
        }
    }

    return model;
}

} // namespace il::frontends::basic
