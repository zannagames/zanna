//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LowererTypes.hpp
// Purpose: Core type definitions shared across Lowerer components.
// Key invariants: Types are POD or simple structs; no methods beyond trivial
//                 accessors.
// Ownership/Lifetime: Included transitively via Lowerer.hpp.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowererTypes.hpp
 * @brief Defines value, symbol, storage, layout, and control-flow metadata.
 *
 * These structures are lightweight records shared by modular BASIC lowering
 * components. Pointer and Value members refer to IL objects owned by the module
 * or active function; AST-derived names and vectors are owned by the records.
 */

#pragma once

#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include "frontends/common/ExprResult.hpp"
#include "frontends/common/StringHash.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "zanna/il/Module.hpp"
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::frontends::basic {

// Import StringHash from common library for backward compatibility
using ::il::frontends::common::StringHash;

/// @brief Result of lowering an expression to a value and type pair.
/// @details Now uses the common ExprResult type for consistency across frontends.
using RVal = ::il::frontends::common::ExprResult;

/// @brief Result of lowering a PRINT# argument to a string.
/// @details Pairs the lowered string value with an optional runtime feature that
///          must be declared when the string was produced via a runtime conversion.
struct PrintChArgString {
    il::core::Value text; ///< IL value holding the string result.
    std::optional<il::runtime::RuntimeFeature>
        feature; ///< Runtime feature needed for the conversion, if any.
};

/// @brief Result of lowering an array access expression.
struct ArrayAccess {
    il::core::Value base;  ///< Array handle loaded from the BASIC slot.
    il::core::Value index; ///< Zero-based element index, coerced to i64.
};

/// @brief Classify how an array access will be consumed.
enum class ArrayAccessKind {
    Load,  ///< The caller will read from the computed element.
    Store, ///< The caller will write to the computed element.
};

/// @brief Aggregated metadata for a BASIC symbol.
struct SymbolInfo {
    Type type{Type::I64};           ///< BASIC type derived from declarations or suffixes.
    bool hasType{false};            ///< True when @ref type was explicitly recorded.
    bool isArray{false};            ///< True when symbol refers to an array.
    bool isBoolean{false};          ///< True when scalar bool storage is required.
    bool referenced{false};         ///< Tracks whether lowering observed the symbol.
    bool isStatic{false};           ///< True when symbol is a STATIC procedure-local variable.
    std::optional<unsigned> slotId; ///< Stack slot id for the variable when materialized.
    std::optional<unsigned> arrayLengthSlot; ///< Optional slot for array length (bounds checks).
    std::string stringLabel;                 ///< Cached label for deduplicated string literals.
    bool isObject{false};                    ///< True when symbol references an object slot.
    std::string objectClass;                 ///< Class name for object symbols; empty otherwise.
    bool isByRefParam{false};                ///< True when symbol represents a BYREF parameter.
};

/// @brief Slot type and metadata for variable storage.
/// @details Describes the IL type and semantic flags for a materialized stack slot.
struct SlotType {
    il::core::Type type{il::core::Type(il::core::Type::Kind::I64)}; ///< IL type of the slot.
    bool isArray{false};     ///< True when the slot holds an array handle.
    bool isBoolean{false};   ///< True when the slot holds a boolean scalar.
    bool isObject{false};    ///< True when the slot holds an object reference.
    std::string objectClass; ///< Qualified class name for object slots; empty otherwise.
};

/// @brief Variable storage location and metadata.
/// @details Combines the slot type descriptor with the IL pointer value
///          produced by alloca or field offset computation.
struct VariableStorage {
    SlotType slotInfo;       ///< Type and semantic flags for the storage.
    il::core::Value pointer; ///< IL value pointing to the storage location.
    bool isField{false};     ///< True when the storage refers to a class field.
};

/// @brief Cached signature for a user-defined procedure.
struct ProcedureSignature {
    il::core::Type retType{il::core::Type(il::core::Type::Kind::I64)}; ///< Declared return type.
    std::vector<il::core::Type> paramTypes; ///< Declared parameter types.
    std::vector<bool> byRefFlags;           ///< True when parameter is BYREF.
};

/// @brief Computed memory layout for a BASIC CLASS or TYPE declaration.
struct ClassLayout {
    /// @brief Metadata describing a single field within the class layout.
    struct Field {
        /// Source-level field name.
        std::string name;
        /// BASIC semantic field type.
        Type type{Type::I64};
        /// Byte offset from the object base.
        std::size_t offset{0};
        /// Field storage width in bytes.
        std::size_t size{0};
        /// @brief True when this field is declared as an array.
        /// @details Preserves array metadata from the AST so lowering can
        ///          distinguish implicit field-array accesses inside
        ///          methods (e.g., `inventory(i)`) from scalar fields.
        bool isArray{false};
        /// @brief Declared array extents (upper bounds per dimension).
        /// @details Used for multi-dimensional index linearization. Each
        ///          entry is an inclusive upper bound; length = bound + 1.
        std::vector<long long> arrayExtents{};
        /// @brief BUG-082 fix: Class name for object-typed fields.
        /// @details Empty for primitive types; holds the class name for object references.
        std::string objectClassName;
    };

    /// @brief Ordered field entries preserving declaration order.
    std::vector<Field> fields;
    /// @brief Mapping from field name to its index within @ref fields.
    std::unordered_map<std::string, std::size_t> fieldIndex;
    /// @brief Total storage size in bytes rounded up to the alignment requirement.
    std::size_t size{0};
    /// @brief Stable identifier assigned during OOP scanning for runtime dispatch.
    std::int64_t classId{0};

    /// @brief Look up a field by name with case-insensitive fallback.
    /// @param name Field identifier to search for.
    /// @return Pointer to the matching Field, or nullptr when not found.
    [[nodiscard]] const Field *findField(std::string_view name) const {
        // Try exact match first
        auto it = fieldIndex.find(std::string(name));
        if (it != fieldIndex.end())
            return &fields[it->second];
        // Case-insensitive fallback (BASIC is case-insensitive)
        for (const auto &kv : fieldIndex) {
            if (kv.first.size() == name.size()) {
                bool match = true;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
                        std::tolower(static_cast<unsigned char>(name[i]))) {
                        match = false;
                        break;
                    }
                }
                if (match)
                    return &fields[kv.second];
            }
        }
        return nullptr;
    }
};

/// @brief Describes the address and type of a resolved member field.
struct MemberFieldAccess {
    il::core::Value ptr; ///< Pointer to the field storage.
    il::core::Type ilType{
        il::core::Type(il::core::Type::Kind::I64)}; ///< IL type used for loads/stores.
    Type astType{Type::I64};                        ///< Original AST type.
    std::string objectClassName; ///< BUG-082: Class name for object-typed fields.
};

/// @brief Field scope for tracking fields during class method lowering.
/// @details Active during class method lowering to make instance fields visible
///          as implicit locals. Pairs the class layout with a per-field symbol map.
struct FieldScope {
    const ClassLayout *layout{nullptr}; ///< Layout of the class whose fields are in scope.
    std::unordered_map<std::string, SymbolInfo, StringHash, std::equal_to<>>
        symbols; ///< Field symbols indexed by name.
};

/// @brief Layout of blocks emitted for an IF/ELSEIF chain.
struct IfBlocks {
    std::vector<std::size_t> tests; ///< indexes of test blocks
    std::vector<std::size_t> thens; ///< indexes of THEN blocks
    std::size_t elseIdx{static_cast<std::size_t>(-1)}; ///< index of ELSE block
    std::size_t exitIdx{static_cast<std::size_t>(-1)}; ///< index of common exit block
};

/// @brief Control-flow state emitted by structured statement helpers.
/// @details `cur` tracks the block left active after lowering, while
///          `after` stores the merge/done block when it survives the
///          lowering step. Helpers mark `fallthrough` when execution can
///          reach `after` without an explicit transfer, ensuring callers
///          can reason about terminators consistently.
struct CtrlState {
    il::core::BasicBlock *cur{nullptr};   ///< Block left active after lowering.
    il::core::BasicBlock *after{nullptr}; ///< Merge/done block if retained.
    bool fallthrough{false};              ///< True when `after` remains reachable.

    /// @brief Check if the control-flow state represents a terminated block.
    /// @return True when no current block exists or the current block has a terminator.
    [[nodiscard]] bool terminated() const {
        return !cur || cur->terminated;
    }
};

} // namespace il::frontends::basic
