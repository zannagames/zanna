//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/utils/Utils.cpp
// Purpose: Provide shared helper utilities for inspecting IL blocks and
//          instructions without pulling in heavy analysis headers.
// Links: docs/internals/codemap.md#il-utils
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements lightweight inspection helpers for IL blocks.
/// @details Centralising the helpers here keeps cross-cutting utilities such as
///          `belongsToBlock` and `isTerminator` available without bloating the
///          headers included throughout the compiler pipeline.
#include "il/analysis/Dominators.hpp"
#include "il/utils/Utils.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace zanna::il {

/// @brief Determine whether instruction @p I resides in block @p B.
/// @details Performs a linear scan over the block's instruction list and
///          compares addresses for equality.  The helper intentionally avoids
///          more expensive IR queries because callers typically use it in
///          assertion checks or debug-only validation contexts.
/// @param I Instruction to locate.
/// @param B Candidate parent block.
/// @return True when @p I is one of @p B's instructions; false otherwise.
bool belongsToBlock(const Instruction &I, const Block &B) {
    for (const auto &inst : B.instructions) {
        if (&inst == &I) {
            return true;
        }
    }
    return false;
}

/// @brief Fetch the terminating instruction of @p B, if any.
/// @details Blocks store their terminator, when present, as the final
///          instruction.  The helper checks the last instruction and returns it
///          when @ref isTerminator confirms the opcode category.  Returning
///          `nullptr` when the block lacks a terminator allows callers to gate
///          validation logic without exceptions.
/// @param B Block to inspect.
/// @return Pointer to the terminator instruction or nullptr if the block is
///         empty or the last instruction is non-terminating.
Instruction *terminator(Block &B) {
    if (B.instructions.empty()) {
        return nullptr;
    }
    Instruction &last = B.instructions.back();
    return isTerminator(last) ? &last : nullptr;
}

/// @brief Classify whether instruction @p I terminates control flow.
/// @details Recognises the opcodes that end a basic block according to the IL
///          specification (branches, returns, traps, and EH resumes).  Anything
///          outside that set is treated as a non-terminator.  Keeping the logic
///          centralised ensures passes agree on which instructions must end
///          basic blocks.
/// @param I Instruction to inspect.
/// @return True when @p I.op is a terminating opcode; false otherwise.
bool isTerminator(const Instruction &I) {
    return ::il::core::getOpcodeInfo(I.op).isTerminator;
}

/// @brief Check whether a block's current final instruction is a terminator.
/// @details Consults the instruction vector rather than the separately stored
///          `terminated` flag, which may lag behind direct vector mutations.
/// @param B Block to inspect.
/// @return `true` when @p B is nonempty and ends with a registered terminator.
bool isTerminated(const Block &B) {
    return !B.instructions.empty() && isTerminator(B.instructions.back());
}

/// @brief Replace all uses of temporary @p tempId with @p replacement in @p F.
/// @details Iterates every instruction and branch argument list, swapping
///          occurrences of temporary @p tempId with the replacement value.
///          This helper is used when optimization passes eliminate instructions
///          and need to redirect all consumers to a new SSA value.
/// @param F Function whose instructions are rewritten.
/// @param tempId Temporary identifier to replace.
/// @param replacement Replacement value assigned to each occurrence.
/// @sideeffect Mutates operands and branch arguments in place.
void replaceAllUses(::il::core::Function &F,
                    unsigned tempId,
                    const ::il::core::Value &replacement) {
    using ::il::core::Value;

    for (auto &B : F.blocks) {
        for (auto &I : B.instructions) {
            // Replace in instruction operands
            for (auto &Op : I.operands) {
                if (Op.kind == Value::Kind::Temp && Op.id == tempId) {
                    Op = replacement;
                }
            }

            // Replace in branch arguments
            for (auto &argList : I.brArgs) {
                for (auto &Arg : argList) {
                    if (Arg.kind == Value::Kind::Temp && Arg.id == tempId) {
                        Arg = replacement;
                    }
                }
            }
        }
    }
}

/// @brief Test dominance from an already-materialized dominator tree.
/// @details Uses the public immediate-dominator map directly so this utility
///          does not depend on the out-of-line `DomTree::dominates` symbol. That
///          keeps `il_utils` usable in small tests that do not link `il_analysis`.
/// @param domTree Dominator tree whose map is queried.
/// @param dominator Candidate dominating block.
/// @param block Candidate dominated block.
/// @return True when @p dominator is on @p block's idom chain.
bool dominatesInTree(const ::zanna::analysis::DomTree &domTree,
                     const ::il::core::BasicBlock *dominator,
                     const ::il::core::BasicBlock *block) {
    if (!dominator || !block)
        return false;
    const auto *current = block;
    while (current) {
        if (current == dominator)
            return true;
        auto it = domTree.idom.find(const_cast<::il::core::BasicBlock *>(current));
        if (it == domTree.idom.end() || it->second == current)
            break;
        current = it->second;
    }
    return false;
}

/// @brief Replace uses of @p tempId only in instructions dominated by @p rootBlock.
/// @details This dominance-aware variant is used by CSE/GVN when the replacement
///          value may be another temporary. It keeps same-block replacements after
///          the redundant definition and rewrites all instructions in blocks
///          dominated by that definition block.
/// @param F Function whose operands are rewritten.
/// @param tempId Temporary identifier to replace.
/// @param replacement Dominating replacement value.
/// @param rootBlock Block containing the redundant definition.
/// @param rootInstrIndex Instruction index of the redundant definition.
/// @param domTree Dominator tree for @p F.
/// @sideeffect Mutates operands and branch arguments in place.
void replaceUsesDominatedBy(::il::core::Function &F,
                            unsigned tempId,
                            const ::il::core::Value &replacement,
                            const ::il::core::BasicBlock &rootBlock,
                            std::size_t rootInstrIndex,
                            const ::zanna::analysis::DomTree &domTree) {
    using ::il::core::BasicBlock;
    using ::il::core::Value;

    /// @brief Decide whether a particular instruction is in the rewrite region.
    /// @details Instructions in the root block qualify only after the root
    ///          definition; instructions in other blocks qualify when the root
    ///          block dominates their block.
    auto shouldRewriteInstruction = [&](const BasicBlock &block, std::size_t instrIndex) {
        if (&block == &rootBlock)
            return instrIndex > rootInstrIndex;
        return dominatesInTree(domTree, &rootBlock, &block);
    };

    for (auto &B : F.blocks) {
        for (std::size_t instrIndex = 0; instrIndex < B.instructions.size(); ++instrIndex) {
            if (!shouldRewriteInstruction(B, instrIndex))
                continue;
            auto &I = B.instructions[instrIndex];
            for (auto &Op : I.operands) {
                if (Op.kind == Value::Kind::Temp && Op.id == tempId)
                    Op = replacement;
            }
            for (auto &argList : I.brArgs) {
                for (auto &Arg : argList) {
                    if (Arg.kind == Value::Kind::Temp && Arg.id == tempId)
                        Arg = replacement;
                }
            }
        }
    }
}

/// @brief Compute the next unused temporary identifier in function @p F.
/// @details Inspects procedure parameters, block parameters, instruction
///          results, operands, and branch arguments to find the highest-numbered
///          temporary currently in use.  The returned value is one greater than
///          that maximum and therefore safe to assign to newly introduced SSA
///          values.  Used by transformation passes that need to allocate fresh
///          temporaries without causing identifier collisions.
/// @param F Function to inspect.
/// @return First unused temporary identifier (max + 1).
unsigned nextTempId(const ::il::core::Function &F) {
    using ::il::core::Value;

    unsigned next = 0;
    /// @brief Incorporate one observed identifier into the next-ID candidate.
    /// @throws std::overflow_error If @p v has no representable successor.
    auto update = [&](unsigned v) {
        if (v == std::numeric_limits<unsigned>::max())
            throw std::overflow_error("nextTempId: temporary identifier space exhausted");
        else
            next = std::max(next, v + 1);
    };

    // Scan function parameters
    for (const auto &p : F.params) {
        update(p.id);
    }

    // Scan blocks
    for (const auto &B : F.blocks) {
        // Scan block parameters
        for (const auto &p : B.params) {
            update(p.id);
        }

        // Scan instructions
        for (const auto &I : B.instructions) {
            // Check instruction result
            if (I.result) {
                update(*I.result);
            }

            // Check operands
            for (const auto &Op : I.operands) {
                if (Op.kind == Value::Kind::Temp) {
                    update(Op.id);
                }
            }

            // Check branch arguments
            for (const auto &argList : I.brArgs) {
                for (const auto &Arg : argList) {
                    if (Arg.kind == Value::Kind::Temp) {
                        update(Arg.id);
                    }
                }
            }
        }
    }

    return next;
}

/// @brief Linear search for a block with @p label in @p F.
/// @param F Mutable function whose blocks are searched in storage order.
/// @param label Exact block label to match.
/// @return Pointer to the first matching block, or `nullptr` when absent.
zanna::il::Block *findBlock(::il::core::Function &F, std::string_view label) {
    for (auto &B : F.blocks) {
        if (B.label == label)
            return &B;
    }
    return nullptr;
}

/// @brief Search a const function for a block with an exact label.
/// @param F Function whose blocks are searched in storage order.
/// @param label Exact block label to match.
/// @return Pointer to the first matching block, or `nullptr` when absent.
const zanna::il::Block *findBlock(const ::il::core::Function &F, std::string_view label) {
    for (const auto &B : F.blocks) {
        if (B.label == label)
            return &B;
    }
    return nullptr;
}

} // namespace zanna::il
