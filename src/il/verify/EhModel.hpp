//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/EhModel.hpp
// Purpose: Declare the verifier's canonical exception-handling CFG model.
// Key invariants:
//   - Successor queries resolve through a deterministic function-local label map.
//   - Resume-token parameters are identified without mutating the borrowed IR.
// Ownership/Lifetime:
//   - EhModel borrows all functions, blocks, and instructions from its caller.
//   - The source function must outlive the model and every returned pointer or view.
// Links: il/verify/EhChecks.hpp, il/verify/ExceptionHandlerAnalysis.hpp,
//        docs/adr/0005-resume-token-provenance.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a borrowed, function-local model of EH-aware control flow.
/// @details `EhModel` resolves block labels, classifies handler and continuation
///          token parameters, records concrete `eh.push` sites, and exposes
///          successor edges with enough label metadata for branch-argument
///          provenance checks.

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/verify/BlockMap.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::verify {

/// @brief Classifies the semantic kind of an EH-aware successor edge.
/// @details Normal edges are ordinary branch-like transfers (`br`, `cbr`, and
///          `switch.i32`). Resume edges are produced by `resume.label` after the
///          resume token has been consumed.
enum class EhEdgeKind {
    Normal, ///< Ordinary control-flow transfer that preserves active EH state.
    Resume  ///< `resume.label` transfer after consuming the active resume token.
};

/// @brief Resolved successor edge for an EH-aware control transfer.
/// @details Stores both the target block and the index of the label on the
///          source instruction so callers can inspect the matching branch
///          argument bundle. Missing labels are omitted by @ref EhModel.
struct EhSuccessorEdge {
    const il::core::BasicBlock *target = nullptr; ///< Resolved destination block.
    std::size_t labelIndex = 0;                   ///< Index into Instr::labels/brArgs.
    EhEdgeKind kind = EhEdgeKind::Normal;         ///< Semantic transfer kind.
};

/// @brief Metadata describing one concrete `eh.push` instruction.
/// @details A handler label may be pushed from multiple dynamic scopes. Keeping
///          a stable push-site id lets verifier traversals distinguish those
///          scopes even when they share the same handler block.
struct EhHandlerPushSite {
    std::size_t id = 0;                            ///< Stable ordinal within the function.
    const il::core::BasicBlock *block = nullptr;   ///< Block containing the push.
    const il::core::Instr *instr = nullptr;        ///< `eh.push` instruction.
    const il::core::BasicBlock *handler = nullptr; ///< Resolved handler target.
};

/// @brief Canonical representation of a function's exception-handling graph.
class EhModel {
  public:
    /// @brief Build the EH model for @p function.
    /// @param function Function whose EH structure will be analysed.
    explicit EhModel(const il::core::Function &function);

    /// @brief Access the function used to construct the model.
    /// @return Reference to the underlying function.
    [[nodiscard]] const il::core::Function &function() const noexcept {
        return *fn;
    }

    /// @brief Retrieve the entry block for the function.
    /// @return Pointer to the entry block or nullptr when no blocks exist.
    [[nodiscard]] const il::core::BasicBlock *entry() const noexcept {
        return entryBlock;
    }

    /// @brief Determine whether the function contains EH-relevant opcodes.
    /// @return True when at least one EH opcode is present.
    [[nodiscard]] bool hasEhInstructions() const noexcept {
        return hasEh;
    }

    /// @brief Resolve a block label to its definition.
    /// @param label Basic-block label to resolve.
    /// @return Pointer to the block or nullptr when missing.
    [[nodiscard]] const il::core::BasicBlock *findBlock(std::string_view label) const;

    /// @brief Enumerate successors for a terminator instruction.
    /// @param terminator Terminator whose successors are requested.
    /// @return Vector of successor block pointers (may be empty).
    [[nodiscard]] std::vector<const il::core::BasicBlock *> gatherSuccessors(
        const il::core::Instr &terminator) const;

    /// @brief Locate the first terminator instruction in a basic block.
    /// @param block Block whose terminator should be identified.
    /// @return Pointer to the terminator or nullptr when absent.
    [[nodiscard]] const il::core::Instr *findTerminator(const il::core::BasicBlock &block) const;

    /// @brief Determine whether @p block is handler-shaped.
    /// @details A handler-shaped block is one whose first instruction is
    ///          `eh.entry`. Signature correctness is enforced by
    ///          ExceptionHandlerAnalysis before EH checks run; this query only
    ///          classifies the CFG shape.
    /// @param block Candidate basic block.
    /// @return True when the block begins with `eh.entry`.
    [[nodiscard]] bool isHandlerBlock(const il::core::BasicBlock &block) const noexcept;

    /// @brief Return the canonical resume-token parameter id for a handler block.
    /// @details The helper returns a value only for handler-shaped blocks whose
    ///          first two parameters are `Error` and `ResumeTok`. Additional
    ///          parameters do not affect this classification. EH provenance
    ///          checks use the result to confirm forwarding into `%tok`.
    /// @param block Handler-shaped block to inspect.
    /// @return Parameter id for `%tok`, or no value when the block is not a
    ///         compatible handler block.
    [[nodiscard]] std::optional<unsigned> handlerResumeTokenParam(
        const il::core::BasicBlock &block) const noexcept;

    /// @brief Return the unique resume-token parameter id for any continuation block.
    /// @details ADR 0005 permits an active token to flow through ordinary block
    ///          parameters as well as canonical handler entries. Blocks with no
    ///          token parameter, or more than one token parameter, return no value.
    /// @param block Block whose parameters should be inspected.
    /// @return The unique `ResumeTok` parameter id, or no value when unavailable.
    [[nodiscard]] std::optional<unsigned> resumeTokenParam(
        const il::core::BasicBlock &block) const noexcept;

    /// @brief Enumerate resolved successor edges for a terminator.
    /// @details Unlike @ref gatherSuccessors, this preserves the label index and
    ///          distinguishes normal branch edges from `resume.label` edges.
    ///          Malformed or unknown labels are ignored so structural verifier
    ///          diagnostics can be emitted by their existing checks.
    /// @param terminator Terminator instruction whose outgoing edges are requested.
    /// @return Resolved successor edge records in label order.
    [[nodiscard]] std::vector<EhSuccessorEdge> gatherSuccessorEdges(
        const il::core::Instr &terminator) const;

    /// @brief Find the push-site metadata for a concrete `eh.push` instruction.
    /// @details The instruction address must come from the function used to
    ///          construct this model. Unknown instructions return null, which
    ///          allows callers to ignore malformed pushes already diagnosed by
    ///          structural verification.
    /// @param instr Candidate instruction.
    /// @return Push-site metadata when @p instr is an `eh.push` known to this model.
    [[nodiscard]] const EhHandlerPushSite *findPushSite(
        const il::core::Instr &instr) const noexcept;

    /// @brief Access all concrete handler push sites in declaration order.
    /// @return Immutable vector of push-site metadata.
    [[nodiscard]] const std::vector<EhHandlerPushSite> &pushSites() const noexcept {
        return handlerPushSites;
    }

    /// @brief Access the internal label-to-block table.
    /// @return Reference to the label map.
    /// @note The returned map uses string_view keys referencing BasicBlock::label
    ///       strings. The map must not outlive the source Function.
    [[nodiscard]] const BlockMap &blockMap() const noexcept {
        return blocks;
    }

  private:
    /// @brief Borrowed function described by this model.
    const il::core::Function *fn = nullptr;

    /// @brief Borrowed first block, or null when the function has no blocks.
    const il::core::BasicBlock *entryBlock = nullptr;

    /// @brief Label-to-block lookup table using string_view keys.
    /// @note Keys reference BasicBlock::label strings owned by the Function.
    ///       This map must not outlive the Function passed to the constructor.
    BlockMap blocks;

    /// @brief Well-shaped `eh.push` sites in function storage order.
    std::vector<EhHandlerPushSite> handlerPushSites;

    /// @brief Map from a borrowed push instruction to its site-vector index.
    std::unordered_map<const il::core::Instr *, std::size_t> pushSiteByInstr;

    /// @brief Whether the function contains an opcode relevant to EH behavior.
    bool hasEh = false;
};

} // namespace il::verify
