//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG/Utils.hpp
// Purpose: Shared helpers for SimplifyCFG transformations -- terminator lookup,
//          value comparison, value substitution, block-index lookup,
//          reachability worklist helpers, side-effect classification, and
//          EH-sensitive block identification.
// Key invariants: Operates on IL CFG structures without mutating ownership.
// Ownership/Lifetime: Stateless free functions that inspect/mutate
//          caller-owned IR structures.
// Links: docs/internals/codemap.md, il/core/BasicBlock.hpp, il/core/Function.hpp,
//        il/core/Instr.hpp, il/core/OpcodeInfo.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares shared control-flow and value helpers for SimplifyCFG.
 *
 * @details The helpers locate terminators, compare and substitute IL values,
 *          analyze cross-block temporary uses, manage reachability worklists,
 *          classify side effects and exception-handling structure, and provide
 *          a small local bit-vector fallback when the project implementation
 *          is unavailable.
 */

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__has_include)
#if __has_include("llvm_like/ADT/BitVector.hpp")
#include "llvm_like/ADT/BitVector.hpp"
#define ZANNA_HAVE_LLVM_LIKE_BITVECTOR 1
#endif
#endif

#ifndef ZANNA_HAVE_LLVM_LIKE_BITVECTOR
namespace llvm_like {

/// @brief Minimal BitVector drop-in used when llvm_like is unavailable.
class BitVector {
  public:
    /// @brief Construct an empty bit vector.
    BitVector() = default;

    /// @brief Construct a bit vector with a uniform initial value.
    /// @param count Number of bits.
    /// @param value Initial value assigned to every bit.
    explicit BitVector(size_t count, bool value = false) : bits_(count, value) {}

    /// @brief Resize the vector, initializing any appended bits.
    /// @param count New number of bits.
    /// @param value Value for bits added beyond the previous end.
    void resize(size_t count, bool value = false) {
        const size_t currentSize = bits_.size();
        if (count <= currentSize) {
            bits_.resize(count);
            return;
        }

        bits_.resize(count, value);
    }

    /// @brief Read one bit with bounds checking.
    /// @param index Zero-based bit position.
    /// @return Stored bit value.
    bool test(size_t index) const {
        return bits_.at(index);
    }

    /// @brief Set one bit to true with bounds checking.
    /// @param index Zero-based bit position.
    void set(size_t index) {
        bits_.at(index) = true;
    }

    /// @brief Return the number of addressable bits.
    /// @return Current bit count.
    size_t size() const {
        return bits_.size();
    }

  private:
    /// @brief Packed storage for the fallback reachability bits.
    std::vector<bool> bits_;
};

} // namespace llvm_like

#endif // ZANNA_HAVE_LLVM_LIKE_BITVECTOR

namespace il::transform::simplify_cfg {

/// @brief Reachability bitset using the project implementation or local fallback.
using BitVector = llvm_like::BitVector;

/// @brief Locate the terminator instruction in a mutable basic block.
/// @param block Block whose last instruction is inspected.
/// @return Pointer to the terminator, or nullptr if the block is empty.
il::core::Instr *findTerminator(il::core::BasicBlock &block);

/// @brief Locate the terminator instruction in a const basic block.
/// @param block Block whose last instruction is inspected.
/// @return Pointer to the terminator, or nullptr if the block is empty.
const il::core::Instr *findTerminator(const il::core::BasicBlock &block);

/// @brief Compare IL values structurally.
/// @param lhs First value.
/// @param rhs Second value.
/// @return True when kind and payload, including exact float bits, are equal.
bool valuesEqual(const il::core::Value &lhs, const il::core::Value &rhs);

/// @brief Compare two vectors of IL values element-wise using valuesEqual.
/// @param lhs First value vector.
/// @param rhs Second value vector.
/// @return True when lengths and corresponding values match.
bool valueVectorsEqual(const std::vector<il::core::Value> &lhs,
                       const std::vector<il::core::Value> &rhs);

/// @brief Substitute temps according to @p mapping; return other values unchanged.
/// @param value Candidate temporary or literal.
/// @param mapping Replacement values indexed by temporary id.
/// @return Mapped value when present, otherwise @p value.
il::core::Value substituteValue(const il::core::Value &value,
                                const std::unordered_map<unsigned, il::core::Value> &mapping);

/// @brief Return true if @p value references @p tempId.
/// @param value Value to inspect.
/// @param tempId SSA id to match.
/// @return Whether @p value is exactly that temporary.
bool valueReferencesTemp(const il::core::Value &value, unsigned tempId);

/// @brief Return true if any instruction outside @p owner uses @p tempId.
/// @param function Function containing the owner and potential uses.
/// @param owner Defining block excluded from the search.
/// @param tempId SSA id to find.
/// @return Whether another block references the id in operands or branch arguments.
bool isTempUsedOutsideBlock(const il::core::Function &function,
                            const il::core::BasicBlock &owner,
                            unsigned tempId);

/// @brief Return true if any parameter of @p block is used outside the block.
/// @param function Function containing @p block.
/// @param block Block whose parameter ids are gathered.
/// @return Whether another block references any gathered id.
bool blockParamsUsedOutside(const il::core::Function &function, const il::core::BasicBlock &block);

/// @brief Return true if any instruction result in @p block is used outside it.
/// @param function Function containing @p block.
/// @param block Block whose result ids are gathered.
/// @return Whether another block references any gathered id.
bool blockResultsUsedOutside(const il::core::Function &function, const il::core::BasicBlock &block);

/// @brief Resolve a basic block index from a label-to-index map.
/// @param labelToIndex Precomputed block index.
/// @param label Label to resolve.
/// @return Block index, or `size_t(-1)` when absent.
size_t lookupBlockIndex(const std::unordered_map<std::string, size_t> &labelToIndex,
                        const std::string &label);

/// @brief Mark @p successor reachable and enqueue it when newly discovered.
/// @param reachable Visited bitset updated in place.
/// @param worklist Pending block-index queue.
/// @param successor Candidate block index, or `size_t(-1)` for no successor.
void enqueueSuccessor(BitVector &reachable, std::deque<size_t> &worklist, size_t successor);

/// @brief Read the environment flag controlling verbose pass logging.
/// @return True when `ZANNA_DEBUG_PASSES` is present and nonempty.
bool readDebugFlagFromEnv();

/// @brief Identify whether an instruction has observable side effects.
/// @param instr Instruction classified through opcode metadata.
/// @return Opcode side-effect flag.
bool hasSideEffects(const il::core::Instr &instr);

/// @brief Check whether a block label denotes an entry block.
/// @param label Candidate block label.
/// @return True for `entry` and `entry_`-prefixed labels.
bool isEntryLabel(const std::string &label);

/// @brief Classify resume-family opcodes used by EH.
/// @param op Opcode to classify.
/// @return True for same, next, and label resume forms.
bool isResumeOpcode(il::core::Opcode op);

/// @brief Classify EH structural opcodes that constrain CFG transforms.
/// @param op Opcode to classify.
/// @return True for handler-stack push/pop and handler entry.
bool isEhStructuralOpcode(il::core::Opcode op);

/// @brief Identify blocks that participate in EH and must not be simplified.
/// @param block Candidate block.
/// @return True for EH entries, handler parameter signatures, structural
///         instructions, or resume terminators.
bool isEHSensitiveBlock(const il::core::BasicBlock &block);

} // namespace il::transform::simplify_cfg
