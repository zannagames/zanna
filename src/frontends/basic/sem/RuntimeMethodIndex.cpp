//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/RuntimeMethodIndex.cpp
// Purpose: Adapt the shared runtime-method resolver to BASIC semantic types and
//          expose the legacy RuntimeMethodIndex facade.
// Key invariants:
//   * Method signatures originate in the shared runtime registry.
//   * Receiver parameters are not included in RuntimeMethodInfo::args.
//   * BASIC and IL scalar conversions preserve Unknown as an error-recovery
//     value instead of inventing a concrete type.
// Ownership: Lookup results copy strings and type vectors from the shared
//            resolver; the process-wide index itself owns no mutable catalog.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Implementation of runtime method lookup for the BASIC frontend.
///
/// @details This file implements the RuntimeMethodIndex class which provides
/// the BASIC frontend's interface to runtime class methods. It delegates to
/// the IL-layer RuntimeRegistry for actual method lookup and converts the
/// results from IL types to BASIC types.
///
/// ## Implementation Strategy
///
/// Rather than maintaining its own method index, this implementation:
///
/// 1. Delegates lookups to RuntimeRegistry::instance()
/// 2. Converts ILScalarType results to BasicType
/// 3. Packages the result in a RuntimeMethodInfo structure
///
/// This ensures all frontends use identical signature information and
/// eliminates duplicate parsing/indexing code.
///
/// ## Performance Characteristics
///
/// - **seed()**: O(1) - no-op since RuntimeRegistry handles indexing
/// - **find()**: O(1) - single hash lookup in RuntimeRegistry
/// - **candidates()**: O(n) where n is the number of methods in the index
///   (only used for error messages, so performance is acceptable)
///
/// ## Thread Safety
///
/// All operations are thread-safe:
/// - RuntimeRegistry is immutable after construction
/// - runtimeMethodIndex() uses function-local static initialization
/// - No mutable state in RuntimeMethodIndex
///
/// @see RuntimeMethodIndex.hpp - Interface documentation
/// @see il::runtime::RuntimeRegistry - Underlying method registry
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "frontends/common/RuntimeMethodResolver.hpp"

namespace il::frontends::basic {

namespace {

/// @brief Convert a shared frontend method result into the BASIC result shape.
/// @details Copies target and object metadata, translates return and argument
///          scalar types, and preserves raw-pointer and receiver flags.
/// @param resolved Shared runtime-method result to adapt.
/// @return An independently owned BASIC RuntimeMethodInfo value.
RuntimeMethodInfo makeRuntimeMethodInfo(const il::frontends::common::RuntimeMethodInfo &resolved) {
    RuntimeMethodInfo info;

    info.target = resolved.target;
    info.ret = toBasicType(resolved.ret);
    info.returnClassQName = resolved.returnClassQName;
    info.rawPointerReturn = resolved.rawPointerReturn;
    info.rawPointerParams = resolved.rawPointerParams;
    info.hasReceiver = resolved.hasReceiver;

    info.args.reserve(resolved.args.size());
    for (auto p : resolved.args)
        info.args.push_back(toBasicType(p));

    return info;
}

/// @brief Convert a BASIC semantic scalar type to the runtime IL type enum.
/// @details Used only for type-directed overload lookup; unsupported or
///          unresolved BASIC types map to ILScalarType::Unknown.
/// @param t BASIC argument type to translate.
/// @return Corresponding runtime IL scalar type.
il::runtime::ILScalarType toILScalarType(BasicType t) {
    switch (t) {
        case BasicType::Int:
            return il::runtime::ILScalarType::I64;
        case BasicType::Float:
            return il::runtime::ILScalarType::F64;
        case BasicType::Bool:
            return il::runtime::ILScalarType::Bool;
        case BasicType::String:
            return il::runtime::ILScalarType::String;
        case BasicType::Void:
            return il::runtime::ILScalarType::Void;
        case BasicType::Object:
            return il::runtime::ILScalarType::Object;
        case BasicType::Unknown:
        default:
            return il::runtime::ILScalarType::Unknown;
    }
}

} // namespace

//===----------------------------------------------------------------------===//
// Type Conversion
//===----------------------------------------------------------------------===//

/// @brief Converts an IL scalar type to a BASIC type.
///
/// @details Maps the frontend-agnostic ILScalarType enumeration to the
/// BASIC-specific BasicType enumeration. The mapping is direct—each IL
/// type has exactly one corresponding BASIC type.
///
/// The switch covers all ILScalarType values:
/// - I64 → Int: BASIC uses "Int" for 64-bit integers
/// - F64 → Float: BASIC uses "Float" for 64-bit floating point
/// - Bool → Bool: Direct mapping
/// - String → String: Direct mapping
/// - Void → Void: Direct mapping (used for SUBs)
/// - Object → Object: Opaque pointer to runtime class instance
/// - Unknown → Unknown: Error/fallback case
///
/// @note The compiler will warn if new ILScalarType values are added
///       but not handled here, ensuring the mapping stays complete.
/// @param t Runtime IL scalar type to translate.
/// @return Corresponding BASIC type, or BasicType::Unknown for an unknown
///         runtime type.
BasicType toBasicType(il::runtime::ILScalarType t) {
    switch (t) {
        case il::runtime::ILScalarType::I64:
            // BASIC uses "Int" terminology for integers
            return BasicType::Int;

        case il::runtime::ILScalarType::F64:
            // BASIC uses "Float" terminology for floating point
            return BasicType::Float;

        case il::runtime::ILScalarType::Bool:
            // Direct mapping - boolean type
            return BasicType::Bool;

        case il::runtime::ILScalarType::String:
            // Direct mapping - string reference type
            return BasicType::String;

        case il::runtime::ILScalarType::Void:
            // Direct mapping - no return value (SUBs)
            return BasicType::Void;

        case il::runtime::ILScalarType::Object:
            // Opaque pointer to runtime class instance
            return BasicType::Object;

        case il::runtime::ILScalarType::Unknown:
        default:
            // Fallback for unrecognized types or parse errors
            return BasicType::Unknown;
    }
}

//===----------------------------------------------------------------------===//
// RuntimeMethodIndex Implementation
//===----------------------------------------------------------------------===//

/// @brief Initializes the method index.
///
/// @details This method is now a no-op. Previously, it parsed all runtime
/// signatures and built an internal index. After the RuntimeRegistry
/// refactoring, indexing is done once at the IL layer and shared across
/// all frontends.
///
/// The method is retained for backward compatibility with existing code
/// that calls seed() during initialization.
///
void RuntimeMethodIndex::seed() {
    // No-op: RuntimeRegistry handles all indexing at the IL layer.
    // This method is kept for API compatibility.
}

/// @brief Finds a runtime method by class, name, and arity.
///
/// @details Delegates to RuntimeRegistry::findMethod() and converts the
/// result from IL types to BASIC types. The conversion process:
///
/// 1. Look up the method in RuntimeRegistry
/// 2. If found, extract the ParsedMethod result
/// 3. Convert the return type using toBasicType()
/// 4. Convert each parameter type using toBasicType()
/// 5. Package everything into a RuntimeMethodInfo
///
/// The arity check is handled by RuntimeRegistry—methods are indexed
/// by class|method#arity, so only exact arity matches are returned.
/// @param classQName Fully qualified runtime class name.
/// @param method Runtime method name.
/// @param arity Number of explicit arguments, excluding the receiver.
/// @return Adapted method metadata, or std::nullopt when no matching entry
///         exists.
std::optional<RuntimeMethodInfo> RuntimeMethodIndex::find(std::string_view classQName,
                                                          std::string_view method,
                                                          std::size_t arity) const {
    auto resolved =
        il::frontends::common::RuntimeMethodResolver::instance().find(classQName, method, arity);
    if (!resolved)
        return std::nullopt;

    return makeRuntimeMethodInfo(*resolved);
}

/// @brief Find a runtime method using the inferred BASIC argument types.
/// @details Converts the argument vector to shared IL scalar types and delegates
///          overload selection to RuntimeMethodResolver.
/// @param classQName Fully qualified runtime class name.
/// @param method Runtime method name.
/// @param argTypes Explicit BASIC argument types, excluding the receiver.
/// @return Adapted metadata for the selected overload, or std::nullopt when no
///         viable overload exists.
std::optional<RuntimeMethodInfo> RuntimeMethodIndex::find(
    std::string_view classQName,
    std::string_view method,
    const std::vector<BasicType> &argTypes) const {
    std::vector<il::runtime::ILScalarType> ilArgTypes;
    ilArgTypes.reserve(argTypes.size());
    for (auto argType : argTypes)
        ilArgTypes.push_back(toILScalarType(argType));

    auto resolved = il::frontends::common::RuntimeMethodResolver::instance().find(
        classQName, method, ilArgTypes);
    if (!resolved)
        return std::nullopt;

    return makeRuntimeMethodInfo(*resolved);
}

/// @brief Lists available method overloads for error messages.
///
/// @details Delegates directly to RuntimeRegistry::methodCandidates().
/// Used when a method call has the wrong number of arguments to show
/// the user what arities are available.
/// @param classQName Fully qualified runtime class name.
/// @param method Runtime method name.
/// @return Candidate descriptions supplied by the shared resolver.
std::vector<std::string> RuntimeMethodIndex::candidates(std::string_view classQName,
                                                        std::string_view method) const {
    return il::frontends::common::RuntimeMethodResolver::instance().candidates(classQName, method);
}

//===----------------------------------------------------------------------===//
// Global Index Access
//===----------------------------------------------------------------------===//

/// @brief Returns the global RuntimeMethodIndex singleton.
///
/// @details Uses a function-local static for thread-safe lazy initialization.
/// The index is constructed on first access and persists for the program
/// lifetime.
///
/// Since RuntimeMethodIndex no longer maintains internal state (it delegates
/// to RuntimeRegistry), this is essentially just providing a consistent
/// access point for the API.
/// @return Reference to the process-wide stateless facade.
RuntimeMethodIndex &runtimeMethodIndex() {
    static RuntimeMethodIndex idx;
    return idx;
}

} // namespace il::frontends::basic
