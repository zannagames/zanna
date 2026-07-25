//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/DiagnosticCodes.hpp
// Purpose: Legacy named diagnostic-code constants for BASIC frontend clients.
// Key invariants: Constants in this header are unique and follow B#### format.
// Ownership/Lifetime: String views refer to static string-literal storage.
// Links: src/frontends/basic/DiagnosticEmitter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DiagnosticCodes.hpp
 * @brief Defines a legacy compile-time catalog of named BASIC diagnostic codes.
 *
 * The constants group intended parser, semantic, lowering, and warning ranges.
 * They are lightweight string views and do not themselves register messages
 * with DiagnosticEmitter or the global diagnostic catalog.
 */

#pragma once

#include <string_view>

namespace zanna::basic::diag {

/// @name Parser error codes
/// Intended range: B0001-B0999.
/// @{
constexpr std::string_view LabelAlreadyDefined = "B0001";
constexpr std::string_view UndefinedLabel = "B0002";
constexpr std::string_view InvalidEscapeSequence = "B0003";
constexpr std::string_view UnexpectedToken = "B0004";
constexpr std::string_view MissingIdentifier = "B0005";
constexpr std::string_view DuplicateVariable = "B0006";
constexpr std::string_view InvalidTypeSuffix = "B0007";
constexpr std::string_view ExpectedExpression = "B0008";
constexpr std::string_view UnterminatedString = "B0009";
constexpr std::string_view InvalidNumericLiteral = "B0010";
/// @}

/// @name Semantic analyzer error codes
/// Intended range: B1000-B1999.
/// @{
constexpr std::string_view TypeMismatch = "B1000";
constexpr std::string_view UndefinedVariable = "B1001";
constexpr std::string_view UndefinedFunction = "B1002";
constexpr std::string_view WrongNumberOfArguments = "B1003";
constexpr std::string_view InvalidOperandType = "B1004";
constexpr std::string_view CannotConvert = "B1005";
constexpr std::string_view InvalidArrayDimension = "B1006";
constexpr std::string_view InvalidLoopVariable = "B1007";
constexpr std::string_view BreakOutsideLoop = "B1008";
constexpr std::string_view ContinueOutsideLoop = "B1009";
constexpr std::string_view InvalidReturnType = "B1010";
constexpr std::string_view AmbiguousOverload = "B1011";
/// @}

/// @name Lowering error codes
/// Intended range: B2000-B2999.
/// @{
constexpr std::string_view CannotLowerExpression = "B2000";
constexpr std::string_view CannotLowerStatement = "B2001";
constexpr std::string_view UnsupportedFeature = "B2002";
constexpr std::string_view InternalCompilerError = "B2003";
/// @}

/// @name Warning codes
/// Intended range: B9000-B9999.
/// @{
constexpr std::string_view UnusedVariable = "B9000";
constexpr std::string_view ShadowedVariable = "B9001";
constexpr std::string_view UnreachableCode = "B9002";
constexpr std::string_view ImplicitConversion = "B9003";
/// @}

} // namespace zanna::basic::diag
