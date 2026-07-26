//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/ParserState.hpp
// Purpose: Declares shared parser state for IL text parsing components.
// Key invariants: Tracks module/function context while parsing input.
// Ownership/Lifetime: Holds references to externally owned module data.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares mutable state shared by the textual IL parser.
 *
 * @details `ParserState` binds parser components to a caller-owned module and
 *          centralizes active function/block context, SSA identifiers,
 *          declaration indexes, unresolved branch bookkeeping, source
 *          locations, and cumulative resource accounting. The state owns only
 *          parser-side containers; referenced IL entities belong to the module.
 */

#pragma once

#include "il/core/fwd.hpp"
#include "il/io/ParserLimits.hpp"
#include "support/source_location.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::io::detail {

/// @brief Mutable context shared among IL parser helpers.
struct ParserState {
    /// @brief Module being populated while parsing proceeds.
    il::core::Module &m;
    /// @brief Resource budgets selected by the parser caller.
    il::io::ParserLimits limits;

    /// @brief Function currently under construction or nullptr at module scope.
    il::core::Function *curFn = nullptr;

    /// @brief Basic block currently receiving parsed instructions.
    il::core::BasicBlock *curBB = nullptr;

    /// @brief Mapping from SSA value names to their numeric identifiers.
    std::unordered_map<std::string, unsigned> tempIds;

    /// @brief Temp names referenced before their defining result/block parameter is parsed.
    std::unordered_set<std::string> forwardTempNames;

    /// @brief Next SSA identifier to assign to a new temporary.
    unsigned nextTemp = 0;

    /// @brief Line number of the input being processed.
    unsigned lineNo = 0;

    /// @brief Cumulative basic-block count used for parser resource limits.
    std::size_t totalBlocks = 0;

    /// @brief Cumulative instruction count used for parser resource limits.
    std::size_t totalInstructions = 0;

    /// @brief Function-name index used for constant-time declaration collision checks.
    std::unordered_set<std::string> functionNames;
    /// @brief External-declaration name index used for collision checks.
    std::unordered_set<std::string> externNames;
    /// @brief Global name index used for collision checks.
    std::unordered_set<std::string> globalNames;

    /// @brief Source location tracked via `.loc` directives.
    il::support::SourceLoc curLoc{};

    /// @brief Expected parameter count for each basic block label.
    std::unordered_map<std::string, size_t> blockParamCount;

    /// @brief Record of forward branches awaiting resolution.
    struct PendingBr {
        /// @brief Target label referenced before its definition.
        std::string label;
        /// @brief Number of arguments supplied with the branch.
        size_t args = 0;
        /// @brief Line where the unresolved branch appeared.
        unsigned line = 0;
    };

    /// @brief Collection of outstanding branch targets to validate later.
    std::vector<PendingBr> pendingBrs;

    /// @brief Tracks whether the module declared its IL version directive.
    bool sawVersion = false;

    /// @brief Construct parser state for the provided module.
    /// @param mod Caller-owned module populated by parser components.
    /// @param parserLimits Resource budgets enforced during parsing.
    ParserState(il::core::Module &mod,
                const il::io::ParserLimits &parserLimits = il::io::ParserLimits{});
};

} // namespace il::io::detail
