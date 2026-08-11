//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/PreRAForwardCopy.hpp
// Purpose: Shared pre-register-allocation copy cleanup used by both backends:
//          removes identity copies and forwards virtual-to-virtual copies
//          whose destination has exactly one direct use before redefinition.
//          The traversal, safety conditions, and compaction are identical on
//          x86-64 and AArch64; backends supply the MIR-specific queries
//          through a traits type.
// Key invariants:
//   - Forwarding never crosses block boundaries or call clobbers: a use found
//     after a call, or any redefinition of the copy source before the use,
//     cancels the rewrite.
//   - A copy is only removed when its destination is confined to the block
//     being rewritten. The forward scan stops at the block's terminator, so it
//     can only ever prove single use *within* the block; a destination that is
//     also read by a successor must keep its defining copy.
//   - Only direct register operands are substituted; uses embedded in memory
//     operands (base/index) count as uses but are never rewritten.
//   - Physical registers are never forwarded (their live ranges are not
//     tracked at this stage).
// Ownership/Lifetime:
//   - Header-only function templates; no state.
// Links: codegen/x86_64/PreRegAllocOpt.cpp,
//        codegen/aarch64/PreRegAllocOpt.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Implements shared pre-allocation identity and single-use copy cleanup.

namespace zanna::codegen::common {

/// @brief Position of a single register use within a basic block.
struct PreRAUseSite {
    std::size_t instrIndex{0};   ///< Index of the consuming instruction.
    std::size_t operandIndex{0}; ///< Operand index within that instruction.
};

/// @brief Result of scanning one instruction for uses of a tracked register.
struct PreRAUseScan {
    std::size_t useCount{0};       ///< All reads, including memory base/index.
    std::size_t directUseCount{0}; ///< Reads as plain register operands only.
    std::size_t directOperand{0};  ///< Operand index of the last direct read.
};

/// @brief Traits contract for @ref runPreRAForwardCopy (documentation only).
///
/// Backends provide a struct with:
///   using BlockT / InstrT / RegT;
///   static std::vector<InstrT>       &instrs(BlockT &block);
///   static const std::vector<InstrT> &instrs(const BlockT &block);
///   static bool isIdentityCopy(const InstrT &instr);
///   static bool isForwardableCopy(const InstrT &instr, RegT &dst, RegT &src);
///       // true for virtual->virtual same-class non-identity reg copies;
///       // fills dst/src on success.
///   static bool definesReg(const InstrT &instr, const RegT &reg);
///   static bool isCall(const InstrT &instr);
///   static bool isNonCallBoundary(const InstrT &instr);
///   static PreRAUseScan scanUses(const InstrT &instr, const RegT &dst);
///   static void forwardUse(InstrT &use, std::size_t operandIndex, const InstrT &copy);
///       // rewrite the direct use to read the copy's source operand.
///   static void collectVRegs(const InstrT &instr, std::vector<RegT> &out);
///       // append every virtual register the instruction references, whether
///       // read or written and including registers buried in addresses.
///   static uint32_t vregKey(const RegT &reg);
///       // stable identity for a virtual register across instructions.

namespace detail {

/// @brief Locate the unique direct use of a copy's destination.
/// @details Walks forward from @p copyIndex looking for the single instruction
///          that reads @p dst as a plain register operand. Bails out when the
///          copy source is redefined before the use, when more than one use
///          exists, when the use sits behind a call (caller-saved clobbers),
///          or at non-call block boundaries.
/// @tparam Traits Backend-specific MIR query and rewrite contract.
/// @param block Basic block containing the candidate copy.
/// @param copyIndex Index of the candidate copy instruction.
/// @param dst Virtual destination whose uses are scanned.
/// @param src Virtual source whose intervening redefinitions are forbidden.
/// @return Unique direct-use position, or `std::nullopt` when forwarding is
///         unsafe or the required use does not exist.
template <typename Traits>
std::optional<PreRAUseSite> findSingleDirectUse(const typename Traits::BlockT &block,
                                                std::size_t copyIndex,
                                                const typename Traits::RegT &dst,
                                                const typename Traits::RegT &src) {
    std::optional<PreRAUseSite> site;
    const auto &instrs = Traits::instrs(block);

    bool crossedCall = false;
    for (std::size_t idx = copyIndex + 1; idx < instrs.size(); ++idx) {
        const auto &instr = instrs[idx];

        if (Traits::definesReg(instr, src) && !site)
            return std::nullopt;

        const PreRAUseScan scan = Traits::scanUses(instr, dst);
        if (scan.useCount != 0) {
            if (crossedCall)
                return std::nullopt;
            if (scan.useCount != 1 || scan.directUseCount != 1 || Traits::definesReg(instr, dst))
                return std::nullopt;
            if (site)
                return std::nullopt;
            site = PreRAUseSite{idx, scan.directOperand};
        }

        if (Traits::definesReg(instr, dst))
            break;
        if (Traits::isCall(instr))
            crossedCall = true;
        if (Traits::isNonCallBoundary(instr))
            break;
    }

    return site;
}

/// @brief Collect the virtual registers referenced by more than one block.
/// @details The forwarding scan is block-local: it stops at the terminator, so
///          "exactly one use" can only ever mean "exactly one use in this
///          block". Removing the copy on that evidence alone drops the
///          definition out from under any successor that also reads the
///          destination, leaving the consumer reading whatever the register
///          allocator later parks there. This pre-pass records the escapees so
///          the rewrite can skip them.
/// @tparam Traits Backend-specific MIR query and rewrite contract.
/// @param fn Function whose blocks are scanned.
/// @return Keys of virtual registers appearing in two or more blocks.
template <typename Traits, typename FunctionT>
std::unordered_set<uint32_t> collectEscapingVRegs(const FunctionT &fn) {
    std::unordered_map<uint32_t, std::size_t> firstBlock;
    std::unordered_set<uint32_t> escaping;
    std::vector<typename Traits::RegT> regs;

    std::size_t blockIdx = 0;
    for (const auto &block : fn.blocks) {
        for (const auto &instr : Traits::instrs(block)) {
            regs.clear();
            Traits::collectVRegs(instr, regs);
            for (const auto &reg : regs) {
                const uint32_t key = Traits::vregKey(reg);
                const auto [it, inserted] = firstBlock.emplace(key, blockIdx);
                if (!inserted && it->second != blockIdx)
                    escaping.insert(key);
            }
        }
        ++blockIdx;
    }
    return escaping;
}

/// @brief Forward each virtual-to-virtual copy whose destination has one use.
/// @tparam Traits Backend-specific MIR query and rewrite contract.
/// @param[in,out] block Basic block whose eligible copies are forwarded and removed.
/// @param escaping Destinations that other blocks also reference; never removed.
/// @return Number of copy instructions removed.
template <typename Traits>
std::size_t rewriteSingleUseCopies(typename Traits::BlockT &block,
                                   const std::unordered_set<uint32_t> &escaping) {
    auto &instrs = Traits::instrs(block);
    std::vector<bool> erase(instrs.size(), false);
    std::size_t removed = 0;

    for (std::size_t idx = 0; idx < instrs.size(); ++idx) {
        typename Traits::RegT dst{};
        typename Traits::RegT src{};
        if (!Traits::isForwardableCopy(instrs[idx], dst, src))
            continue;

        // A destination read by another block outlives this scan's evidence.
        if (escaping.count(Traits::vregKey(dst)) != 0)
            continue;

        auto site = findSingleDirectUse<Traits>(block, idx, dst, src);
        if (!site)
            continue;

        Traits::forwardUse(instrs[site->instrIndex], site->operandIndex, instrs[idx]);
        erase[idx] = true;
        ++removed;
    }

    if (removed == 0)
        return 0;

    std::vector<typename Traits::InstrT> kept;
    kept.reserve(instrs.size() - removed);
    for (std::size_t idx = 0; idx < instrs.size(); ++idx) {
        if (!erase[idx])
            kept.push_back(std::move(instrs[idx]));
    }
    instrs = std::move(kept);
    return removed;
}

} // namespace detail

/// @brief Run identity-copy removal and single-use copy forwarding over @p fn.
/// @tparam Traits Backend trait type (see the contract above).
/// @tparam FunctionT Deduced backend MFunction type exposing @c blocks.
/// @param fn Function rewritten in place.
/// @return Number of MIR instructions removed.
template <typename Traits, typename FunctionT> std::size_t runPreRAForwardCopy(FunctionT &fn) {
    std::size_t removed = 0;
    const auto escaping = detail::collectEscapingVRegs<Traits>(fn);
    for (auto &block : fn.blocks) {
        auto &instrs = Traits::instrs(block);
        const auto oldSize = instrs.size();
        instrs.erase(std::remove_if(instrs.begin(),
                                    instrs.end(),
                                    /// Select backend-recognized no-op copies.
                                    [](const typename Traits::InstrT &instr) {
                                        return Traits::isIdentityCopy(instr);
                                    }),
                     instrs.end());
        removed += oldSize - instrs.size();
        removed += detail::rewriteSingleUseCopies<Traits>(block, escaping);
    }
    return removed;
}

} // namespace zanna::codegen::common
