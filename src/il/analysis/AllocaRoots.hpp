//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/AllocaRoots.hpp
// Purpose: Shared helpers for resolving stack allocation roots through derived
//          pointer values and block-parameter forwarding.
// Key invariants:
//   - Only SSA temporaries with recorded definitions participate in root sets.
//   - GEP-derived pointers inherit roots from their base pointer.
//   - Branch arguments propagate roots into destination block parameters.
// Ownership/Lifetime: Header-only value helpers; all returned containers own
//          their data and borrow nothing from the source function.
// Links: il/analysis/MemorySSA.cpp, il/transform/DSE.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines header-only discovery and escape analysis for alloca-derived pointers.
/// @details The worklist propagates root allocation identities through GEP
///          definitions and branch arguments, resolves unique roots safely in
///          cyclic graphs, and identifies stack addresses that cannot escape.

#pragma once

#include "il/core/Function.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::analysis {

/// @brief Minimal definition summary needed for alloca-root propagation.
/// @details Analyses only need the defining opcode and operands to decide
///          whether a temporary is an alloca, a derived GEP, or an unrelated
///          value.  Keeping this summary separate avoids coupling transforms to
///          the full instruction object lifetime.
struct AllocaRootDefInfo {
    il::core::Opcode op{il::core::Opcode::Count}; ///< Defining opcode.
    std::vector<il::core::Value> operands;        ///< Defining instruction operands.
};

/// @brief Maps a temporary id to the set of alloca ids it may derive from.
using AllocaRootMap = std::unordered_map<unsigned, std::unordered_set<unsigned>>;

/// @brief Collect temporary definitions required for alloca-root analysis.
/// @param F Function to scan.
/// @return Map from result temporary id to opcode/operand summary.
inline std::unordered_map<unsigned, AllocaRootDefInfo> collectAllocaRootDefs(
    const il::core::Function &F) {
    std::unordered_map<unsigned, AllocaRootDefInfo> defs;
    defs.reserve(F.valueNames.size());
    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            if (I.result)
                defs.emplace(*I.result, AllocaRootDefInfo{I.op, I.operands});
        }
    }
    return defs;
}

/// @brief Compute alloca roots for temporaries and forwarded block parameters.
/// @details Roots are propagated to GEP results from their base operand and to
///          block parameters from predecessor branch arguments. A reverse-use
///          worklist propagates only from temporaries whose root set changed,
///          avoiding repeated full scans of every definition and edge.
/// @param F Function whose block parameters and terminators are inspected.
/// @param defs Definition summary from @ref collectAllocaRootDefs.
/// @return Temporary-to-alloca-root map.
inline AllocaRootMap computeAllocaRoots(
    const il::core::Function &F, const std::unordered_map<unsigned, AllocaRootDefInfo> &defs) {
    using namespace il::core;

    AllocaRootMap roots;
    roots.reserve(defs.size());

    std::vector<unsigned> worklist;
    worklist.reserve(defs.size());

    /// @brief Seeds an alloca temporary as its own root.
    /// @param id Alloca temporary identifier.
    auto seedRoot = [&](unsigned id) {
        if (roots[id].insert(id).second)
            worklist.push_back(id);
    };

    for (const auto &[id, def] : defs) {
        if (def.op == Opcode::Alloca)
            seedRoot(id);
    }

    std::unordered_map<unsigned, std::vector<unsigned>> reverseUsers;
    reverseUsers.reserve(defs.size());
    for (const auto &[id, def] : defs) {
        if (def.op == Opcode::GEP && !def.operands.empty() &&
            def.operands[0].kind == Value::Kind::Temp)
            reverseUsers[def.operands[0].id].push_back(id);
    }

    std::unordered_map<std::string, const BasicBlock *> blocksByLabel;
    blocksByLabel.reserve(F.blocks.size());
    for (const auto &B : F.blocks)
        blocksByLabel.emplace(B.label, &B);

    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            for (std::size_t edge = 0; edge < I.labels.size() && edge < I.brArgs.size(); ++edge) {
                auto targetIt = blocksByLabel.find(I.labels[edge]);
                if (targetIt == blocksByLabel.end())
                    continue;

                const auto *target = targetIt->second;
                const auto &args = I.brArgs[edge];
                const std::size_t count = std::min(args.size(), target->params.size());
                for (std::size_t idx = 0; idx < count; ++idx) {
                    if (args[idx].kind == Value::Kind::Temp)
                        reverseUsers[args[idx].id].push_back(target->params[idx].id);
                }
            }
        }
    }

    /// @brief Merges one temporary's root set into another.
    /// @param dst Destination temporary identifier.
    /// @param src Source temporary identifier.
    /// @return `true` when the destination root set changed.
    auto mergeRootsFromTemp = [&](unsigned dst, unsigned src) {
        auto srcIt = roots.find(src);
        if (srcIt == roots.end())
            return false;
        auto &dstRoots = roots[dst];
        bool changed = false;
        for (unsigned root : srcIt->second)
            changed |= dstRoots.insert(root).second;
        return changed;
    };

    while (!worklist.empty()) {
        const unsigned src = worklist.back();
        worklist.pop_back();

        auto usersIt = reverseUsers.find(src);
        if (usersIt == reverseUsers.end())
            continue;

        for (unsigned dst : usersIt->second) {
            if (mergeRootsFromTemp(dst, src))
                worklist.push_back(dst);
        }
    }

    for (const auto &[id, def] : defs) {
        if (def.op == Opcode::Alloca && !roots.contains(id))
            roots[id].insert(id);
    }

    return roots;
}

/// @brief Resolve a pointer temporary to its unique root alloca id.
/// @details Follows direct alloca definitions and GEP bases iteratively. A
///          visited set rejects malformed cyclic SSA graphs without imposing an
///          arbitrary limit on valid derived-pointer chains.
/// @param ptr Pointer value to inspect.
/// @param defs Definition summary from @ref collectAllocaRootDefs.
/// @return Root alloca id, or nullopt if no unique alloca root is found.
inline std::optional<unsigned> getAllocaId(
    const il::core::Value &ptr,
    const std::unordered_map<unsigned, AllocaRootDefInfo> &defs) {
    using namespace il::core;

    const Value *current = &ptr;
    std::unordered_set<unsigned> visited;
    while (current->kind == Value::Kind::Temp) {
        if (!visited.insert(current->id).second)
            return std::nullopt;
        auto it = defs.find(current->id);
        if (it == defs.end())
            return std::nullopt;
        if (it->second.op == Opcode::Alloca)
            return current->id;
        if (it->second.op != Opcode::GEP || it->second.operands.empty())
            return std::nullopt;
        current = &it->second.operands[0];
    }
    return std::nullopt;
}

/// @brief Test whether a value may carry a particular alloca root.
/// @param value Value to inspect.
/// @param allocaId Root alloca id being queried.
/// @param roots Root map from @ref computeAllocaRoots.
/// @return True if @p value is a temporary derived from @p allocaId.
inline bool valueContainsAllocaRoot(const il::core::Value &value,
                                    unsigned allocaId,
                                    const AllocaRootMap &roots) {
    if (value.kind != il::core::Value::Kind::Temp)
        return false;
    auto rootIt = roots.find(value.id);
    return rootIt != roots.end() && rootIt->second.count(allocaId) != 0;
}

/// @brief Check whether an alloca-derived address escapes the function.
/// @details Escapes are effectful calls receiving the address, stores of the
///          address into memory, and returns of the address. Explicitly pure
///          calls are ignored because capturing or dereferencing the pointer
///          would contradict the call's no-side-effect/no-memory contract.
///          Branch argument forwarding is already accounted for in @p roots.
/// @param F Function to inspect.
/// @param allocaId Root alloca id.
/// @param roots Root map from @ref computeAllocaRoots.
/// @return True when the alloca address may escape.
inline bool allocaEscapes(const il::core::Function &F,
                          unsigned allocaId,
                          const AllocaRootMap &roots) {
    using namespace il::core;

    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            if (I.op == Opcode::Call || I.op == Opcode::CallIndirect) {
                if (I.CallAttr.pure)
                    continue;
                for (const auto &op : I.operands)
                    if (valueContainsAllocaRoot(op, allocaId, roots))
                        return true;
            }
            if (I.op == Opcode::Store && I.operands.size() >= 2) {
                if (valueContainsAllocaRoot(I.operands[1], allocaId, roots))
                    return true;
            }
            if (I.op == Opcode::Ret) {
                for (const auto &op : I.operands)
                    if (valueContainsAllocaRoot(op, allocaId, roots))
                        return true;
            }
        }
    }
    return false;
}

/// @brief Compute all alloca ids whose addresses do not escape.
/// @param F Function to inspect.
/// @param defs Definition summary from @ref collectAllocaRootDefs.
/// @param roots Root map from @ref computeAllocaRoots.
/// @return Set of alloca result ids proven non-escaping.
inline std::unordered_set<unsigned> nonEscapingAllocas(
    const il::core::Function &F,
    const std::unordered_map<unsigned, AllocaRootDefInfo> &defs,
    const AllocaRootMap &roots) {
    using namespace il::core;

    std::unordered_set<unsigned> result;
    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            if (I.op == Opcode::Alloca && I.result) {
                if (!allocaEscapes(F, *I.result, roots))
                    result.insert(*I.result);
            }
        }
    }
    return result;
}

} // namespace zanna::analysis
