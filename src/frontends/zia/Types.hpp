//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Types.hpp
/// @brief Semantic type representation for the Zia programming language.
///
/// @details This file defines the semantic type system for Zia, which is
/// distinct from the syntactic type nodes in AST.hpp. While AST type nodes
/// represent how types are written in source code, the types defined here
/// represent the resolved, semantic meaning of types after name resolution.
///
/// ## Design Overview
///
/// The Zia type system includes:
///
/// **Primitive Types:**
/// - `Integer` (i64): 64-bit signed integer
/// - `Number` (f64): 64-bit IEEE 754 floating point
/// - `Boolean` (i1): True or false value
/// - `String` (str): UTF-8 string reference
/// - `Byte` (i32): 8-bit value stored as 32-bit integer (IL has no i8)
/// - `Unit`: The singleton unit value, like void but with a value
/// - `Void`: No return type for functions
///
/// **Wrapper Types:**
/// - `Optional[T]`: Nullable type, written as `T?`
/// - `Result[T]`: Success/error type for error handling
///
/// **Collection Types:**
/// - `List[T]`: Dynamic array of elements
/// - `Map[K, V]`: Key-value dictionary
/// - `Set[T]`: Collection of unique elements
///
/// **User-Defined Types:**
/// - `Struct`: Copy-semantics value type
/// - `Class`: Reference-semantics type (class)
/// - `Interface`: Abstract type contract
///
/// **Function Type:**
/// - `(A, B) -> C`: Function taking A and B, returning C
///
/// ## Type Interning
///
/// Primitive types use singleton instances to avoid duplication. The
/// `types::` namespace provides factory functions that return shared
/// pointers to canonical type instances:
///
/// ```cpp
/// TypeRef intType = types::integer();    // Always same instance
/// TypeRef strType = types::string();     // Always same instance
/// TypeRef optInt = types::optional(types::integer());  // Creates new
/// ```
///
/// ## IL Type Mapping
///
/// Zia types are mapped to IL types for code generation:
/// - `Integer` → `i64`
/// - `Number` → `f64`
/// - `Boolean` → `i1` (0 or 1)
/// - `String` → `str`
/// - Reference types → `ptr` (pointer to object)
/// - Struct values → `ptr` to separately managed inline aggregate storage
///
/// ## Type Compatibility
///
/// The type system supports:
/// - Exact type matching for primitives
/// - Subtype polymorphism for entities (inheritance)
/// - Interface implementation checking
/// - Optional unwrapping and coalescing
/// - Generic type parameter substitution
///
/// @invariant Types are immutable after construction.
/// @invariant Primitive types use singleton instances.
/// @invariant TypeRef is non-null for valid types (Unknown for unresolved).
///
/// @see AST.hpp - Syntactic type annotations
/// @see Sema.hpp - Type checking and resolution
/// @see Lowerer.hpp - Type mapping to IL
///
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/Type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
/// @name Type Reference
/// @brief Forward declaration and smart pointer for semantic types.
/// @{
//===----------------------------------------------------------------------===//

struct ZannaType;

/// @brief Shared pointer to an immutable semantic type.
/// @details Types are shared via shared_ptr for efficient comparison and
/// interning. Once created, types are never modified.
using TypeRef = std::shared_ptr<const ZannaType>;

/// @}

//===----------------------------------------------------------------------===//
/// @name Type Kinds
/// @brief Enumeration of all semantic type categories.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Semantic type kinds for Zia.
/// @details This enum categorizes all types in the Zia type system.
/// Each kind has specific semantics for operations, memory layout, and
/// code generation.
enum class TypeKindSem {
    // =========================================================================
    /// @name Primitive Types
    /// @brief Built-in struct types with fixed representation.
    /// @{
    // =========================================================================

    /// @brief 64-bit signed integer type.
    /// @details Maps to IL `i64`. Supports arithmetic, comparison, and
    /// bitwise operations. Range: -2^63 to 2^63-1.
    Integer,

    /// @brief 64-bit IEEE 754 floating-point type.
    /// @details Maps to IL `f64`. Supports arithmetic and comparison
    /// operations. Follows IEEE 754 semantics for special values.
    Number,

    /// @brief Boolean type with true/false values.
    /// @details Stored as i64 (0 for false, 1 for true) for IL compatibility.
    /// Supports logical operations (&&, ||, !).
    Boolean,

    /// @brief UTF-8 string type.
    /// @details Maps to IL `ptr` pointing to a runtime string structure.
    /// Strings are immutable and reference-counted.
    String,

    /// @brief 8-bit byte value.
    /// @details Stored as i32 because IL doesn't have an i8 type.
    /// Used for low-level byte manipulation and I/O.
    Byte,

    /// @brief Unit type with a single value ().
    /// @details Represents "void with a value". Used in Result[Unit] for
    /// operations that succeed but have no meaningful return value.
    Unit,

    /// @brief Void type indicating no return value.
    /// @details Used only for function return types. Functions with void
    /// return type don't produce a value.
    Void,

    /// @}
    // =========================================================================
    /// @name Wrapper Types
    /// @brief Types that wrap other types with additional semantics.
    /// @{
    // =========================================================================

    /// @brief Optional (nullable) type: `T?`.
    /// @details Wraps a type to allow null values. For reference types,
    /// null is represented as a null pointer. For struct types, requires
    /// a flag + value pair.
    Optional,

    /// @brief Result type for error handling: `Result[T]`.
    /// @details Represents either a success value of type T or an error.
    /// Enables functional error handling without exceptions.
    Result,

    /// @}
    // =========================================================================
    /// @name Collection Types
    /// @brief Generic collection types.
    /// @{
    // =========================================================================

    /// @brief Dynamic array type: `List[T]`.
    /// @details Heap-allocated, growable array of elements. Elements are
    /// stored contiguously. Supports index access and iteration.
    List,

    /// @brief Key-value dictionary: `Map[K, V]`.
    /// @details Hash-based dictionary for key-value pairs. Keys must be
    /// hashable (primitives, strings, or types implementing Hashable).
    Map,

    /// @brief Set of unique elements: `Set[T]`.
    /// @details Hash-based collection of unique elements. Elements must
    /// be hashable and comparable for equality.
    Set,

    /// @brief Fixed-size inline array: `T[N]`.
    /// @details Compile-time-sized array stored inline in the parent class or
    /// struct type. No heap allocation. Elements are accessed via GEP + load/store.
    /// The element type is stored in typeArgs[0]; the count in elementCount.
    FixedArray,

    /// @}
    // =========================================================================
    /// @name Function Type
    /// @{
    // =========================================================================

    /// @brief Function type: `(A, B) -> C`.
    /// @details Represents a callable with parameter types and return type.
    /// Used for function references, lambdas, and closures.
    Function,

    /// @brief Tuple type: `(A, B, C)`.
    /// @details Fixed-size, heterogeneous collection of values.
    /// Elements are accessed by index: tuple.0, tuple.1, etc.
    Tuple,

    /// @}
    // =========================================================================
    /// @name User-Defined Types
    /// @brief Types defined by the programmer.
    /// @{
    // =========================================================================

    /// @brief Struct type with copy semantics.
    /// @details Instances are copied on assignment. No identity or reference
    /// counting. Defined with the `struct` keyword.
    Struct,

    /// @brief Class type with reference semantics.
    /// @details Instances are heap-allocated with reference counting.
    /// Support inheritance and interfaces. Defined with `class` keyword.
    Class,

    /// @brief Interface type (abstract contract).
    /// @details Defines method signatures that implementing types must
    /// provide. Used for polymorphism via interface references.
    Interface,

    /// @brief Enum type: named set of integer constants.
    /// @details Each variant maps to an I64 value. Enum types are distinct
    /// from Integer — assignment between them requires explicit conversion.
    /// At the IL level, enum values are plain I64 constants.
    Enum,

    /// @}
    // =========================================================================
    /// @name Special Types
    /// @brief Types with special purposes in the type system.
    /// @{
    // =========================================================================

    /// @brief Error struct type.
    /// @details Represents an error in a Result type. Contains error
    /// information for error handling.
    Error,

    /// @brief Opaque pointer type.
    /// @details Used for FFI, thread arguments, and other low-level
    /// scenarios where a type-erased pointer is needed.
    Ptr,

    /// @brief Unknown/unresolved type placeholder.
    /// @details Used during type inference when a type hasn't been
    /// determined yet. Should be resolved before code generation.
    Unknown,

    /// @brief Bottom type (never returns).
    /// @details The type of expressions that never complete normally,
    /// such as infinite loops or always-throwing functions.
    Never,

    /// @brief Top type for interop.
    /// @details Can hold any value. Used for FFI and dynamic scenarios.
    /// Requires runtime type checks for safe use.
    Any,

    /// @}
    // =========================================================================
    /// @name Generic Type Parameter
    /// @{
    // =========================================================================

    /// @brief Generic type parameter placeholder: `T`, `U`, etc.
    /// @details Represents an uninstantiated type parameter in a generic
    /// type or function. Replaced with concrete types during instantiation.
    TypeParam,

    /// @brief Bound module namespace.
    /// @details Represents a bound module that can be used to access
    /// its exported symbols via dot notation (e.g., `colors.initColors()`).
    Module,

    /// @}
};

/// @}

//===----------------------------------------------------------------------===//
/// @name Semantic Type Structure
/// @brief The main type representation used throughout semantic analysis.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Semantic type representation.
/// @details Represents resolved types after parsing and name resolution.
/// Types are immutable once constructed and shared via TypeRef.
///
/// ## Structure
/// Each type has:
/// - `kind`: The type category (primitive, collection, user-defined, etc.)
/// - `name`: For named types (Struct, Class, Interface, TypeParam)
/// - `typeArgs`: For generic types (List[T], Map[K,V], Function types)
///
/// ## Type Predicates
/// The struct provides numerous predicate methods to check type properties:
/// - `isPrimitive()`, `isNumeric()`, `isIntegral()`
/// - `isReference()`, `isOptional()`, `isResult()`
/// - `isCallable()`, `isGeneric()`, `isUserDefined()`
///
/// ## Type Accessors
/// For compound types, accessor methods extract inner types:
/// - `innerType()`: For Optional[T], returns T
/// - `elementType()`: For List[T] or Set[T], returns T
/// - `keyType()`, `valueType()`: For Map[K,V]
/// - `paramTypes()`, `returnType()`: For Function types
struct ZannaType {
    /// @brief The type kind identifying this type's category.
    TypeKindSem kind;

    /// @brief The type name for user-defined and parameter types.
    /// @details Used for Struct, Class, Interface, and TypeParam kinds.
    /// Empty for primitive and built-in generic types.
    std::string name;

    /// @brief Type arguments for generic types.
    /// @details For example:
    /// - List[Integer]: typeArgs = [Integer]
    /// - Map[String, Integer]: typeArgs = [String, Integer]
    /// - (Int, Int) -> Bool: typeArgs = [Int, Int, Bool]
    /// - FixedArray[Integer, 64]: typeArgs = [Integer], elementCount = 64
    std::vector<TypeRef> typeArgs;

    /// @brief Element count for FixedArray types.
    /// @details Only meaningful when kind == TypeKindSem::FixedArray.
    /// For all other types this field is zero.
    size_t elementCount = 0;

    /// @brief Default constructor creates an Unknown type.
    /// @details Unknown types are placeholders during type inference.
    ZannaType() : kind(TypeKindSem::Unknown) {}

    /// @brief Construct a primitive or simple type.
    /// @param k The type kind.
    explicit ZannaType(TypeKindSem k) : kind(k) {}

    /// @brief Construct a named type (Struct, Class, Interface, TypeParam).
    /// @param k The type kind.
    /// @param n The type name.
    ZannaType(TypeKindSem k, std::string n) : kind(k), name(std::move(n)) {}

    /// @brief Construct a generic type with type arguments.
    /// @param k The type kind (List, Map, Set, Optional, Result, Function).
    /// @param args The type arguments.
    ZannaType(TypeKindSem k, std::vector<TypeRef> args) : kind(k), typeArgs(std::move(args)) {}

    /// @brief Construct a named generic type.
    /// @param k The type kind.
    /// @param n The type name.
    /// @param args The type arguments.
    /// @details Used for user-defined generic types like MyList[T].
    ZannaType(TypeKindSem k, std::string n, std::vector<TypeRef> args)
        : kind(k), name(std::move(n)), typeArgs(std::move(args)) {}

    /// @brief Construct a fixed-size array type.
    /// @param k Type kind, expected to be FixedArray.
    /// @param elemType The element type (stored in typeArgs[0]).
    /// @param count Number of elements (stored in elementCount).
    ZannaType(TypeKindSem k, TypeRef elemType, size_t count)
        : kind(k), typeArgs({std::move(elemType)}), elementCount(count) {}

    //=========================================================================
    /// @name Type Predicates
    /// @brief Methods to check type properties.
    /// @{
    //=========================================================================

    /// @brief Check if this is a primitive type.
    /// @return True for Integer, Number, Boolean, String, Byte, Unit.
    /// @details Primitive types have fixed representation and built-in
    /// operations. They are always struct types (copied on assignment).
    bool isPrimitive() const {
        switch (kind) {
            case TypeKindSem::Integer:
            case TypeKindSem::Number:
            case TypeKindSem::Boolean:
            case TypeKindSem::String:
            case TypeKindSem::Byte:
            case TypeKindSem::Unit:
                return true;
            default:
                return false;
        }
    }

    /// @brief Check if this is a numeric type.
    /// @return True for Integer, Number, Byte, Enum.
    /// @details Numeric types support arithmetic operations.
    /// Enum variants are I64 constants and participate in integer arithmetic.
    bool isNumeric() const {
        return kind == TypeKindSem::Integer || kind == TypeKindSem::Number ||
               kind == TypeKindSem::Byte || kind == TypeKindSem::Enum;
    }

    /// @brief Check if this is an integral (whole number) type.
    /// @return True for Integer, Byte, Enum.
    /// @details Integral types support bitwise operations and integer division.
    bool isIntegral() const {
        return kind == TypeKindSem::Integer || kind == TypeKindSem::Byte ||
               kind == TypeKindSem::Enum;
    }

    /// @brief Check if this is a reference type.
    /// @return True for Class, Interface, List, Map, Set.
    /// @details Reference types are heap-allocated and use reference semantics.
    /// They are passed by pointer and may be null when wrapped in Optional.
    bool isReference() const {
        return kind == TypeKindSem::Class || kind == TypeKindSem::Interface ||
               kind == TypeKindSem::List || kind == TypeKindSem::Map || kind == TypeKindSem::Set;
    }

    /// @brief Check if this is an optional type.
    /// @return True for Optional[T] types.
    /// @details Optional types can hold either a value or null.
    bool isOptional() const {
        return kind == TypeKindSem::Optional;
    }

    /// @brief Check if this is a result type.
    /// @return True for Result[T] types.
    /// @details Result types can hold either a success value or an error.
    bool isResult() const {
        return kind == TypeKindSem::Result;
    }

    /// @brief Check if this is the void type.
    /// @return True only for Void.
    /// @details Void indicates no return value from a function.
    bool isVoid() const {
        return kind == TypeKindSem::Void;
    }

    /// @brief Check if this is the unit type.
    /// @return True only for Unit.
    /// @details Unit is like void but has an actual value ().
    bool isUnit() const {
        return kind == TypeKindSem::Unit;
    }

    /// @brief Check if this is an unknown/unresolved type.
    /// @return True for Unknown types.
    /// @details Unknown types are placeholders during type inference.
    bool isUnknown() const {
        return kind == TypeKindSem::Unknown;
    }

    /// @brief Check if this is the never (bottom) type.
    /// @return True only for Never.
    /// @details Never indicates a computation that never completes normally.
    bool isNever() const {
        return kind == TypeKindSem::Never;
    }

    /// @brief Check if this is a callable (function) type.
    /// @return True for Function types.
    /// @details Callable types can be invoked with arguments.
    bool isCallable() const {
        return kind == TypeKindSem::Function;
    }

    /// @brief Check if this is a tuple type.
    /// @return True for Tuple types.
    /// @details Tuple types are fixed-size collections of potentially different types.
    bool isTuple() const {
        return kind == TypeKindSem::Tuple;
    }

    /// @brief Check if this is a generic type with type arguments.
    /// @return True if typeArgs is non-empty.
    /// @details Generic types have been instantiated with specific type arguments.
    bool isGeneric() const {
        return !typeArgs.empty();
    }

    /// @brief Check if this is a user-defined type.
    /// @return True for Struct, Class, Interface types.
    /// @details User-defined types are declared in source code.
    bool isUserDefined() const {
        return kind == TypeKindSem::Struct || kind == TypeKindSem::Class ||
               kind == TypeKindSem::Interface || kind == TypeKindSem::Enum;
    }

    /// @}
    //=========================================================================
    /// @name Type Accessors
    /// @brief Methods to access inner types for compound types.
    /// @{
    //=========================================================================

    /// @brief Get the inner type for Optional[T].
    /// @return The wrapped type T, or nullptr if not Optional.
    /// @details For `Integer?`, returns the Integer type.
    TypeRef innerType() const {
        if (kind == TypeKindSem::Optional && !typeArgs.empty())
            return typeArgs[0];
        return nullptr;
    }

    /// @brief Get the success type for Result[T].
    /// @return The success type T, or nullptr if not Result.
    /// @details For `Result[User]`, returns the User type.
    TypeRef successType() const {
        if (kind == TypeKindSem::Result && !typeArgs.empty())
            return typeArgs[0];
        return nullptr;
    }

    /// @brief Get the element type for List[T], Set[T], typed runtime collections, or FixedArray.
    /// @return The element type T, or nullptr if not a collection.
    /// @details For `List[Integer]`, `Seq[String]`, or `Integer[64]`, returns the element type.
    TypeRef elementType() const {
        if ((kind == TypeKindSem::List || kind == TypeKindSem::Set ||
             kind == TypeKindSem::FixedArray) &&
            !typeArgs.empty())
            return typeArgs[0];
        if (kind == TypeKindSem::Ptr && !typeArgs.empty() &&
            (name == "Zanna.Collections.Seq" || name == "Zanna.Collections.Queue" ||
             name == "Zanna.Collections.Stack" || name == "Zanna.Collections.Deque" ||
             name == "Zanna.Collections.List" || name == "Zanna.Collections.Ring" ||
             name == "Zanna.Collections.Heap"))
            return typeArgs[0];
        return nullptr;
    }

    /// @brief Get the key type for Map[K, V].
    /// @return The key type K, or nullptr if not a Map.
    /// @details For `Map[String, Integer]`, returns the String type.
    TypeRef keyType() const {
        if (kind == TypeKindSem::Map && typeArgs.size() >= 2)
            return typeArgs[0];
        if (kind == TypeKindSem::Ptr && typeArgs.size() >= 2 &&
            (name == "Zanna.Collections.Map" || name == "Zanna.Collections.OrderedMap" ||
             name == "Zanna.Collections.SortedMap" || name == "Zanna.Collections.Trie" ||
             name == "Zanna.Collections.FrozenMap" || name == "Zanna.Collections.DefaultMap" ||
             name == "Zanna.Collections.WeakMap" || name == "Zanna.Collections.LruCache" ||
             name == "Zanna.Collections.MultiMap"))
            return typeArgs[0];
        return nullptr;
    }

    /// @brief Get the struct type for Map[K, V].
    /// @return The struct type V, or nullptr if not a Map.
    /// @details For `Map[String, Integer]`, returns the Integer type.
    TypeRef valueType() const {
        if (kind == TypeKindSem::Map && typeArgs.size() >= 2)
            return typeArgs[1];
        if (kind == TypeKindSem::Ptr && typeArgs.size() >= 2 &&
            (name == "Zanna.Collections.Map" || name == "Zanna.Collections.OrderedMap" ||
             name == "Zanna.Collections.SortedMap" || name == "Zanna.Collections.Trie" ||
             name == "Zanna.Collections.FrozenMap" || name == "Zanna.Collections.DefaultMap" ||
             name == "Zanna.Collections.WeakMap" || name == "Zanna.Collections.LruCache" ||
             name == "Zanna.Collections.MultiMap"))
            return typeArgs[1];
        return nullptr;
    }

    /// @brief Get the return type for Function types.
    /// @return The return type, or nullptr if not a Function.
    /// @details For `(Int, Int) -> Bool`, returns Bool.
    /// The return type is the last element in typeArgs.
    TypeRef returnType() const {
        if (kind == TypeKindSem::Function && !typeArgs.empty())
            return typeArgs.back();
        return nullptr;
    }

    /// @brief Get the parameter types for Function types.
    /// @return Vector of parameter types, empty if not a Function.
    /// @details For `(Int, Int) -> Bool`, returns [Int, Int].
    /// Parameters are all typeArgs except the last (return type).
    std::vector<TypeRef> paramTypes() const {
        if (kind == TypeKindSem::Function && typeArgs.size() > 1) {
            return std::vector<TypeRef>(typeArgs.begin(), typeArgs.end() - 1);
        }
        return {};
    }

    /// @brief Get the element types for Tuple types.
    /// @return Vector of element types, empty if not a Tuple.
    /// @details For `(Int, String)`, returns [Int, String].
    const std::vector<TypeRef> &tupleElementTypes() const {
        return typeArgs;
    }

    /// @brief Get a specific tuple element type.
    /// @param index The element index.
    /// @return The type at the given index, or nullptr if out of bounds or not a tuple.
    TypeRef tupleElementType(size_t index) const {
        if (kind == TypeKindSem::Tuple && index < typeArgs.size())
            return typeArgs[index];
        return nullptr;
    }

    /// @}
    //=========================================================================
    /// @name Type Comparison
    /// @brief Methods for comparing types.
    /// @{
    //=========================================================================

    /// @brief Check if this type equals another type.
    /// @param other The type to compare with.
    /// @return True if the types are structurally equal.
    /// @details Compares kind, name, and all type arguments recursively.
    bool equals(const ZannaType &other) const;

    /// @brief Check if a source type can be assigned to this type.
    /// @param source The source type being assigned.
    /// @return True if assignment is valid.
    /// @details Considers subtyping for entities and interface implementation.
    bool isAssignableFrom(const ZannaType &source) const;

    /// @brief Check if this type can be converted to a target type.
    /// @param target The target type for conversion.
    /// @return True if conversion is possible.
    /// @details Includes implicit conversions (e.g., Int to Number).
    bool isConvertibleTo(const ZannaType &target) const;

    /// @}
    //=========================================================================
    /// @name String Representation
    /// @{
    //=========================================================================

    /// @brief Get a human-readable string representation of this type.
    /// @return String like "Integer", "List[String]", "Map[String, Integer]".
    /// @details Used for stable internal/debug output. Named runtime handles
    ///          preserve the legacy opaque `Ptr` spelling here.
    std::string toString() const;

    /// @brief Get a developer-facing string representation of this type.
    /// @return String like "Integer", "GUI.Font", "Map[String, Integer]".
    /// @details Used for diagnostics, completion, hover, and editor tooling.
    std::string toDisplayString() const;

    /// @}
};

/// @}

//===----------------------------------------------------------------------===//
/// @name Type Factory Functions
/// @brief Factory functions for creating type instances.
/// @details These functions provide a clean API for creating types and
/// ensure singleton behavior for primitive types.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Type factory namespace.
/// @details Provides singleton instances for primitives and constructors
/// for compound types. Using these functions ensures proper type interning.
namespace types {

// =========================================================================
/// @name Primitive Type Singletons
/// @brief Return the singleton instance for each primitive type.
/// @details These functions always return the same shared instance.
/// @{
// =========================================================================

/// @brief Get the Integer type (64-bit signed integer).
/// @return The singleton Integer type.
TypeRef integer();

/// @brief Get the Number type (64-bit floating point).
/// @return The singleton Number type.
TypeRef number();

/// @brief Get the Boolean type.
/// @return The singleton Boolean type.
TypeRef boolean();

/// @brief Get the String type.
/// @return The singleton String type.
TypeRef string();

/// @brief Get the Byte type.
/// @return The singleton Byte type.
TypeRef byte();

/// @brief Get the Unit type.
/// @return The singleton Unit type.
TypeRef unit();

/// @brief Get the Void type.
/// @return The singleton Void type.
TypeRef voidType();

/// @brief Get the Error type.
/// @return The singleton Error type.
TypeRef error();

/// @brief Get the Ptr (opaque pointer) type.
/// @return The singleton Ptr type.
TypeRef ptr();

/// @brief Get the Unknown type placeholder.
/// @return The singleton Unknown type.
TypeRef unknown();

/// @brief Get the Never (bottom) type.
/// @return The singleton Never type.
TypeRef never();

/// @brief Get the Any (top) type.
/// @return The singleton Any type.
TypeRef any();

/// @}
// =========================================================================
/// @name Generic Type Constructors
/// @brief Create compound types with type arguments.
/// @{
// =========================================================================

/// @brief Create an Optional[T] type.
/// @param inner The inner type T.
/// @return A new Optional type wrapping T.
/// @details Creates a nullable version of the inner type.
TypeRef optional(TypeRef inner);

/// @brief Create a Result[T] type.
/// @param successType The success type T.
/// @return A new Result type.
/// @details Creates an error-handling type.
TypeRef result(TypeRef successType);

/// @brief Create a List[T] type.
/// @param element The element type T.
/// @return A new List type.
/// @details Creates a dynamic array type.
TypeRef list(TypeRef element);

/// @brief Create a typed Seq (lazy sequence) type for rt_seq-returning runtime functions.
/// @param element The element type T.
/// @return A Ptr type carrying the element type, representing an rt_seq.
/// @details Represented as Ptr{name="Zanna.Collections.Seq", typeArgs=[T]}.
/// This allows the lowerer to use kSeqLen/kSeqGet instead of kListCount/kListGet,
/// since rt_seq and rt_list have incompatible internal structures.
TypeRef seqOf(TypeRef element);

/// @brief Create a typed Future[T] runtime type.
/// @param payload The value produced by awaiting the future.
/// @return A Ptr type carrying the payload type, representing Zanna.Threads.Future.
TypeRef futureOf(TypeRef payload);

/// @brief Create a Set[T] type.
/// @param element The element type T.
/// @return A new Set type.
/// @details Creates a unique collection type.
TypeRef set(TypeRef element);

/// @brief Create a Map[K, V] type.
/// @param key The key type K.
/// @param value The struct type V.
/// @return A new Map type.
/// @details Creates a dictionary type.
TypeRef map(TypeRef key, TypeRef value);

/// @brief Create a function type.
/// @param params The parameter types.
/// @param ret The return type.
/// @return A new Function type.
/// @details For `(A, B) -> C`, params = [A, B], ret = C.
TypeRef function(std::vector<TypeRef> params, TypeRef ret);

/// @brief Create a tuple type.
/// @param elements The element types.
/// @return A new Tuple type.
/// @details For `(A, B)`, elements = [A, B].
TypeRef tuple(std::vector<TypeRef> elements);

/// @}
// =========================================================================
/// @name User-Defined Type Constructors
/// @brief Create types for user-defined struct, class, and interface types.
/// @{
// =========================================================================

/// @brief Create a struct type reference.
/// @param name The struct type name.
/// @param typeParams Optional type parameters for generic types.
/// @return A new Struct type.
/// @details Struct types have copy semantics.
TypeRef structType(const std::string &name, std::vector<TypeRef> typeParams = {});

/// @brief Create a class type reference.
/// @param name The class type name.
/// @param typeParams Optional type parameters for generic types.
/// @return A new Class type.
/// @details Class types have reference semantics.
TypeRef classType(const std::string &name, std::vector<TypeRef> typeParams = {});

/// @brief Create an interface type reference.
/// @param name The interface type name.
/// @param typeParams Optional type parameters for generic types.
/// @return A new Interface type.
/// @details Interface types define abstract contracts.
TypeRef interface(const std::string &name, std::vector<TypeRef> typeParams = {});

/// @brief Create an enum type reference.
/// @param name The enum type name.
/// @return A new Enum type.
/// @details Enum types define named integer constants.
TypeRef enumType(const std::string &name);

/// @brief Clear the interface implementation registry.
/// @details Called by the semantic analyzer to avoid cross-module leakage.
void clearInterfaceImplementations();

/// @brief Record that @p typeName implements @p interfaceName.
/// @param typeName Concrete nominal type name.
/// @param interfaceName Canonical implemented interface name.
void registerInterfaceImplementation(const std::string &typeName, const std::string &interfaceName);

/// @brief Check whether @p typeName implements @p interfaceName.
/// @param typeName Concrete class or struct name.
/// @param interfaceName Canonical interface name.
/// @return True for a direct implementation or one inherited from a registered base class.
bool implementsInterface(const std::string &typeName, const std::string &interfaceName);

/// @brief Clear all class inheritance registrations.
void clearClassInheritance();

/// @brief Register that @p childName extends @p parentName.
/// @param childName Derived class name.
/// @param parentName Direct base class name.
void registerClassInheritance(const std::string &childName, const std::string &parentName);

/// @brief Check whether @p childName is a subclass of @p parentName.
/// @param childName Candidate derived class.
/// @param parentName Candidate ancestor.
/// @return True when the registered parent chain reaches @p parentName.
bool isSubclassOf(const std::string &childName, const std::string &parentName);

/// @brief Create a type parameter placeholder.
/// @param name The type parameter name (e.g., "T", "U").
/// @return A new TypeParam type.
/// @details Used for uninstantiated generic type parameters.
TypeRef typeParam(const std::string &name);

/// @brief Create a runtime class type (pointer type with a name).
/// @param name The full runtime class name (e.g., "Zanna.Graphics.Canvas").
/// @return A new pointer type that carries the class name.
/// @details Used for runtime classes where we need to track the type name
/// for method call resolution.
TypeRef runtimeClass(const std::string &name);

/// @brief Create a parameterized runtime class type.
/// @param name Fully qualified runtime class name.
/// @param typeArgs Element, key/value, or payload metadata carried by the handle.
/// @return A named pointer type retaining @p typeArgs for semantic refinement.
TypeRef runtimeClass(const std::string &name, std::vector<TypeRef> typeArgs);

/// @brief Create a module namespace type.
/// @param name The module name (e.g., "colors").
/// @return A new module type for accessing bound symbols.
/// @details Used for bound modules to enable qualified access like `colors.func()`.
TypeRef module(const std::string &name);

/// @brief Create a fixed-size array type: `T[N]`.
/// @param elemType The element type T.
/// @param count Number of elements N (compile-time constant).
/// @return A new FixedArray type.
/// @details Used for inline array fields in class types. No heap allocation.
TypeRef fixedArray(TypeRef elemType, size_t count);

/// @}

} // namespace types

/// @}

//===----------------------------------------------------------------------===//
/// @name IL Type Mapping
/// @brief Functions for mapping Zia types to IL types.
/// @details These functions bridge the semantic type system with the
/// intermediate language (IL) representation used for code generation.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Maps Zia semantic types to IL primitive types.
/// @param type The Zia type to map.
/// @return The corresponding IL type kind.
///
/// @details Type mapping rules:
/// - Integer → i64
/// - Number → f64
/// - Boolean → i1
/// - String → str
/// - Byte → i32 (IL has no i8)
/// - Class → ptr (pointer to object)
/// - List/Map/Set → ptr (pointer to collection)
/// - Optional of reference → ptr (null for none)
/// - Optional of value → ptr (nullable boxed payload)
///
/// @note Reference types (Class, collections) map to Ptr.
/// @note Optionals of primitive and struct types carry nullable boxed payloads.
il::core::Type::Kind toILType(const ZannaType &type);

/// @brief Get the size in bytes for a type in memory.
/// @param type The type to get the size for.
/// @return Size in bytes, or 0 for unsized types.
///
/// @details Size rules:
/// - Integer: 8 bytes
/// - Number: 8 bytes
/// - Boolean: 8 bytes (stored as i64)
/// - Byte: 4 bytes (stored as i32)
/// - String: pointer size (8 bytes on 64-bit)
/// - Class: pointer size
/// - Collections: pointer size
size_t typeSize(const ZannaType &type);

/// @brief Get the alignment in bytes for a type.
/// @param type The type to get alignment for.
/// @return Alignment in bytes.
///
/// @details Alignment typically matches size for primitive types.
/// Composite types may have stricter alignment requirements.
size_t typeAlignment(const ZannaType &type);

/// @brief Convert type kind to human-readable string.
/// @param kind The type kind to convert.
/// @return String name like "Integer", "Class", "List".
///
/// @details Used for error messages and debugging output.
const char *kindToString(TypeKindSem kind);

/// @}

} // namespace il::frontends::zia
