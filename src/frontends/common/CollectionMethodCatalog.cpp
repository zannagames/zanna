//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/CollectionMethodCatalog.cpp
// Purpose: Implement shared collection method lookup tables.
// Key invariants: Every collection method accepted by Zia semantic analysis has
//                 one shared descriptor containing both dispatch ID and return
//                 category metadata.
// Ownership/Lifetime: Method descriptors and tables are static and immutable.
// Links: src/frontends/common/CollectionMethodCatalog.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements shared collection method descriptors and lookup tables.
//
//===----------------------------------------------------------------------===//

#include "frontends/common/CollectionMethodCatalog.hpp"

#include "frontends/common/CharUtils.hpp"
#include "frontends/common/StringUtils.hpp"

#include <array>
#include <span>
#include <unordered_map>

namespace il::frontends::common {
namespace {

using Id = CollectionMethodId;
using Ret = CollectionReturnKind;

constexpr CollectionMethodDescriptor kListMethods[] = {
    {"get", Id::Get, Ret::ElementType},     {"first", Id::First, Ret::ElementType},
    {"last", Id::Last, Ret::ElementType},   {"pop", Id::Pop, Ret::ElementType},
    {"len", Id::Len, Ret::Integer},         {"count", Id::Count, Ret::Integer},
    {"size", Id::Size, Ret::Integer},       {"length", Id::Length, Ret::Integer},
    {"find", Id::Find, Ret::Integer},       {"indexOf", Id::IndexOf, Ret::Integer},
    {"isEmpty", Id::IsEmpty, Ret::Boolean}, {"contains", Id::Contains, Ret::Boolean},
    {"has", Id::Has, Ret::Boolean},         {"remove", Id::Remove, Ret::Boolean},
    {"push", Id::Push, Ret::Void},          {"add", Id::Add, Ret::Void},
    {"insert", Id::Insert, Ret::Void},      {"set", Id::Set, Ret::Void},
    {"clear", Id::Clear, Ret::Void},        {"reverse", Id::Reverse, Ret::Void},
    {"sort", Id::Sort, Ret::Void},          {"sortDesc", Id::SortDesc, Ret::Void},
    {"shuffle", Id::Shuffle, Ret::Void},    {"removeAt", Id::RemoveAt, Ret::Void},
};

constexpr CollectionMethodDescriptor kMapMethods[] = {
    {"get", Id::Get, Ret::OptionalValueType},
    {"getOr", Id::GetOr, Ret::ValueType},
    {"set", Id::Set, Ret::Void},
    {"put", Id::Put, Ret::Void},
    {"clear", Id::Clear, Ret::Void},
    {"setIfMissing", Id::SetIfMissing, Ret::Boolean},
    {"containsKey", Id::ContainsKey, Ret::Boolean},
    {"hasKey", Id::HasKey, Ret::Boolean},
    {"has", Id::Has, Ret::Boolean},
    {"remove", Id::Remove, Ret::Boolean},
    {"len", Id::Len, Ret::Integer},
    {"size", Id::Size, Ret::Integer},
    {"count", Id::Count, Ret::Integer},
    {"length", Id::Length, Ret::Integer},
    {"keys", Id::Keys, Ret::KeySeqType},
    {"values", Id::Values, Ret::ValueSeqType},
};

constexpr CollectionMethodDescriptor kSetMethods[] = {
    {"contains", Id::Contains, Ret::Boolean},
    {"has", Id::Has, Ret::Boolean},
    {"add", Id::Add, Ret::Boolean},
    {"remove", Id::Remove, Ret::Boolean},
    {"len", Id::Len, Ret::Integer},
    {"size", Id::Size, Ret::Integer},
    {"count", Id::Count, Ret::Integer},
    {"length", Id::Length, Ret::Integer},
    {"clear", Id::Clear, Ret::Void},
};

constexpr CollectionMethodDescriptor kStringMethods[] = {
    {"length", Id::Length, Ret::Integer},
    {"count", Id::Count, Ret::Integer},
    {"size", Id::Size, Ret::Integer},
    {"isEmpty", Id::IsEmpty, Ret::Boolean},
};

/**
 * @brief Return the static descriptor span for one collection kind.
 *
 * @param kind Collection family being queried.
 * @return Span over immutable descriptors for @p kind.
 */
std::span<const CollectionMethodDescriptor> descriptorsFor(CollectionKind kind) {
    switch (kind) {
        case CollectionKind::List:
            return kListMethods;
        case CollectionKind::Map:
            return kMapMethods;
        case CollectionKind::Set:
            return kSetMethods;
        case CollectionKind::String:
            return kStringMethods;
    }
    return {};
}

/**
 * @brief Return the kind-agnostic dispatch table.
 *
 * @details The table is built once from all descriptor arrays. Repeated names
 * map to the same dispatch ID across collection kinds, so later insertions are
 * harmless and do not alter behavior.
 *
 * @return Lowercase method-name to dispatch-ID map.
 */
const std::unordered_map<std::string, CollectionMethodId> &dispatchTable() {
    /// @brief Builds the kind-agnostic collection-method dispatch table.
    /// @return Lowercase method-name map.
    static const auto table = [] {
        std::unordered_map<std::string, CollectionMethodId> out;
        out.reserve(std::size(kListMethods) + std::size(kMapMethods) + std::size(kSetMethods) +
                    std::size(kStringMethods));
        for (CollectionKind kind : {CollectionKind::List,
                                    CollectionKind::Map,
                                    CollectionKind::Set,
                                    CollectionKind::String}) {
            for (const auto &descriptor : descriptorsFor(kind)) {
                out.emplace(normalizeCollectionMethodName(descriptor.name), descriptor.id);
            }
        }
        return out;
    }();
    return table;
}

/**
 * @brief Return the per-collection descriptor lookup tables.
 *
 * @details Builds one lowercase-name map for each CollectionKind on first use.
 * Array indices intentionally match the contiguous CollectionKind enumerators.
 *
 * @return Immutable array of descriptor maps indexed by CollectionKind.
 */
const std::array<std::unordered_map<std::string, CollectionMethodDescriptor>, 4> &descriptorTables() {
    /// @brief Builds one descriptor lookup table per collection kind.
    /// @return Array indexed by `CollectionKind`.
    static const auto tables = [] {
        std::array<std::unordered_map<std::string, CollectionMethodDescriptor>, 4> out;
        for (CollectionKind kind : {CollectionKind::List,
                                    CollectionKind::Map,
                                    CollectionKind::Set,
                                    CollectionKind::String}) {
            auto &table = out[static_cast<std::size_t>(kind)];
            table.reserve(descriptorsFor(kind).size());
            for (const auto &descriptor : descriptorsFor(kind))
                table.emplace(normalizeCollectionMethodName(descriptor.name), descriptor);
        }
        return out;
    }();
    return tables;
}

} // namespace

/**
 * @brief Normalize a source method spelling for catalog lookup.
 * @param name Source-level method name.
 * @return Lowercase ASCII copy of @p name.
 */
std::string normalizeCollectionMethodName(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower += char_utils::toLower(c);
    }
    return lower;
}

/**
 * @brief Find a method descriptor supported by a collection family.
 * @param kind Receiver collection family.
 * @param methodName Source-level method spelling.
 * @return Matching descriptor, or std::nullopt when unsupported.
 */
std::optional<CollectionMethodDescriptor> findCollectionMethod(CollectionKind kind,
                                                               std::string_view methodName) {
    const auto &table = descriptorTables()[static_cast<std::size_t>(kind)];
    auto found = table.find(normalizeCollectionMethodName(methodName));
    if (found != table.end())
        return found->second;
    return std::nullopt;
}

/**
 * @brief Resolve a collection-kind-independent dispatch identifier.
 * @param methodName Source-level method spelling.
 * @return Stable method ID, or CollectionMethodId::Unknown when absent.
 */
CollectionMethodId lookupCollectionMethod(std::string_view methodName) {
    const auto &table = dispatchTable();
    auto it = table.find(normalizeCollectionMethodName(methodName));
    if (it == table.end()) {
        return CollectionMethodId::Unknown;
    }
    return it->second;
}

} // namespace il::frontends::common
