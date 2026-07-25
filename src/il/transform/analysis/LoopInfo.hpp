//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/analysis/LoopInfo.hpp
// Purpose: Natural loop discovery and representation for IL functions. Each
//          Loop stores header label, member block labels, latch labels, exit
//          edges, and nesting relationships. LoopInfo collects all loops for
//          a function, supporting membership queries and parent lookups.
// Key invariants:
//   - Loop membership uses block labels (not pointers) for stability across
//     IR transformations that may reallocate blocks.
//   - A natural loop is defined by a back edge (B -> H where H dominates B).
// Ownership/Lifetime: LoopInfo and its Loop entries own their label strings
//          by value. Computed from Module + Function; result is self-contained.
// Links: il/core/fwd.hpp, il/analysis/Dominators.hpp, il/analysis/CFG.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/core/fwd.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::transform {

/// @brief Describes an edge leaving a natural loop body.
/// @details An exit edge connects a block inside the loop (@c from) to a block
///          outside the loop (@c to). Exit edges are identified during loop
///          discovery by checking whether successor blocks belong to the loop body.
struct LoopExit {
    std::string from; ///< Block label inside the loop that branches out.
    std::string to;   ///< Block label outside the loop that receives control.
};

/// @brief Hash functor for heterogeneous string lookup (C++20).
struct LoopStringHash {
    /// @brief Marker enabling heterogeneous unordered-container lookup.
    using is_transparent = void;

    /// @brief Hash any key convertible to `std::string_view`.
    /// @tparam T String-like key type.
    /// @param key Label to hash.
    /// @return Standard string-view hash.
    template <typename T> [[nodiscard]] std::size_t operator()(const T &key) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(key));
    }
};

/// @brief Summary of a single natural loop discovered in a function.
struct Loop {
    /// Label identifying the loop header.
    std::string headerLabel;
    /// Labels of blocks that participate in the loop, including the header.
    std::vector<std::string> blockLabels;
    /// Labels of latch blocks (predecessors that branch back to the header).
    std::vector<std::string> latchLabels;
    /// Labels of exit edges (from -> to) leaving the loop body.
    std::vector<LoopExit> exits;
    /// Child loop headers nested immediately inside this loop.
    std::vector<std::string> childHeaders;
    /// Parent loop header if nested, empty otherwise.
    std::string parentHeader;

    /// @brief Determine whether @p label belongs to the loop body.
    /// @param label Block label to query.
    /// @return True when the cached member set contains @p label.
    [[nodiscard]] bool contains(std::string_view label) const;

  private:
    std::unordered_set<std::string, LoopStringHash, std::equal_to<>> members_;

    friend class LoopInfo;
    /// @brief Rebuild the membership cache after the label list is finalized.
    void finalize();
};

/// @brief Loop collection discovered for a function.
class LoopInfo {
  public:
    /// @brief Access the detected loops.
    /// @return Borrowed loop vector in discovery order.
    [[nodiscard]] const std::vector<Loop> &loops() const {
        return loops_;
    }

    /// @brief Find the loop whose header has label @p headerLabel.
    /// @param headerLabel Header label to query.
    /// @return Borrowed loop metadata, or null when absent.
    [[nodiscard]] const Loop *findLoop(std::string_view headerLabel) const;

    /// @brief Add a loop description owned by the summary.
    /// @param loop Loop moved into the collection after cache finalization.
    void addLoop(Loop loop);

    /// @brief Find the parent loop for @p loop header.
    /// @param loop Nested loop whose parent label is resolved.
    /// @return Borrowed parent metadata, or null for a top-level loop.
    [[nodiscard]] const Loop *parent(const Loop &loop) const;

  private:
    friend LoopInfo computeLoopInfo(il::core::Module &module, il::core::Function &function);
    std::vector<Loop> loops_;
    std::unordered_map<std::string, std::size_t, LoopStringHash, std::equal_to<>> headerIndex_;
};

/// @brief Compute loop information for @p function.
/// @param module Module used to construct CFG context.
/// @param function Function analyzed for natural loops.
/// @return Self-contained loop membership, nesting, latch, and exit metadata.
LoopInfo computeLoopInfo(il::core::Module &module, il::core::Function &function);

} // namespace il::transform
