//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/OopIndex.hpp
// Purpose: Pure data model for OOP metadata without AST dependencies.
// Key invariants: Index stores one entry per class name with immutable signature data.
// Ownership/Lifetime: OopIndex stores copies of metadata without owning AST nodes.
//
//===----------------------------------------------------------------------===//

/// @file OopIndex.hpp
/// @brief Declares the AST-independent BASIC OOP metadata index.
/// @details The index owns copied class, interface, and enum descriptions used
///          by semantic analysis and lowering. Query results point into these
///          owned containers and can be invalidated by subsequent mutation.

#pragma once

#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include "support/source_location.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::frontends::basic {

/// @brief Hash functor for heterogeneous string lookup (C++20).
struct OopStringHash {
    /// Marker enabling heterogeneous lookup in compatible unordered containers.
    using is_transparent = void;

    /// @brief Hash a string-like key through its `std::string_view` representation.
    /// @tparam T Type constructible as `std::string_view`.
    /// @param key String-like value to hash.
    /// @return Standard-library hash of the key's character sequence.
    template <typename T> [[nodiscard]] std::size_t operator()(const T &key) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(key));
    }
};

/// @brief Signature used for interface slots (parameters + return type).
struct IfaceMethodSig {
    std::string name;               ///< Method name within the interface.
    std::vector<Type> paramTypes;   ///< Parameter types in order.
    std::optional<Type> returnType; ///< Optional return type.
};

/// @brief Interface metadata including stable ID and slot layout.
struct InterfaceInfo {
    int ifaceId = -1;                  ///< Monotonic stable interface identifier.
    std::string qualifiedName;         ///< Fully-qualified interface name (A.B.I).
    std::vector<IfaceMethodSig> slots; ///< Declared methods in slot order.
};

/// @brief Captures the signature of a CLASS method.
/// @details Stores parameter types, return type, and access control for a method.
///          The implicit instance receiver is not included in paramTypes.
struct MethodSig {
    /// @brief Ordered parameter types, excluding the implicit instance parameter.
    std::vector<Type> paramTypes;

    /// @brief Optional return type for methods producing a value.
    std::optional<Type> returnType;

    /// @brief Qualified class name when method returns an object.
    /// @details Empty string indicates primitive or void return type. (BUG-099)
    std::string returnClassName;

    /// @brief Access specifier for the method (default Public).
    Access access{Access::Public};
};

/// @brief Aggregated information extracted from a CLASS declaration.
struct ClassInfo {
    /// @brief Field metadata copied from the CLASS definition.
    struct FieldInfo {
        std::string name;                    ///< Declared field name.
        Type type = Type::I64;               ///< Declared field type.
        Access access{Access::Public};       ///< Field access control.
        bool isArray{false};                 ///< Whether field is an array. (BUG-059)
        std::vector<long long> arrayExtents; ///< Array dimensions if isArray. (BUG-059)
        std::string objectClassName;         ///< Class name for object-typed fields. (BUG-082)
    };

    /// @brief Signature metadata for constructor parameters.
    struct CtorParam {
        Type type = Type::I64; ///< Declared parameter type.
        bool isArray = false;  ///< True when parameter declared with trailing ().
    };

    std::string name;              ///< Unqualified class identifier.
    std::string qualifiedName;     ///< Fully-qualified class name (namespaces + name).
    std::string baseQualified;     ///< Fully-qualified base name (empty when none or unresolved).
    bool isAbstract{false};        ///< True when class is abstract.
    bool isFinal{false};           ///< True when class is final.
    il::support::SourceLoc loc{};  ///< Location of the CLASS keyword.
    std::vector<FieldInfo> fields; ///< Ordered instance field declarations.
    std::vector<FieldInfo> staticFields; ///< Ordered static field declarations.
    bool hasConstructor = false;         ///< True if CLASS declares a constructor.
    bool hasSynthCtor = false;           ///< True when lowering must synthesise a constructor.
    bool hasDestructor = false;          ///< True if CLASS declares a destructor.
    bool hasStaticCtor = false;          ///< True if CLASS declares a static constructor.
    std::vector<CtorParam> ctorParams;   ///< Constructor signature if declared.

    /// @brief Extended method metadata used for vtable construction and checks.
    struct MethodInfo {
        MethodSig sig;                   ///< Signature (params/return/access).
        bool isStatic = false;           ///< True when declared STATIC (no implicit receiver).
        bool isVirtual = false;          ///< Declared or implied virtual.
        bool isAbstract = false;         ///< Declared abstract.
        bool isFinal = false;            ///< Declared final.
        int slot = -1;                   ///< Virtual slot index; -1 for non-virtual.
        bool isPropertyAccessor = false; ///< True when synthesized from a PROPERTY.
        bool isGetter = false;           ///< True for getter; false for setter when accessor.
    };

    /// @brief Declared methods indexed by name (heterogeneous lookup enabled).
    std::unordered_map<std::string, MethodInfo, OopStringHash, std::equal_to<>> methods;

    /// @brief Ordered virtual method names by slot for deterministic ABI layout.
    std::vector<std::string> vtable;

    /// @brief Method declaration source locations (for diagnostics).
    std::unordered_map<std::string, il::support::SourceLoc, OopStringHash, std::equal_to<>>
        methodLocs;

    /// @brief Interfaces implemented by this class (by stable ID).
    std::vector<int> implementedInterfaces;

    /// @brief Mapping from interface ID to concrete method mappings (slot index to method name).
    std::unordered_map<int, std::vector<std::string>> ifaceSlotImpl;

    /// @brief Raw IMPLEMENTS list captured during parsing (dotted names, unresolved).
    std::vector<std::string> rawImplements;
};

/// @brief Container mapping class names to extracted metadata.
/// @details Stores one ClassInfo entry per declared CLASS and one InterfaceInfo
///          entry per declared INTERFACE. Populated during the OOP scanning phase
///          and consulted throughout lowering for layout, vtable, and method resolution.
class OopIndex {
  public:
    /// @brief Map from class name to its metadata.
    using ClassTable = std::unordered_map<std::string, ClassInfo>;
    /// @brief Map from qualified interface name to its metadata.
    using IfaceTable = std::unordered_map<std::string, InterfaceInfo>;

    /// @brief Access the mutable class table.
    /// @return Mutable reference to the index-owned class map.
    [[nodiscard]] ClassTable &classes() noexcept {
        return classes_;
    }

    /// @brief Access the immutable class table.
    /// @return Const reference to the index-owned class map.
    [[nodiscard]] const ClassTable &classes() const noexcept {
        return classes_;
    }

    /// @brief Reset class and interface metadata and restart interface IDs.
    /// @details Enum metadata is intentionally retained.
    void clear() noexcept {
        classes_.clear();
        interfacesByQname_.clear();
        nextInterfaceId_ = 0;
    }

    /// @brief Find a class by name using case-insensitive BASIC comparison.
    /// @param name Qualified or unqualified class name to search for.
    /// @return Pointer to the ClassInfo, or nullptr when not found.
    [[nodiscard]] ClassInfo *findClass(const std::string &name);

    /// @brief Find a class by name using case-insensitive BASIC comparison.
    /// @param name Qualified or unqualified class name to search for.
    /// @return Pointer to the ClassInfo, or nullptr when not found.
    [[nodiscard]] const ClassInfo *findClass(const std::string &name) const;

    // =========================================================================
    // Field Query API
    // =========================================================================

    /// @brief Find a field in a class (case-insensitive).
    /// @details Searches instance fields before static fields and does not
    ///          inspect base classes.
    /// @param className Qualified class name.
    /// @param fieldName Field identifier to find.
    /// @return Pointer to field info or nullptr if not found.
    [[nodiscard]] const ClassInfo::FieldInfo *findField(const std::string &className,
                                                        std::string_view fieldName) const;

    /// @brief Find a field in a class or any of its base classes (case-insensitive).
    /// @details Searches instance fields before static fields at each level.
    /// @param className Qualified class name to start search from.
    /// @param fieldName Field identifier to find.
    /// @return Pointer to field info or nullptr if not found in hierarchy.
    [[nodiscard]] const ClassInfo::FieldInfo *findFieldInHierarchy(
        const std::string &className, std::string_view fieldName) const;

    // =========================================================================
    // Method Query API
    // =========================================================================

    /// @brief Find a method declared directly by a class using BASIC casing rules.
    /// @param className Qualified class name.
    /// @param methodName Method identifier to find.
    /// @return Pointer to method info or nullptr if not found.
    [[nodiscard]] const ClassInfo::MethodInfo *findMethod(const std::string &className,
                                                          std::string_view methodName) const;

    /// @brief Find a method in a class or its first declaring base class.
    /// @param className Qualified class name to start search from.
    /// @param methodName Method identifier to find.
    /// @return Pointer to method info or nullptr if not found in hierarchy.
    [[nodiscard]] const ClassInfo::MethodInfo *findMethodInHierarchy(
        const std::string &className, std::string_view methodName) const;

    /// @brief Access the interface table by qualified name.
    /// @return Mutable reference to the index-owned interface map.
    [[nodiscard]] IfaceTable &interfacesByQname() noexcept {
        return interfacesByQname_;
    }

    /// @brief Access the immutable interface table by qualified name.
    /// @return Const reference to the interface table.
    [[nodiscard]] const IfaceTable &interfacesByQname() const noexcept {
        return interfacesByQname_;
    }

    /// @brief Allocate the next stable interface ID.
    /// @return A unique, monotonically increasing interface identifier.
    int allocateInterfaceId() noexcept {
        return nextInterfaceId_++;
    }

    /// @brief Metadata for an ENUM type: named integer constants.
    struct EnumInfo {
        /// Declared enum name used as the table key.
        std::string name;

        /// @brief One named integral constant in an enum declaration.
        struct Member {
            /// Declared variant name.
            std::string name;
            /// Integral value assigned by explicit or implicit enumeration.
            long long value{0};
        };

        /// Variants in declaration order.
        std::vector<Member> members;
    };

    /// Map from enum names to owned enum metadata.
    using EnumTable = std::unordered_map<std::string, EnumInfo, OopStringHash, std::equal_to<>>;

    /// @brief Access the enum table.
    /// @return Mutable reference to the index-owned enum map.
    [[nodiscard]] EnumTable &enums() noexcept {
        return enums_;
    }

    /// @brief Access the immutable enum table.
    /// @return Const reference to the index-owned enum map.
    [[nodiscard]] const EnumTable &enums() const noexcept {
        return enums_;
    }

    /// @brief Look up an enum variant value.
    /// @details Enum and variant comparisons use the map's and loop's exact
    ///          string equality; no BASIC case folding is performed here.
    /// @param enumName Exact enum-table key.
    /// @param variantName Exact member name to locate.
    /// @return The variant's integer value, or std::nullopt if not found.
    [[nodiscard]] std::optional<long long> findEnumVariant(const std::string &enumName,
                                                           const std::string &variantName) const {
        auto it = enums_.find(enumName);
        if (it == enums_.end())
            return std::nullopt;
        for (const auto &m : it->second.members) {
            if (m.name == variantName)
                return m.value;
        }
        return std::nullopt;
    }

  private:
    /// Owned class metadata keyed by stored class name.
    ClassTable classes_;
    /// Owned interface metadata keyed by fully qualified interface name.
    IfaceTable interfacesByQname_;
    /// Owned enum metadata; retained by @ref clear().
    EnumTable enums_;
    /// Next interface identifier returned by @ref allocateInterfaceId().
    int nextInterfaceId_ = 0;
};

/// @brief Query the virtual slot for a method if it is virtual.
/// @param index OOP index containing class metadata.
/// @param qualifiedClass Fully-qualified class name.
/// @param methodName Method identifier.
/// @return Slot index (>=0) when virtual; -1 for non-virtual or when not found.
int getVirtualSlot(const OopIndex &index,
                   const std::string &qualifiedClass,
                   const std::string &methodName);

} // namespace il::frontends::basic