//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ILTypeUtils.cpp
// Purpose: Implement BASIC AST, field-layout, and runtime-token IL type mappings.
// Ownership/Lifetime: Stateless conversions return value-owned type descriptors
//                     or views into caller-owned token storage.
// Links: src/frontends/basic/ILTypeUtils.hpp,
//        src/frontends/basic/BasicTypes.hpp,
//        src/frontends/basic/ast/NodeFwd.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements BASIC-to-IL type conversion helpers.
/// @details Provides small, stateless utilities that map BASIC AST types to IL
///          core types and compute ABI-relevant sizes for field storage. The
///          mappings are canonical and shared across the lowering pipeline to
///          keep type reasoning consistent.
//
//===----------------------------------------------------------------------===//

#include "ILTypeUtils.hpp"

#include "BasicTypes.hpp"
#include "ast/NodeFwd.hpp"
#include "il/core/Type.hpp"

#include <cstddef>
#include <string_view>

namespace il::frontends::basic::type_conv {
namespace {

/// @brief Trim a runtime type token and drop a trailing optional marker.
/// @details Removes surrounding ASCII whitespace and, if present, a single
///          trailing `?` so an optional type (`obj?`) classifies the same as
///          its non-optional form.
/// @param token Raw runtime type token from a catalog signature.
/// @return The trimmed, optional-stripped view (aliases @p token's storage).
std::string_view normalizeRuntimeToken(std::string_view token) noexcept {
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t' ||
                              token.front() == '\n' || token.front() == '\r'))
        token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\n' ||
                              token.back() == '\r'))
        token.remove_suffix(1);
    if (!token.empty() && token.back() == '?')
        token.remove_suffix(1);
    return token;
}

/// @brief Test whether a runtime token denotes a reference/pointer-backed type.
/// @details Recognises `obj`, `ptr`, `seq`, `list` and their generic
///          `obj<...>`/`ptr<...>`/`seq<...>`/`list<...>` spellings, all of which
///          lower to an IL pointer. The token is normalized first.
/// @param token Runtime type token to classify.
/// @return True when @p token is an object/pointer-category type.
bool isRuntimeObjectToken(std::string_view token) noexcept {
    token = normalizeRuntimeToken(token);
    return token == "obj" || token == "ptr" || token.rfind("obj<", 0) == 0 ||
           token.rfind("ptr<", 0) == 0 || token == "seq" || token.rfind("seq<", 0) == 0 ||
           token == "list" || token.rfind("list<", 0) == 0;
}

} // namespace

/// @brief Convert a BASIC AST scalar type to an IL core type.
/// @details Returns the canonical IL kind used by the lowering pipeline for the
///          given BASIC type. The mapping is total; unknown cases fall back to
///          I64 to keep lowering resilient.
/// @param ty BASIC AST type enumerator.
/// @return Corresponding IL core type.
il::core::Type astToIlType(::il::frontends::basic::Type ty) noexcept {
    using IlType = il::core::Type;
    switch (ty) {
        case ::il::frontends::basic::Type::I64:
            return IlType(IlType::Kind::I64);
        case ::il::frontends::basic::Type::F64:
            return IlType(IlType::Kind::F64);
        case ::il::frontends::basic::Type::Str:
            return IlType(IlType::Kind::Str);
        case ::il::frontends::basic::Type::Bool:
            return IlType(IlType::Kind::I1);
    }
    return IlType(IlType::Kind::I64);
}

/// @brief Return the storage size for a BASIC field type.
/// @details Reports the byte size used when laying out fields for the BASIC
///          frontend. Strings are represented as pointers, floating-point values
///          as 64-bit, booleans as 1 byte, and integers as 8 bytes.
/// @param type BASIC field type.
/// @return Size in bytes for the type's storage representation.
std::size_t getFieldSize(::il::frontends::basic::Type type) noexcept {
    /// Host pointer width used by managed string fields in class layouts.
    constexpr std::size_t kPointerSize = sizeof(void *);

    switch (type) {
        case ::il::frontends::basic::Type::Str:
            return kPointerSize;
        case ::il::frontends::basic::Type::F64:
            return 8;
        case ::il::frontends::basic::Type::Bool:
            return 1;
        case ::il::frontends::basic::Type::I64:
        default:
            return 8;
    }
}

/// @copydoc basicTypeToIlKind()
il::core::Type::Kind basicTypeToIlKind(BasicType t) noexcept {
    using Kind = il::core::Type::Kind;
    switch (t) {
        case BasicType::String:
            return Kind::Str;
        case BasicType::Float:
            return Kind::F64;
        case BasicType::Bool:
            return Kind::I1;
        case BasicType::Void:
            return Kind::Void;
        case BasicType::Object:
            return Kind::Ptr;
        case BasicType::Int:
        case BasicType::Unknown:
        default:
            return Kind::I64;
    }
}

/// @copydoc runtimeScalarToType()
il::core::Type runtimeScalarToType(std::string_view token) noexcept {
    using IlType = il::core::Type;
    token = normalizeRuntimeToken(token);
    if (token == "i64")
        return IlType(IlType::Kind::I64);
    if (token == "f64")
        return IlType(IlType::Kind::F64);
    if (token == "i1")
        return IlType(IlType::Kind::I1);
    if (token == "str")
        return IlType(IlType::Kind::Str);
    if (isRuntimeObjectToken(token))
        return IlType(IlType::Kind::Ptr);
    if (token == "void")
        return IlType(IlType::Kind::Void);
    return IlType(IlType::Kind::I64);
}

} // namespace il::frontends::basic::type_conv
