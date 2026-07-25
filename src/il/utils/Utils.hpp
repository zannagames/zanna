//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/utils/Utils.hpp
// Purpose: Provide small helper utilities for IL blocks and instructions.
// Key invariants: Non-owning queries that depend only on il_core types.
// Ownership/Lifetime: Callers retain ownership of IL structures.
// Links: docs/dev/analysis.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared queries and rewrites over core IL containers.
/// @details These helpers centralize terminator classification, temporary
///          allocation, dominance-scoped replacement, and label lookup without
///          transferring ownership of any function, block, instruction, or
///          value supplied by a caller.

#pragma once

#include <cstddef>
#include <string_view>

namespace il::core {
struct Instr;
struct BasicBlock;
struct Function;
struct Value;
} // namespace il::core

namespace zanna::analysis {
struct DomTree;
} // namespace zanna::analysis

namespace zanna::il {

using Instruction = ::il::core::Instr;
using Block = ::il::core::BasicBlock;

/// @brief Check whether instruction @p I belongs to block @p B.
/// @param I Instruction to look for.
/// @param B Block to scan.
/// @return True if @p I is contained within @p B.
bool belongsToBlock(const Instruction &I, const Block &B);

/// @brief Return the terminator instruction of block @p B.
/// @param B Block to inspect.
/// @return Pointer to the terminator or nullptr if none exists.
Instruction *terminator(Block &B);

/// @brief Determine whether instruction @p I is a terminator.
/// @param I Instruction to inspect.
/// @return True if @p I is br, cbr, ret, or trap.
bool isTerminator(const Instruction &I);

/// @brief Determine whether block @p B currently ends in a terminator.
/// @details This derives the answer from the instruction list so callers do not
///          depend on a stale BasicBlock::terminated flag after vector edits.
/// @param B Block whose final instruction is inspected.
/// @return `true` when @p B is nonempty and its final opcode is a terminator.
bool isTerminated(const Block &B);

/// @brief Replace all uses of a temporary identifier with a new value.
/// @details Scans all instructions and branch arguments in the function,
///          replacing occurrences of temporary @p tempId with @p replacement.
///          Used by optimization passes to substitute SSA values.
/// @param F Function to modify.
/// @param tempId Temporary identifier to replace.
/// @param replacement New value to substitute.
/// @sideeffect Mutates operands and branch arguments in place.
void replaceAllUses(::il::core::Function &F, unsigned tempId, const ::il::core::Value &replacement);

/// @brief Query whether one block dominates another using an existing tree.
/// @details Accepts const block pointers because the query is read-only; the
///          underlying dominator tree stores non-const block keys from analysis
///          construction.
/// @param domTree Dominator tree for the containing function.
/// @param dominator Candidate dominator block.
/// @param block Candidate dominated block.
/// @return True when @p dominator is on @p block's immediate-dominator chain.
bool dominatesInTree(const ::zanna::analysis::DomTree &domTree,
                     const ::il::core::BasicBlock *dominator,
                     const ::il::core::BasicBlock *block);

/// @brief Replace uses of a temporary only where a defining instruction dominates them.
/// @details Rewrites uses in instructions dominated by @p rootBlock. Uses in the
///          root block are rewritten only after @p rootInstrIndex, so callers can
///          remove the root instruction without introducing same-block
///          use-before-def problems. This helper is intended for CSE/GVN style
///          substitutions where @p replacement may itself be a temporary.
/// @param F Function to modify.
/// @param tempId Temporary identifier produced by the redundant instruction.
/// @param replacement Dominating value to substitute.
/// @param rootBlock Block containing the redundant instruction.
/// @param rootInstrIndex Index of the redundant instruction in @p rootBlock.
/// @param domTree Dominator tree for @p F computed before the substitution.
/// @sideeffect Mutates operands and branch arguments in dominated instructions.
void replaceUsesDominatedBy(::il::core::Function &F,
                            unsigned tempId,
                            const ::il::core::Value &replacement,
                            const ::il::core::BasicBlock &rootBlock,
                            std::size_t rootInstrIndex,
                            const ::zanna::analysis::DomTree &domTree);

/// @brief Compute the next available temporary identifier in a function.
/// @details Scans all parameters, block parameters, instruction results,
///          operands, and branch arguments to find the highest temp ID in use.
///          Returns one greater than the maximum, ensuring no collision.
/// @param F Function to analyze.
/// @return First unused temporary identifier.
/// @throws std::overflow_error If an observed identifier is `UINT_MAX`, leaving
///         no representable successor identifier.
unsigned nextTempId(const ::il::core::Function &F);

/// @brief Find a basic block by label in @p F.
/// @param F Function to search.
/// @param label Label to match exactly.
/// @return Pointer to the block when found; nullptr otherwise.
Block *findBlock(::il::core::Function &F, std::string_view label);

/// @brief Find a basic block by label in @p F (const overload).
/// @param F Function to search.
/// @param label Label to match exactly.
/// @return Pointer to the block when found; nullptr otherwise.
const Block *findBlock(const ::il::core::Function &F, std::string_view label);

} // namespace zanna::il
