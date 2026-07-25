//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/CollectionMethodCatalog.hpp
// Purpose: Shared collection method names, dispatch identifiers, and return
//          categories for language frontends.
// Key invariants: Zia semantic analysis and lowering consume the same catalog
//                 entries so accepted method names, dispatch IDs, and return
//                 categories cannot drift independently.
// Ownership/Lifetime: Descriptors refer to static string literals and are valid
//                     for the lifetime of the process.
// Links: src/frontends/zia/Sema_Expr_Call.cpp,
//        src/frontends/zia/Lowerer_Expr_Method.cpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the shared collection method catalog used by frontends.
/// @details A single descriptor source keeps semantic return inference and
///          lowering dispatch identifiers synchronized.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace il::frontends::common {

/**
 * @brief Collection family whose method table should be queried.
 */
enum class CollectionKind {
    List,  ///< Zia List<T> methods.
    Map,   ///< Zia Map<K,V> methods.
    Set,   ///< Zia Set<T> methods.
    String ///< String convenience methods treated like collection queries.
};

/**
 * @brief Stable collection method identifier used by semantic and lowering code.
 */
enum class CollectionMethodId {
    Unknown = 0,
    Get,
    First,
    Last,
    Set,
    Add,
    Push,
    Remove,
    RemoveAt,
    Insert,
    Find,
    IndexOf,
    Has,
    Contains,
    IsEmpty,
    Size,
    Count,
    Length,
    Len,
    Clear,
    Pop,
    Sort,
    SortDesc,
    Reverse,
    Shuffle,
    Put,
    GetOr,
    ContainsKey,
    HasKey,
    SetIfMissing,
    Keys,
    Values
};

/**
 * @brief Return category for a collection method.
 *
 * @details Frontends resolve these categories against the receiver type. For
 * example, @ref ElementType means `List<T>.first()` returns T, while
 * @ref KeySeqType means `Map<K,V>.keys()` returns a sequence of K.
 */
enum class CollectionReturnKind {
    ElementType,
    KeyType,
    ValueType,
    OptionalValueType,
    KeySeqType,
    ValueSeqType,
    Integer,
    Boolean,
    Void,
    Unknown
};

/**
 * @brief Static descriptor for one collection method.
 */
struct CollectionMethodDescriptor {
    std::string_view name; ///< Source-level method spelling.
    /// Stable dispatch identifier consumed by lowering.
    CollectionMethodId id{CollectionMethodId::Unknown};
    /// Receiver-relative result category consumed by semantic analysis.
    CollectionReturnKind returnKind{CollectionReturnKind::Unknown};
};

/**
 * @brief Normalize a collection method name for case-insensitive lookup.
 *
 * @param name Source-level method name.
 * @return Lowercase ASCII method name.
 */
[[nodiscard]] std::string normalizeCollectionMethodName(std::string_view name);

/**
 * @brief Look up a method descriptor for a specific collection kind.
 *
 * @param kind Collection family being queried.
 * @param methodName Source-level method name.
 * @return Matching descriptor, or std::nullopt when unsupported for @p kind.
 */
[[nodiscard]] std::optional<CollectionMethodDescriptor> findCollectionMethod(
    CollectionKind kind, std::string_view methodName);

/**
 * @brief Look up the dispatch identifier for a collection method name.
 *
 * @details This lookup is collection-kind agnostic and is used by lowerers that
 * already know the receiver kind from their call path. Unsupported names return
 * @ref CollectionMethodId::Unknown.
 *
 * @param methodName Source-level method name.
 * @return Dispatch identifier for @p methodName.
 */
[[nodiscard]] CollectionMethodId lookupCollectionMethod(std::string_view methodName);

} // namespace il::frontends::common
