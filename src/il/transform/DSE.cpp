//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements dead-store elimination with two levels of analysis:
// 1. Intra-block DSE: Backward scan within each basic block
// 2. Cross-block DSE: Forward propagation of stores through the CFG to find
//    stores that are killed on all paths before being read
//
// The pass uses BasicAA for alias disambiguation and is conservative about
// calls that may modify or reference memory.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements intra-block and MemorySSA-backed dead-store elimination.
/// @details Stores are erased only with alias, overwrite-coverage, and
///          nontrapping proofs; exception-handling functions conservatively
///          bypass cross-block elimination.

#include "il/transform/DSE.hpp"

#include "il/analysis/AllocaRoots.hpp"
#include "il/analysis/BasicAA.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/analysis/MemorySSA.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/LoadSafety.hpp"
#include "il/transform/analysis/Liveness.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {
/// @brief Identifies an instruction by owning block and stable pre-erasure index.
using InstructionSite = std::pair<Block *, size_t>;

/// @brief Erase instruction sites without relying on raw pointer ordering.
/// @details Sites are sorted by the owning block's index in @p F and then by
///          descending instruction index inside each block. This keeps same-block
///          erasures stable while avoiding relational comparisons between
///          unrelated @ref BasicBlock pointers.
/// @param F Function that owns every block referenced in @p sites.
/// @param sites Block/instruction pairs to remove in place.
/// @return Number of instructions erased.
std::size_t eraseInstructionSites(Function &F, std::vector<InstructionSite> &sites) {
    std::unordered_map<const Block *, std::size_t> blockOrder;
    blockOrder.reserve(F.blocks.size());
    for (std::size_t i = 0; i < F.blocks.size(); ++i)
        blockOrder.emplace(&F.blocks[i], i);

    /// Order sites by block and then back-to-front within each block.
    std::sort(sites.begin(), sites.end(), [&](const auto &a, const auto &b) {
        const std::size_t aOrder = blockOrder.at(a.first);
        const std::size_t bOrder = blockOrder.at(b.first);
        if (aOrder != bOrder)
            return aOrder < bOrder;
        return a.second > b.second;
    });

    std::size_t erased = 0;
    for (const auto &[block, idx] : sites) {
        block->instructions.erase(block->instructions.begin() + static_cast<long>(idx));
        ++erased;
    }
    return erased;
}

/// @brief Memory location key with optional access width.
struct Addr {
    // We track by Value plus optional byte width for more precise AA queries.
    /// Pointer value naming the location.
    Value v;
    /// Access width in bytes when known.
    std::optional<unsigned> size;
};

/// @brief Hash memory location keys for the intra-block kill set.
struct AddrHash {
    /// @brief Hash a location's value identity and optional width.
    /// @param a Address key.
    /// @return Combined hash value.
    size_t operator()(const Addr &a) const noexcept {
        // Hash by kind and id/str
        size_t h = static_cast<size_t>(a.v.kind) * 1315423911u;
        switch (a.v.kind) {
            case Value::Kind::Temp:
                h ^= static_cast<size_t>(a.v.id + 0x9e3779b97f4a7c15ULL);
                break;
            case Value::Kind::NullPtr:
                h ^= 0xdeadbeefULL;
                break;
            case Value::Kind::GlobalAddr:
            case Value::Kind::ConstStr:
                h ^= std::hash<std::string>{}(a.v.str);
                break;
            default:
                h ^= 0x1234567ULL;
                break;
        }
        if (a.size)
            h ^= static_cast<size_t>(*a.size) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// @brief Exact equality comparator for memory location keys.
struct AddrEq {
    /// @brief Compare value identity and access width.
    /// @param a First location.
    /// @param b Second location.
    /// @return True only for structurally identical address keys.
    bool operator()(const Addr &a, const Addr &b) const noexcept {
        // Exact match only; potential aliasing is handled via AA when needed.
        if (a.v.kind != b.v.kind)
            return false;
        if (a.size != b.size)
            return false;
        using K = Value::Kind;
        switch (a.v.kind) {
            case K::Temp:
                return a.v.id == b.v.id;
            case K::GlobalAddr:
            case K::ConstStr:
                return a.v.str == b.v.str;
            case K::NullPtr:
                return true;
            default:
                return false;
        }
    }
};

/// @brief Test whether an instruction stores through an SSA temporary pointer.
/// @param I Instruction to inspect.
/// @return True for well-shaped temporary-address stores.
inline bool isStoreToTempPtr(const Instr &I) {
    return I.op == Opcode::Store && !I.operands.empty() && I.operands[0].kind == Value::Kind::Temp;
}

/// @brief Test whether an instruction loads through an SSA temporary pointer.
/// @param I Instruction to inspect.
/// @return True for well-shaped temporary-address loads.
inline bool isLoadFromTempPtr(const Instr &I) {
    return I.op == Opcode::Load && !I.operands.empty() && I.operands[0].kind == Value::Kind::Temp;
}

/// @brief Derive a memory instruction's access width from its IL type.
/// @param I Load or store instruction.
/// @return Width in bytes when the type has a known fixed size.
inline std::optional<unsigned> accessSize(const Instr &I) {
    return zanna::analysis::BasicAA::typeSizeBytes(I.type);
}

/// @brief Prove that a later store completely covers an earlier store.
/// @param laterPtr Pointer written by the later store.
/// @param laterSize Later access width.
/// @param earlierPtr Pointer written by the candidate dead store.
/// @param earlierSize Earlier access width.
/// @param AA Alias analysis used to require identical starting locations.
/// @return True when both widths are known, the later write is at least as wide,
///         and the pointers must alias.
bool fullyOverwrites(const Value &laterPtr,
                     std::optional<unsigned> laterSize,
                     const Value &earlierPtr,
                     std::optional<unsigned> earlierSize,
                     zanna::analysis::BasicAA &AA) {
    if (!laterSize || !earlierSize)
        return false;
    if (*laterSize < *earlierSize)
        return false;
    return AA.alias(laterPtr, earlierPtr, laterSize, earlierSize) ==
           zanna::analysis::AliasResult::MustAlias;
}

} // namespace

/// @brief Eliminate overwritten stores within individual basic blocks.
/// @param F Function updated in place.
/// @param AM Analysis manager providing BasicAA.
/// @return True when at least one nontrapping store was removed.
bool runDSE(Function &F, AnalysisManager &AM) {
    // Acquire BasicAA when available
    zanna::analysis::BasicAA &AA =
        AM.getFunctionResult<zanna::analysis::BasicAA>(kAnalysisBasicAA, F);
    std::vector<InstructionSite> toRemove;

    for (auto &B : F.blocks) {
        // Backward scan in the block
        std::unordered_set<Addr, AddrHash, AddrEq> killed;

        for (std::size_t i = B.instructions.size(); i-- > 0;) {
            Instr &I = B.instructions[i];

            // Loads block further elimination for the specific address
            if (isLoadFromTempPtr(I)) {
                Addr a{I.operands[0], accessSize(I)};
                for (auto it = killed.begin(); it != killed.end();) {
                    if (AA.alias(a.v, it->v, a.size, it->size) !=
                        zanna::analysis::AliasResult::NoAlias)
                        it = killed.erase(it);
                    else
                        ++it;
                }
                continue;
            }

            // Calls: conservative clobber when may Mod/Ref
            if (I.op == Opcode::Call || I.op == Opcode::CallIndirect) {
                auto mr = AA.modRef(I);
                if (mr != zanna::analysis::ModRefResult::NoModRef)
                    killed.clear();
                continue;
            }

            // Store: if the address is already killed by a later store and no read intervened,
            // then this store is dead.
            if (isStoreToTempPtr(I)) {
                Addr a{I.operands[0], accessSize(I)};
                bool dead = false;

                // A later store kills this one only if it is proven to start at
                // the same address and fully cover the earlier write. MayAlias
                // is not enough: it may be a distinct pointer.
                for (const auto &k : killed) {
                    if (fullyOverwrites(k.v, k.size, a.v, a.size, AA)) {
                        dead = true;
                        break;
                    }
                }

                if (dead && isStoreKnownNonTrapping(F, I)) {
                    toRemove.emplace_back(&B, i);
                    continue;
                }
                // Not dead: mark this address as killed for earlier stores
                killed.insert(a);
                continue;
            }
        }
    }

    return eraseInstructionSites(F, toRemove) != 0;
}

namespace {

/// @brief Detect exception-handling state that blocks cross-block DSE.
/// @param F Function to scan.
/// @return True when any EH stack, entry, or resume opcode is present.
bool hasExceptionHandling(const Function &F) {
    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            switch (I.op) {
                case Opcode::EhPush:
                case Opcode::EhPop:
                case Opcode::EhEntry:
                case Opcode::ResumeSame:
                case Opcode::ResumeNext:
                case Opcode::ResumeLabel:
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

/// @brief Erase stores identified dead by a MemorySSA result.
/// @param function Function updated in place.
/// @param memorySSA MemorySSA proof corresponding to @p function.
/// @return True when at least one proven nontrapping store was removed.
bool eliminateMemorySSADeadStores(Function &function,
                                  const zanna::analysis::MemorySSA &memorySSA) {
    std::vector<InstructionSite> toRemove;
    for (auto &block : function.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            if (block.instructions[index].op == Opcode::Store &&
                memorySSA.isDeadStore(&block, index) &&
                isStoreKnownNonTrapping(function, block.instructions[index]))
                toRemove.emplace_back(&block, index);
        }
    }
    return eraseInstructionSites(function, toRemove) != 0;
}

} // namespace

/// @brief Cross-block DSE compatibility entry point.
/// @details The MemorySSA implementation is the single authoritative
///          cross-block path proof, avoiding divergent call/loop semantics.
/// @param F Function updated in place.
/// @param AM Analysis manager providing BasicAA.
/// @return True when MemorySSA proves and removes at least one store.
bool runCrossBlockDSE(Function &F, AnalysisManager &AM) {
    if (hasExceptionHandling(F))
        return false;
    zanna::analysis::BasicAA &aliasAnalysis =
        AM.getFunctionResult<zanna::analysis::BasicAA>(kAnalysisBasicAA, F);
    const zanna::analysis::MemorySSA memorySSA =
        zanna::analysis::computeMemorySSA(F, aliasAnalysis);
    return eliminateMemorySSADeadStores(F, memorySSA);
}

/// @brief Eliminate cross-block dead stores using cached MemorySSA.
///
/// Uses the MemorySSA analysis as the canonical cross-block proof. Since its
/// dead-store computation skips calls for
/// non-escaping allocas (they cannot access non-escaping stack memory), this
/// pass eliminates stores in functions that contain runtime calls inside loops
/// or conditional branches — the most common pattern in Zia-lowered code.
/// @param F Function updated in place.
/// @param AM Analysis manager providing MemorySSA.
/// @return True when at least one store was removed.
bool runMemorySSADSE(Function &F, AnalysisManager &AM) {
    if (hasExceptionHandling(F))
        return false;

    zanna::analysis::MemorySSA &mssa =
        AM.getFunctionResult<zanna::analysis::MemorySSA>(kAnalysisMemorySSA, F);

    return eliminateMemorySSADeadStores(F, mssa);
}

} // namespace il::transform
