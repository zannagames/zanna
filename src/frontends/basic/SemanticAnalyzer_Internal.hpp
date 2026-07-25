//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Internal.hpp
// Purpose: Declares shared helper utilities for SemanticAnalyzer implementation
// Key invariants: Helpers remain internal to the BASIC front end.
// Ownership/Lifetime: Stateless free functions used by SemanticAnalyzer
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Internal.hpp
/// @brief Declares shared semantic-expression helpers and rule metadata.
/// @details This internal aggregation header connects the main analyzer to
///          split statement/checking modules and declares the free functions
///          used across those implementations. It is not a stable frontend API.

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "frontends/basic/SemanticAnalyzer_Stmts_Control.hpp"
#include "frontends/basic/SemanticAnalyzer_Stmts_IO.hpp"
#include "frontends/basic/SemanticAnalyzer_Stmts_Runtime.hpp"
#include "frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp"

namespace il::frontends::basic::sem {
/// @brief Restricted analyzer facade used by expression-check helpers.
class ExprCheckContext;

/// @brief Validates a unary expression and computes its result category.
/// @details Evaluates an optional operand. `NOT` accepts boolean or integer and
///          preserves that category; unary plus/minus accept integer, float, or
///          unknown recovery values. Invalid known operands emit diagnostics.
/// @param analyzer Analyzer supplying evaluation and diagnostics.
/// @param expr Unary expression borrowed without mutation.
/// @return Operand-derived result type, or @ref SemanticAnalyzer::Type::Unknown
///         after failure.
SemanticAnalyzer::Type analyzeUnaryExpr(SemanticAnalyzer &analyzer, const UnaryExpr &expr);

/// @brief Applies table-driven validation and typing to a binary expression.
/// @details Evaluates both optional operands, records integer-to-float
///          promotions for mixed addition/subtraction/multiplication, records
///          implicit string conversion for mixed string addition, invokes the
///          operator's validator, and then invokes its result callback.
/// @param analyzer Analyzer supplying evaluation, conversion metadata, and
///                 diagnostics.
/// @param expr Binary expression borrowed during checking.
/// @return Rule-computed result, or @ref SemanticAnalyzer::Type::Unknown when
///         no result callback exists.
SemanticAnalyzer::Type analyzeBinaryExpr(SemanticAnalyzer &analyzer, const BinaryExpr &expr);

/// @brief Resolves and validates a function-call expression.
/// @details An unshadowed active-instance array field is treated as indexed
///          field access and its index types are checked. Otherwise the callee
///          is resolved specifically as a FUNCTION, all arguments are checked,
///          and the signature's result type is inferred.
/// @param analyzer Analyzer supplying scopes, OOP fields, procedures, and
///                 diagnostics.
/// @param expr Call expression to inspect.
/// @return Array element or function result type, or unknown after failure.
SemanticAnalyzer::Type analyzeCallExpr(SemanticAnalyzer &analyzer, const CallExpr &expr);

/// @brief Resolves and classifies a variable reference.
/// @details Recognizes `NOTHING`, instance `BASE`, implicit-instance fields,
///          enum names, tracked variables, and suffix defaults. Unknown names
///          emit a diagnostic with a Levenshtein suggestion when a symbol lies
///          within distance three.
/// @param analyzer Analyzer supplying resolution state and diagnostics.
/// @param expr Mutable variable whose name may be replaced by a scoped mapping.
/// @return Tracked/default type, integer for enum names, or unknown for null and
///         unresolved values.
SemanticAnalyzer::Type analyzeVarExpr(SemanticAnalyzer &analyzer, VarExpr &expr);

/// @brief Validates an array access and returns its element type.
/// @details Resolves the symbol, visits indices even on unknown/not-array error
///          paths, validates index types and dimension count, warns for constant
///          out-of-range indices when extents are known, and copies known
///          extents into the AST for lowering after scope rollback.
/// @param analyzer Analyzer supplying array/type metadata and diagnostics.
/// @param expr Mutable access receiving resolved name/extents/conversions.
/// @return String or object for those array categories, integer by default, or
///         unknown on early symbol/category failure.
SemanticAnalyzer::Type analyzeArrayExpr(SemanticAnalyzer &analyzer, ArrayExpr &expr);

/// @brief Validates `LBOUND` and resolves its requested dimension.
/// @param analyzer Analyzer supplying array metadata and diagnostics.
/// @param expr Mutable query receiving a zero-based resolved dimension when
///             compile-time resolution succeeds.
/// @return Integer after a known-array/category check, otherwise unknown.
SemanticAnalyzer::Type analyzeLBoundExpr(SemanticAnalyzer &analyzer, LBoundExpr &expr);

/// @brief Validates `UBOUND` and resolves dimension/known upper bound.
/// @param analyzer Analyzer supplying array metadata and diagnostics.
/// @param expr Mutable query receiving a zero-based resolved dimension and,
///             when extents are known, its resolved upper bound.
/// @return Integer after a known-array/category check, otherwise unknown.
SemanticAnalyzer::Type analyzeUBoundExpr(SemanticAnalyzer &analyzer, UBoundExpr &expr);

} // namespace il::frontends::basic::sem

/// @brief Internal implementation details for the BASIC semantic analyzer.
///
/// This namespace contains helper functions and data structures used by
/// the semantic analyzer implementation. These are not part of the public
/// API and may change without notice. External code should use the
/// SemanticAnalyzer class directly.
namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Recovers a concrete object class name for an expression when possible.
/// @details Handles variables with tracked runtime classes, constructor calls,
///          runtime functions, and method calls that surface typed runtime
///          returns. Returns std::nullopt when the expression is not known to
///          produce a concrete object class.
/// @param analyzer Analyzer supplying active instance, imports, tracked object
///                 types, and class indexes.
/// @param expr Expression to inspect without mutation.
/// @return Qualified class name, or @c std::nullopt when not inferable.
std::optional<std::string> inferObjectClassQName(SemanticAnalyzer &analyzer, const Expr &expr);

/// @brief Converts indexed field metadata to a semantic category.
/// @param field Field metadata to classify.
/// @return Object for class-typed fields, otherwise the corresponding scalar
///         type or unknown.
SemanticAnalyzer::Type semanticTypeFromOopField(const ClassInfo::FieldInfo &field);

/// @brief Finds an implicit-instance field visible in the active member.
/// @param analyzer Analyzer supplying the active class and OOP index.
/// @param name Field spelling to search through the class hierarchy.
/// @return Pointer to index-owned field metadata, or @c nullptr.
const ClassInfo::FieldInfo *findActiveInstanceField(const SemanticAnalyzer &analyzer,
                                                    std::string_view name);

/// @brief Defines validation and type computation rules for binary operators.
///
/// Each binary operator in BASIC has specific rules for what operand types
/// are allowed and how the result type is computed. This structure captures
/// those rules in a table-driven format for efficient lookup during semantic
/// analysis of binary expressions.
///
/// @see exprRule() to look up rules by operator
struct ExprRule {
    /// @brief Function pointer type for validating operand type combinations.
    ///
    /// Called during binary expression analysis to check if the left and right
    /// operand types are compatible with the operator. Should emit diagnostics
    /// via the ExprCheckContext if validation fails.
    ///
    /// @param ctx The expression checking context for emitting diagnostics.
    /// @param expr The binary expression being validated.
    /// @param lhs The type of the left operand.
    /// @param rhs The type of the right operand.
    /// @param diagMsg Diagnostic identifier forwarded to mismatch reporting.
    using OperandValidator = void (*)(sem::ExprCheckContext &,
                                      const BinaryExpr &,
                                      SemanticAnalyzer::Type,
                                      SemanticAnalyzer::Type,
                                      std::string_view);

    /// @brief Function pointer type for computing the result type of a binary operation.
    ///
    /// Given the types of both operands, returns the type of the expression result.
    /// For example, Integer + Double yields Double due to numeric promotion.
    ///
    /// @param lhs The type of the left operand.
    /// @param rhs The type of the right operand.
    /// @return The computed result type of the binary operation.
    using ResultTypeFn = SemanticAnalyzer::Type (*)(SemanticAnalyzer::Type, SemanticAnalyzer::Type);

    /// @brief Binary operator represented by this entry.
    BinaryExpr::Op op{BinaryExpr::Op::Add}; ///< The binary operator this rule applies to.
    /// @brief Optional operand validator invoked before result calculation.
    OperandValidator validator{nullptr};    ///< Function to validate operand type compatibility.
    /// @brief Optional result-type callback.
    ResultTypeFn result{nullptr};           ///< Function to compute the result type.
    /// @brief Diagnostic identifier supplied to @ref validator.
    std::string_view mismatchDiag; ///< Diagnostic message template for type errors.
};

/// @brief Retrieves the expression rule for a given binary operator.
///
/// Looks up the validation and result type computation rules for the specified
/// operator from the internal rule table. Every valid BinaryExpr::Op value
/// must have a corresponding rule entry.
///
/// @param op The binary operator to look up.
/// @return Reference to the ExprRule for this operator.
/// @pre @p op must have an ordinal within the static rule array; lookup uses
///      bounds-checked access.
const ExprRule &exprRule(BinaryExpr::Op op);

/// @brief Formats the boolean-only logical-operand mismatch text.
/// @param op Operator rendered by @ref logicalOpName.
/// @param lhs Actual left type.
/// @param rhs Actual right type.
/// @return Message naming the operator and both actual types.
std::string formatLogicalOperandMessage(BinaryExpr::Op op,
                                        SemanticAnalyzer::Type lhs,
                                        SemanticAnalyzer::Type rhs);

/// @brief Delegates common numeric promotion to the shared numeric rules.
/// @param lhs Left semantic type.
/// @param rhs Right semantic type.
/// @return Result of `nr::promoteNumeric(lhs, rhs)`.
SemanticAnalyzer::Type commonNumericType(SemanticAnalyzer::Type lhs,
                                         SemanticAnalyzer::Type rhs) noexcept;

/// @brief Computes case-sensitive bytewise Levenshtein distance.
/// @param a Source string.
/// @param b Destination string.
/// @return Minimum unit-cost insertions, deletions, and substitutions.
size_t levenshtein(const std::string &a, const std::string &b);

/// @brief Converts a compact AST scalar type to semantic type space.
/// @param ty Integer, float, string, or boolean AST type.
/// @return Corresponding semantic category; defensive fall-through is integer.
SemanticAnalyzer::Type astToSemanticType(::il::frontends::basic::Type ty);

/// @brief Returns a builtin registry entry's name.
/// @param b Builtin identifier.
/// @return Non-owning null-terminated name pointer.
const char *builtinName(BuiltinCallExpr::Builtin b);

/// @brief Returns uppercase diagnostic text for a semantic type.
/// @param type Type category to render.
/// @return Static null-terminated text, including array element categories.
const char *semanticTypeName(SemanticAnalyzer::Type type);

/// @brief Returns the BASIC keyword for eager/short-circuit AND or OR.
/// @param op Binary operator to inspect.
/// @return `"ANDALSO"`, `"ORELSE"`, `"AND"`, or `"OR"` for recognized
///         operators; `"<logical>"` otherwise.
const char *logicalOpName(BinaryExpr::Op op);

/// @brief Renders a simple condition operand for diagnostics.
/// @param expr Variable or scalar literal to inspect.
/// @return Source-like variable/literal text, or an empty string for other
///         expression kinds.
std::string conditionExprText(const Expr &expr);

/// @brief Decodes the subset of BASIC type suffixes used for return inference.
/// @param name Name whose final byte is inspected.
/// @return String for `$`, float for `#`, integer for `%`, or
///         @c std::nullopt for every other suffix (including `!` and `&`).
std::optional<BasicType> suffixBasicType(std::string_view name);

/// @brief Converts supported BASIC scalar types to semantic categories.
/// @param type BASIC type to translate.
/// @return Integer, float, or string; @c std::nullopt for all other values.
std::optional<SemanticAnalyzer::Type> semanticTypeFromBasic(BasicType type);

/// @brief Uppercases the canonical text for a BASIC type.
/// @param type Type passed through @ref toString.
/// @return Uppercase copy suitable for diagnostics.
std::string uppercaseBasicTypeName(BasicType type);

/// @brief Tests the broad numeric category used by shared diagnostics.
/// @param type Type to classify.
/// @return @c true for integer, float, or boolean; @c false otherwise.
bool isNumericSemanticType(SemanticAnalyzer::Type type) noexcept;

} // namespace il::frontends::basic::semantic_analyzer_detail
