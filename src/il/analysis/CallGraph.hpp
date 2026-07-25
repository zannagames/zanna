//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/CallGraph.hpp
// Purpose: Lightweight direct call graph utilities for inlining heuristics.
//          Scans the module, counts direct call sites per callee name, and
//          records caller-to-callee edges. Computes strongly connected
//          components (SCCs) via Tarjan's algorithm to expose mutually
//          recursive function groups.
// Key invariants:
//   - Only direct calls with explicit callee names are tracked.
//   - Indirect calls are ignored by design.
//   - sccs is in reverse topological order (callees before callers).
//   - sccIndex maps each function name to its SCC index in sccs.
// Ownership/Lifetime: CallGraph is a value type owning its maps. The
//          buildCallGraph function takes a module reference and returns a
//          self-contained CallGraph.
// Links: il/core/fwd.hpp, il/transform/Inline.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Lightweight direct call graph utilities for IL analysis.
/// @details Provides a minimal call graph representation suitable for inlining
///          heuristics. The analysis counts direct call sites and optionally
///          records caller-to-callee edges by name.

#pragma once

#include "il/core/fwd.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::analysis {

/// @brief Direct-call graph summary for a module.
/// @details Tracks per-callee call counts, caller→callee edges, and strongly
///          connected components (SCCs). The graph only includes direct calls
///          with explicit callee names.
struct CallGraph {
    /// @brief Total direct call sites per callee name.
    std::unordered_map<std::string, unsigned> callCounts;
    /// @brief Unique caller-to-callee topology edges keyed by caller function name.
    std::unordered_map<std::string, std::vector<std::string>> edges;

    /// @brief SCCs in reverse topological order (callees before callers).
    /// @details Each element is a set of mutually recursive function names.
    ///          Single-element SCCs without a self-edge are non-recursive.
    std::vector<std::vector<std::string>> sccs;

    /// @brief Maps each function name to its SCC index in @ref sccs.
    std::unordered_map<std::string, std::size_t> sccIndex;

    /// @brief Return true if @p fn is part of a recursive SCC (size > 1 or
    ///        has a self-edge).
    /// @param fn Function name to query.
    /// @return True when @p fn participates in direct or mutual recursion.
    bool isRecursive(const std::string &fn) const;
};

/// @brief Build a direct-call graph for a module, including SCC computation.
/// @details Scans all functions and instructions, counting direct call sites
///          and recording caller→callee edges when a call has a named target.
///          Computes SCCs via Tarjan's algorithm after building the edge set.
///          Indirect calls are ignored by design.
/// @param module Module to analyze.
/// @return CallGraph summary for the module, including SCCs.
CallGraph buildCallGraph(il::core::Module &module);

} // namespace zanna::analysis
