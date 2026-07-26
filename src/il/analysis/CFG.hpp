//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/CFG.hpp
// Purpose: Control flow graph analysis utilities for IL functions -- successor/
//          predecessor queries, DFS post-order, reverse post-order, topological
//          order, and acyclicity testing. Provides the CFGContext caching layer
//          that precomputes and stores CFG metadata for efficient reuse.
// Key invariants:
//   - CFGContext must be rebuilt if the module's function/block layout changes.
//   - All query functions take const references and are read-only.
//   - Traversal orders assume entry block is the first block in the function.
// Ownership/Lifetime: CFGContext owns its internal maps and caches. Created
//          per-module; callers must ensure the referenced module outlives the
//          context. Query functions borrow state from the context.
// Links: il/analysis/Dominators.hpp, il/core/Function.hpp,
//        il/core/BasicBlock.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares cached control-flow graph construction and traversal utilities.
 *
 * @details A `CFGContext` indexes block ownership, label resolution, and
 *          predecessor/successor edges for either one function or an entire
 *          module. The free functions use that cache to derive common traversal
 *          orders and to test graph properties without repeatedly decoding IL
 *          terminators.
 *
 *          Contexts borrow the indexed module and block storage. Callers must
 *          rebuild them after mutations that can change functions, blocks,
 *          labels, or terminator targets.
 */

#pragma once

#include "support/symbol.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::core {
struct Module;
struct Function;
struct BasicBlock;
using Block = BasicBlock;
} // namespace il::core

namespace zanna::analysis {

/// @brief Structural problem encountered while indexing an unverified CFG.
struct CFGIssue {
    /// @brief Classification of a malformed CFG relationship.
    enum class Kind { DuplicateBlockLabel, UnknownTargetLabel };

    /// @brief Category of the discovered structural problem.
    Kind kind{Kind::UnknownTargetLabel};
    /// @brief Name of the function containing the problem.
    std::string function;
    /// @brief Label of the block whose indexing or terminator exposed the problem.
    std::string block;
    /// @brief Duplicate or unresolved label associated with the problem.
    std::string label;
};

/// @brief Lightweight context bundling module information for CFG queries.
///
/// Stores a reference to the active module alongside lookup tables mapping
/// basic blocks to their owning functions. Successor and predecessor lists are
/// computed eagerly so subsequent CFG utilities reuse cached edge data without
/// rescanning block terminators. The module constructor indexes every function;
/// the function constructor indexes only the requested function for
/// per-function analyses. The caller is responsible for rebuilding the context
/// if the indexed function/block layout changes.
struct CFGContext {
    /// @brief Index the control-flow graphs of every function in a module.
    /// @param module Module whose function/block storage must outlive the context.
    explicit CFGContext(il::core::Module &module);

    /// @brief Index only one function from a module.
    /// @param module Module that owns @p function and label-symbol storage.
    /// @param function Sole function whose blocks and edges are cached.
    CFGContext(il::core::Module &module, il::core::Function &function);

    /// @brief Build a CFG cache for one function whose identifier sidecars are current.
    /// @details CFG construction is read-only. This named factory documents that
    ///          callers expect symbol sidecars to be current while still retaining
    ///          string lookup fallback for unsymbolized programmatic IR.
    /// @param module Module that owns @p function and its identifier storage.
    /// @param function Sole function to index into the returned CFG context.
    /// @return A function-scoped CFG context safe for read-only concurrent analysis.
    /// @pre module identifier sidecars match their corresponding string fields.
    [[nodiscard]] static CFGContext forInternedFunction(il::core::Module &module,
                                                        il::core::Function &function);

    /// @brief Borrowed module that owns every indexed function and block.
    il::core::Module *module{nullptr};
    /// @brief Maps each indexed block to its owning function.
    std::unordered_map<const il::core::Block *, il::core::Function *> blockToFunction;
    /// @brief Cache mapping function pointers to their blocks indexed by label.
    std::unordered_map<il::core::Function *, std::unordered_map<std::string, il::core::Block *>>
        functionLabelToBlock;
    /// @brief Cache mapping function pointers to their blocks indexed by interned label.
    /// @details Populated from BasicBlock::labelSymbol when the module has been
    ///          interned. Consumers use this for hot successor resolution while
    ///          retaining string fallback for manually constructed unsymbolized IR.
    std::unordered_map<il::core::Function *,
                       std::unordered_map<il::support::Symbol, il::core::Block *>>
        functionLabelSymbolToBlock;
    /// @brief Cached successor edge targets per block constructed eagerly.
    /// @details Duplicate target blocks are preserved because branch arguments
    ///          are edge-specific in IL.
    std::unordered_map<const il::core::Block *, std::vector<il::core::Block *>> blockSuccessors;
    /// @brief Cached predecessor edge sources derived from the successor cache.
    /// @details A predecessor appears once per incoming edge.
    std::unordered_map<const il::core::Block *, std::vector<il::core::Block *>> blockPredecessors;
    /// @brief Problems found while indexing malformed or programmatically built IR.
    /// @details Analyses ignore unresolved edges conservatively; verifier/tooling
    ///          clients may inspect this list to produce structured diagnostics.
    std::vector<CFGIssue> issues;

    /// @brief Check whether CFG indexing found a structural problem.
    /// @return True when no duplicate labels or unknown targets were observed.
    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }
};

/// @brief Return successors of block @p B by inspecting its terminator.
/// @param ctx CFG cache containing @p B.
/// @param B Block whose outgoing edges are requested.
/// @return Cached list of successor edge targets (may contain duplicate blocks).
const std::vector<il::core::Block *> &successors(const CFGContext &ctx, const il::core::Block &B);

/// @brief Return predecessors of block @p B within function @p F.
/// @param ctx CFG cache containing @p B.
/// @param B Target block whose incoming edges are requested.
/// @return Cached list of predecessor edge sources (may contain duplicate blocks).
const std::vector<il::core::Block *> &predecessors(const CFGContext &ctx, const il::core::Block &B);

/// @brief Compute DFS post-order of blocks in @p F starting from the entry block.
/// @param ctx CFG cache containing @p F.
/// @param F Function whose blocks are traversed.
/// @return Blocks in post-order; the entry block is last.
std::vector<il::core::Block *> postOrder(const CFGContext &ctx, il::core::Function &F);

/// @brief Compute reverse post-order (RPO) of blocks in @p F.
/// @param ctx CFG cache containing @p F.
/// @param F Function whose blocks are traversed.
/// @return Blocks in RPO; the entry block is first.
std::vector<il::core::Block *> reversePostOrder(const CFGContext &ctx, il::core::Function &F);

/// @brief Check whether the control-flow graph of @p F has no cycles.
/// @param ctx CFG cache containing @p F.
/// @param F Function whose CFG is inspected.
/// @return True if the CFG is acyclic; false otherwise.
bool isAcyclic(const CFGContext &ctx, il::core::Function &F);

/// @brief Compute a topological order of blocks in @p F.
/// @param ctx CFG cache containing @p F.
/// @param F Function whose blocks are ordered.
/// @return Blocks in topological order; empty if @p F contains cycles.
std::vector<il::core::Block *> topoOrder(const CFGContext &ctx, il::core::Function &F);

} // namespace zanna::analysis
