//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/Reassociate.cpp
// Purpose: Canonicalizes commutative+associative expression trees.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements operand canonicalization for commutative+associative ops.
/// @details Flattens single-use same-block trees, sorts their leaves, and
///          rebuilds a deterministic right-deep tree. Binary operands are also
///          sorted when a larger tree cannot safely be rewritten.

#include "il/transform/Reassociate.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::transform {

using namespace il::core;

namespace {

/// @brief Check if an opcode is commutative and associative for integers.
/// @details Only includes operations where reassociation is provably correct:
///          plain integer arithmetic (no overflow checks) and bitwise ops.
/// @param op Opcode to classify.
/// @return `true` for the supported plain add/multiply and bitwise operations.
bool isReassociable(Opcode op) {
    switch (op) {
        case Opcode::Add:
        case Opcode::Mul:
        case Opcode::And:
        case Opcode::Or:
        case Opcode::Xor:
            return true;
        default:
            return false;
    }
}

/// @brief Compute a canonical rank for operand sorting.
/// @details Constants get the lowest rank (sorted last in descending order)
///          so that `a + 1` and `1 + a` both become `a + 1` (temp first).
///          Among temporaries, higher IDs rank higher for a stable sort.
/// @param v Operand to rank.
/// @return Pair of value-kind priority and deterministic within-kind payload.
std::pair<unsigned, std::uint64_t> operandRank(const Value &v) {
    switch (v.kind) {
        case Value::Kind::Temp:
            return {4, v.id}; // High rank → sorted first
        case Value::Kind::ConstInt:
            return {1, static_cast<std::uint64_t>(v.i64)};
        case Value::Kind::ConstFloat:
            return {2, 0};
        case Value::Kind::ConstStr:
        case Value::Kind::GlobalAddr:
            return {3, 0};
        case Value::Kind::NullPtr:
            return {0, 0};
    }
    return {0, 0};
}

/// @brief Canonicalize operand order for a single instruction.
/// @param I Candidate binary instruction, modified in place.
/// @return `true` if the operand order was changed.
bool canonicalizeOperands(Instr &I) {
    if (I.operands.size() != 2)
        return false;
    if (!isReassociable(I.op))
        return false;

    const auto rank0 = operandRank(I.operands[0]);
    const auto rank1 = operandRank(I.operands[1]);

    // Sort by descending rank: higher-ranked operand first.
    // This puts temporaries before constants.
    if (rank0 < rank1) {
        std::swap(I.operands[0], I.operands[1]);
        return true;
    }
    return false;
}

/// @brief Same-block instruction indices keyed by result temporary id.
using DefIndex = std::unordered_map<unsigned, std::size_t>;

/// @brief Recursively flatten a single-use same-op operand subtree.
/// @param block Block containing all candidate definitions.
/// @param opcode Associative opcode required at every internal node.
/// @param value Current operand to classify as a leaf or descend through.
/// @param rootIndex Exclusive upper bound on usable definition indices.
/// @param defs Same-block result-definition index.
/// @param useCounts Function-wide use counts that enforce single-use nodes.
/// @param visited Temporary ids already expanded, preventing malformed cycles.
/// @param treeNodes Output instruction indices belonging to the flattened tree.
/// @param leaves Output operands at the frontier of that tree.
/// @return `true` after safely classifying the operand.
bool flattenOperand(const BasicBlock &block,
                    Opcode opcode,
                    const Value &value,
                    std::size_t rootIndex,
                    const DefIndex &defs,
                    const std::unordered_map<unsigned, unsigned> &useCounts,
                    std::unordered_set<unsigned> &visited,
                    std::vector<std::size_t> &treeNodes,
                    std::vector<Value> &leaves) {
    if (value.kind != Value::Kind::Temp) {
        leaves.push_back(value);
        return true;
    }
    auto defIt = defs.find(value.id);
    auto useIt = useCounts.find(value.id);
    if (defIt == defs.end() || defIt->second >= rootIndex || useIt == useCounts.end() ||
        useIt->second != 1) {
        leaves.push_back(value);
        return true;
    }
    Instr const &def = block.instructions[defIt->second];
    if (def.op != opcode || def.operands.size() != 2 || !visited.insert(value.id).second) {
        leaves.push_back(value);
        return true;
    }
    if (!flattenOperand(block,
                        opcode,
                        def.operands[0],
                        defIt->second,
                        defs,
                        useCounts,
                        visited,
                        treeNodes,
                        leaves) ||
        !flattenOperand(block,
                        opcode,
                        def.operands[1],
                        defIt->second,
                        defs,
                        useCounts,
                        visited,
                        treeNodes,
                        leaves)) {
        return false;
    }
    treeNodes.push_back(defIt->second);
    return true;
}

/// @brief Rebuild an associative expression tree in deterministic right-deep order.
/// @param block Block containing the root and all eligible internal nodes.
/// @param rootIndex Index of the candidate root instruction.
/// @param defs Same-block definition index.
/// @param useCounts Function-wide temporary use counts.
/// @return `true` when any node's operand vector changes.
/// @details Multi-use and forward-defined temporaries remain leaves. A tree with
///          inconsistent node/leaf cardinality falls back to binary operand sorting.
bool canonicalizeTree(BasicBlock &block,
                      std::size_t rootIndex,
                      const DefIndex &defs,
                      const std::unordered_map<unsigned, unsigned> &useCounts) {
    Instr &root = block.instructions[rootIndex];
    if (!root.result || !isReassociable(root.op) || root.operands.size() != 2)
        return false;

    std::vector<std::size_t> nodes;
    std::vector<Value> leaves;
    std::unordered_set<unsigned> visited;
    flattenOperand(block,
                   root.op,
                   root.operands[0],
                   rootIndex,
                   defs,
                   useCounts,
                   visited,
                   nodes,
                   leaves);
    flattenOperand(block,
                   root.op,
                   root.operands[1],
                   rootIndex,
                   defs,
                   useCounts,
                   visited,
                   nodes,
                   leaves);
    nodes.push_back(rootIndex);
    if (leaves.size() != nodes.size() + 1)
        return canonicalizeOperands(root);

    /// Sort operands by descending canonical rank before rebuilding the tree.
    std::sort(leaves.begin(), leaves.end(), [](const Value &lhs, const Value &rhs) {
        return operandRank(lhs) > operandRank(rhs);
    });
    std::sort(nodes.begin(), nodes.end());

    bool changed = false;
    Value accumulated;
    for (std::size_t nodePos = 0; nodePos < nodes.size(); ++nodePos) {
        Instr &node = block.instructions[nodes[nodePos]];
        const std::size_t leafIndex = leaves.size() - 2 - nodePos;
        std::vector<Value> replacement;
        if (nodePos == 0)
            replacement = {leaves[leafIndex], leaves.back()};
        else
            replacement = {leaves[leafIndex], accumulated};
        const bool sameOperands = node.operands.size() == replacement.size() &&
                                  valueEquals(node.operands[0], replacement[0]) &&
                                  valueEquals(node.operands[1], replacement[1]);
        if (!sameOperands) {
            node.operands = std::move(replacement);
            changed = true;
        }
        accumulated = Value::temp(*node.result);
    }
    return changed;
}

} // namespace

/// @copydoc reassociate()
void reassociate(Module &M) {
    for (auto &F : M.functions) {
        std::unordered_map<unsigned, unsigned> useCounts;
        for (const auto &B : F.blocks)
            for (const auto &I : B.instructions)
                for (const auto &operand : I.operands)
                    if (operand.kind == Value::Kind::Temp)
                        ++useCounts[operand.id];
        for (auto &B : F.blocks) {
            DefIndex defs;
            for (std::size_t i = 0; i < B.instructions.size(); ++i)
                if (B.instructions[i].result)
                    defs.emplace(*B.instructions[i].result, i);
            for (std::size_t i = 0; i < B.instructions.size(); ++i)
                canonicalizeTree(B, i, defs, useCounts);
        }
    }
    M.internOwnedIdentifiers();
}

} // namespace il::transform
