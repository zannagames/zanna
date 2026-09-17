//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ReservedSymbolGuard.hpp
// Purpose: Rename locally defined Machine IR functions whose names would claim a
//          C or POSIX runtime symbol, together with every reference to them.
// Key invariants:
//   - Only names the module itself defines are renamed; an undefined reference
//     such as `setjmp` still resolves to the C runtime.
//   - A definition and every reference to it are renamed together, so a call
//     never outlives the symbol it targets.
//   - The guarded spelling is unique within the module and stable across runs.
// Ownership/Lifetime:
//   - Operates in place on caller-owned Machine IR; owns no storage.
// Links: src/common/Mangle.hpp, src/codegen/x86_64/Backend.cpp,
//        src/codegen/aarch64/CodegenPipeline.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Keeps user symbols out of the C runtime's flat symbol namespace.
/// @details A natively linked program shares one symbol namespace with the C
///          runtime it links against. A Zia program is free to name a function
///          `floor`, but linking that definition satisfies libm's own internal
///          reference to `floor`, so the runtime's ellipse rasterizer calls the
///          game's function instead of the real one. Nothing diagnoses this: the
///          program simply computes wrong answers, or traps, only in native
///          builds. This pass renames such definitions before emission so the
///          collision cannot arise.

#include "common/Mangle.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::common {

/// @brief Rename Machine IR functions that would claim a C runtime symbol.
/// @details Builds the rename map from the module's own function definitions,
///          then rewrites each function's name and every symbol reference the
///          backend exposes through @p forEachSymbol. References are matched by
///          exact spelling, so only a label naming a renamed function changes: a
///          block label collides with a function name only when it is that
///          function's entry block, which must be renamed in step with it.
/// @tparam MFunction Backend Machine IR function type exposing a `name` member.
/// @tparam ForEachSymbol Callable invoked as `forEachSymbol(fn, visit)`, which
///         must call `visit(std::string &)` once for every mutable symbol
///         reference in @p fn: its block labels and its label operands.
/// @param mir Machine IR functions for one module, rewritten in place.
/// @param forEachSymbol Backend-specific walker over symbol references.
/// @return Number of functions renamed; zero leaves @p mir untouched.
template <typename MFunction, typename ForEachSymbol>
std::size_t guardReservedSymbols(std::vector<MFunction> &mir, ForEachSymbol forEachSymbol) {
    std::unordered_set<std::string> defined;
    defined.reserve(mir.size());
    for (const auto &fn : mir)
        defined.insert(fn.name);

    std::unordered_map<std::string, std::string> renames;
    for (const auto &fn : mir) {
        if (!zanna::common::IsReservedRuntimeName(fn.name))
            continue;
        std::string guarded = zanna::common::GuardReservedRuntimeName(fn.name);
        // A module that already defines the guarded spelling keeps both symbols
        // distinct rather than silently merging them.
        while (defined.count(guarded) != 0U)
            guarded.push_back('_');
        defined.insert(guarded);
        renames.emplace(fn.name, guarded);
    }
    if (renames.empty())
        return 0U;

    for (auto &fn : mir) {
        forEachSymbol(fn, [&renames](std::string &symbol) {
            if (auto it = renames.find(symbol); it != renames.end())
                symbol = it->second;
        });
        if (auto it = renames.find(fn.name); it != renames.end())
            fn.name = it->second;
    }
    return renames.size();
}

} // namespace zanna::codegen::common
