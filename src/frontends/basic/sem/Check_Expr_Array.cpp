//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/Check_Expr_Array.cpp
// Purpose: Validate BASIC array access expressions and infer their element type
//          during semantic analysis.
// Key invariants:
//   * Array references are resolved against the symbol table so undefined
//     arrays are detected early.
//   * Index expressions must be integers; float indices trigger warnings.
//   * Bounds checking is performed for constant indices when extents are known.
// References: docs/internals/codemap/basic.md, docs/tutorials/basic-tutorial.md#arrays
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Semantic analysis helper for array access expressions.
/// @details Resolves array symbols, validates indices, and performs static
///          bounds checking where possible.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/Diag.hpp"
#include "frontends/basic/sem/Check_Common.hpp"

#include <optional>

namespace il::frontends::basic::sem {

/// @brief Validate an array index expression and emit diagnostics as needed.
///
/// @param context Expression checking context.
/// @param indexExpr Index expression to validate.
/// @param arrayLoc Source location of the array access.
/// @return Type of the index expression.
static SemanticAnalyzer::Type validateArrayIndex(ExprCheckContext &context,
                                                 Expr &indexExpr,
                                                 il::support::SourceLoc arrayLoc) {
    using Type = SemanticAnalyzer::Type;

    Type ty = context.evaluate(indexExpr);
    if (ty == Type::Float) {
        if (as<FloatExpr>(indexExpr) != nullptr) {
            context.insertImplicitCast(indexExpr, Type::Int);
            std::string msg = "narrowing conversion from FLOAT to INT in array index";
            context.diagnostics().emit(
                il::support::Severity::Warning, "B2002", arrayLoc, 1, std::move(msg));
        } else {
            std::string msg = "index type mismatch";
            context.diagnostics().emit(
                il::support::Severity::Error, "B2001", arrayLoc, 1, std::move(msg));
        }
    } else if (ty != Type::Unknown && ty != Type::Int) {
        std::string msg = "index type mismatch";
        context.diagnostics().emit(
            il::support::Severity::Error, "B2001", arrayLoc, 1, std::move(msg));
    }
    return ty;
}

/// @brief Resolve a one-based LBOUND/UBOUND dimension argument against array metadata.
/// @details The parser stores the dimension as an expression so diagnostics can
///          be attached precisely. Semantic analysis requires a constant integer
///          dimension when the argument is supplied and records the zero-based
///          index on the bound expression for lowering.
/// @param context Expression checking context.
/// @param dimension Optional dimension expression from the AST.
/// @param metadata Array metadata containing declared extents when available.
/// @param loc Source location of the bound intrinsic.
/// @return Zero-based dimension index, or std::nullopt when invalid/unknown.
static std::optional<std::size_t> resolveBoundDimension(ExprCheckContext &context,
                                                        ExprPtr &dimension,
                                                        const ArrayMetadata *metadata,
                                                        il::support::SourceLoc loc) {
    if (!dimension)
        return std::size_t{0};

    const auto ty = context.evaluate(*dimension);
    if (ty != SemanticAnalyzer::Type::Unknown && ty != SemanticAnalyzer::Type::Int) {
        context.diagnostics().emit(
            il::support::Severity::Error, "B2001", loc, 1, "array bound dimension must be integer");
        return std::nullopt;
    }

    const auto *literal = as<IntExpr>(*dimension);
    if (!literal) {
        context.diagnostics().emit(il::support::Severity::Error,
                                   "B2001",
                                   loc,
                                   1,
                                   "array bound dimension must be a constant integer");
        return std::nullopt;
    }
    if (literal->value < 1) {
        context.diagnostics().emit(il::support::Severity::Error,
                                   "B2001",
                                   loc,
                                   1,
                                   "array bound dimension must be one-based");
        return std::nullopt;
    }

    const auto zeroBased = static_cast<std::size_t>(literal->value - 1);
    if (metadata && !metadata->extents.empty() && zeroBased >= metadata->extents.size()) {
        std::string msg = "array bound dimension out of range: expected 1 to " +
                          std::to_string(metadata->extents.size());
        context.diagnostics().emit(il::support::Severity::Error, "B2001", loc, 1, std::move(msg));
        return std::nullopt;
    }
    return zeroBased;
}

/// @brief Type-check a BASIC array access expression and compute its element type.
///
/// @details Validates that the symbol is a known array, checks index types,
///          and performs static bounds checking for constant indices.
///
/// @param analyzer Semantic analyzer coordinating the current compilation.
/// @param expr Array expression to validate.
/// @return Semantic type of the array element (or Unknown if errors occurred).
SemanticAnalyzer::Type analyzeArrayExpr(SemanticAnalyzer &analyzer, ArrayExpr &expr) {
    using Type = SemanticAnalyzer::Type;

    ExprCheckContext context(analyzer);
    context.resolveAndTrackSymbolRef(expr.name);

    if (!context.hasArray(expr.name)) {
        context.diagnostics().emit(
            diag::BasicDiag::UnknownArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});

        // Visit all indices for type checking even on error path
        if (expr.index)
            context.evaluate(*expr.index);
        for (auto &indexPtr : expr.indices) {
            if (indexPtr)
                context.evaluate(*indexPtr);
        }
        return Type::Unknown;
    }

    auto varTy = context.varType(expr.name);
    if (varTy && !semantic_analyzer_detail::isSemanticArrayType(*varTy)) {
        context.diagnostics().emit(
            diag::BasicDiag::NotAnArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});

        // Visit all indices for type checking even on error path
        if (expr.index)
            context.evaluate(*expr.index);
        for (auto &indexPtr : expr.indices) {
            if (indexPtr)
                context.evaluate(*indexPtr);
        }
        return Type::Unknown;
    }

    // Validate indices
    if (expr.index) {
        // Single index provided (backward compatible path from parser)
        validateArrayIndex(context, *expr.index, expr.loc);

        // Bounds check: verify dimension count and index range
        const auto *meta = context.arrayMetadata(expr.name);
        if (meta && !meta->extents.empty()) {
            const std::size_t numDims = meta->extents.size();

            // Check dimension count: if array has more than 1 dimension, this is an error
            if (numDims != 1) {
                std::string msg = "wrong number of indices for array '" + expr.name +
                                  "': expected " + std::to_string(numDims) + ", got 1";
                context.diagnostics().emit(
                    il::support::Severity::Error, "B3002", expr.loc, 1, std::move(msg));
            } else {
                // Single-dimensional array: check bounds
                long long upperBound = meta->extents[0];
                if (upperBound >= 0) {
                    if (auto *ci = as<const IntExpr>(*expr.index)) {
                        if (ci->value < 0 || ci->value > upperBound) {
                            std::string msg = "index out of bounds";
                            context.diagnostics().emit(il::support::Severity::Warning,
                                                       "B3001",
                                                       expr.loc,
                                                       1,
                                                       std::move(msg));
                        }
                    }
                }
            }
        }
    } else {
        // Multi-dimensional array (new path)
        for (auto &indexPtr : expr.indices) {
            if (indexPtr)
                validateArrayIndex(context, *indexPtr, expr.loc);
        }

        // Bounds check for multi-dimensional arrays
        const auto *meta = context.arrayMetadata(expr.name);
        if (meta && !meta->extents.empty()) {
            const std::size_t numDims = meta->extents.size();
            const std::size_t numIndices = expr.indices.size();

            // Check dimension count mismatch
            if (numIndices != numDims) {
                std::string msg = "wrong number of indices for array '" + expr.name +
                                  "': expected " + std::to_string(numDims) + ", got " +
                                  std::to_string(numIndices);
                context.diagnostics().emit(
                    il::support::Severity::Error, "B3002", expr.loc, 1, std::move(msg));
            } else {
                // Check each dimension's bounds for constant indices
                for (std::size_t i = 0; i < numIndices; ++i) {
                    if (!expr.indices[i])
                        continue;

                    const long long upperBound = meta->extents[i];
                    if (upperBound < 0)
                        continue; // Dynamic/unknown extent, skip static check

                    if (auto *ci = as<const IntExpr>(*expr.indices[i])) {
                        if (ci->value < 0 || ci->value > upperBound) {
                            std::string msg = "index out of bounds for dimension " +
                                              std::to_string(i + 1) + ": " +
                                              std::to_string(ci->value) + " not in [0, " +
                                              std::to_string(upperBound) + "]";
                            context.diagnostics().emit(il::support::Severity::Warning,
                                                       "B3001",
                                                       expr.loc,
                                                       1,
                                                       std::move(msg));
                        }
                    }
                }
            }
        }
    }

    // BUG-020 fix: Store resolved extents in AST node so the lowerer can access them
    // even after procedure scope cleanup erases ArrayMetadata from the semantic analyzer.
    const auto *metaForLowerer = context.arrayMetadata(expr.name);
    if (metaForLowerer && !metaForLowerer->extents.empty()) {
        expr.resolvedExtents = metaForLowerer->extents;
    }

    // Return element type based on array type
    if (varTy && *varTy == Type::ArrayString)
        return Type::String;
    if (varTy && *varTy == Type::ArrayObject)
        return Type::Object;
    if (varTy && *varTy == Type::ArrayFloat)
        return Type::Float;

    return Type::Int;
}

/// @brief Analyse an LBOUND expression returning the lower index bound.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param expr LBOUND expression node.
/// @return Integer type on success or Unknown when diagnostics were emitted.
SemanticAnalyzer::Type analyzeLBoundExpr(SemanticAnalyzer &analyzer, LBoundExpr &expr) {
    using Type = SemanticAnalyzer::Type;

    ExprCheckContext context(analyzer);
    context.resolveAndTrackSymbolRef(expr.name);

    if (!context.hasArray(expr.name)) {
        context.diagnostics().emit(
            diag::BasicDiag::UnknownArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});
        return Type::Unknown;
    }

    auto varTy = context.varType(expr.name);
    if (varTy && !semantic_analyzer_detail::isSemanticArrayType(*varTy)) {
        context.diagnostics().emit(
            diag::BasicDiag::NotAnArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});
        return Type::Unknown;
    }

    const auto *meta = context.arrayMetadata(expr.name);
    expr.resolvedDimension = resolveBoundDimension(context, expr.dimension, meta, expr.loc);

    return Type::Int;
}

/// @brief Analyse a UBOUND expression returning the upper index bound.
///
/// @param analyzer Semantic analyzer coordinating validation.
/// @param expr UBOUND expression node.
/// @return Integer type on success or Unknown when diagnostics were emitted.
SemanticAnalyzer::Type analyzeUBoundExpr(SemanticAnalyzer &analyzer, UBoundExpr &expr) {
    using Type = SemanticAnalyzer::Type;

    ExprCheckContext context(analyzer);
    context.resolveAndTrackSymbolRef(expr.name);

    if (!context.hasArray(expr.name)) {
        context.diagnostics().emit(
            diag::BasicDiag::UnknownArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});
        return Type::Unknown;
    }

    auto varTy = context.varType(expr.name);
    if (varTy && !semantic_analyzer_detail::isSemanticArrayType(*varTy)) {
        context.diagnostics().emit(
            diag::BasicDiag::NotAnArray,
            expr.loc,
            static_cast<uint32_t>(expr.name.size()),
            std::initializer_list<diag::Replacement>{diag::Replacement{"name", expr.name}});
        return Type::Unknown;
    }

    const auto *meta = context.arrayMetadata(expr.name);
    expr.resolvedDimension = resolveBoundDimension(context, expr.dimension, meta, expr.loc);
    if (expr.resolvedDimension && meta && !meta->extents.empty() &&
        *expr.resolvedDimension < meta->extents.size()) {
        expr.resolvedUpperBound = meta->extents[*expr.resolvedDimension];
    }

    return Type::Int;
}

} // namespace il::frontends::basic::sem
