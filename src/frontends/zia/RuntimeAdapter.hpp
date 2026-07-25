//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file RuntimeAdapter.hpp
/// @brief Type conversion utilities for Zia frontend to use RuntimeRegistry.
///
/// @details This file provides the bridge between the IL-layer's frontend-agnostic
/// type system (ILScalarType) and Zia's semantic type system (TypeRef). This
/// adapter enables the Zia frontend to leverage the unified RuntimeRegistry
/// for runtime function signatures without duplicating type parsing logic.
///
/// ## Architecture Overview
///
/// The Zanna compiler uses a layered architecture where runtime function
/// signatures are defined once in `runtime.def` and parsed into structured
/// form by the IL layer's RuntimeRegistry. Each frontend (BASIC, Zia)
/// then provides a thin adapter to map IL types to their native type systems:
///
/// ```
/// ┌─────────────┐     ┌────────────────────┐     ┌─────────────────┐
/// │ runtime.def │────▶│ RuntimeClasses.inc │────▶│ RuntimeRegistry │
/// └─────────────┘     └────────────────────┘     │ (IL Layer)      │
///                                                │ - ILScalarType  │
///                                                └────────┬────────┘
///                                                         │
///                 ┌───────────────────────────────────────┼───────────────┐
///                 │                                       │               │
///                 ▼                                       ▼               ▼
///           ┌──────────┐                           ┌──────────┐    ┌──────────┐
///           │  BASIC   │                           │   Zia    │
///           │ BasicType│                           │ TypeRef  │
///           └──────────┘                           └──────────┘
/// ```
///
/// ## Type Mapping
///
/// The adapter maps IL scalar types to Zia semantic types as follows:
///
/// | ILScalarType | Zia TypeRef     | Description                          |
/// |--------------|-----------------|--------------------------------------|
/// | I64          | types::integer()| 64-bit signed integer                |
/// | F64          | types::number() | 64-bit IEEE 754 floating point       |
/// | Bool         | types::boolean()| Boolean true/false                   |
/// | String       | types::string() | Immutable string reference           |
/// | Void         | types::voidType()| No return value                     |
/// | Object       | types::any()    | Type-erased runtime value            |
/// | Unknown      | types::unknown()| Parse error or unrecognized type     |
///
/// ## Usage Example
///
/// ```cpp
/// // In Sema_Runtime.cpp when registering runtime functions:
/// auto sig = il::runtime::parseRuntimeSignature(method.signature);
/// if (sig.isValid()) {
///     TypeRef returnType = toZiaReturnType(sig);
///     std::vector<TypeRef> paramTypes = toZiaParamTypes(sig);
///     defineExternFunction(method.target, returnType, paramTypes);
/// }
/// ```
///
/// ## Benefits
///
/// Using this adapter provides several advantages:
///
/// 1. **Single Source of Truth**: Runtime signatures are defined once in
///    `runtime.def` and automatically propagated to all frontends.
///
/// 2. **Type Safety**: The Zia frontend can now perform full parameter type
///    checking on runtime function calls, catching errors at compile time.
///
/// 3. **Maintainability**: Adding new runtime classes or methods requires no
///    changes to frontend code—they are automatically available.
///
/// 4. **Consistency**: All frontends use identical signature information,
///    eliminating potential mismatches between frontend bindings.
///
/// @see il::runtime::RuntimeRegistry - The unified registry for runtime signatures
/// @see il::runtime::ILScalarType - The IL-layer scalar type enumeration
/// @see il::runtime::ParsedSignature - Structured signature representation
/// @see Sema_Runtime.cpp - Where this adapter is used to register runtime functions
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Types.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include <vector>

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
/// @name Type Conversion Functions
/// @brief Convert IL-layer types to Zia semantic types.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Convert an IL scalar type to a Zia semantic type reference.
///
/// @details This function maps the frontend-agnostic ILScalarType enumeration
/// values from the RuntimeRegistry to corresponding Zia TypeRef values. This
/// enables the Zia semantic analyzer to use runtime function signatures for
/// type checking without needing to parse signature strings itself.
///
/// The mapping preserves type semantics across the boundary:
/// - Numeric types map to their Zia equivalents (I64→Integer, F64→Number)
/// - Reference types (String, Object) map to appropriate Zia reference types
/// - Unknown types map to a sentinel value for error handling
///
/// ## Example
///
/// ```cpp
/// // Converting a runtime method's return type:
/// auto sig = parseRuntimeSignature("str(i64,i64)");  // String.Substring
/// TypeRef retType = toZiaType(sig.returnType);       // types::string()
/// ```
///
/// @param t The IL scalar type from a parsed runtime signature.
///
/// @return The corresponding Zia TypeRef:
///         - I64 → types::integer() (64-bit signed integer)
///         - F64 → types::number() (64-bit floating point)
///         - Bool → types::boolean() (boolean)
///         - String → types::string() (string reference)
///         - Void → types::voidType() (void/unit type)
///         - Object → types::any() (type-erased runtime value)
///         - Unknown → types::unknown() (error/unrecognized)
///
/// @note Annotated object signatures such as obj<Zanna.Graphics.Canvas> are
///       converted by toZiaReturnType(); plain obj remains a safe Any value.
///
TypeRef toZiaType(il::runtime::ILScalarType t);

/// @brief Convert a parsed signature's parameter types to Zia type references.
///
/// @details This function transforms the parameter type list from a parsed
/// runtime signature into a vector of Zia TypeRef values. This is used when
/// registering runtime functions to provide complete type information for
/// the semantic analyzer's function call checking.
///
/// The function preserves parameter order and converts each ILScalarType
/// using toZiaType(). The resulting vector can be passed directly to
/// Sema::defineExternFunction() to create a properly typed extern declaration.
///
/// ## Example
///
/// ```cpp
/// // Registering String.Substring(start: i64, length: i64) -> str
/// auto sig = parseRuntimeSignature("str(i64,i64)");
/// std::vector<TypeRef> params = toZiaParamTypes(sig);
/// // params = [types::integer(), types::integer()]
///
/// defineExternFunction("Zanna.String.Substring",
///                      toZiaType(sig.returnType),  // types::string()
///                      params);                     // [Integer, Integer]
/// ```
///
/// @param sig The parsed signature containing parameter type information.
///            The signature's params vector contains ILScalarType values
///            for each parameter (excluding the implicit receiver for methods).
///
/// @return A vector of Zia TypeRef values corresponding to the signature's
///         parameters, in the same order. Returns an empty vector if the
///         signature has no parameters.
///
/// @note For runtime class methods, the receiver (self/this pointer) is NOT
///       included in the signature's params vector—it is handled separately
///       at the call site. This function only converts explicit parameters.
///
/// @see toZiaType() - Used internally to convert each parameter type.
/// @see il::runtime::ParsedSignature - The source signature structure.
///
std::vector<TypeRef> toZiaParamTypes(const il::runtime::ParsedSignature &sig);

/// @brief Convert a parsed signature's return type to Zia, handling typed seq returns.
///
/// @details Extends toZiaType() for signatures that carry element type information
/// (e.g. "seq<str>" in runtime.def). When `sig.elementTypeName` is non-empty, this
/// function returns a typed Seq type (types::seqOf) so the lowerer can emit the correct
/// kSeqLen/kSeqGet opcodes for rt_seq objects. Without this, seq-returning functions
/// would produce an untyped Ptr that could cause "store void" errors in the IL verifier.
///
/// @param sig The parsed signature (may have elementTypeName set from "seq<str>" syntax).
/// @return The corresponding Zia TypeRef, with proper collection typing when available.
///
TypeRef toZiaReturnType(const il::runtime::ParsedSignature &sig);

/// @}

} // namespace il::frontends::zia
