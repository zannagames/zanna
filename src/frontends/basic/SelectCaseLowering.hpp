//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SelectCaseLowering.hpp
// Purpose: Lowers BASIC SELECT CASE statements to IL conditional branch chains.
//          Evaluates the selector once, then emits per-arm compare-and-branch
//          sequences with support for IS, TO, value list, and CASE ELSE clauses.
// Key invariants: Selector is evaluated once and stored in a temporary.
//                 All arm bodies branch to a common exit block.
// Ownership/Lifetime: Borrows Lowerer reference; does not own AST or IL.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/basic/SelectModel.hpp"
#include "frontends/basic/ast/StmtControl.hpp"

#include "support/source_location.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace il::core {
struct BasicBlock;
struct Value;
} // namespace il::core

namespace il::frontends::basic {

/// @brief BASIC statement lowerer whose mutable IL-building state is borrowed
///        by @ref SelectCaseLowering.
class Lowerer;

/// @file SelectCaseLowering.hpp
/// @brief Declares the control-flow helper used to lower BASIC `SELECT CASE`.
/// @details The helper evaluates one selector and constructs either a string
///          equality chain or a numeric comparison chain followed by an
///          integer switch. It stores block indices across operations that may
///          reallocate the function's block vector and reacquires pointers only
///          when they are needed.

/// @brief Lowers one BASIC `SELECT CASE` statement into IL control flow.
/// @details Encapsulates block allocation, dispatch-plan construction,
///          comparison-chain emission, discrete-label switch generation, and
///          arm-body lowering. The object borrows a live @ref Lowerer and
///          mutates its current function, insertion block, source location, and
///          block-name state.
/// @invariant @ref lowerer_ refers to the same @ref Lowerer for the entire
///            lifetime of this helper.
class SelectCaseLowering {
  public:
    /// @brief Binds a SELECT CASE helper to an existing lowerer.
    /// @param lowerer Lowerer whose IL context and emission helpers will be
    ///                used. It must outlive this object.
    /// @post No IL is emitted; the helper only retains a reference to
    ///       @p lowerer.
    explicit SelectCaseLowering(Lowerer &lowerer) noexcept;

    /// @brief Lowers one parsed `SELECT CASE` statement.
    /// @details A missing selector or missing active function/block causes an
    ///          early return. Otherwise the selector is evaluated exactly once,
    ///          dispatch and arm blocks are emitted, and the lowerer's current
    ///          block is left at the shared SELECT exit.
    /// @param stmt Statement whose selector, normalized model, and bodies are
    ///             consumed. The statement remains owned by the caller.
    void lower(const SelectCaseStmt &stmt);

  private:
    /// @brief Stable indices for the blocks participating in one SELECT.
    /// @details Indices, rather than pointers, survive growth of the function's
    ///          block vector while nested statements are lowered.
    struct Blocks {
        /// @brief Block that was current when SELECT lowering began.
        size_t currentIdx{};           ///< Index of the block active at SELECT entry.
        /// @brief One entry block per explicit CASE arm, in AST order.
        std::vector<size_t> armIdx;    ///< Indices of per-arm body blocks.
        /// @brief Entry block for the non-empty CASE ELSE body, when present.
        std::optional<size_t> elseIdx; ///< Index of the CASE ELSE block, if present.
        /// @brief Block containing the discrete numeric switch.
        /// @details Equals @ref currentIdx when a separate dispatch block was
        ///          not requested.
        size_t switchIdx{};            ///< Index of the dispatch/switch block.
        /// @brief Shared continuation reached by unterminated arm bodies.
        size_t endIdx{};               ///< Index of the common exit block.
    };

    /// @brief One ordered test or fallback in a comparison dispatch plan.
    struct CasePlanEntry {
        /// @brief Operation represented by a plan entry.
        enum class Kind {
            StringLabel, ///< Case tests a string literal via equality.
            RelLT,       ///< Relational: selector < value.
            RelLE,       ///< Relational: selector <= value.
            RelEQ,       ///< Relational: selector == value.
            RelGE,       ///< Relational: selector >= value.
            RelGT,       ///< Relational: selector > value.
            Range,       ///< Range test: lo <= selector <= hi.
            Default,     ///< CASE ELSE (unconditional fallback).
        };

        /// @brief Operation to emit for this entry.
        Kind kind{Kind::Default}; ///< Comparison classification.
        /// @brief Numeric operands used by the selected operation.
        /// @details Ranges use both elements; equality stores the same value in
        ///          both; less-than operations use the second; greater-than
        ///          operations use the first.
        std::pair<int32_t, int32_t> valueRange{0,
                                               0}; ///< Integer bounds for Range/relational entries.
        /// @brief Zero-based AST arm associated with a non-default entry.
        size_t armIndex{};                         ///< Index into the arm body block vector.
        /// @brief Stable destination block index, or `SIZE_MAX` while the
        ///        comparison-chain fallback still needs allocation.
        size_t targetIdx{SIZE_MAX};                ///< Block index of the branch target.
        /// @brief Source position assigned to instructions emitted for the test.
        il::support::SourceLoc loc{};              ///< Source location for diagnostics.
        /// @brief Non-owning literal text used only by @ref Kind::StringLabel.
        std::string_view strLiteral{};             ///< String literal for StringLabel entries.
    };

    /// @brief Callable that emits the boolean condition for a non-default plan
    ///        entry at the lowerer's current insertion point.
    /// @return An IL value of the lowerer's boolean type.
    using ConditionEmitter = std::function<il::core::Value(const CasePlanEntry &)>;

    /// @brief Appends the block skeleton needed by one SELECT statement.
    /// @details Creates one block per explicit arm, optionally creates CASE
    ///          ELSE and dedicated numeric-dispatch blocks, and always creates
    ///          a shared exit. The original current block is restored by index
    ///          after all appends.
    /// @param stmt Statement whose arm count determines block allocation.
    /// @param hasCaseElse Whether to allocate a CASE ELSE body block.
    /// @param needsDispatch Whether to allocate a distinct switch block.
    /// @return Stable indices for all participating blocks.
    /// @pre The lowerer's context has a current function and block.
    Blocks prepareBlocks(const SelectCaseStmt &stmt, bool hasCaseElse, bool needsDispatch);

    /// @brief Emits ordered runtime string-equality dispatch.
    /// @details Each normalized string label is tested with `rt_str_eq`.
    ///          Failure advances to the next label; total failure branches to
    ///          CASE ELSE when allocated and otherwise to the shared exit.
    /// @param stmt Statement supplying source locations.
    /// @param model Normalized string labels and their arm indices.
    /// @param blocks Previously allocated block indices.
    /// @param stringSelector Evaluated selector value; it is reused by every
    ///                       comparison and is not reevaluated.
    void lowerStringArms(const SelectCaseStmt &stmt,
                         const SelectModel &model,
                         const Blocks &blocks,
                         il::core::Value stringSelector);

    /// @brief Emits numeric relation/range tests followed by discrete dispatch.
    /// @details Relations and inclusive ranges are tested in model order with
    ///          the widened selector. The fall-through block receives a
    ///          `SwitchI32` over discrete labels using @p selector.
    /// @param stmt Statement supplying selector and diagnostic locations.
    /// @param model Normalized numeric dispatch data.
    /// @param blocks Previously allocated block indices.
    /// @param selWide Signed 64-bit selector used by relational and range
    ///                comparisons.
    /// @param selector Narrowed 32-bit selector used by the switch.
    void lowerNumericDispatch(const SelectCaseStmt &stmt,
                              const SelectModel &model,
                              const Blocks &blocks,
                              il::core::Value selWide,
                              il::core::Value selector);

    /// @brief Materializes an ordered comparison plan as conditional branches.
    /// @details The final entry must be @ref CasePlanEntry::Kind::Default. A
    ///          fallback block is allocated if its target is `SIZE_MAX`, and
    ///          intermediate false-path blocks are allocated between tests.
    /// @param startIdx Block in which the first condition is emitted.
    /// @param plan Mutable plan; an unresolved default target may be filled in.
    /// @param emitCond Callback invoked once for every non-default entry.
    /// @return Index of the default/fall-through block, which is also installed
    ///         as the lowerer's current block.
    /// @pre @p startIdx and every resolved target index address blocks in the
    ///      current function.
    size_t emitCompareChain(size_t startIdx,
                            std::vector<CasePlanEntry> &plan,
                            const ConditionEmitter &emitCond);

    /// @brief Maps a plan operation to its generated block-name stem.
    /// @param entry Entry whose operation is inspected.
    /// @return `"select_check"`, `"select_rel"`, `"select_range"`, or
    ///         `"select_dispatch"` as a static-lifetime view.
    static std::string_view blockTagFor(const CasePlanEntry &entry);

    /// @brief Emits the `SwitchI32` terminator for discrete numeric labels.
    /// @details The default successor is CASE ELSE when allocated, otherwise
    ///          the shared exit. One operand/target pair is appended for each
    ///          normalized discrete label; labels are not deduplicated here.
    /// @param stmt Statement supplying the instruction location.
    /// @param model Normalized discrete labels and arm indices.
    /// @param blocks Previously allocated target indices.
    /// @param selector Narrowed integer switch operand.
    /// @param switchIdx Block that receives and is terminated by the switch.
    void emitSwitchJumpTable(const SelectCaseStmt &stmt,
                             const SelectModel &model,
                             const Blocks &blocks,
                             il::core::Value selector,
                             size_t switchIdx);

    /// @brief Lowers one CASE body and closes any remaining fall-through path.
    /// @details Null statements are skipped and lowering stops after a statement
    ///          terminates the active block. If a current unterminated block
    ///          remains, it branches to the shared exit, reacquired by index
    ///          after nested lowering may have reallocated block storage.
    /// @param body Ordered statement nodes owned by the AST.
    /// @param entry Initial insertion block for the arm.
    /// @param loc Location assigned to a synthesized exit branch.
    /// @param endBlkIdx Stable index of the shared exit block.
    void emitArmBody(const std::vector<StmtPtr> &body,
                     il::core::BasicBlock *entry,
                     il::support::SourceLoc loc,
                     size_t endBlkIdx);

    /// @brief Borrowed owner of all mutable lowering and IL-builder state.
    Lowerer &lowerer_; ///< Parent lowerer providing context and helpers.
};

} // namespace il::frontends::basic
