//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/utils/UseDefInfo.cpp
// Purpose: Implement temporary use-count tracking and safe SSA value
//          replacement for mutable IL.
// Links: docs/internals/codemap.md#il-utils
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements use-count construction and safe replacement.
/// @details Counts are rebuilt from the function when replacements occur so the
///          helper remains correct even after instruction vectors are mutated.

#include "il/utils/UseDefInfo.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"

namespace zanna::il {

/// @brief Initialize use information from a function's current instruction operands.
/// @param F Function to retain and scan; it must outlive this object.
UseDefInfo::UseDefInfo(::il::core::Function &F) {
    build(F);
}

/// @brief Rebuild the use-count and cached-site indexes from @p F.
/// @details Records uses found in both ordinary operands and per-successor
///          branch argument lists. Previous indexes and the retained function
///          pointer are replaced before scanning.
/// @param F Function whose current operand graph is indexed.
void UseDefInfo::build(::il::core::Function &F) {
    function_ = &F;
    useCounts_.clear();
    useSites_.clear();

    for (auto &B : F.blocks) {
        for (auto &I : B.instructions) {
            // Record uses in instruction operands
            for (auto &Op : I.operands) {
                recordUse(Op);
            }

            // Record uses in branch arguments
            for (auto &argList : I.brArgs) {
                for (auto &Arg : argList) {
                    recordUse(Arg);
                }
            }
        }
    }
}

/// @brief Add a temporary operand to both use indexes.
/// @param v Operand location to inspect and, for a temporary, cache.
void UseDefInfo::recordUse(::il::core::Value &v) {
    if (v.kind == ::il::core::Value::Kind::Temp) {
        ++useCounts_[v.id];
        useSites_[v.id].push_back(&v);
    }
}

/// @brief Rewrite cached uses while all indexed operand addresses remain valid.
/// @details Removes the source temporary's cache entry, assigns the replacement
///          at each recorded site, and transfers those sites into the
///          replacement temporary's entries when applicable.
/// @param tempId Temporary whose cached sites are rewritten.
/// @param replacement Value stored at each cached site.
/// @return Number of cached sites rewritten.
std::size_t UseDefInfo::replaceAllUsesStableStorage(
    unsigned tempId,
    const ::il::core::Value &replacement) {
    auto found = useSites_.find(tempId);
    if (found == useSites_.end())
        return 0;

    std::vector<::il::core::Value *> sites = std::move(found->second);
    useSites_.erase(found);
    useCounts_.erase(tempId);
    for (auto *site : sites) {
        *site = replacement;
        if (replacement.kind == ::il::core::Value::Kind::Temp) {
            useSites_[replacement.id].push_back(site);
            ++useCounts_[replacement.id];
        }
    }
    return sites.size();
}

/// @brief Replace a temporary by rescanning every operand in the retained function.
/// @details Rewrites ordinary instruction operands and branch arguments, then
///          rebuilds both indexes so subsequent queries reflect the new graph.
/// @param tempId Temporary identifier to match.
/// @param replacement Value substituted at every matching use.
/// @return Number of matching operands and branch arguments rewritten.
std::size_t UseDefInfo::replaceAllUses(unsigned tempId, const ::il::core::Value &replacement) {
    if (!function_) {
        return 0;
    }

    std::size_t count = 0;
    for (auto &B : function_->blocks) {
        for (auto &I : B.instructions) {
            for (auto &Op : I.operands) {
                if (Op.kind == ::il::core::Value::Kind::Temp && Op.id == tempId) {
                    Op = replacement;
                    ++count;
                }
            }
            for (auto &argList : I.brArgs) {
                for (auto &Arg : argList) {
                    if (Arg.kind == ::il::core::Value::Kind::Temp && Arg.id == tempId) {
                        Arg = replacement;
                        ++count;
                    }
                }
            }
        }
    }

    // Rebuild observed counts from the mutated function so later queries are
    // coherent even after instruction insertion/erasure shifted storage.
    build(*function_);
    return count;
}

/// @brief Test whether the latest index contains a nonzero use count.
/// @param tempId Temporary identifier to query.
/// @return `true` when at least one indexed use exists.
bool UseDefInfo::hasUses(unsigned tempId) const {
    auto it = useCounts_.find(tempId);
    return it != useCounts_.end() && it->second != 0;
}

/// @brief Return the latest indexed number of uses for a temporary.
/// @param tempId Temporary identifier to query.
/// @return Indexed use count, or zero when @p tempId is absent.
std::size_t UseDefInfo::useCount(unsigned tempId) const {
    auto it = useCounts_.find(tempId);
    return it != useCounts_.end() ? it->second : 0;
}

} // namespace zanna::il
