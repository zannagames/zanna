//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/BlockManager.hpp
// Purpose: Basic block creation and management for language frontends.
//
// This provides a unified abstraction for creating and tracking basic blocks
// during lowering. Language frontends need deterministic block naming and
// insertion point management.
//
// Key invariants:
//   * Generated block names use a monotonically increasing suffix.
//   * A current block is accessed only after a builder/function pair is bound.
//   * Block indices remain stable within the bound function.
// Ownership: Holds non-owning pointers to the builder and function; both must
//            outlive every operation performed through the manager.
// References: src/il/build/IRBuilder.hpp, src/il/core/Function.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares shared frontend basic-block creation and navigation.
/// @details BlockManager centralizes deterministic naming, insertion-point
///          updates, and bound-state validation for frontend lowerers.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/build/IRBuilder.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

namespace il::frontends::common {

/// @brief Manages basic block creation, naming, and insertion point tracking.
/// @details Provides deterministic block naming and tracks the current block
///          for instruction emission. Used by all language frontends.
class BlockManager {
  public:
    using Function = il::core::Function;
    using BasicBlock = il::core::BasicBlock;

    /// @brief Default constructor creates an unbound manager.
    /// @post Operations that require a function throw until bind() or reset()
    ///       supplies the necessary state.
    BlockManager() = default;

    /// @brief Construct with an IR builder and function.
    /// @details The pointers are borrowed and are not validated until an
    ///          operation requiring bound state is invoked.
    /// @param builder The IR builder for block creation.
    /// @param func The function to manage blocks for.
    BlockManager(il::build::IRBuilder *builder, Function *func)
        : builder_(builder), currentFunc_(func) {}

    /// @brief Bind to a new function (resets block counter).
    /// @details Also selects block index zero as the prospective current block;
    ///          no builder insertion point is changed until setBlock() is used.
    /// @param builder The IR builder.
    /// @param func The function to manage.
    void bind(il::build::IRBuilder *builder, Function *func) {
        builder_ = builder;
        currentFunc_ = func;
        currentBlockIdx_ = 0;
        blockCounter_ = 0;
    }

    /// @brief Reset for a new function without changing the builder.
    /// @details Clears the generated-name counter and prospective current index.
    /// @param func The new function to manage.
    void reset(Function *func) {
        currentFunc_ = func;
        currentBlockIdx_ = 0;
        blockCounter_ = 0;
    }

    // =========================================================================
    // Block Creation
    // =========================================================================

    /// @brief Create a new basic block with a unique name.
    /// @details Appends `_<counter>` to @p base and increments the counter after
    ///          selecting the name.
    /// @param base Base name for the block (e.g., "if_then", "loop_body").
    /// @return Index of the created block within the function.
    [[nodiscard]] std::size_t createBlock(const std::string &base) {
        requireBound();
        std::ostringstream oss;
        oss << base << "_" << blockCounter_++;
        builder_->createBlock(*currentFunc_, oss.str());
        return currentFunc_->blocks.size() - 1;
    }

    /// @brief Create a block with an exact name (no counter suffix).
    /// @details Rejects a label already present in the bound function.
    /// @param name Exact name for the block.
    /// @return Index of the created block.
    /// @throws std::logic_error If no builder/function pair is bound.
    /// @throws std::invalid_argument If @p name duplicates an existing label.
    [[nodiscard]] std::size_t createBlockExact(const std::string &name) {
        requireBound();
        for (const auto &existing : currentFunc_->blocks) {
            if (existing.label == name)
                throw std::invalid_argument("duplicate basic block label: " + name);
        }
        builder_->createBlock(*currentFunc_, name);
        return currentFunc_->blocks.size() - 1;
    }

    // =========================================================================
    // Block Navigation
    // =========================================================================

    /// @brief Set the current block for instruction emission.
    /// @details Updates both the manager's index and the IR builder insertion
    ///          point.
    /// @param blockIdx Index of the block to make current.
    /// @throws std::logic_error If the manager is unbound.
    /// @throws std::out_of_range If @p blockIdx is not present.
    void setBlock(std::size_t blockIdx) {
        requireBound();
        if (blockIdx >= currentFunc_->blocks.size())
            throw std::out_of_range("basic block index out of range");
        currentBlockIdx_ = blockIdx;
        builder_->setInsertPoint(currentFunc_->blocks[blockIdx]);
    }

    /// @brief Get the current block.
    /// @return Pointer to the current basic block.
    /// @throws std::logic_error If the manager is unbound or has no block at
    ///         the current index.
    [[nodiscard]] BasicBlock *currentBlock() {
        requireCurrentBlock();
        return &currentFunc_->blocks[currentBlockIdx_];
    }

    /// @brief Get the current block (const).
    /// @return Const pointer to the current basic block.
    /// @throws std::logic_error If the manager is unbound or has no current block.
    [[nodiscard]] const BasicBlock *currentBlock() const {
        requireCurrentBlock();
        return &currentFunc_->blocks[currentBlockIdx_];
    }

    /// @brief Get a block by index.
    /// @param idx Index of the block.
    /// @return Reference to the block.
    /// @throws std::logic_error If the manager is unbound.
    /// @throws std::out_of_range If @p idx is not present.
    [[nodiscard]] BasicBlock &getBlock(std::size_t idx) {
        requireBound();
        return currentFunc_->blocks.at(idx);
    }

    /// @brief Get the current block index.
    /// @return Stored insertion block index.
    [[nodiscard]] std::size_t currentBlockIndex() const noexcept {
        return currentBlockIdx_;
    }

    /// @brief Get the label for a block by index.
    /// @param idx Index of the block.
    /// @return The block's label.
    /// @throws std::logic_error If the manager is unbound.
    /// @throws std::out_of_range If @p idx is not present.
    [[nodiscard]] const std::string &getBlockLabel(std::size_t idx) const {
        requireBound();
        return currentFunc_->blocks.at(idx).label;
    }

    // =========================================================================
    // State Queries
    // =========================================================================

    /// @brief Check if the current block is terminated.
    /// @return True when the current block already contains a terminator.
    /// @throws std::logic_error If no current block exists.
    [[nodiscard]] bool isTerminated() const {
        requireCurrentBlock();
        return currentFunc_->blocks[currentBlockIdx_].terminated;
    }

    /// @brief Get the number of blocks in the current function.
    /// @return Number of blocks owned by the bound function.
    /// @throws std::logic_error If the manager is unbound.
    [[nodiscard]] std::size_t blockCount() const {
        requireBound();
        return currentFunc_->blocks.size();
    }

    /// @brief Get the current function.
    /// @return Borrowed function pointer, or nullptr while unbound.
    [[nodiscard]] Function *function() {
        return currentFunc_;
    }

    /// @brief Get the current function (const).
    /// @return Borrowed const function pointer, or nullptr while unbound.
    [[nodiscard]] const Function *function() const {
        return currentFunc_;
    }

    /// @brief Get the next block counter value (for external naming).
    /// @return Suffix that createBlock() will assign next.
    [[nodiscard]] unsigned nextBlockId() const noexcept {
        return blockCounter_;
    }

    /// @brief Restore the next block counter value (for saved/restore contexts).
    /// @param nextId Next suffix value to use when creating new blocks.
    void setNextBlockId(unsigned nextId) noexcept {
        blockCounter_ = nextId;
    }

  private:
    /// @brief Require both a builder and function binding.
    /// @throws std::logic_error If either borrowed pointer is null.
    void requireBound() const {
        if (!builder_ || !currentFunc_)
            throw std::logic_error("BlockManager operation requires a bound builder and function");
    }

    /// @brief Require a valid current block in the bound function.
    /// @throws std::logic_error If the manager is unbound or the stored block
    ///         index is outside the function's block array.
    void requireCurrentBlock() const {
        requireBound();
        if (currentBlockIdx_ >= currentFunc_->blocks.size())
            throw std::logic_error("BlockManager has no current basic block");
    }

    /// @brief Borrowed builder used to create blocks and set insertion points.
    il::build::IRBuilder *builder_{nullptr};
    /// @brief Borrowed function whose blocks are managed.
    Function *currentFunc_{nullptr};
    /// @brief Index of the current insertion block.
    std::size_t currentBlockIdx_{0};
    /// @brief Suffix assigned to the next generated block name.
    unsigned blockCounter_{0};
};

} // namespace il::frontends::common
