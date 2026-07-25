//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Intrinsics.cpp
// Purpose: Define static signatures and exact lookup for selected legacy BASIC
//          string and conversion intrinsics.
// Ownership/Lifetime: Descriptors and parameter arrays have static lifetime.
// Links: src/frontends/basic/Intrinsics.hpp,
//        src/frontends/basic/BuiltinRegistry.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Registry of BASIC intrinsic descriptors.
/// @details Provides signature constants, the canonical intrinsic table, and
///          lookup/dumping helpers used by the semantic analyser and tooling.

#include "frontends/basic/Intrinsics.hpp"

#include <array>

namespace il::frontends::basic::intrinsics {
namespace {
// Common parameter descriptors.
/// Signature: (string)
constexpr Param strParam[] = {{Type::String, false}};
/// Signature: (int)
constexpr Param intParam[] = {{Type::Int, false}};
/// Signature: (numeric)
constexpr Param numParam[] = {{Type::Numeric, false}};

/// Signature: (string, int)
constexpr Param leftRightParams[] = {
    {Type::String, false},
    {Type::Int, false},
};

/// Signature: (string, int, [int])
constexpr Param midParams[] = {
    {Type::String, false},
    {Type::Int, false},
    {Type::Int, true},
};

/// Signature: ([int], string, string)
constexpr Param instrParams[] = {
    {Type::Int, true},
    {Type::String, false},
    {Type::String, false},
};

/// Registry mapping intrinsic names to return types and parameter signatures.
//
// NOTE: Maintain the order used in docs and user-facing dumps; @ref dumpNames
// relies on this sequence to keep output stable without sorting at runtime.
constexpr Intrinsic table[] = {
    {"LEFT$", Type::String, leftRightParams, std::size(leftRightParams)},
    {"RIGHT$", Type::String, leftRightParams, std::size(leftRightParams)},
    {"MID$", Type::String, midParams, std::size(midParams)},
    {"INSTR", Type::Int, instrParams, std::size(instrParams)},
    {"LEN", Type::Int, strParam, std::size(strParam)},
    {"LTRIM$", Type::String, strParam, std::size(strParam)},
    {"RTRIM$", Type::String, strParam, std::size(strParam)},
    {"TRIM$", Type::String, strParam, std::size(strParam)},
    {"UCASE$", Type::String, strParam, std::size(strParam)},
    {"LCASE$", Type::String, strParam, std::size(strParam)},
    {"CHR$", Type::String, intParam, std::size(intParam)},
    {"ASC", Type::Int, strParam, std::size(strParam)},
    {"VAL", Type::Numeric, strParam, std::size(strParam)},
    {"STR$", Type::String, numParam, std::size(numParam)},
};
} // namespace

/// @brief Perform a linear lookup against the intrinsic registry.
///
/// The registry is small (currently a handful of entries), so a linear search
/// is sufficient and avoids the complexity of building auxiliary indices.
///
/// @param name BASIC intrinsic name such as "LEFT$".
/// @return Pointer to the matching descriptor, or nullptr if @p name is
///         unsupported.
const Intrinsic *lookup(std::string_view name) {
    for (const auto &intr : table)
        if (intr.name == name)
            return &intr;
    return nullptr;
}

/// @brief Emit all intrinsic names separated by a single space.
///
/// Names are written in @ref table order, which matches declaration order and
/// keeps user-facing listings deterministic. The implementation avoids
/// trailing whitespace by only inserting separators between entries.
///
/// @param os Output stream receiving the formatted list.
void dumpNames(std::ostream &os) {
    for (std::size_t i = 0; i < std::size(table); ++i) {
        os << table[i].name;
        if (i + 1 < std::size(table))
            os << ' ';
    }
}

} // namespace il::frontends::basic::intrinsics
