//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/MethodDispatchHelpers.hpp
// Purpose: Extracted helpers for OOP method dispatch during BASIC lowering.
//
// What this handles:
//   - Static method calls on user-defined classes
//   - Static method calls on runtime classes (Zanna.String, Zanna.Core.Object, etc.)
//   - Instance method calls with virtual dispatch
//   - Interface dispatch (IFace.Method pattern)
//   - Runtime method catalog lookups
//   - Access control checks (private method enforcement)
//
// Invariants from Lowerer/LoweringContext:
//   - OopIndex must be populated with class/interface metadata
//   - Runtime method/property indexes must be initialized
//   - Symbol table must have current class scope set for access control
//
// IL Builder interaction:
//   - Emits call/call.indirect instructions via Lowerer::emitCall*
//   - Generates GEP for vtable/itable slot lookups
//   - Uses Lowerer's coercion helpers for argument type matching
//
// Ownership/Lifetime: Non-owning reference to Lowerer context.
// Links: docs/internals/codemap.md, docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/LowererTypes.hpp"
#include "frontends/basic/OopIndex.hpp"

#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief Declares method dispatch resolution and bounds-check emission helpers
///        used by BASIC OOP lowering.

namespace il::frontends::basic {

class Lowerer;

/// @brief Helper for mapping BasicType (from runtime catalogs) to IL Type::Kind.
/// @details Used when dispatching to runtime class methods where parameter/return
///          types are expressed as BasicType enums rather than AST types.
/// @param t BasicType value from RuntimeMethodIndex.
/// @return Corresponding IL Type::Kind.
inline il::core::Type::Kind basicTypeToILKind(BasicType t) {
    switch (t) {
        case BasicType::String:
            return il::core::Type::Kind::Str;
        case BasicType::Float:
            return il::core::Type::Kind::F64;
        case BasicType::Bool:
            return il::core::Type::Kind::I1;
        case BasicType::Void:
            return il::core::Type::Kind::Void;
        case BasicType::Object:
            return il::core::Type::Kind::Ptr;
        case BasicType::Int:
        case BasicType::Unknown:
        default:
            return il::core::Type::Kind::I64;
    }
}

/// @brief Provides method dispatch resolution for BASIC OOP method calls.
///
/// @details Encapsulates the logic for resolving method calls to their targets,
///          handling static vs instance dispatch, runtime class methods, virtual
///          dispatch via vtables, and interface dispatch via itables. This class
///          does not emit IL directly but returns dispatch information that the
///          caller uses with Lowerer's emit* methods.
///
/// What BASIC syntax it handles:
///   - obj.Method(args) - instance method calls
///   - Class.Method(args) - static method calls
///   - (obj AS IFace).Method(args) - interface dispatch
///   - BASE.Method(args) - explicit base class calls (non-virtual)
///   - Runtime class methods (Zanna.String.Substring, etc.)
///
/// Invariants expected:
///   - Lowerer must have valid OopIndex with class/interface metadata
///   - For interface dispatch, AS-cast base expression must be present
///   - For virtual dispatch, class vtables must be populated at runtime
///
/// IL Builder interaction:
///   - Returns target function names for direct calls
///   - Returns slot indices for indirect calls (vtable/itable)
///   - Does not emit instructions directly
class MethodDispatchResolver {
  public:
    /// @brief Resolution result for a method call.
    struct Resolution {
        enum class Kind {
            Direct,         ///< Direct call to a known function
            Virtual,        ///< Virtual dispatch via vtable slot
            Interface,      ///< Interface dispatch via itable slot
            RuntimeCatalog, ///< Call to runtime catalog method
            Unresolved      ///< Could not resolve
        };

        Kind kind = Kind::Unresolved;

        /// For Direct/RuntimeCatalog: the target function name
        std::string target;

        /// For Virtual/Interface: the slot index in the dispatch table
        int slot = -1;

        /// For Interface: the interface ID for itable lookup
        int ifaceId = -1;

        /// Return type (IL type kind)
        il::core::Type::Kind returnKind = il::core::Type::Kind::I64;

        /// For RuntimeCatalog: expected parameter types for coercion
        std::vector<BasicType> expectedArgs;

        /// Whether this needs receiver as first argument
        bool hasReceiver = false;

        /// For access control errors
        bool accessDenied = false;
        std::string accessErrorMsg;
    };

    /// @brief Construct a resolver using Lowerer's OOP context.
    /// @param lowerer Lowerer providing OopIndex and symbol table access.
    explicit MethodDispatchResolver(Lowerer &lowerer) noexcept;

    /// @brief Resolve a static method call on a class.
    /// @details Handles both user-defined classes and runtime catalog classes.
    /// @param classQName Fully qualified class name (e.g., "MyNamespace.MyClass").
    /// @param methodName Method name to call.
    /// @param argTypes AST types of the call arguments for overload resolution.
    /// @param loc Source location for error reporting.
    /// @return Resolution describing how to emit the call.
    Resolution resolveStaticCall(const std::string &classQName,
                                 const std::string &methodName,
                                 const std::vector<Type> &argTypes,
                                 il::support::SourceLoc loc);

    /// @brief Resolve an instance method call.
    /// @details Determines whether to use direct, virtual, or interface dispatch.
    /// @param receiverClassQName Qualified class of the receiver object.
    /// @param methodName Method name to call.
    /// @param argTypes AST types of the call arguments.
    /// @param isBaseQualified True if call uses BASE.Method() syntax.
    /// @param loc Source location for error reporting.
    /// @return Resolution describing how to emit the call.
    Resolution resolveInstanceCall(const std::string &receiverClassQName,
                                   const std::string &methodName,
                                   const std::vector<Type> &argTypes,
                                   bool isBaseQualified,
                                   il::support::SourceLoc loc);

    /// @brief Resolve an interface dispatch call.
    /// @details For (obj AS IFace).Method() calls, locates the itable slot.
    /// @param interfaceQName Qualified interface name.
    /// @param methodName Method name in the interface.
    /// @param argCount Number of arguments for arity matching.
    /// @return Resolution with Interface kind and slot info.
    Resolution resolveInterfaceCall(const std::string &interfaceQName,
                                    const std::string &methodName,
                                    std::size_t argCount);

    /// @brief Check if a method call on a runtime class can be handled by catalog.
    /// @details Looks up Zanna.String, Zanna.Core.Object, etc. in the runtime method index.
    /// @param classQName Runtime class qualified name.
    /// @param methodName Method to look up.
    /// @param argCount Number of user arguments (excluding receiver).
    /// @return Resolution if found in catalog, nullopt otherwise.
    std::optional<Resolution> tryRuntimeCatalog(const std::string &classQName,
                                                const std::string &methodName,
                                                std::size_t argCount);

  private:
    /// @brief Check access control for a method call.
    /// @param classInfo Class declaring the candidate method.
    /// @param methodName Candidate method name.
    /// @param loc Source location retained for interface symmetry.
    /// @return Error message if access denied, empty string if allowed.
    std::string checkAccessControl(const ClassInfo &classInfo,
                                   const std::string &methodName,
                                   il::support::SourceLoc loc);

    /// Borrowed lowerer supplying OOP metadata and current-class context.
    Lowerer &lowerer_;
};

/// @brief Helper for emitting bounds-checked array index operations.
///
/// @details Consolidates the repeated pattern of emitting bounds-check blocks
///          for array element access. Used by both RuntimeStatementLowerer and
///          direct field array access in OOP lowering.
///
/// What BASIC syntax it handles:
///   - arr(index) bounds checking
///   - obj.arrayField(index) bounds checking
///   - Multi-dimensional index flattening and bounds checking
///
/// Invariants expected:
///   - Lowerer must have active procedure context with function/blocks
///   - Array handle must be valid (not null)
///
/// IL Builder interaction:
///   - Creates conditional branch blocks (ok/oob paths)
///   - Emits rt_arr_oob_panic call on out-of-bounds path
///   - Emits trap instruction after panic
class BoundsCheckEmitter {
  public:
    /// @brief Construct a bounds check emitter using Lowerer context.
    /// @param lowerer Lowerer providing emit helpers and context.
    explicit BoundsCheckEmitter(Lowerer &lowerer) noexcept;

    /// @brief Emit bounds checking code for an array index.
    /// @details Creates ok/oob blocks, emits conditional branch, and traps on oob.
    ///          After return, current block is set to the ok block for continued emission.
    /// @param arrHandle Array handle value for length query.
    /// @param index Index value being checked.
    /// @param elemKind Element type (determines which rt_arr_*_len to call).
    /// @param isObjectArray True if array holds objects (uses rt_arr_obj_len).
    /// @param loc Source location for diagnostics.
    /// @param labelPrefix Prefix for generated block labels.
    void emitBoundsCheck(il::core::Value arrHandle,
                         il::core::Value index,
                         Type elemKind,
                         bool isObjectArray,
                         il::support::SourceLoc loc,
                         std::string_view labelPrefix);

    /// @brief Compute flattened index for multi-dimensional array access.
    /// @details Uses row-major order: flat = i0*L1*L2*... + i1*L2*... + ...
    /// @param indices Index values for each dimension.
    /// @param extents Declared extent for each dimension.
    /// @param loc Source location for emitted instructions.
    /// @return Flattened single index value.
    il::core::Value computeFlattenedIndex(const std::vector<il::core::Value> &indices,
                                          const std::vector<long long> &extents,
                                          il::support::SourceLoc loc);

  private:
    /// Borrowed lowerer supplying block construction and runtime helpers.
    Lowerer &lowerer_;
};

} // namespace il::frontends::basic
