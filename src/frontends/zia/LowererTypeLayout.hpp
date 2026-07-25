//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererTypeLayout.hpp
/// @brief Registry and primitive layout utilities for Zia type lowering.
///
/// @details The registry owns struct, class, and interface layout records for
///          one module-lowering operation. It also allocates monotonically
///          increasing runtime identifiers and exposes the IL size/alignment
///          rules used when computing aggregate field offsets.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/zia/LowererTypes.hpp"
#include "il/core/Type.hpp"
#include "support/alignment.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>

namespace il::frontends::zia {

/// @brief Owns resolved nominal-type layouts for one Zia module.
/// @invariant Type layouts are populated before their method bodies are
///            lowered.
/// @invariant Allocated class and interface identifiers are unique within the
///            registry.
class LowererTypeLayout {
  public:
    using Type = il::core::Type;

    /// @brief Construct an empty layout registry with identifiers starting at
    ///        one.
    LowererTypeLayout() = default;

    /// @brief Find mutable layout metadata for a value type.
    /// @param name Qualified value-type name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    StructTypeInfo *findValueType(const std::string &name) {
        auto it = structTypes_.find(name);
        return it != structTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Find read-only layout metadata for a value type.
    /// @param name Qualified value-type name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    const StructTypeInfo *findValueType(const std::string &name) const {
        auto it = structTypes_.find(name);
        return it != structTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Test whether a value-type layout is registered.
    /// @param name Qualified value-type name.
    /// @return True when @p name has stored layout metadata.
    bool hasValueType(const std::string &name) const {
        return structTypes_.count(name) > 0;
    }

    /// @brief Record the field layout for value (struct) type @p name.
    /// @param name Qualified value-type name to define or replace.
    /// @param info Completed layout metadata transferred into the registry.
    void registerValueType(const std::string &name, StructTypeInfo info);

    /// @brief Access all registered value-type layouts.
    /// @return Modifiable map keyed by qualified type name.
    std::unordered_map<std::string, StructTypeInfo> &valueTypes() {
        return structTypes_;
    }

    /// @brief Access all registered value-type layouts.
    /// @return Read-only map keyed by qualified type name.
    const std::unordered_map<std::string, StructTypeInfo> &valueTypes() const {
        return structTypes_;
    }

    /// @brief Find mutable layout metadata for an entity type.
    /// @param name Qualified class name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    ClassTypeInfo *findEntityType(const std::string &name) {
        auto it = classTypes_.find(name);
        return it != classTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Find read-only layout metadata for an entity type.
    /// @param name Qualified class name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    const ClassTypeInfo *findEntityType(const std::string &name) const {
        auto it = classTypes_.find(name);
        return it != classTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Test whether an entity-type layout is registered.
    /// @param name Qualified class name.
    /// @return True when @p name has stored layout metadata.
    bool hasEntityType(const std::string &name) const {
        return classTypes_.count(name) > 0;
    }

    /// @brief Record the field layout for class type @p name.
    /// @param name Qualified class name to define or replace.
    /// @param info Completed class and dispatch metadata transferred into the
    ///        registry.
    void registerEntityType(const std::string &name, ClassTypeInfo info);

    /// @brief Access all registered entity-type layouts.
    /// @return Modifiable map keyed by qualified class name.
    std::unordered_map<std::string, ClassTypeInfo> &entityTypes() {
        return classTypes_;
    }

    /// @brief Access all registered entity-type layouts.
    /// @return Read-only map keyed by qualified class name.
    const std::unordered_map<std::string, ClassTypeInfo> &entityTypes() const {
        return classTypes_;
    }

    /// @brief Find mutable layout metadata for an interface type.
    /// @param name Qualified interface name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    InterfaceTypeInfo *findInterfaceType(const std::string &name) {
        auto it = interfaceTypes_.find(name);
        return it != interfaceTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Find read-only layout metadata for an interface type.
    /// @param name Qualified interface name.
    /// @return Pointer to the stored layout, or `nullptr` when absent.
    const InterfaceTypeInfo *findInterfaceType(const std::string &name) const {
        auto it = interfaceTypes_.find(name);
        return it != interfaceTypes_.end() ? &it->second : nullptr;
    }

    /// @brief Record the method/slot layout for interface type @p name.
    /// @param name Qualified interface name to define or replace.
    /// @param info Completed interface dispatch metadata transferred into the
    ///        registry.
    void registerInterfaceType(const std::string &name, InterfaceTypeInfo info);

    /// @brief Access all registered interface-type layouts.
    /// @return Modifiable map keyed by qualified interface name.
    std::unordered_map<std::string, InterfaceTypeInfo> &interfaceTypes() {
        return interfaceTypes_;
    }

    /// @brief Access all registered interface-type layouts.
    /// @return Read-only map keyed by qualified interface name.
    const std::unordered_map<std::string, InterfaceTypeInfo> &interfaceTypes() const {
        return interfaceTypes_;
    }

    /// @brief Allocate the next runtime class identifier.
    /// @return Current class identifier before advancing the counter.
    int nextClassId() {
        return nextClassId_++;
    }

    /// @brief Inspect the next class identifier without allocating it.
    /// @return Identifier that the next nextClassId() call will return.
    int peekNextClassId() const {
        return nextClassId_;
    }

    /// @brief Allocate the next runtime interface identifier.
    /// @return Current interface identifier before advancing the counter.
    int nextIfaceId() {
        return nextIfaceId_++;
    }

    /// @brief Inspect the next interface identifier without allocating it.
    /// @return Identifier that the next nextIfaceId() call will return.
    int peekNextIfaceId() const {
        return nextIfaceId_;
    }

    /// @brief Return the storage size used for a primitive IL type.
    /// @param type IL type to classify.
    /// @return Size in bytes; unsupported and aggregate kinds conservatively
    ///         use eight bytes.
    static size_t getILTypeSize(Type type) {
        switch (type.kind) {
            case Type::Kind::I64:
            case Type::Kind::F64:
            case Type::Kind::Ptr:
            case Type::Kind::Str:
                return 8;
            case Type::Kind::I32:
                return 4;
            case Type::Kind::I16:
                return 2;
            case Type::Kind::I1:
                return 1;
            default:
                return 8;
        }
    }

    /// @brief Return the field alignment used for a primitive IL type.
    /// @param type IL type to classify.
    /// @return Required alignment in bytes; unsupported kinds and `i1` use
    ///         eight-byte alignment.
    static size_t getILTypeAlignment(Type type) {
        switch (type.kind) {
            case Type::Kind::I64:
            case Type::Kind::F64:
            case Type::Kind::Ptr:
            case Type::Kind::Str:
                return 8;
            case Type::Kind::I32:
                return 4;
            case Type::Kind::I16:
                return 2;
            case Type::Kind::I1:
                return 8;
            default:
                return 8;
        }
    }

    /// @brief Round a byte offset up to an alignment boundary.
    /// @param offset Unaligned byte offset.
    /// @param alignment Required byte alignment.
    /// @return Smallest aligned offset greater than or equal to @p offset.
    static size_t alignTo(size_t offset, size_t alignment) {
        return il::support::alignUp(offset, alignment);
    }

  private:
    /// Value-type layouts keyed by qualified source name.
    std::unordered_map<std::string, StructTypeInfo> structTypes_;
    /// Entity-type layouts keyed by qualified source name.
    std::unordered_map<std::string, ClassTypeInfo> classTypes_;
    /// Interface layouts keyed by qualified source name.
    std::unordered_map<std::string, InterfaceTypeInfo> interfaceTypes_;
    /// Next unallocated runtime class identifier.
    int nextClassId_{1};
    /// Next unallocated runtime interface identifier.
    int nextIfaceId_{1};
};

} // namespace il::frontends::zia
