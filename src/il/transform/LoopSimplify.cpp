//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements a conservative loop canonicalisation pass.  The pass ensures each
// natural loop has a dedicated preheader and optionally merges multiple trivial
// latches into a single forwarding block so downstream analyses observe a
// predictable structure.
//
//===----------------------------------------------------------------------===//

#include "il/transform/LoopSimplify.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/analysis/LoopInfo.hpp"
// Reuse shared CFG utilities to avoid duplication of value equality and
// terminator lookup logic used by SimplifyCFG.
#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/analysis/Dominators.hpp"
// Shared IL utilities (include after Dominators to avoid nested name lookup issues)
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Represents an incoming CFG edge using stable indices instead of pointers.
/// @details Using indices into function.blocks avoids pointer invalidation when
///          blocks are added to the function. The blockIdx field indexes into
///          function.blocks, and edgeIdx indexes into the terminator's labels.
struct IncomingEdge {
    /// Index of the predecessor block in `Function::blocks`.
    size_t blockIdx;
    /// Successor slot in the predecessor terminator.
    size_t edgeIdx;
};

/// @brief Finds the index of a block with the given label in function.blocks.
/// @param function Function whose block vector is searched.
/// @param label Block label to locate.
/// @return The index if found, or SIZE_MAX if not found.
size_t findBlockIndex(const Function &function, const std::string &label) {
    for (size_t i = 0; i < function.blocks.size(); ++i) {
        if (function.blocks[i].label == label)
            return i;
    }
    return SIZE_MAX;
}

/// @brief Return a mutable block terminator using the shared CFG utility.
/// @param block Block whose instruction tail is inspected.
/// @return Terminator pointer, or null when no valid terminator is present.
Instr *getTerminator(BasicBlock &block) {
    return il::transform::simplify_cfg::findTerminator(block);
}

/// @brief Return a read-only block terminator using the shared CFG utility.
/// @param block Block whose instruction tail is inspected.
/// @return Terminator pointer, or null when no valid terminator is present.
const Instr *getTerminator(const BasicBlock &block) {
    return il::transform::simplify_cfg::findTerminator(block);
}

/// @brief Compute the first unused temporary id in a function.
/// @param function Function whose SSA ids are scanned.
/// @return Fresh id suitable as the start of a consecutive allocation range.
static inline unsigned nextTempId(Function &function) {
    return zanna::il::nextTempId(function);
}

/// @brief Derive a block label that does not collide in a function.
/// @param function Function defining the label namespace.
/// @param base Preferred label before numeric suffixes.
/// @return @p base or a dot-suffixed unique variant.
std::string makeUniqueLabel(const Function &function, const std::string &base) {
    std::string candidate = base;
    unsigned suffix = 0;
    /// Return whether a candidate label is already used by a block.
    auto labelExists = [&](const std::string &label) {
        for (const auto &block : function.blocks) {
            if (block.label == label)
                return true;
        }
        return false;
    };

    while (labelExists(candidate)) {
        candidate = base + "." + std::to_string(++suffix);
    }
    return candidate;
}

/// @brief Collect value names already present in a function.
/// @details Includes the explicit value-name table and parameter names because
///          serializers and diagnostics may consult either source.
/// @param function Function whose names form the reservation set.
/// @return Set of non-empty names already used in @p function.
std::unordered_set<std::string> collectUsedValueNames(const Function &function) {
    std::unordered_set<std::string> used;
    for (const auto &name : function.valueNames)
        if (!name.empty())
            used.insert(name);
    for (const auto &param : function.params)
        if (!param.name.empty())
            used.insert(param.name);
    for (const auto &block : function.blocks)
        for (const auto &param : block.params)
            if (!param.name.empty())
                used.insert(param.name);
    return used;
}

/// @brief Reserve a unique value name derived from @p base.
/// @param used Mutable set of names already reserved.
/// @param base Preferred name, or "tmp" if empty.
/// @return A unique name inserted into @p used.
std::string reserveUniqueValueName(std::unordered_set<std::string> &used, std::string base) {
    if (base.empty())
        base = "tmp";
    if (used.insert(base).second)
        return base;

    unsigned suffix = 0;
    std::string candidate;
    do {
        candidate = base + "." + std::to_string(++suffix);
    } while (!used.insert(candidate).second);
    return candidate;
}

/// @brief Compare two branch-argument lists using shared IL value equality.
/// @param lhs First argument list.
/// @param rhs Second argument list.
/// @return `true` when both lists contain pairwise-equal values.
static inline bool valueVectorsEqual(const std::vector<Value> &lhs, const std::vector<Value> &rhs) {
    return il::transform::simplify_cfg::valueVectorsEqual(lhs, rhs);
}

/// @brief Create a unique forwarding preheader when a loop has multiple entry edges.
/// @param function Function receiving the new block and redirected edges.
/// @param loop Loop whose header entry is canonicalized.
/// @return `true` when a preheader is added, or `false` when none is needed or possible.
/// @details The new block clones header parameters with fresh ids, redirects
///          every out-of-loop edge to it, and forwards its parameters unchanged
///          to the original header.
bool ensurePreheader(Function &function, const Loop &loop) {
    size_t headerIdx = findBlockIndex(function, loop.headerLabel);
    if (headerIdx == SIZE_MAX)
        return false;

    // Capture header properties before any modifications.
    // We store the label and params separately to avoid pointer invalidation.
    const std::string headerLabel = function.blocks[headerIdx].label;
    const std::vector<Param> headerParams = function.blocks[headerIdx].params;

    // Collect edges from outside the loop that target the header.
    // Store block indices instead of pointers to survive vector reallocation.
    std::vector<IncomingEdge> outsideEdges;
    for (size_t blockIdx = 0; blockIdx < function.blocks.size(); ++blockIdx) {
        BasicBlock &block = function.blocks[blockIdx];
        Instr *term = getTerminator(block);
        if (!term)
            continue;
        for (size_t edgeIdx = 0; edgeIdx < term->labels.size(); ++edgeIdx) {
            if (term->labels[edgeIdx] != headerLabel)
                continue;
            if (loop.contains(block.label))
                continue;
            outsideEdges.push_back({blockIdx, edgeIdx});
        }
    }

    if (outsideEdges.empty())
        return false;

    // Check if there's already a dedicated preheader (single predecessor with
    // unconditional branch to header).
    bool hasDedicatedPreheader = false;
    if (outsideEdges.size() == 1) {
        const auto &edge = outsideEdges.front();
        const Instr *term = getTerminator(function.blocks[edge.blockIdx]);
        hasDedicatedPreheader =
            term && term->labels.size() == 1 && term->labels.front() == headerLabel;
    }

    if (hasDedicatedPreheader)
        return false;

    std::string base = headerLabel + ".preheader";
    std::string preheaderLabel = makeUniqueLabel(function, base);

    // Build the preheader block with cloned parameters.
    BasicBlock preheader;
    preheader.label = preheaderLabel;

    unsigned id = nextTempId(function);
    auto usedValueNames = collectUsedValueNames(function);
    preheader.params.reserve(headerParams.size());
    for (const auto &param : headerParams) {
        Param clone = param;
        clone.id = id++;
        clone.name = reserveUniqueValueName(usedValueNames, clone.name + ".preheader");
        preheader.params.push_back(clone);
        if (function.valueNames.size() <= clone.id)
            function.valueNames.resize(clone.id + 1);
        function.valueNames[clone.id] = clone.name;
    }

    // Create unconditional branch to the original header.
    Instr branch;
    branch.op = Opcode::Br;
    branch.type = Type(Type::Kind::Void);
    branch.addBranchTarget(headerLabel);
    auto &forwardArgs = branch.brArgs.back();
    forwardArgs.reserve(preheader.params.size());
    for (const auto &param : preheader.params)
        forwardArgs.push_back(Value::temp(param.id));
    preheader.instructions.push_back(std::move(branch));
    preheader.terminated = true;

    // Redirect outside edges to the new preheader.
    // Use indices to access blocks safely even after potential reallocation.
    for (const auto &edge : outsideEdges) {
        Instr *term = getTerminator(function.blocks[edge.blockIdx]);
        if (!term)
            continue;
        term->labels[edge.edgeIdx] = preheaderLabel;
    }

    function.blocks.push_back(std::move(preheader));
    return true;
}

/// @brief Merge multiple equivalent forwarding latches behind one dedicated block.
/// @param function Function receiving the merged latch and redirected edges.
/// @param loop Loop whose recorded latch set is examined.
/// @return `true` when a new latch is created.
/// @details All source latches must consist solely of an unconditional branch
///          to the header and must carry identical argument vectors. Otherwise
///          the loop is left unchanged.
bool mergeTrivialLatches(Function &function, const Loop &loop) {
    if (loop.latchLabels.size() <= 1)
        return false;

    size_t headerIdx = findBlockIndex(function, loop.headerLabel);
    if (headerIdx == SIZE_MAX)
        return false;

    // Capture header properties before any modifications.
    const std::string headerLabel = function.blocks[headerIdx].label;
    const std::vector<Param> headerParams = function.blocks[headerIdx].params;

    // Collect latch block indices instead of pointers to survive vector reallocation.
    std::vector<size_t> latchIndices;
    latchIndices.reserve(loop.latchLabels.size());
    for (const auto &label : loop.latchLabels) {
        size_t idx = findBlockIndex(function, label);
        if (idx != SIZE_MAX)
            latchIndices.push_back(idx);
    }

    if (latchIndices.size() <= 1)
        return false;

    // Validate that all latches are trivial (single unconditional branch to header)
    // and collect canonical arguments.
    std::vector<Value> canonicalArgs;
    canonicalArgs.reserve(headerParams.size());

    for (size_t i = 0; i < latchIndices.size(); ++i) {
        const BasicBlock &latch = function.blocks[latchIndices[i]];
        const Instr *term = getTerminator(latch);
        if (!term || term->op != Opcode::Br)
            return false;
        if (latch.instructions.size() != 1)
            return false;
        if (term->labels.size() != 1 || term->labels.front() != headerLabel)
            return false;
        std::vector<Value> args;
        if (!term->brArgs.empty())
            args = term->brArgs.front();
        if (i == 0) {
            canonicalArgs = args;
        } else if (!valueVectorsEqual(canonicalArgs, args)) {
            return false;
        }
    }

    std::string base = headerLabel + ".latch";
    std::string newLatchLabel = makeUniqueLabel(function, base);

    // Build the merged latch block with cloned parameters.
    BasicBlock newLatch;
    newLatch.label = newLatchLabel;

    unsigned id = nextTempId(function);
    auto usedValueNames = collectUsedValueNames(function);
    newLatch.params.reserve(headerParams.size());
    for (const auto &param : headerParams) {
        Param clone = param;
        clone.id = id++;
        clone.name = reserveUniqueValueName(usedValueNames, clone.name + ".latch");
        newLatch.params.push_back(clone);
        if (function.valueNames.size() <= clone.id)
            function.valueNames.resize(clone.id + 1);
        function.valueNames[clone.id] = clone.name;
    }

    // Create unconditional branch to the original header.
    Instr branch;
    branch.op = Opcode::Br;
    branch.type = Type(Type::Kind::Void);
    branch.addBranchTarget(headerLabel);
    auto &forwardArgs = branch.brArgs.back();
    forwardArgs.reserve(newLatch.params.size());
    for (const auto &param : newLatch.params)
        forwardArgs.push_back(Value::temp(param.id));
    newLatch.instructions.push_back(std::move(branch));
    newLatch.terminated = true;

    // Redirect all latch branches to the new merged latch.
    // Use indices to access blocks safely even after potential reallocation.
    for (size_t latchIdx : latchIndices) {
        Instr *term = getTerminator(function.blocks[latchIdx]);
        if (!term)
            continue;
        term->labels.front() = newLatchLabel;
        if (!canonicalArgs.empty())
            term->brArgs.front() = canonicalArgs;
        else if (!term->brArgs.empty())
            term->brArgs.front().clear();
    }

    function.blocks.push_back(std::move(newLatch));
    return true;
}

} // namespace

/// @copydoc LoopSimplify::id()
std::string_view LoopSimplify::id() const {
    return "loop-simplify";
}

/// @copydoc LoopSimplify::run()
PreservedAnalyses LoopSimplify::run(Function &function, AnalysisManager &analysis) {
    bool changed = false;
    for (;;) {
        LoopInfo loopInfo = computeLoopInfo(analysis.module(), function);
        bool changedThisIteration = false;

        for (const Loop &loop : loopInfo.loops()) {
            if (ensurePreheader(function, loop) || mergeTrivialLatches(function, loop)) {
                changed = true;
                changedThisIteration = true;
                break;
            }
        }

        if (!changedThisIteration)
            break;
    }

    if (!changed)
        return PreservedAnalyses::all();

    PreservedAnalyses preserved;
    preserved.preserveAllModules();
    return preserved;
}

} // namespace il::transform
