//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the backwards data-flow analysis used to compute SSA liveness for
// IL functions.  The helpers construct a compact CFG summary, determine the
// register universe, and iterate until fixed point to determine live-in/live-out
// sets per block.
//
//===----------------------------------------------------------------------===//

#include "il/transform/analysis/Liveness.hpp"

#include "il/analysis/CFG.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <utility>

using namespace il::core;

namespace il::transform {

/// @brief Test whether a value identifier appears in the tracked set.
///
/// @param valueId Dense SSA identifier to query.
/// @return @c true when the bitset contains the identifier.
bool LivenessInfo::SetView::contains(unsigned valueId) const {
    return bits_ != nullptr && valueId < bits_->size() && (*bits_)[valueId];
}

/// @brief Determine whether the view represents an empty set.
///
/// @return @c true when no bitset is attached or every bit is clear.
bool LivenessInfo::SetView::empty() const {
    /// Treat a bit as empty only when it is clear.
    return bits_ == nullptr ||
           std::none_of(bits_->begin(), bits_->end(), [](bool bit) { return bit; });
}

/// @brief Access the underlying bitset describing the view.
///
/// @return Reference to the underlying bitset.
/// @throws Assertion failure when the view is empty.
const std::vector<bool> &LivenessInfo::SetView::bits() const {
    assert(bits_ && "liveness set view is empty");
    return *bits_;
}

/// @brief Construct a view over an existing bitset pointer.
///
/// @param bits Pointer to the bitset describing the set; may be @c nullptr to
///             represent an empty view.
LivenessInfo::SetView::SetView(const std::vector<bool> *bits) : bits_(bits) {}

/// @brief Retrieve the live-in set for a block.
///
/// @param block Block whose live-in values are requested.
/// @return View over the block's live-in bitset.
LivenessInfo::SetView LivenessInfo::liveIn(const core::BasicBlock &block) const {
    return liveIn(&block);
}

/// @brief Retrieve the live-in set for an optional block pointer.
///
/// @param block Block pointer or @c nullptr.
/// @return View over the block's live-in bitset or an empty view when @p block is null.
LivenessInfo::SetView LivenessInfo::liveIn(const core::BasicBlock *block) const {
    if (!block)
        return SetView();
    auto it = blockIndex_.find(block);
    if (it == blockIndex_.end() || it->second >= liveInBits_.size())
        return SetView();
    return SetView(&liveInBits_[it->second]);
}

/// @brief Retrieve the live-out set for a block.
///
/// @param block Block whose live-out values are requested.
/// @return View over the block's live-out bitset.
LivenessInfo::SetView LivenessInfo::liveOut(const core::BasicBlock &block) const {
    return liveOut(&block);
}

/// @brief Retrieve the live-out set for an optional block pointer.
///
/// @param block Block pointer or @c nullptr.
/// @return View over the block's live-out bitset or an empty view when @p block is null.
LivenessInfo::SetView LivenessInfo::liveOut(const core::BasicBlock *block) const {
    if (!block)
        return SetView();
    auto it = blockIndex_.find(block);
    if (it == blockIndex_.end() || it->second >= liveOutBits_.size())
        return SetView();
    return SetView(&liveOutBits_[it->second]);
}

/// @brief Report the number of dense SSA identifiers tracked by the analysis.
///
/// @return Value identifier capacity discovered during analysis.
std::size_t LivenessInfo::valueCount() const {
    return valueCount_;
}

namespace {

//===----------------------------------------------------------------------===//
// ChunkedBitset - SIMD-friendly bitset using uint64_t chunks
//===----------------------------------------------------------------------===//

/// @brief Fast bitset using 64-bit chunks for efficient bulk operations.
/// @details Uses uint64_t words instead of std::vector<bool> to enable
///          SIMD-friendly OR operations and better cache behavior during
///          the iterative liveness fixed-point computation.
class ChunkedBitset {
  public:
    /// Number of bits stored in one machine word.
    static constexpr std::size_t kBitsPerChunk = 64;

    /// @brief Construct an empty bitset.
    ChunkedBitset() = default;

    /// @brief Construct a zero-filled bitset.
    /// @param bitCount Number of addressable bits.
    explicit ChunkedBitset(std::size_t bitCount)
        : bitCount_(bitCount), chunks_((bitCount + kBitsPerChunk - 1) / kBitsPerChunk, 0) {}

    /// @brief Set a bit at the given index.
    /// @param idx Zero-based index; out-of-range indices are ignored.
    void set(std::size_t idx) {
        if (idx >= bitCount_)
            return;
        chunks_[idx / kBitsPerChunk] |= (uint64_t{1} << (idx % kBitsPerChunk));
    }

    /// @brief Test if a bit is set.
    /// @param idx Zero-based index.
    /// @return False for an out-of-range or clear bit.
    bool test(std::size_t idx) const {
        if (idx >= bitCount_)
            return false;
        return (chunks_[idx / kBitsPerChunk] & (uint64_t{1} << (idx % kBitsPerChunk))) != 0;
    }

    /// @brief Clear all bits.
    void clear() {
        std::fill(chunks_.begin(), chunks_.end(), 0);
    }

    /// @brief Merge (OR) another bitset into this one.
    /// @details Uses word-level OR for SIMD-friendly bulk operation.
    /// @param other Equal-sized source bitset.
    void merge(const ChunkedBitset &other) {
        assert(chunks_.size() == other.chunks_.size());
        for (std::size_t i = 0; i < chunks_.size(); ++i)
            chunks_[i] |= other.chunks_[i];
    }

    /// @brief Copy assignment from another bitset.
    /// @param other Equal-sized source bitset.
    void copyFrom(const ChunkedBitset &other) {
        assert(chunks_.size() == other.chunks_.size());
        chunks_ = other.chunks_;
    }

    /// @brief Check equality with another bitset.
    /// @param other Bitset to compare.
    /// @return True when all chunks match.
    bool operator==(const ChunkedBitset &other) const {
        return chunks_ == other.chunks_;
    }

    /// @brief Check inequality with another bitset.
    /// @param other Bitset to compare.
    /// @return Negation of equality.
    bool operator!=(const ChunkedBitset &other) const {
        return !(*this == other);
    }

    /// @brief Compute liveIn = uses | (liveOut & ~defs) efficiently.
    /// @details Performs the data-flow transfer function in a single pass
    ///          over the chunks, avoiding multiple iterations.
    /// @param uses Values used before definition in the block.
    /// @param defs Values defined by the block.
    /// @param liveOut Values live on outgoing edges.
    void computeLiveIn(const ChunkedBitset &uses,
                       const ChunkedBitset &defs,
                       const ChunkedBitset &liveOut) {
        assert(chunks_.size() == uses.chunks_.size());
        assert(chunks_.size() == defs.chunks_.size());
        assert(chunks_.size() == liveOut.chunks_.size());
        for (std::size_t i = 0; i < chunks_.size(); ++i)
            chunks_[i] = uses.chunks_[i] | (liveOut.chunks_[i] & ~defs.chunks_[i]);
    }

    /// @brief Convert to std::vector<bool> for API compatibility.
    /// @return Dense boolean vector containing the same bits.
    std::vector<bool> toVectorBool() const {
        std::vector<bool> result(bitCount_, false);
        for (std::size_t i = 0; i < bitCount_; ++i) {
            if (test(i))
                result[i] = true;
        }
        return result;
    }

    /// @brief Return the number of addressable bits.
    /// @return Bit capacity supplied at construction.
    std::size_t bitCount() const {
        return bitCount_;
    }

  private:
    std::size_t bitCount_{0};
    std::vector<uint64_t> chunks_;
};

struct BlockInfo {
    /// @brief Prepare per-block definition/use bitsets sized to @p valueCount.
    explicit BlockInfo(std::size_t valueCount = 0) : defs(valueCount), uses(valueCount) {}

    /// Values defined by block parameters or instruction results.
    ChunkedBitset defs;
    /// Values used before their first definition in the block.
    ChunkedBitset uses;
};

/// @brief Determine how many dense SSA identifiers the function may reference.
///
/// Scans function arguments, block parameters, instruction operands, branch
/// arguments, and instruction results to compute the maximum identifier used.
///
/// @param fn Function being analysed.
/// @return Capacity large enough to index every identifier observed in @p fn.
std::size_t determineValueCapacity(const core::Function &fn) {
    unsigned maxId = 0;
    bool sawId = false;
    /// Extend the observed dense-id range with one temporary.
    auto noteId = [&](unsigned id) {
        maxId = std::max(maxId, id);
        sawId = true;
    };

    for (const auto &param : fn.params)
        noteId(param.id);

    for (const auto &block : fn.blocks) {
        for (const auto &param : block.params)
            noteId(param.id);

        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                if (operand.kind == Value::Kind::Temp)
                    noteId(operand.id);
            }
            for (const auto &argList : instr.brArgs) {
                for (const auto &arg : argList) {
                    if (arg.kind == Value::Kind::Temp)
                        noteId(arg.id);
                }
            }
            if (instr.result)
                noteId(*instr.result);
        }
    }

    std::size_t capacity = sawId ? static_cast<std::size_t>(maxId) + 1 : 0;
    if (fn.valueNames.size() > capacity)
        capacity = fn.valueNames.size();
    return capacity;
}

} // namespace

/// @brief Build a lightweight CFG summary for the function.
///
/// Populates predecessor/successor relationships using the shared CFG helpers so
/// the liveness analysis can avoid full context recomputation.
///
/// @param module Module containing the function.
/// @param fn Function whose CFG summary should be constructed.
/// @return Populated CFG information ready for data-flow analysis.
CFGInfo buildCFG(core::Module &module, core::Function &fn) {
    CFGInfo info;
    zanna::analysis::CFGContext ctx(module, fn);

    for (auto &block : fn.blocks) {
        auto &succ = info.successors[&block];
        const auto &succBlocks = zanna::analysis::successors(ctx, block);
        succ.reserve(succBlocks.size());
        for (auto *succBlock : succBlocks)
            succ.push_back(succBlock);
    }

    for (auto &block : fn.blocks) {
        auto &pred = info.predecessors[&block];
        const auto &predBlocks = zanna::analysis::predecessors(ctx, block);
        pred.reserve(predBlocks.size());
        for (auto *predBlock : predBlocks)
            pred.push_back(predBlock);
    }

    return info;
}

/// @brief Compute backwards liveness for @p fn using an existing CFG summary.
///
/// @param module Module owning the function (unused but kept for symmetry).
/// @param fn Function being analysed.
/// @param cfg Precomputed CFG summary for @p fn.
/// @return Populated liveness information including live-in/live-out bitsets.
///
/// @details Uses ChunkedBitset internally for efficient fixed-point iteration.
///          The bitsets use uint64_t chunks enabling SIMD-friendly merge operations
///          and better cache behavior compared to std::vector<bool>. Results are
///          converted to std::vector<bool> at the end for API compatibility.
LivenessInfo computeLiveness(core::Module &module, core::Function &fn, const CFGInfo &cfg) {
    static_cast<void>(module);
    const std::size_t valueCount = determineValueCapacity(fn);

    // Use ChunkedBitset for efficient fixed-point computation
    std::vector<ChunkedBitset> liveInChunks(fn.blocks.size(), ChunkedBitset(valueCount));
    std::vector<ChunkedBitset> liveOutChunks(fn.blocks.size(), ChunkedBitset(valueCount));

    // Build block index and compute per-block def/use sets
    std::unordered_map<const core::BasicBlock *, std::size_t> blockIndex;
    std::vector<const core::BasicBlock *> blocks;
    blocks.reserve(fn.blocks.size());

    std::vector<BlockInfo> blockInfo(fn.blocks.size(), BlockInfo(valueCount));

    for (std::size_t idx = 0; idx < fn.blocks.size(); ++idx) {
        auto &block = fn.blocks[idx];
        blocks.push_back(&block);
        blockIndex[&block] = idx;

        BlockInfo &state = blockInfo[idx];

        for (const auto &param : block.params)
            state.defs.set(param.id);

        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                if (operand.kind != Value::Kind::Temp)
                    continue;
                const unsigned id = operand.id;
                if (id >= valueCount)
                    continue;
                if (!state.defs.test(id))
                    state.uses.set(id);
            }
            for (const auto &argList : instr.brArgs) {
                for (const auto &arg : argList) {
                    if (arg.kind != Value::Kind::Temp)
                        continue;
                    const unsigned id = arg.id;
                    if (id >= valueCount)
                        continue;
                    if (!state.defs.test(id))
                        state.uses.set(id);
                }
            }
            if (instr.result)
                state.defs.set(*instr.result);
        }
    }

    // Fixed-point iteration using ChunkedBitset for efficient operations
    ChunkedBitset scratchOut(valueCount);
    ChunkedBitset scratchIn(valueCount);

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t reverseIdx = fn.blocks.size(); reverseIdx-- > 0;) {
            const BasicBlock *block = blocks[reverseIdx];
            BlockInfo &state = blockInfo[reverseIdx];

            // Compute liveOut = union of liveIn of all successors
            scratchOut.clear();
            auto succIt = cfg.successors.find(block);
            if (succIt != cfg.successors.end()) {
                for (const BasicBlock *succ : succIt->second) {
                    auto succIdxIt = blockIndex.find(succ);
                    if (succIdxIt == blockIndex.end())
                        continue;
                    scratchOut.merge(liveInChunks[succIdxIt->second]);
                }
            }
            if (scratchOut != liveOutChunks[reverseIdx]) {
                liveOutChunks[reverseIdx].copyFrom(scratchOut);
                changed = true;
            }

            // Compute liveIn = uses | (liveOut & ~defs) using optimized method
            scratchIn.computeLiveIn(state.uses, state.defs, liveOutChunks[reverseIdx]);
            if (scratchIn != liveInChunks[reverseIdx]) {
                liveInChunks[reverseIdx].copyFrom(scratchIn);
                changed = true;
            }
        }
    }

    // Convert to std::vector<bool> for API compatibility
    LivenessInfo info;
    info.valueCount_ = valueCount;
    info.blocks_ = std::move(blocks);
    info.blockIndex_ = std::move(blockIndex);
    info.liveInBits_.reserve(fn.blocks.size());
    info.liveOutBits_.reserve(fn.blocks.size());

    for (std::size_t idx = 0; idx < fn.blocks.size(); ++idx) {
        info.liveInBits_.push_back(liveInChunks[idx].toVectorBool());
        info.liveOutBits_.push_back(liveOutChunks[idx].toVectorBool());
    }

    return info;
}

/// @brief Compute backwards liveness for @p fn, constructing a CFG summary on demand.
///
/// @param module Module containing the function.
/// @param fn Function being analysed.
/// @return Populated liveness information including live-in/live-out bitsets.
LivenessInfo computeLiveness(core::Module &module, core::Function &fn) {
    CFGInfo cfg = buildCFG(module, fn);
    return computeLiveness(module, fn, cfg);
}

} // namespace il::transform
