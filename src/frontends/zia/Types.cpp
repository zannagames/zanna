//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Types.cpp
/// @brief Implementation of Zia semantic type system.
///
/// @details This file implements the ZannaType class and type factory
/// functions. Key implementation details:
///
/// ## Type Interning
///
/// Primitive types (Integer, Number, Boolean, String, etc.) use singleton
/// instances stored in a thread-safe TypeCache. This ensures type comparison
/// can use pointer equality for primitives.
///
/// ## Type Equality and Assignment
///
/// - equals(): Structural equality checking, recursively comparing type args
/// - isAssignableFrom(): Checks if a source type can be assigned to this type,
///   handling optional wrapping, numeric promotions, and interface assignment
/// - isConvertibleTo(): Includes explicit conversions like Int<->String
///
/// ## IL Type Mapping
///
/// The toILType() function maps Zia types to IL types:
/// - Integer → i64, Number → f64, Boolean → i1
/// - String and all reference types → ptr
/// - Struct types → ptr (passed by reference to stack slot)
///
/// @see Types.hpp for type definitions and factory function declarations
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Types.hpp"
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace il::frontends::zia {

namespace {
// Threading model: a Sema/Lowerer instance is single-threaded — its member
// state is mutated without locking and must not be shared across threads. These
// FILE-SCOPE relationship registries are the sole exception: they are
// process-global (shared by every analyzer/lowerer in the process), so every
// read or write MUST hold g_relationship_mutex. Do not access g_interface_impls
// or g_class_parents without it.
using InterfaceSet = std::unordered_set<std::string>;
std::unordered_map<std::string, InterfaceSet> g_interface_impls;
// BUG-VL-007 fix: Track class inheritance (child -> parent)
std::unordered_map<std::string, std::string> g_class_parents;
std::mutex g_relationship_mutex;

/// @brief Recursively append a human-readable spelling of @p type to @p ss.
/// @param ss Destination stream.
/// @param type Semantic type to render.
/// @param developerFacing When true, emit internal/developer detail; when
///        false, the user-facing form. Type arguments render as `[A, B]`,
///        with `?` for an unresolved argument.
void appendTypeString(std::ostringstream &ss, const ZannaType &type, bool developerFacing) {
    // Guard against a pathologically deep type tree overflowing the stack during
    // formatting (mirrors the parser/sema nesting limits). thread_local keeps it
    // correct under concurrent formatting.
    static thread_local int recursionDepth = 0;
    if (recursionDepth > 256) {
        ss << "...";
        return;
    }
    ++recursionDepth;
    struct DepthGuard {
        int &d;
        ~DepthGuard() { --d; }
    } depthGuard{recursionDepth};

    /// @brief Appends a bracketed semantic type-argument list.
    /// @param args Type arguments to format.
    auto appendArgs = [&](const std::vector<TypeRef> &args) {
        ss << "[";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0)
                ss << ", ";
            if (args[i]) {
                appendTypeString(ss, *args[i], developerFacing);
            } else {
                ss << "?";
            }
        }
        ss << "]";
    };

    switch (type.kind) {
        case TypeKindSem::Integer:
            ss << "Integer";
            return;
        case TypeKindSem::Number:
            ss << "Number";
            return;
        case TypeKindSem::Boolean:
            ss << "Boolean";
            return;
        case TypeKindSem::String:
            ss << "String";
            return;
        case TypeKindSem::Byte:
            ss << "Byte";
            return;
        case TypeKindSem::Unit:
            ss << "Unit";
            return;
        case TypeKindSem::Void:
            ss << "Void";
            return;
        case TypeKindSem::Error:
            ss << "Error";
            return;
        case TypeKindSem::Ptr:
            if (developerFacing && !type.name.empty()) {
                ss << type.name;
                if (!type.typeArgs.empty())
                    appendArgs(type.typeArgs);
                return;
            }
            ss << "Ptr";
            return;
        case TypeKindSem::Unknown:
            ss << "?";
            return;
        case TypeKindSem::Never:
            ss << "Never";
            return;
        case TypeKindSem::Any:
            ss << "Any";
            return;
        case TypeKindSem::Optional:
            if (developerFacing && !type.typeArgs.empty() && type.typeArgs[0] &&
                type.typeArgs[0]->kind == TypeKindSem::Unknown) {
                ss << "null";
                return;
            }
            if (!type.typeArgs.empty() && type.typeArgs[0]) {
                appendTypeString(ss, *type.typeArgs[0], developerFacing);
                ss << "?";
            } else {
                ss << "?";
            }
            return;
        case TypeKindSem::Result:
            ss << "Result[";
            if (!type.typeArgs.empty() && type.typeArgs[0])
                appendTypeString(ss, *type.typeArgs[0], developerFacing);
            ss << "]";
            return;
        case TypeKindSem::List:
            ss << "List[";
            if (!type.typeArgs.empty() && type.typeArgs[0])
                appendTypeString(ss, *type.typeArgs[0], developerFacing);
            ss << "]";
            return;
        case TypeKindSem::Set:
            ss << "Set[";
            if (!type.typeArgs.empty() && type.typeArgs[0])
                appendTypeString(ss, *type.typeArgs[0], developerFacing);
            ss << "]";
            return;
        case TypeKindSem::Map:
            ss << "Map[";
            if (type.typeArgs.size() >= 2) {
                if (type.typeArgs[0])
                    appendTypeString(ss, *type.typeArgs[0], developerFacing);
                else
                    ss << "?";
                ss << ", ";
                if (type.typeArgs[1])
                    appendTypeString(ss, *type.typeArgs[1], developerFacing);
                else
                    ss << "?";
            }
            ss << "]";
            return;
        case TypeKindSem::Function:
            ss << "(";
            for (size_t i = 0; i + 1 < type.typeArgs.size(); ++i) {
                if (i > 0)
                    ss << ", ";
                if (type.typeArgs[i]) {
                    appendTypeString(ss, *type.typeArgs[i], developerFacing);
                } else {
                    ss << "?";
                }
            }
            ss << ") -> ";
            if (!type.typeArgs.empty() && type.typeArgs.back()) {
                appendTypeString(ss, *type.typeArgs.back(), developerFacing);
            } else {
                ss << "Void";
            }
            return;
        case TypeKindSem::Tuple:
            ss << "(";
            for (size_t i = 0; i < type.typeArgs.size(); ++i) {
                if (i > 0)
                    ss << ", ";
                if (type.typeArgs[i]) {
                    appendTypeString(ss, *type.typeArgs[i], developerFacing);
                } else {
                    ss << "?";
                }
            }
            ss << ")";
            return;
        case TypeKindSem::Struct:
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::Enum:
            ss << type.name;
            if (!type.typeArgs.empty())
                appendArgs(type.typeArgs);
            return;
        case TypeKindSem::TypeParam:
            ss << type.name;
            return;
        case TypeKindSem::Module:
            ss << (type.name.empty() ? "Module" : type.name);
            return;
        case TypeKindSem::FixedArray:
            if (!type.typeArgs.empty() && type.typeArgs[0]) {
                appendTypeString(ss, *type.typeArgs[0], developerFacing);
            } else {
                ss << "?";
            }
            ss << "[" << type.elementCount << "]";
            return;
    }

    ss << "?";
}
} // namespace

//=============================================================================
// ZannaType Implementation
//=============================================================================

/// @brief Compare two semantic types structurally.
/// @param other Type to compare.
/// @return True when kind, name, argument count, and every nested argument are equal.
bool ZannaType::equals(const ZannaType &other) const {
    if (kind != other.kind)
        return false;
    if (name != other.name)
        return false;
    if (typeArgs.size() != other.typeArgs.size())
        return false;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (!typeArgs[i] || !other.typeArgs[i])
            return typeArgs[i] == other.typeArgs[i];
        if (!typeArgs[i]->equals(*other.typeArgs[i]))
            return false;
    }
    return true;
}

namespace {

/// @brief Canonical `Zanna.Collections.*` class name for a collection type.
/// @details Bridges the two spellings the type system uses for the same runtime
///          class: the dedicated List/Map/Set kinds and the named-Ptr sentinels
///          that carry Seq, Queue, Stack, Deque, Ring, Heap, and Bytes.
/// @param type Candidate semantic type.
/// @return Canonical collection class name, or an empty string when @p type is
///         not a runtime collection.
std::string collectionClassName(const ZannaType &type) {
    switch (type.kind) {
        case TypeKindSem::List:
            return "Zanna.Collections.List";
        case TypeKindSem::Map:
            return "Zanna.Collections.Map";
        case TypeKindSem::Set:
            return "Zanna.Collections.Set";
        case TypeKindSem::Ptr:
            if (type.name.rfind("Zanna.Collections.", 0) == 0)
                return type.name;
            return {};
        default:
            return {};
    }
}

} // namespace

/// @brief Test whether a value of @p source may be assigned to this type.
/// @param source Source value type.
/// @return True for exact/compatible types, supported promotions, optional lifting, declared
///         interface/class relationships, and unresolved recovery placeholders.
bool ZannaType::isAssignableFrom(const ZannaType &source) const {
    // Exact match
    if (equals(source))
        return true;

    // Any accepts everything
    if (kind == TypeKindSem::Any)
        return true;

    // Nothing is assignable from Never
    if (source.kind == TypeKindSem::Never)
        return true;

    // Unknown can be assigned to any type (inference placeholder, e.g., null literal)
    if (source.kind == TypeKindSem::Unknown)
        return true;

    // Reference-like class/interface values and Ptr accept null (Optional[Unknown]).
    // This allows patterns like: func findItem() -> Entity { ... return null; }
    if ((kind == TypeKindSem::Class || kind == TypeKindSem::Interface ||
         kind == TypeKindSem::Ptr) &&
        source.kind == TypeKindSem::Optional && !source.typeArgs.empty() &&
        source.typeArgs[0]->kind == TypeKindSem::Unknown)
        return true;

    // Optional accepts its inner type and null.
    if (kind == TypeKindSem::Optional) {
        if (typeArgs.empty())
            return false;
        if (source.kind == TypeKindSem::Optional) {
            // Optional[T] from Optional[T]
            return typeArgs[0]->isAssignableFrom(*source.typeArgs[0]);
        }
        // Optional[T] from T
        return typeArgs[0]->isAssignableFrom(source);
    }

    // Numeric promotions
    if (kind == TypeKindSem::Number && source.kind == TypeKindSem::Integer)
        return true; // Integer -> Number
    if (kind == TypeKindSem::Integer && source.kind == TypeKindSem::Byte)
        return true; // Byte -> Integer
    if (kind == TypeKindSem::Number && source.kind == TypeKindSem::Byte)
        return true; // Byte -> Number
    // Enum → Integer (enum variants are I64 constants at the IL level)
    if (kind == TypeKindSem::Integer && source.kind == TypeKindSem::Enum)
        return true;
    if (kind == TypeKindSem::Number && source.kind == TypeKindSem::Enum)
        return true; // Enum -> Number (via Integer)

    // Ptr compatibility: concrete runtime reference values lower to pointers,
    // so they can be stored in an explicitly type-erased Ptr slot.
    //
    // Collections are the exception. `Zanna.Collections.*` classes are distinct
    // runtime classes with distinct class ids, and every receiver check is an
    // exact class-id comparison (rt_obj_is_instance performs no hierarchy walk).
    // Letting a List satisfy a Seq slot therefore does not type-erase, it just
    // defers the failure to a trap like "Seq: invalid Seq object", which aborts
    // the process. Require an exact collection match on both sides; unrelated
    // runtime classes keep the historical permissive behaviour because Zia does
    // not model the runtime GUI class hierarchy (a FloatingPanel is a Widget).
    if (kind == TypeKindSem::Ptr) {
        const std::string targetCollection = collectionClassName(*this);
        const std::string sourceCollection = collectionClassName(source);
        if (!targetCollection.empty() && !sourceCollection.empty() &&
            targetCollection != sourceCollection)
            return false;
        return source.kind == TypeKindSem::Ptr || source.kind == TypeKindSem::Function ||
               source.isReference();
    }

    if (kind == TypeKindSem::Result && source.kind == TypeKindSem::Result && !typeArgs.empty() &&
        !source.typeArgs.empty()) {
        if (source.typeArgs[0]->kind == TypeKindSem::Unknown)
            return true;
        return typeArgs[0]->isAssignableFrom(*source.typeArgs[0]);
    }

    // Interface assignment (requires declared implementation)
    if (kind == TypeKindSem::Interface &&
        (source.kind == TypeKindSem::Class || source.kind == TypeKindSem::Struct))
        return types::implementsInterface(source.name, name);

    // BUG-VL-007 fix: Class inheritance (Animal from Dog where Dog extends Animal)
    if (kind == TypeKindSem::Class && source.kind == TypeKindSem::Class)
        return types::isSubclassOf(source.name, name);

    // Generic container assignment: List[Unknown] -> List[T], etc.
    // This handles empty literal inference ([] can be assigned to List[Integer])
    if ((kind == TypeKindSem::List && source.kind == TypeKindSem::List) ||
        (kind == TypeKindSem::Set && source.kind == TypeKindSem::Set) ||
        (kind == TypeKindSem::Map && source.kind == TypeKindSem::Map)) {
        if (typeArgs.empty() || source.typeArgs.empty())
            return false;

        // Empty literals and unresolved container arguments can flow into a
        // concrete container annotation.
        if (source.typeArgs[0]->kind == TypeKindSem::Unknown)
            return true;

        if (kind != TypeKindSem::Map)
            return typeArgs[0]->isAssignableFrom(*source.typeArgs[0]);

        if (typeArgs.size() < 2 || source.typeArgs.size() < 2)
            return false;
        if (source.typeArgs[1]->kind == TypeKindSem::Unknown)
            return true;
        return typeArgs[0]->isAssignableFrom(*source.typeArgs[0]) &&
               typeArgs[1]->isAssignableFrom(*source.typeArgs[1]);
    }

    return false;
}

/// @brief Test whether an explicit semantic conversion to @p target is supported.
/// @param target Destination type.
/// @return True for assignment-compatible types and implemented numeric conversions.
bool ZannaType::isConvertibleTo(const ZannaType &target) const {
    // Assignment is conversion
    if (target.isAssignableFrom(*this))
        return true;

    // Explicit numeric conversions the lowerer actually implements.
    // Integer <-> Number
    if ((kind == TypeKindSem::Integer && target.kind == TypeKindSem::Number) ||
        (kind == TypeKindSem::Number && target.kind == TypeKindSem::Integer))
        return true;

    // Byte <-> Integer
    if ((kind == TypeKindSem::Byte && target.kind == TypeKindSem::Integer) ||
        (kind == TypeKindSem::Integer && target.kind == TypeKindSem::Byte))
        return true;

    // String <-> scalar conversions are intentionally NOT `as` casts: there is
    // no defined failure behavior for parsing, and the previous rows compiled to
    // broken IL. Use string interpolation ("${x}") to format and the
    // Zanna.Core.Parse.* helpers to parse. analyzeAs() emits a targeted hint.
    return false;
}

/// @brief Format the user-facing semantic spelling.
/// @return Source-oriented type spelling; an untyped null optional is rendered as `?`.
std::string ZannaType::toString() const {
    std::ostringstream ss;
    appendTypeString(ss, *this, false);
    return ss.str();
}

/// @brief Format the developer/tooling semantic spelling.
/// @return Detailed spelling retaining named runtime pointer identity and rendering an untyped
///         null optional as `null`.
std::string ZannaType::toDisplayString() const {
    std::ostringstream ss;
    appendTypeString(ss, *this, true);
    return ss.str();
}

//=============================================================================
// Type Factory Implementation
//=============================================================================

namespace types {

/// @brief Remove all process-wide type-to-interface relationships.
/// @details Called when a new semantic analyzer begins so prior compilation state cannot leak.
void clearInterfaceImplementations() {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    g_interface_impls.clear();
}

/// @brief Record that a nominal type implements an interface.
/// @param typeName Concrete semantic type name.
/// @param interfaceName Canonical interface name.
void registerInterfaceImplementation(const std::string &typeName,
                                     const std::string &interfaceName) {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    g_interface_impls[typeName].insert(interfaceName);
}

/// @brief Test direct or inherited implementation of an interface.
/// @param typeName Concrete class or struct name.
/// @param interfaceName Canonical interface name.
/// @return True when the type or one of its registered base classes implements the interface.
bool implementsInterface(const std::string &typeName, const std::string &interfaceName) {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    std::string current = typeName;
    while (!current.empty()) {
        auto it = g_interface_impls.find(current);
        if (it != g_interface_impls.end() && it->second.find(interfaceName) != it->second.end())
            return true;

        auto parentIt = g_class_parents.find(current);
        if (parentIt == g_class_parents.end())
            break;
        current = parentIt->second;
    }
    return false;
}

// BUG-VL-007 fix: Class inheritance tracking
/// @brief Remove all process-wide class-parent relationships.
void clearClassInheritance() {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    g_class_parents.clear();
}

/// @brief Record a direct class inheritance edge.
/// @param childName Derived class name.
/// @param parentName Direct base class name.
void registerClassInheritance(const std::string &childName, const std::string &parentName) {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    g_class_parents[childName] = parentName;
}

/// @brief Test whether one class transitively derives from another.
/// @param childName Candidate derived class.
/// @param parentName Candidate ancestor.
/// @return True when @p parentName appears in the registered parent chain of @p childName.
bool isSubclassOf(const std::string &childName, const std::string &parentName) {
    std::lock_guard<std::mutex> lock(g_relationship_mutex);
    // Walk up the inheritance chain
    std::string current = childName;
    while (!current.empty()) {
        auto it = g_class_parents.find(current);
        if (it == g_class_parents.end())
            return false; // No parent, not a subclass
        if (it->second == parentName)
            return true;      // Found the parent
        current = it->second; // Check grandparent
    }
    return false;
}

namespace {
// Singleton cache for primitive types
struct TypeCache {
    TypeRef integerType;
    TypeRef numberType;
    TypeRef booleanType;
    TypeRef stringType;
    TypeRef byteType;
    TypeRef unitType;
    TypeRef voidType;
    TypeRef errorType;
    TypeRef ptrType;
    TypeRef unknownType;
    TypeRef neverType;
    TypeRef anyType;

    /// @brief Access the process-wide primitive-type cache (Meyers singleton).
    static TypeCache &instance() {
        static TypeCache cache;
        return cache;
    }

  private:
    /// @brief Eagerly construct the shared TypeRef instance for every
    ///        primitive kind so factory accessors return stable identities.
    TypeCache() {
        integerType = std::make_shared<ZannaType>(TypeKindSem::Integer);
        numberType = std::make_shared<ZannaType>(TypeKindSem::Number);
        booleanType = std::make_shared<ZannaType>(TypeKindSem::Boolean);
        stringType = std::make_shared<ZannaType>(TypeKindSem::String);
        byteType = std::make_shared<ZannaType>(TypeKindSem::Byte);
        unitType = std::make_shared<ZannaType>(TypeKindSem::Unit);
        voidType = std::make_shared<ZannaType>(TypeKindSem::Void);
        errorType = std::make_shared<ZannaType>(TypeKindSem::Error);
        ptrType = std::make_shared<ZannaType>(TypeKindSem::Ptr);
        unknownType = std::make_shared<ZannaType>(TypeKindSem::Unknown);
        neverType = std::make_shared<ZannaType>(TypeKindSem::Never);
        anyType = std::make_shared<ZannaType>(TypeKindSem::Any);
    }
};
} // anonymous namespace

/// @brief Return the interned Integer type.
/// @return Process-wide immutable singleton.
TypeRef integer() {
    return TypeCache::instance().integerType;
}

/// @brief Return the interned Number type.
/// @return Process-wide immutable singleton.
TypeRef number() {
    return TypeCache::instance().numberType;
}

/// @brief Return the interned Boolean type.
/// @return Process-wide immutable singleton.
TypeRef boolean() {
    return TypeCache::instance().booleanType;
}

/// @brief Return the interned String type.
/// @return Process-wide immutable singleton.
TypeRef string() {
    return TypeCache::instance().stringType;
}

/// @brief Return the interned Byte type.
/// @return Process-wide immutable singleton.
TypeRef byte() {
    return TypeCache::instance().byteType;
}

/// @brief Return the interned Unit type.
/// @return Process-wide immutable singleton.
TypeRef unit() {
    return TypeCache::instance().unitType;
}

/// @brief Return the interned Void type.
/// @return Process-wide immutable singleton.
TypeRef voidType() {
    return TypeCache::instance().voidType;
}

/// @brief Return the interned Error type.
/// @return Process-wide immutable singleton.
TypeRef error() {
    return TypeCache::instance().errorType;
}

/// @brief Return the interned anonymous pointer type.
/// @return Process-wide immutable singleton.
TypeRef ptr() {
    return TypeCache::instance().ptrType;
}

/// @brief Return the interned Unknown inference placeholder.
/// @return Process-wide immutable singleton.
TypeRef unknown() {
    return TypeCache::instance().unknownType;
}

/// @brief Return the interned Never type.
/// @return Process-wide immutable singleton.
TypeRef never() {
    return TypeCache::instance().neverType;
}

/// @brief Return the interned Any type.
/// @return Process-wide immutable singleton.
TypeRef any() {
    return TypeCache::instance().anyType;
}

/// @brief Construct an Optional type.
/// @param inner Wrapped value type.
/// @return New Optional semantic type.
TypeRef optional(TypeRef inner) {
    return std::make_shared<ZannaType>(TypeKindSem::Optional, std::vector<TypeRef>{inner});
}

/// @brief Construct a Result type.
/// @param successType Success payload type; the error payload is implicit.
/// @return New Result semantic type.
TypeRef result(TypeRef successType) {
    return std::make_shared<ZannaType>(TypeKindSem::Result, std::vector<TypeRef>{successType});
}

/// @brief Construct a List type.
/// @param element Element type.
/// @return New List semantic type.
TypeRef list(TypeRef element) {
    return std::make_shared<ZannaType>(TypeKindSem::List, std::vector<TypeRef>{element});
}

/// @brief Construct a typed runtime sequence handle.
/// @param element Sequence element type.
/// @return Named pointer type that preserves `rt_seq` layout and element metadata.
TypeRef seqOf(TypeRef element) {
    // Represent a typed rt_seq as Ptr{name="Zanna.Collections.Seq", typeArgs=[element]}.
    // This sentinel allows the lowerer to route to kSeqLen/kSeqGet rather than
    // kListCount/kListGet, since rt_seq and rt_list have incompatible layouts.
    return std::make_shared<ZannaType>(TypeKindSem::Ptr,
                                       std::string("Zanna.Collections.Seq"),
                                       std::vector<TypeRef>{std::move(element)});
}

/// @brief Construct a typed runtime Future handle.
/// @param payload Awaited result type.
/// @return Named `Zanna.Threads.Future` pointer with payload metadata.
TypeRef futureOf(TypeRef payload) {
    return std::make_shared<ZannaType>(TypeKindSem::Ptr,
                                       std::string("Zanna.Threads.Future"),
                                       std::vector<TypeRef>{std::move(payload)});
}

/// @brief Construct a Set type.
/// @param element Element type.
/// @return New Set semantic type.
TypeRef set(TypeRef element) {
    return std::make_shared<ZannaType>(TypeKindSem::Set, std::vector<TypeRef>{element});
}

/// @brief Construct a Map type.
/// @param key Key type.
/// @param value Value type.
/// @return New Map semantic type.
TypeRef map(TypeRef key, TypeRef value) {
    return std::make_shared<ZannaType>(TypeKindSem::Map, std::vector<TypeRef>{key, value});
}

/// @brief Construct a function type.
/// @param params Parameter types in source order.
/// @param ret Return type.
/// @return New Function type storing the return type after all parameters.
TypeRef function(std::vector<TypeRef> params, TypeRef ret) {
    params.push_back(ret); // Store return type at the end
    return std::make_shared<ZannaType>(TypeKindSem::Function, std::move(params));
}

/// @brief Construct a tuple type.
/// @param elements Element types in positional order.
/// @return New Tuple semantic type.
TypeRef tuple(std::vector<TypeRef> elements) {
    return std::make_shared<ZannaType>(TypeKindSem::Tuple, std::move(elements));
}

/// @brief Construct a named struct type.
/// @param name Semantic struct name.
/// @param typeParams Concrete or placeholder type arguments.
/// @return New Struct semantic type.
TypeRef structType(const std::string &name, std::vector<TypeRef> typeParams) {
    return std::make_shared<ZannaType>(TypeKindSem::Struct, name, std::move(typeParams));
}

/// @brief Construct a named class type.
/// @param name Semantic class name.
/// @param typeParams Concrete or placeholder type arguments.
/// @return New Class semantic type.
TypeRef classType(const std::string &name, std::vector<TypeRef> typeParams) {
    return std::make_shared<ZannaType>(TypeKindSem::Class, name, std::move(typeParams));
}

/// @brief Construct a named interface type.
/// @param name Semantic interface name.
/// @param typeParams Concrete or placeholder type arguments.
/// @return New Interface semantic type.
TypeRef interface(const std::string &name, std::vector<TypeRef> typeParams) {
    return std::make_shared<ZannaType>(TypeKindSem::Interface, name, std::move(typeParams));
}

/// @brief Construct a named enum type.
/// @param name Semantic enum name.
/// @return New Enum semantic type.
TypeRef enumType(const std::string &name) {
    return std::make_shared<ZannaType>(TypeKindSem::Enum, name);
}

/// @brief Construct a generic type-parameter placeholder.
/// @param name Parameter name.
/// @return New TypeParam semantic type.
TypeRef typeParam(const std::string &name) {
    return std::make_shared<ZannaType>(TypeKindSem::TypeParam, name);
}

/// @brief Construct a named runtime-class pointer type.
/// @param name Fully qualified runtime class name.
/// @return New named Ptr semantic type.
TypeRef runtimeClass(const std::string &name) {
    // Create a Ptr type with the runtime class name
    // This allows us to track the class name for method resolution
    return std::make_shared<ZannaType>(TypeKindSem::Ptr, name);
}

/// @brief Construct a parameterized runtime-class pointer type.
/// @param name Fully qualified runtime class name.
/// @param typeArgs Container or class type arguments.
/// @return New named Ptr semantic type retaining its type arguments.
TypeRef runtimeClass(const std::string &name, std::vector<TypeRef> typeArgs) {
    return std::make_shared<ZannaType>(TypeKindSem::Ptr, name, std::move(typeArgs));
}

/// @brief Construct a non-value module namespace type.
/// @param name Qualified module name.
/// @return New Module semantic type used for qualified lookup.
TypeRef module(const std::string &name) {
    // Create a Module type with the module name
    // This allows qualified access like moduleName.symbol
    return std::make_shared<ZannaType>(TypeKindSem::Module, name);
}

/// @brief Construct a fixed-size inline array type.
/// @param elemType Element type.
/// @param count Compile-time element count.
/// @return New FixedArray semantic type.
TypeRef fixedArray(TypeRef elemType, size_t count) {
    return std::make_shared<ZannaType>(TypeKindSem::FixedArray, std::move(elemType), count);
}

} // namespace types

//=============================================================================
// IL Type Mapping
//=============================================================================

/// @brief Map a Zia semantic type to its lowered IL value kind.
/// @param type Semantic type to lower.
/// @return IL scalar/storage kind used at instruction boundaries.
/// @details Aggregate and reference-like types lower to pointers; nullable reference types retain
///          their pointer/string representation while other optionals use boxed pointer storage.
il::core::Type::Kind toILType(const ZannaType &type) {
    switch (type.kind) {
        case TypeKindSem::Integer:
        case TypeKindSem::Enum:
            return il::core::Type::Kind::I64;

        case TypeKindSem::Number:
            return il::core::Type::Kind::F64;

        case TypeKindSem::Boolean:
            return il::core::Type::Kind::I1;

        case TypeKindSem::String:
            return il::core::Type::Kind::Str;

        case TypeKindSem::Byte:
            return il::core::Type::Kind::I32; // IL has no i8

        case TypeKindSem::Void:
            return il::core::Type::Kind::Void;

        case TypeKindSem::Unit:
            return il::core::Type::Kind::Ptr;

        case TypeKindSem::Error:
            return il::core::Type::Kind::Error;

        case TypeKindSem::Ptr:
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::List:
        case TypeKindSem::Map:
        case TypeKindSem::Set:
            return il::core::Type::Kind::Ptr;

        // Struct types need special handling at lowering time
        // (passed as ptr to stack slot)
        case TypeKindSem::Struct:
            return il::core::Type::Kind::Ptr;

        // Optional reference types (String?, Entity?) use the inner type directly
        // at the IL level since they are already nullable pointers (null = none).
        // Primitive and struct payloads are boxed so Optional[T] still lowers to Ptr.
        case TypeKindSem::Optional: {
            if (!type.typeArgs.empty()) {
                auto innerKind = toILType(*type.typeArgs[0]);
                // Ptr and Str are both nullable pointer types at the IL level,
                // so Optional wrapping them is a no-op — just use the inner type.
                if (innerKind == il::core::Type::Kind::Ptr ||
                    innerKind == il::core::Type::Kind::Str)
                    return innerKind;
            }
            return il::core::Type::Kind::Ptr;
        }

        // Result needs special handling
        // (in-memory representation: tag + payload)
        case TypeKindSem::Result:
            return il::core::Type::Kind::Ptr;

        // Functions are function pointers or closure objects
        case TypeKindSem::Function:
            return il::core::Type::Kind::Ptr;

        // Tuples are stored inline as structs (accessed via pointer)
        case TypeKindSem::Tuple:
            return il::core::Type::Kind::Ptr;

        // Fixed-size arrays are stored inline; accessing the field yields
        // a pointer to the first element within the parent object.
        case TypeKindSem::FixedArray:
            return il::core::Type::Kind::Ptr;

        // Unknown types (inference placeholder)
        case TypeKindSem::Unknown:
        case TypeKindSem::TypeParam:
        case TypeKindSem::Any:
            return il::core::Type::Kind::Ptr;

        // Never type doesn't produce values
        case TypeKindSem::Never:
            return il::core::Type::Kind::Void;

        // Module types are not values
        case TypeKindSem::Module:
            return il::core::Type::Kind::Void;
    }

    return il::core::Type::Kind::Void;
}

/// @brief Compute the semantic storage size used by aggregate layout helpers.
/// @param type Semantic type.
/// @return Size in bytes, or zero for unsized/non-value kinds.
/// @details Tuple and fixed-array sizes include inline layout. User struct size remains dependent
///          on declaration metadata and is therefore not computed here.
size_t typeSize(const ZannaType &type) {
    switch (type.kind) {
        case TypeKindSem::Integer:
            return 8;
        case TypeKindSem::Number:
            return 8;
        case TypeKindSem::Boolean:
            return 8; // Stored as i64
        case TypeKindSem::String:
            return 8; // Pointer
        case TypeKindSem::Byte:
            return 4; // i32
        case TypeKindSem::Void:
            return 0;
        case TypeKindSem::Unit:
            return 8;
        case TypeKindSem::Error:
            return 8; // Pointer to error object
        case TypeKindSem::Ptr:
            return 8;
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::Enum:
        case TypeKindSem::List:
        case TypeKindSem::Map:
        case TypeKindSem::Set:
        case TypeKindSem::Function:
            return 8; // Pointer / I64
        case TypeKindSem::Optional:
            // flag (8) + value size
            if (!type.typeArgs.empty())
                return 8 + typeSize(*type.typeArgs[0]);
            return 16; // Default
        case TypeKindSem::Result: {
            // In-memory representation: tag (8 bytes) + payload, where the payload must
            // hold either the success value or an error-object pointer (8 bytes). The
            // success type is the sole type arg (see types::result); the error is implicit.
            // NOTE: this helper is secondary — the lowerer's getSemanticTypeSize /
            // getTupleStorageSize (Lowerer_Emit.cpp) are the layout source of truth.
            const size_t valueSize = type.typeArgs.empty() ? 0 : typeSize(*type.typeArgs[0]);
            const size_t errorSize = 8; // pointer to error object
            return 8 + (valueSize > errorSize ? valueSize : errorSize);
        }
        case TypeKindSem::Struct:
            // User-defined value size determined by fields
            return 0; // Must be computed from type definition
        case TypeKindSem::Tuple: {
            // Inline struct layout: each element starts at its alignment boundary and the
            // total is rounded up to the aggregate's max alignment. Mirrors the lowerer's
            // getTupleStorageSize (Lowerer_Emit.cpp), the actual layout source of truth.
            size_t size = 0;
            size_t maxAlign = 1;
            for (const auto &elem : type.typeArgs) {
                const size_t a = typeAlignment(*elem);
                if (a > maxAlign)
                    maxAlign = a;
                size = (size + a - 1) / a * a; // align element start
                size += typeSize(*elem);
            }
            return (size + maxAlign - 1) / maxAlign * maxAlign; // round to aggregate alignment
        }
        case TypeKindSem::FixedArray:
            // Inline storage: elementSize * elementCount
            if (!type.typeArgs.empty())
                return typeSize(*type.typeArgs[0]) * type.elementCount;
            return 0;
        case TypeKindSem::Unknown:
        case TypeKindSem::Never:
        case TypeKindSem::Any:
        case TypeKindSem::TypeParam:
        case TypeKindSem::Module:
            return 0;
    }
    return 0;
}

/// @brief Compute the semantic storage alignment used by aggregate layout helpers.
/// @param type Semantic type.
/// @return Required alignment in bytes under the current portable layout policy.
size_t typeAlignment(const ZannaType &type) {
    switch (type.kind) {
        case TypeKindSem::Integer:
        case TypeKindSem::Number:
        case TypeKindSem::Boolean:
        case TypeKindSem::String:
        case TypeKindSem::Ptr:
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::Enum:
        case TypeKindSem::List:
        case TypeKindSem::Map:
        case TypeKindSem::Set:
        case TypeKindSem::Function:
        case TypeKindSem::Error:
        case TypeKindSem::Optional:
        case TypeKindSem::Result:
        case TypeKindSem::Tuple:
        case TypeKindSem::Unit:
            return 8;
        case TypeKindSem::Byte:
            return 4;
        case TypeKindSem::Void:
        case TypeKindSem::Unknown:
        case TypeKindSem::Never:
        case TypeKindSem::Any:
        case TypeKindSem::TypeParam:
        case TypeKindSem::Module:
            return 1;
        case TypeKindSem::FixedArray:
            // Alignment matches element alignment
            if (!type.typeArgs.empty())
                return typeAlignment(*type.typeArgs[0]);
            return 8;
        case TypeKindSem::Struct:
            return 8; // Default alignment
    }
    return 1;
}

/// @brief Convert a semantic kind enumerator to its canonical name.
/// @param kind Semantic type kind.
/// @return Static null-terminated name, or `?` if no enumerator matches.
const char *kindToString(TypeKindSem kind) {
    switch (kind) {
        case TypeKindSem::Integer:
            return "Integer";
        case TypeKindSem::Number:
            return "Number";
        case TypeKindSem::Boolean:
            return "Boolean";
        case TypeKindSem::String:
            return "String";
        case TypeKindSem::Byte:
            return "Byte";
        case TypeKindSem::Unit:
            return "Unit";
        case TypeKindSem::Void:
            return "Void";
        case TypeKindSem::Optional:
            return "Optional";
        case TypeKindSem::Result:
            return "Result";
        case TypeKindSem::List:
            return "List";
        case TypeKindSem::Map:
            return "Map";
        case TypeKindSem::Set:
            return "Set";
        case TypeKindSem::Function:
            return "Function";
        case TypeKindSem::Tuple:
            return "Tuple";
        case TypeKindSem::Struct:
            return "Struct";
        case TypeKindSem::Class:
            return "Class";
        case TypeKindSem::Interface:
            return "Interface";
        case TypeKindSem::Enum:
            return "Enum";
        case TypeKindSem::Error:
            return "Error";
        case TypeKindSem::Ptr:
            return "Ptr";
        case TypeKindSem::Unknown:
            return "Unknown";
        case TypeKindSem::Never:
            return "Never";
        case TypeKindSem::Any:
            return "Any";
        case TypeKindSem::TypeParam:
            return "TypeParam";
        case TypeKindSem::Module:
            return "Module";
        case TypeKindSem::FixedArray:
            return "FixedArray";
    }
    return "?";
}

} // namespace il::frontends::zia
