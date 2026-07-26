//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/diag_catalog.cpp
// Purpose: Implements diagnostic-code catalog lookup with prefix fallback.
// Key invariants:
//   - The catalog is materialized once and never mutated.
//   - Prefix matching returns the first family in the declared fallback order.
// Ownership/Lifetime:
//   - Static data with program lifetime.
// Links: support/diag_catalog.hpp, support/diag_catalog.def
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements diagnostic-code catalog enumeration and lookup.
/// @details The X-macro catalog is materialized as static entries, with an
///          immutable exact-code hash map and ordered family fallbacks for
///          uncataloged codes.

#include "support/diag_catalog.hpp"

#include <array>
#include <cctype>
#include <unordered_map>

namespace il::support {
namespace {
/// @brief Static diagnostic catalog materialized directly from the X-macro list.
/// @details The array has program lifetime and stores only string views into
///          literals.  Vector and map views are derived from this single source so
///          catalog iteration and lookup cannot disagree.
static constexpr std::array kDiagCatalogEntries = {
#define DIAG_CODE(code, subsystem, summary) DiagCatalogEntry{code, subsystem, summary},
#include "support/diag_catalog.def"
#undef DIAG_CODE
};

/// @brief Build the exact-code lookup map for the diagnostic catalog.
/// @return Map from stable diagnostic code to its static catalog entry.
/// @details The map is initialized once and then treated as immutable, making
///          repeated `zanna explain CODE` lookups constant-time while preserving
///          the definition-order span used for listing.
const std::unordered_map<std::string_view, const DiagCatalogEntry *> &diagCatalogMap() {
    /// @brief Build the immutable diagnostic-code lookup table on first use.
    /// @return Map whose values point into the static catalog entry array.
    static const std::unordered_map<std::string_view, const DiagCatalogEntry *> map = [] {
        std::unordered_map<std::string_view, const DiagCatalogEntry *> result;
        result.reserve(kDiagCatalogEntries.size());
        for (const auto &entry : kDiagCatalogEntries)
            result.emplace(entry.code, &entry);
        return result;
    }();
    return map;
}
} // namespace

/// @brief Get a legacy vector view of catalog entries in definition order.
/// @return Program-lifetime immutable vector containing every catalog entry.
const std::vector<DiagCatalogEntry> &diagCatalog() {
    static const std::vector<DiagCatalogEntry> entries(kDiagCatalogEntries.begin(),
                                                       kDiagCatalogEntries.end());
    return entries;
}

/// @brief Get a zero-allocation contiguous view of the compiled catalog.
/// @return Span over program-lifetime entries in definition order.
std::span<const DiagCatalogEntry> diagCatalogEntries() {
    return std::span<const DiagCatalogEntry>{kDiagCatalogEntries.data(),
                                             kDiagCatalogEntries.size()};
}

/// @brief Find an exact diagnostic-code catalog entry.
/// @param code Stable code to look up with case-sensitive byte equality.
/// @return Program-lifetime entry pointer, or nullptr when uncataloged.
const DiagCatalogEntry *findDiagCode(std::string_view code) {
    const auto &map = diagCatalogMap();
    auto it = map.find(code);
    return it == map.end() ? nullptr : it->second;
}

/// @brief Infer a diagnostic subsystem from an ordered prefix convention.
/// @details Named `V-*` prefixes are checked in table order, followed by
///          numeric BASIC, Zia-warning, and IL forms. The returned descriptions
///          are string literals with program lifetime.
/// @param code Diagnostic code whose family should be inferred.
/// @return Family description, or std::nullopt when no convention matches.
std::optional<std::string_view> diagCodeFamily(std::string_view code) {
    struct Family {
        std::string_view prefix;
        std::string_view description;
    };

    // Prefixes are matched in declared priority order.
    static constexpr Family kFamilies[] = {
        {"V-ZIA-LEX", "Zia lexer diagnostics"},
        {"V-ZIA-PARSE", "Zia parser diagnostics"},
        {"V-ZIA-LOWER", "Zia lowering invariant diagnostics"},
        {"V-ZIA-", "Zia semantic analysis diagnostics"},
        {"V-IL-", "IL verification diagnostics"},
        {"V-BC-", "Bytecode compiler diagnostics"},
        {"V-CG-", "Native codegen diagnostics"},
        {"V-CLI-", "Zanna command-line diagnostics"},
        {"V-IL-IO", "IL parser/serializer diagnostics"},
        {"V-OPT-", "IL optimizer pipeline diagnostics"},
        {"V-SRC-", "Source loading and registration diagnostics"},
        {"V-LSP-", "Language server bridge diagnostics"},
    };
    for (const auto &family : kFamilies) {
        if (code.size() >= family.prefix.size() &&
            code.substr(0, family.prefix.size()) == family.prefix)
            return family.description;
    }

    /// @brief Check whether the suffix beginning at an index is non-empty and numeric.
    /// @param start First suffix byte to inspect.
    /// @return `true` when the suffix exists and contains only decimal digits.
    auto allDigitsFrom = [&](size_t start) {
        if (code.size() <= start)
            return false;
        for (size_t i = start; i < code.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(code[i])))
                return false;
        }
        return true;
    };
    if (!code.empty() && code[0] == 'B' && allDigitsFrom(1)) {
        if (code.size() >= 2 && code[1] == '9')
            return "BASIC frontend warnings";
        return "BASIC frontend diagnostics";
    }
    if (!code.empty() && code[0] == 'W' && allDigitsFrom(1))
        return "Zia warnings";
    if (!code.empty() && code[0] == 'E' && allDigitsFrom(1))
        return "BASIC semantic analysis diagnostics";
    if (code.size() >= 3 && code[0] == 'I' && code[1] == 'L' && allDigitsFrom(2))
        return "IL parser/serializer diagnostics";
    return std::nullopt;
}

} // namespace il::support
