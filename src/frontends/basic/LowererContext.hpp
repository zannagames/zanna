//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LowererContext.hpp
// Purpose: Defines helper context structures embedded in the Lowerer for
//          procedure-scoped state: block naming, loop tracking, error handlers,
//          and GOSUB return-address management.
// Key invariants: Context state is reset between procedures; block labels are
//                 deterministic given the procedure name and counter state.
// Ownership/Lifetime: Owned by Lowerer; references to IL objects are borrowed.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowererContext.hpp
 * @brief Defines procedure-local naming, loop, EH, and GOSUB state.
 *
 * All stored function and block pointers are non-owning. Block indexes are
 * preferred for state that must survive vector reallocation, while live block
 * pointers are refreshed by the lowering routines when necessary.
 */

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Value.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// All context structures are in the il::frontends::basic namespace
namespace il::frontends::basic {

using Function = il::core::Function;
using BasicBlock = il::core::BasicBlock;
using Value = il::core::Value;

/// @brief Deterministic block label generator scoped to a single procedure.
/// @details Produces unique, human-readable IL basic block labels by combining
///          a fixed procedure name suffix with sequential counters for each
///          control-flow construct (IF, WHILE, FOR, DO, etc.). This ensures
///          reproducible IL output regardless of compilation order.
struct BlockNamer {
    std::string proc;        ///< Procedure name suffix.
    unsigned ifCounter{0};   ///< Sequential IF identifiers.
    unsigned loopCounter{0}; ///< WHILE/FOR/DO/call_cont identifiers.
    std::unordered_map<std::string, unsigned> genericCounters; ///< Counters for other label shapes.

    /// @brief Construct a block namer for the given procedure.
    /// @param p Procedure name used as a suffix in all generated labels.
    explicit BlockNamer(std::string p) : proc(std::move(p)) {}

    /// @brief Generate the entry block label for this procedure.
    /// @return Label string in the form "entry_<proc>".
    [[nodiscard]] std::string entry() const {
        return "entry_" + proc;
    }

    /// @brief Generate the return block label for this procedure.
    /// @return Label string in the form "ret_<proc>".
    [[nodiscard]] std::string ret() const {
        return "ret_" + proc;
    }

    /// @brief Generate a label for a numbered source line.
    /// @param line Source line number.
    /// @return Label string in the form "L<line>_<proc>".
    [[nodiscard]] std::string line(int line) const {
        return "L" + std::to_string(line) + "_" + proc;
    }

    /// @brief Allocate the next sequential IF identifier.
    /// @return The allocated IF identifier.
    unsigned nextIf() {
        return ifCounter++;
    }

    /// @brief Generate the IF test block label.
    /// @param id IF identifier from nextIf().
    /// @return Label string in the form "if_test_<id>_<proc>".
    [[nodiscard]] std::string ifTest(unsigned id) const {
        return "if_test_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the IF THEN block label.
    /// @param id IF identifier from nextIf().
    /// @return Label string in the form "if_then_<id>_<proc>".
    [[nodiscard]] std::string ifThen(unsigned id) const {
        return "if_then_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the IF ELSE block label.
    /// @param id IF identifier from nextIf().
    /// @return Label string in the form "if_else_<id>_<proc>".
    [[nodiscard]] std::string ifElse(unsigned id) const {
        return "if_else_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the IF END (join) block label.
    /// @param id IF identifier from nextIf().
    /// @return Label string in the form "if_end_<id>_<proc>".
    [[nodiscard]] std::string ifEnd(unsigned id) const {
        return "if_end_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Allocate the next sequential WHILE/loop identifier.
    /// @return The allocated loop identifier.
    unsigned nextWhile() {
        return loopCounter++;
    }

    /// @brief Generate the WHILE head (condition test) block label.
    /// @param id Loop identifier from nextWhile().
    /// @return Label string in the form "while_head_<id>_<proc>".
    [[nodiscard]] std::string whileHead(unsigned id) const {
        return "while_head_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the WHILE body block label.
    /// @param id Loop identifier from nextWhile().
    /// @return Label string in the form "while_body_<id>_<proc>".
    [[nodiscard]] std::string whileBody(unsigned id) const {
        return "while_body_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the WHILE end (exit) block label.
    /// @param id Loop identifier from nextWhile().
    /// @return Label string in the form "while_end_<id>_<proc>".
    [[nodiscard]] std::string whileEnd(unsigned id) const {
        return "while_end_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Allocate the next sequential DO loop identifier.
    /// @return The allocated loop identifier.
    unsigned nextDo() {
        return loopCounter++;
    }

    /// @brief Generate the DO head (condition test) block label.
    /// @param id Loop identifier from nextDo().
    /// @return Label string in the form "do_head_<id>_<proc>".
    [[nodiscard]] std::string doHead(unsigned id) const {
        return "do_head_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the DO body block label.
    /// @param id Loop identifier from nextDo().
    /// @return Label string in the form "do_body_<id>_<proc>".
    [[nodiscard]] std::string doBody(unsigned id) const {
        return "do_body_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the DO end (exit) block label.
    /// @param id Loop identifier from nextDo().
    /// @return Label string in the form "do_end_<id>_<proc>".
    [[nodiscard]] std::string doEnd(unsigned id) const {
        return "do_end_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Allocate the next sequential FOR loop identifier.
    /// @return The allocated loop identifier.
    unsigned nextFor() {
        return loopCounter++;
    }

    /// @brief Allocate next sequential ID for a call continuation.
    /// @return The allocated continuation identifier.
    unsigned nextCall() {
        return loopCounter++;
    }

    /// @brief Generate the FOR head (condition test) block label.
    /// @param id Loop identifier from nextFor().
    /// @return Label string in the form "for_head_<id>_<proc>".
    [[nodiscard]] std::string forHead(unsigned id) const {
        return "for_head_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the FOR body block label.
    /// @param id Loop identifier from nextFor().
    /// @return Label string in the form "for_body_<id>_<proc>".
    [[nodiscard]] std::string forBody(unsigned id) const {
        return "for_body_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the FOR increment (step) block label.
    /// @param id Loop identifier from nextFor().
    /// @return Label string in the form "for_inc_<id>_<proc>".
    [[nodiscard]] std::string forInc(unsigned id) const {
        return "for_inc_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate the FOR end (exit) block label.
    /// @param id Loop identifier from nextFor().
    /// @return Label string in the form "for_end_<id>_<proc>".
    [[nodiscard]] std::string forEnd(unsigned id) const {
        return "for_end_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Build label for a synthetic call continuation block.
    /// @param id Continuation identifier from nextCall().
    /// @return Label string in the form "call_cont_<id>_<proc>".
    [[nodiscard]] std::string callCont(unsigned id) const {
        return "call_cont_" + std::to_string(id) + "_" + proc;
    }

    /// @brief Generate a label from a freeform hint with a sequential counter.
    /// @param hint Descriptive prefix for the label.
    /// @return Label string in the form "<hint>_<n>_<proc>".
    [[nodiscard]] std::string generic(const std::string &hint) {
        auto &n = genericCounters[hint];
        return hint + "_" + std::to_string(n++) + "_" + proc;
    }

    /// @brief Append the procedure suffix to a base label.
    /// @param base Label prefix.
    /// @return Label string in the form "<base>_<proc>".
    [[nodiscard]] std::string tag(const std::string &base) const {
        return base + "_" + proc;
    }
};

/// @brief Block indices produced during FOR loop lowering.
/// @details Tracks the indices of all basic blocks allocated for a FOR/NEXT
///          loop, including separate positive-step and negative-step condition
///          test blocks for variable-step loops.
struct ForBlocks {
    size_t headIdx{0};    ///< Index of the loop condition test block.
    size_t headPosIdx{0}; ///< Index of the positive-step condition test block.
    size_t headNegIdx{0}; ///< Index of the negative-step condition test block.
    size_t bodyIdx{0};    ///< Index of the loop body block.
    size_t incIdx{0};     ///< Index of the step/increment block.
    size_t doneIdx{0};    ///< Index of the loop exit block.
};

/// @brief Procedure-scoped lowering state aggregating block naming, loop tracking,
///        error handler management, and GOSUB return-address state.
/// @details Created once per procedure lowering pass and reset between procedures.
///          Provides accessor methods for each sub-state category so that internals
///          can be evolved without exposing raw member variables.
struct ProcedureContext {
    /// @brief Manages the block namer and line-to-block mapping for a procedure.
    struct BlockNameState {
        /// @brief Reset all block naming state for a new procedure.
        void reset() noexcept {
            lineBlocks_.clear();
            namer_.reset();
        }

        /// @brief Access the mutable line-number to block-index mapping.
        /// @return Mutable reference to the map.
        [[nodiscard]] std::unordered_map<int, size_t> &lineBlocks() noexcept {
            return lineBlocks_;
        }

        /// @brief Access the immutable line-number to block-index mapping.
        /// @return Const reference to the map.
        [[nodiscard]] const std::unordered_map<int, size_t> &lineBlocks() const noexcept {
            return lineBlocks_;
        }

        /// @brief Access the mutable block namer for this procedure.
        /// @return Pointer to the BlockNamer, or nullptr if not yet initialized.
        [[nodiscard]] BlockNamer *namer() noexcept {
            return namer_.get();
        }

        /// @brief Access the immutable block namer for this procedure.
        /// @return Pointer to the BlockNamer, or nullptr if not yet initialized.
        [[nodiscard]] const BlockNamer *namer() const noexcept {
            return namer_.get();
        }

        /// @brief Install a new block namer, taking ownership.
        /// @param namer Unique pointer to the new BlockNamer instance.
        void setNamer(std::unique_ptr<BlockNamer> namer) noexcept {
            namer_ = std::move(namer);
        }

        /// @brief Destroy the current block namer.
        void resetNamer() noexcept {
            namer_.reset();
        }

      private:
        /// Source/virtual line to active-function block index.
        std::unordered_map<int, size_t> lineBlocks_;
        /// Procedure-scoped label generator, absent before skeleton setup.
        std::unique_ptr<BlockNamer> namer_;
    };

    /// @brief Tracks nested loop exit targets for EXIT statement lowering.
    /// @details Maintains a stack of exit-block pointers so that EXIT FOR/DO/WHILE
    ///          can resolve the correct branch target at any nesting depth.
    struct LoopState {
        /// @brief Reset loop state for a new procedure.
        void reset() noexcept {
            function_ = nullptr;
            exitTargets_.clear();
            exitTaken_.clear();
        }

        /// @brief Bind the loop state to a new IL function and clear stacks.
        /// @param function The IL function being lowered.
        void setFunction(Function *function) noexcept {
            function_ = function;
            exitTargets_.clear();
            exitTaken_.clear();
        }

        /// @brief Push a new loop exit target onto the stack.
        /// @param exitBlock Pointer to the exit basic block for the loop.
        void push(BasicBlock *exitBlock) {
            exitTargets_.push_back(exitBlock);
            exitTaken_.push_back(false);
        }

        /// @brief Pop the innermost loop exit target from the stack.
        void pop() {
            if (exitTargets_.empty())
                return;
            exitTargets_.pop_back();
            exitTaken_.pop_back();
        }

        /// @brief Get the exit block for the innermost active loop.
        /// @return Pointer to the exit basic block, or nullptr when no loop is active.
        [[nodiscard]] BasicBlock *current() const {
            if (exitTargets_.empty() || !function_)
                return nullptr;
            return exitTargets_.back();
        }

        /// @brief Mark the innermost loop exit as having been taken.
        void markTaken() {
            if (!exitTaken_.empty())
                exitTaken_.back() = true;
        }

        /// @brief Update the exit block for the innermost loop (after block reallocation).
        /// @param exitBlock New pointer to the exit basic block.
        void refresh(BasicBlock *exitBlock) {
            if (exitTargets_.empty() || !function_)
                return;
            exitTargets_.back() = exitBlock;
        }

        /// @brief Check if the innermost loop exit has been taken.
        /// @return True when the exit was marked as taken.
        [[nodiscard]] bool taken() const {
            return !exitTaken_.empty() && exitTaken_.back();
        }

      private:
        /// Active function owning every exit target.
        Function *function_{nullptr};
        /// Nested exit block pointers from outermost to innermost loop.
        std::vector<BasicBlock *> exitTargets_;
        /// Per-loop flag set when an EXIT branch was emitted.
        std::vector<bool> exitTaken_;
    };

    /// @brief Tracks ON ERROR GOTO / RESUME state for structured error handling.
    /// @details Maintains the currently active error handler block, a mapping from
    ///          target lines to handler block indices, and reverse mappings for
    ///          RESUME dispatch.
    struct ErrorHandlerState {
        /// @brief Reset all error handler state for a new procedure.
        void reset() noexcept {
            active_ = false;
            activeIndex_.reset();
            activeLine_.reset();
            blocks_.clear();
            handlerTargets_.clear();
        }

        /// @brief Check if an error handler is currently active.
        /// @return True when an ON ERROR GOTO handler is installed.
        [[nodiscard]] bool active() const noexcept {
            return active_;
        }

        /// @brief Set whether an error handler is active.
        /// @param active True to mark a handler as active.
        void setActive(bool active) noexcept {
            active_ = active;
        }

        /// @brief Get the block index of the active error handler.
        /// @return Block index, or nullopt when no handler is active.
        [[nodiscard]] std::optional<size_t> activeIndex() const noexcept {
            return activeIndex_;
        }

        /// @brief Set the block index of the active error handler.
        /// @param index Block index, or nullopt to clear.
        void setActiveIndex(std::optional<size_t> index) noexcept {
            activeIndex_ = index;
        }

        /// @brief Get the source line targeted by the active ON ERROR GOTO.
        /// @return Target line number, or nullopt when not set.
        [[nodiscard]] std::optional<int> activeLine() const noexcept {
            return activeLine_;
        }

        /// @brief Set the source line targeted by the active ON ERROR GOTO.
        /// @param line Target line number, or nullopt to clear.
        void setActiveLine(std::optional<int> line) noexcept {
            activeLine_ = line;
        }

        /// @brief Access the mutable target-line to handler-block-index mapping.
        /// @return Mutable reference to the map.
        [[nodiscard]] std::unordered_map<int, size_t> &blocks() noexcept {
            return blocks_;
        }

        /// @brief Access the immutable target-line to handler-block-index mapping.
        /// @return Const reference to the map.
        [[nodiscard]] const std::unordered_map<int, size_t> &blocks() const noexcept {
            return blocks_;
        }

        /// @brief Access the mutable handler-block-index to target-line mapping.
        /// @return Mutable reference to the reverse map.
        [[nodiscard]] std::unordered_map<size_t, int> &handlerTargets() noexcept {
            return handlerTargets_;
        }

        /// @brief Access the immutable handler-block-index to target-line mapping.
        /// @return Const reference to the reverse map.
        [[nodiscard]] const std::unordered_map<size_t, int> &handlerTargets() const noexcept {
            return handlerTargets_;
        }

      private:
        /// Whether ON ERROR currently routes failures to a handler.
        bool active_{false};
        /// Stable index of the active handler block.
        std::optional<size_t> activeIndex_{};
        /// BASIC line targeted by the active handler.
        std::optional<int> activeLine_{};
        /// Target line to handler block index.
        std::unordered_map<int, size_t> blocks_;
        /// Handler block index to target line.
        std::unordered_map<size_t, int> handlerTargets_;
    };

    /// @brief Tracks GOSUB return-address stack state for a procedure.
    /// @details Manages the stack-pointer slot, stack array slot, and continuation
    ///          block registrations used to implement GOSUB/RETURN dispatch.
    struct GosubState {
        /// @brief Reset all GOSUB state for a new procedure.
        void reset() noexcept {
            hasPrologue_ = false;
            spSlot_ = Value{};
            stackSlot_ = Value{};
            continuationBlocks_.clear();
            stmtToIndex_.clear();
        }

        /// @brief Clear continuation registrations while keeping prologue slots.
        void clearContinuations() noexcept {
            continuationBlocks_.clear();
            stmtToIndex_.clear();
        }

        /// @brief Record the prologue slots allocated for the GOSUB stack.
        /// @param spSlot IL value for the stack-pointer slot.
        /// @param stackSlot IL value for the return-address array slot.
        void setPrologue(Value spSlot, Value stackSlot) noexcept {
            hasPrologue_ = true;
            spSlot_ = spSlot;
            stackSlot_ = stackSlot;
        }

        /// @brief Check if the GOSUB prologue has been emitted.
        /// @return True when the stack-pointer and stack-array slots are initialized.
        [[nodiscard]] bool hasPrologue() const noexcept {
            return hasPrologue_;
        }

        /// @brief Get the stack-pointer slot value.
        /// @return IL value for the GOSUB stack pointer.
        [[nodiscard]] Value spSlot() const noexcept {
            return spSlot_;
        }

        /// @brief Get the return-address stack array slot value.
        /// @return IL value for the GOSUB return-address array.
        [[nodiscard]] Value stackSlot() const noexcept {
            return stackSlot_;
        }

        /// @brief Register a continuation block for a GOSUB call site.
        /// @param stmt GOSUB statement that will branch to the subroutine.
        /// @param blockIdx Block index of the continuation (return) block.
        /// @return Zero-based continuation index stored in the return stack.
        unsigned registerContinuation(const GosubStmt *stmt, size_t blockIdx) {
            unsigned idx = static_cast<unsigned>(continuationBlocks_.size());
            continuationBlocks_.push_back(blockIdx);
            stmtToIndex_[stmt] = idx;
            return idx;
        }

        /// @brief Look up the continuation index for a previously registered GOSUB.
        /// @param stmt GOSUB statement to look up.
        /// @return Continuation index, or nullopt when not registered.
        [[nodiscard]] std::optional<unsigned> indexFor(const GosubStmt *stmt) const noexcept {
            auto it = stmtToIndex_.find(stmt);
            if (it == stmtToIndex_.end())
                return std::nullopt;
            return it->second;
        }

        /// @brief Resolve a continuation index to its block index.
        /// @param idx Continuation index from registerContinuation.
        /// @return Block index of the continuation block.
        [[nodiscard]] size_t blockIndexFor(unsigned idx) const {
            return continuationBlocks_.at(idx);
        }

        /// @brief Access the ordered list of continuation block indices.
        /// @return Const reference to the continuation block index vector.
        [[nodiscard]] const std::vector<size_t> &continuations() const noexcept {
            return continuationBlocks_;
        }

      private:
        /// Whether stack-pointer and stack-array slots were emitted.
        bool hasPrologue_{false};
        /// Stack pointer alloca.
        Value spSlot_{};
        /// Fixed-depth continuation-index array alloca.
        Value stackSlot_{};
        /// Continuation block indexes in registration order.
        std::vector<size_t> continuationBlocks_{};
        /// GOSUB statement identity to continuation ordinal.
        std::unordered_map<const GosubStmt *, unsigned> stmtToIndex_{};
    };

    /// @brief Reset all procedure-level state for a new procedure lowering pass.
    void reset() noexcept {
        function_ = nullptr;
        current_ = nullptr;
        exitIndex_ = 0;
        nextTemp_ = 0;
        boundsCheckId_ = 0;
        blockNames_.reset();
        loopState_.reset();
        errorHandlers_.reset();
        gosub_.reset();
    }

    /// @brief Get the IL function currently being lowered.
    /// @return Pointer to the active IL function.
    [[nodiscard]] Function *function() const noexcept {
        return function_;
    }

    /// @brief Set the IL function being lowered and reset loop state.
    /// @param function Pointer to the new IL function.
    void setFunction(Function *function) noexcept {
        function_ = function;
        loopState_.setFunction(function);
    }

    /// @brief Get the basic block that the builder is currently emitting into.
    /// @return Pointer to the current basic block.
    [[nodiscard]] BasicBlock *current() const noexcept {
        return current_;
    }

    /// @brief Set the current basic block for emission.
    /// @param block Pointer to the basic block to emit into.
    void setCurrent(BasicBlock *block) noexcept {
        current_ = block;
    }

    /// @brief Get the index of the current basic block within the function.
    /// @return Zero-based block index.
    [[nodiscard]] size_t currentIndex() const noexcept {
        return blockIndex(current_);
    }

    /// @brief Get the index of a basic block within the active function.
    /// @param block Basic block owned by the active function.
    /// @return Zero-based block index.
    [[nodiscard]] size_t blockIndex(BasicBlock *block) const noexcept {
        auto *f = function();
        assert(f && "blockIndex requires an active function");
        assert(block && "blockIndex requires a non-null block");
        if (!f || !block)
            return 0;
        for (size_t i = 0; i < f->blocks.size(); ++i) {
            if (&f->blocks[i] == block)
                return i;
        }
        assert(!block && "block does not belong to active function");
        // Foreign blocks resolve to the one-past-the-end sentinel; callers
        // must treat it as "not found" rather than indexing blindly.
        return f->blocks.size();
    }

    /// @brief Set the current basic block by its index within the function.
    /// @param idx Zero-based block index.
    /// @pre An active function exists and @p idx is within its block vector.
    void setCurrentByIndex(size_t idx) noexcept {
        auto *f = function();
        setCurrent(&f->blocks[idx]);
    }

    /// @brief Get the block index of the procedure's synthetic exit block.
    /// @return Block index of the exit block.
    [[nodiscard]] size_t exitIndex() const noexcept {
        return exitIndex_;
    }

    /// @brief Set the block index of the procedure's synthetic exit block.
    /// @param index Block index of the exit block.
    void setExitIndex(size_t index) noexcept {
        exitIndex_ = index;
    }

    /// @brief Get the next temporary variable ID (read-only peek).
    /// @return Current temporary ID counter value.
    [[nodiscard]] unsigned nextTemp() const noexcept {
        return nextTemp_;
    }

    /// @brief Set the next temporary variable ID counter.
    /// @param next New counter value.
    void setNextTemp(unsigned next) noexcept {
        nextTemp_ = next;
    }

    /// @brief Get the current bounds-check identifier (read-only peek).
    /// @return Current bounds-check ID counter value.
    [[nodiscard]] unsigned boundsCheckId() const noexcept {
        return boundsCheckId_;
    }

    /// @brief Set the bounds-check identifier counter.
    /// @param id New counter value.
    void setBoundsCheckId(unsigned id) noexcept {
        boundsCheckId_ = id;
    }

    /// @brief Allocate and return the next bounds-check identifier.
    /// @return A unique bounds-check ID.
    unsigned consumeBoundsCheckId() noexcept {
        return boundsCheckId_++;
    }

    /// @brief Access the mutable loop state.
    /// @return Reference to the LoopState.
    [[nodiscard]] LoopState &loopState() noexcept {
        return loopState_;
    }

    /// @brief Access the immutable loop state.
    /// @return Const reference to the LoopState.
    [[nodiscard]] const LoopState &loopState() const noexcept {
        return loopState_;
    }

    /// @brief Access the mutable block naming state.
    /// @return Reference to the BlockNameState.
    [[nodiscard]] BlockNameState &blockNames() noexcept {
        return blockNames_;
    }

    /// @brief Access the immutable block naming state.
    /// @return Const reference to the BlockNameState.
    [[nodiscard]] const BlockNameState &blockNames() const noexcept {
        return blockNames_;
    }

    /// @brief Access the mutable error handler state.
    /// @return Reference to the ErrorHandlerState.
    [[nodiscard]] ErrorHandlerState &errorHandlers() noexcept {
        return errorHandlers_;
    }

    /// @brief Access the immutable error handler state.
    /// @return Const reference to the ErrorHandlerState.
    [[nodiscard]] const ErrorHandlerState &errorHandlers() const noexcept {
        return errorHandlers_;
    }

    /// @brief Access the mutable GOSUB state.
    /// @return Reference to the GosubState.
    [[nodiscard]] GosubState &gosub() noexcept {
        return gosub_;
    }

    /// @brief Access the immutable GOSUB state.
    /// @return Const reference to the GosubState.
    [[nodiscard]] const GosubState &gosub() const noexcept {
        return gosub_;
    }

  private:
    /// Active function borrowed from the module being emitted.
    Function *function_{nullptr};
    /// Current insertion block borrowed from @ref function_.
    BasicBlock *current_{nullptr};
    /// Stable index of the synthetic exit block.
    size_t exitIndex_{0};
    /// Next SSA temporary identifier.
    unsigned nextTemp_{0};
    /// Next unique bounds-check label identifier.
    unsigned boundsCheckId_{0};
    /// Procedure block names and line mapping.
    BlockNameState blockNames_{};
    /// Nested loop exits.
    LoopState loopState_{};
    /// ON ERROR/RESUME mappings.
    ErrorHandlerState errorHandlers_{};
    /// GOSUB stack and continuation metadata.
    GosubState gosub_{};
};

} // namespace il::frontends::basic
