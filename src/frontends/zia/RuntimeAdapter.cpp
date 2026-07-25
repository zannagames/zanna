//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file RuntimeAdapter.cpp
/// @brief Implementation of type conversion utilities for Zia/RuntimeRegistry.
///
/// @details This file implements the bridge functions that convert IL-layer
/// type representations (ILScalarType) to Zia semantic types (TypeRef). These
/// conversions enable the Zia frontend to use the unified RuntimeRegistry
/// for type-safe runtime function binding.
///
/// ## Implementation Notes
///
/// The conversion functions use direct switch-case mapping rather than lookup
/// tables. Unknown and future unrecognized scalar values conservatively map
/// to the Zia `Unknown` sentinel.
///
/// ## Type System Alignment
///
/// The IL type system is intentionally minimal, supporting only the scalar
/// types that can cross the IL/runtime boundary:
///
/// - **Integers**: IL uses i64 exclusively; Zia maps this to Integer
/// - **Floats**: IL uses f64 exclusively; Zia maps this to Number
/// - **Booleans**: IL uses i1; Zia maps this to Boolean
/// - **Strings**: IL uses str (a reference type); Zia maps to String
/// - **Objects**: IL uses ptr/obj for runtime handles; Zia maps unannotated obj to Any
///
/// Collection types (List, Map, Set) and user-defined types are represented
/// as Object/ptr at the IL level—their specific type information is tracked
/// in the Zia type registry, not in the IL signature.
///
/// @see RuntimeAdapter.hpp - Interface documentation and architecture overview
/// @see il::runtime::RuntimeRegistry - Source of parsed signatures
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/RuntimeAdapter.hpp"

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
// toZiaType Implementation
//===----------------------------------------------------------------------===//

/// @brief Map a frontend-neutral runtime scalar to a Zia semantic type.
/// @param t Scalar type parsed from a runtime signature.
/// @return Canonical Zia type, or `Unknown` for an unrecognized scalar.
TypeRef toZiaType(il::runtime::ILScalarType t) {
    // Map each IL scalar type to its Zia semantic equivalent.
    // The default preserves a safe Unknown result for malformed or future
    // signature kinds that do not yet have a Zia mapping.
    switch (t) {
        case il::runtime::ILScalarType::I64:
            // 64-bit signed integer. This is the only integer width at the IL
            // level; Zia's Integer type is semantically equivalent.
            return types::integer();

        case il::runtime::ILScalarType::F64:
            // 64-bit IEEE 754 floating point. Zia calls this "Number" to match
            // its high-level semantics (no distinction between float/double).
            return types::number();

        case il::runtime::ILScalarType::Bool:
            // Boolean type (i1 in IL). Maps directly to Zia's Boolean type.
            return types::boolean();

        case il::runtime::ILScalarType::String:
            // Immutable string reference. IL strings are reference-counted
            // internally by the runtime; Zia treats them as value-like.
            return types::string();

        case il::runtime::ILScalarType::Void:
            // No return value. Used for procedures and setters.
            return types::voidType();

        case il::runtime::ILScalarType::Object:
            // Plain obj is type-erased runtime data. Annotated obj<Class> and seq<T>
            // signatures are handled by toZiaReturnType() before reaching here.
            return types::any();

        case il::runtime::ILScalarType::Unknown:
        default:
            // Unknown type indicates a parse error or unrecognized type token
            // in the signature. Return unknown() to signal the error; the
            // caller should check isValid() on the signature and not register
            // functions with unknown types.
            return types::unknown();
    }
}

//===----------------------------------------------------------------------===//
// toZiaParamTypes Implementation
//===----------------------------------------------------------------------===//

/// @brief Convert explicit runtime parameter scalars to Zia types.
/// @param sig Parsed runtime signature.
/// @return Parameter types in ABI order, excluding any implicit receiver.
std::vector<TypeRef> toZiaParamTypes(const il::runtime::ParsedSignature &sig) {
    // Pre-allocate the result vector to avoid reallocations.
    // The signature's params vector contains one ILScalarType per parameter,
    // excluding the implicit receiver for method calls.
    std::vector<TypeRef> result;
    result.reserve(sig.params.size());

    // Convert each parameter type using toZiaType(). The order is preserved
    // so parameter positions match between the IL signature and Zia's
    // function type representation.
    for (auto p : sig.params)
        result.push_back(toZiaType(p));

    return result;
}

/// @brief Convert a runtime return signature with parameterized-type metadata.
/// @param sig Parsed runtime signature.
/// @return Runtime-class type for annotated objects, typed Seq for element-
///         annotated sequences, or the scalar mapping of the return token.
TypeRef toZiaReturnType(const il::runtime::ParsedSignature &sig) {
    if (!sig.objectTypeName.empty())
        return types::runtimeClass(sig.objectTypeName);

    // When the signature carries an element type (e.g. "seq<str>"), produce a typed
    // Seq type so the lowerer can use kSeqLen/kSeqGet for safe rt_seq iteration.
    if (!sig.elementTypeName.empty()) {
        auto elemScalar = il::runtime::mapILToken(sig.elementTypeName);
        TypeRef elemType = toZiaType(elemScalar);
        return types::seqOf(elemType);
    }
    return toZiaType(sig.returnType);
}

} // namespace il::frontends::zia
