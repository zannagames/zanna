//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the memory-to-register promotion pass using the "seal and rename"
// algorithm.  Allocations whose addresses do not escape are rewritten into SSA
// temporaries by introducing block parameters that model phi nodes.  The pass
// runs entirely in place, mutating control-flow edges and instruction operands
// while tracking statistics for promoted variables and eliminated loads/stores.
//
// KNOWN LIMITATIONS:
//
// 1. Cross-block non-entry allocas are promoted only when the defining block
//    dominates all uses and is not re-entered by a loop backedge. Re-executing
//    a dynamic alloca inside a loop creates a fresh slot; carrying promoted
//    state across that backedge would change semantics.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements bounded SROA and sealed-SSA memory-to-register promotion.
 *
 * @details Small aggregate allocas are first partitioned into non-overlapping,
 *          consistently typed scalar fields. Promotable scalar allocas then use
 *          a seal-and-rename algorithm that creates block parameters, wires
 *          edge arguments, substitutes loads, removes stores/allocas, and
 *          repairs remaining argument gaps. Each function is snapshotted so an
 *          unrecoverable CFG edge rolls the transformation back atomically.
 */

#include "il/transform/Mem2Reg.hpp"
#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/utils/Utils.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <functional>
#include <iostream>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace il::core;

namespace zanna::passes {
namespace {
/// @brief Maximum number of disjoint scalar fields produced from one allocation.
constexpr unsigned kMaxSROAFields = 8;

/// @brief Largest aggregate allocation considered by the scalar-replacement prepass.
constexpr unsigned kMaxSROAAllocaSize = 128;

/// @brief One typed byte range discovered within an SROA candidate.
struct SROAField {
    /// @brief Scalar type used consistently by every access to the range.
    Type type{};
    /// @brief Width of the range in bytes.
    unsigned size = 0;
    /// @brief Fresh allocation id assigned when scalar replacement commits.
    unsigned allocaId = 0;
};

/// @brief Aggregate allocation and derived-pointer facts used by the SROA prepass.
/// @details A candidate remains valid only while every observed use is a typed,
///          in-bounds scalar load/store or a constant-offset GEP.
struct SROACandidate {
    /// @brief Block containing the original allocation.
    BasicBlock *block = nullptr;
    /// @brief Original instruction index, used only as an initial location hint.
    std::size_t allocaIndex = 0;
    /// @brief Result id of the aggregate allocation.
    unsigned baseId = 0;
    /// @brief Constant aggregate size in bytes.
    unsigned allocSize = 0;
    /// @brief Whether all uses seen so far remain replaceable.
    bool ok = false;
    /// @brief Byte offset for the base and each derived pointer temporary.
    std::unordered_map<unsigned, unsigned> offsets; // temp id -> byte offset (includes base)
    /// @brief Scalar field information indexed by byte offset.
    std::unordered_map<unsigned, SROAField> fields; // offset -> field info
};

/// @brief Test whether mem2reg can represent a type directly in SSA.
/// @param type Candidate allocation access type.
/// @return `true` for supported integer, boolean, and floating-point scalar types.
static bool isPromotableScalarType(const Type &type) {
    switch (type.kind) {
        case Type::Kind::I1:
        case Type::Kind::I16:
        case Type::Kind::I32:
        case Type::Kind::I64:
        case Type::Kind::F64:
            return true;
        default:
            return false;
    }
}

/// @brief Return the storage width of a promotable scalar type.
/// @param type Scalar type to measure.
/// @return Width in bytes, or `std::nullopt` for an unsupported type.
static std::optional<unsigned> scalarSize(const Type &type) {
    switch (type.kind) {
        case Type::Kind::I1:
            return 1;
        case Type::Kind::I16:
            return 2;
        case Type::Kind::I32:
            return 4;
        case Type::Kind::I64:
        case Type::Kind::F64:
            return 8;
        default:
            return std::nullopt;
    }
}

/// @brief Extract an unsigned byte offset from an IL integer constant.
/// @param v Candidate offset value.
/// @return Offset when nonnegative and representable as `unsigned`, otherwise null.
static std::optional<unsigned> constOffset(const Value &v) {
    if (v.kind != Value::Kind::ConstInt || v.i64 < 0)
        return std::nullopt;
    if (v.i64 > static_cast<int64_t>(UINT_MAX))
        return std::nullopt;
    return static_cast<unsigned>(v.i64);
}

/// @brief Store a nonempty diagnostic name for an SSA id.
/// @param F Function whose value-name table is extended.
/// @param id Temporary or parameter id to name.
/// @param name Name to record; an empty value is ignored.
static void ensureValueName(Function &F, unsigned id, const std::string &name) {
    if (name.empty())
        return;
    if (F.valueNames.size() <= id)
        F.valueNames.resize(id + 1);
    F.valueNames[id] = name;
}

/// @brief Use and type facts collected for one allocation.
struct AllocaInfo {
    /// @brief Block containing the allocation instruction.
    BasicBlock *block{nullptr};
    /// @brief Result identifier of the allocation pointer.
    unsigned id{0};
    /// @brief Consistent scalar type observed across loads and stores.
    Type type{};
    /// @brief Whether any use exposes the pointer beyond direct loads/stores.
    bool addressTaken{false};
    /// @brief Whether at least one store initializes the allocation.
    bool hasStore{false};
    /// @brief Whether all observed uses remain in the defining block.
    bool singleBlock{true};
    /// @brief Whether all direct memory accesses agree on one scalar type.
    bool typeConsistent{true};           ///< False if loads/stores use different types.
    /// @brief Non-defining blocks that contain uses of the allocation.
    std::vector<BasicBlock *> useBlocks; ///< Blocks (other than defining) containing uses.
};

/// @brief Current sealed-SSA state for one promoted allocation.
struct VarState {
    /// Scalar type of values stored in the allocation.
    Type type{};
    /// Block that dynamically creates the original allocation.
    BasicBlock *allocaBlock = nullptr;
    /// Value observed before the first promoted store.
    Value initialValue{};
    /// Reaching promoted value at blocks already visited.
    std::unordered_map<BasicBlock *, Value> defs;
};

/// @brief Per-block bookkeeping for incremental SSA sealing.
struct BlockState {
    /// Whether every predecessor edge is known and may be queried.
    bool sealed = false;
    /// Number of incoming CFG edges, including duplicate successor slots.
    unsigned totalPreds = 0;
    /// Number of predecessor visits observed by the traversal.
    unsigned seenPreds = 0;
    /// Destination parameter index allocated for each promoted variable.
    std::unordered_map<unsigned, unsigned> params;
    /// Variables awaiting predecessor values before the block can be sealed.
    std::vector<unsigned> incomplete;
};

/// @brief Allocation metadata indexed by original pointer result id.
using AllocaMap = std::unordered_map<unsigned, AllocaInfo>;
/// @brief Promotion state indexed by original allocation id.
using VarMap = std::unordered_map<unsigned, VarState>;
/// @brief Sealing state indexed by block address.
using BlockMap = std::unordered_map<BasicBlock *, BlockState>;
/// @brief Transitive replacement values indexed by removed result id.
using ReplacementMap = std::unordered_map<unsigned, Value>;

/// @brief One concrete incoming successor slot and its predecessor block.
struct IncomingEdge {
    /// Block owning the terminator edge.
    BasicBlock *pred = nullptr;
    /// Index into the terminator's label and branch-argument vectors.
    std::size_t edgeIndex = 0;
};

/// @brief Incoming edges indexed by their target block.
using IncomingEdgeMap = std::unordered_map<const BasicBlock *, std::vector<IncomingEdge>>;

/// @brief Return a zero literal compatible with @p type.
/// @details Mem2Reg synthesizes an initial value for loads that can reach an
///          uninitialized promoted stack slot.  Boolean values must retain the
///          IL boolean flag so downstream printers and optimizers do not widen
///          them to an integer-looking constant.
/// @param type Promoted scalar type.
/// @return False for @c i1, @c 0.0 for @c f64, and integer zero otherwise.
static Value zeroValueForType(const Type &type) {
    switch (type.kind) {
        case Type::Kind::I1:
            return Value::constBool(false);
        case Type::Kind::F64:
            return Value::constFloat(0.0);
        default:
            return Value::constInt(0);
    }
}

/// @brief Follow a chain of removed temporary substitutions to its final value.
/// @param replacements Mapping from removed ids to replacement values.
/// @param value Initial value to resolve.
/// @return First non-temporary or unmapped/cyclic temporary reached.
static Value resolveReplacementValue(const ReplacementMap &replacements, Value value) {
    std::unordered_set<unsigned> seen;
    while (value.kind == Value::Kind::Temp) {
        auto it = replacements.find(value.id);
        if (it == replacements.end() || seen.contains(value.id))
            break;
        seen.insert(value.id);
        value = it->second;
    }
    return value;
}

/// @brief Rewrite one value through the transitive replacement map.
/// @param value Value updated in place.
/// @param replacements Mapping produced while eliminating promoted loads.
static void rewriteValue(Value &value, const ReplacementMap &replacements) {
    value = resolveReplacementValue(replacements, value);
}

/// @brief Apply promoted-value substitutions to all uses in an instruction.
/// @param instr Instruction whose operands and branch bundles are rewritten.
/// @param replacements Mapping from removed results to surviving values.
static void rewriteInstructionUses(Instr &instr, const ReplacementMap &replacements) {
    if (replacements.empty())
        return;
    for (auto &operand : instr.operands)
        rewriteValue(operand, replacements);
    for (auto &argList : instr.brArgs)
        for (auto &arg : argList)
            rewriteValue(arg, replacements);
}

/// @brief Apply all accumulated substitutions throughout a function.
/// @param F Function whose instruction uses are rewritten.
/// @param replacements Mapping from eliminated load results to promoted values.
static void applyReplacements(Function &F, const ReplacementMap &replacements) {
    if (replacements.empty())
        return;
    for (auto &B : F.blocks)
        for (auto &I : B.instructions)
            rewriteInstructionUses(I, replacements);
}

/// @brief Mark a variable as awaiting values without introducing duplicates.
/// @param state Unsealed block state updated in place.
/// @param varId Original allocation id to append when absent.
static void addIncomplete(BlockState &state, unsigned varId) {
    if (std::find(state.incomplete.begin(), state.incomplete.end(), varId) ==
        state.incomplete.end()) {
        state.incomplete.push_back(varId);
    }
}

/// @brief Build an edge-specific predecessor index for @p F.
/// @details The seal-and-rename algorithm repeatedly asks for the incoming
///          edges of the same join blocks.  Indexing once avoids recursive
///          O(blocks * edges) rescans while preserving duplicate edges, which
///          are semantically distinct because each carries its own argument
///          bundle.
/// @param F Function whose terminators are indexed.
/// @return Map from target block pointer to incoming predecessor edges.
static IncomingEdgeMap buildIncomingEdgeMap(Function &F) {
    IncomingEdgeMap incoming;
    incoming.reserve(F.blocks.size());

    std::unordered_map<std::string, BasicBlock *> labels;
    labels.reserve(F.blocks.size());
    for (auto &block : F.blocks) {
        incoming.emplace(&block, std::vector<IncomingEdge>{});
        labels.emplace(block.label, &block);
    }

    for (auto &pred : F.blocks) {
        if (pred.instructions.empty())
            continue;
        const Instr &term = pred.instructions.back();
        for (std::size_t edge = 0; edge < term.labels.size(); ++edge) {
            auto targetIt = labels.find(term.labels[edge]);
            if (targetIt == labels.end())
                continue;
            incoming[targetIt->second].push_back(IncomingEdge{&pred, edge});
        }
    }
    return incoming;
}

/// @brief Look up incoming edges for @p target in a precomputed edge map.
/// @param incoming Edge map returned by @ref buildIncomingEdgeMap.
/// @param target Target block to query.
/// @return Borrowed edge list, or an immutable empty list for null/unindexed blocks.
static const std::vector<IncomingEdge> &incomingEdges(const IncomingEdgeMap &incoming,
                                                      const BasicBlock *target) {
    static const std::vector<IncomingEdge> empty;
    if (!target)
        return empty;
    auto it = incoming.find(target);
    return it == incoming.end() ? empty : it->second;
}

/// @brief Gather information about @c alloca instructions within a function.
///
/// @details Performs two sweeps over @p F.  The first collects every alloca
/// result and records its defining block.  The second inspects each use to mark
/// whether the address escapes, whether a store writes to it, whether all uses
/// stay inside a single block, and which blocks contain uses.
/// The resulting table drives the promotion logic; the caller uses the
/// @c useBlocks field with a dominator tree to filter non-entry-block allocas.
///
/// @param F Function to analyze.
/// @return Map from temp ids to their @c AllocaInfo metadata.
static AllocaMap collectAllocas(Function &F) {
    AllocaMap infos;
    infos.reserve(F.valueNames.size());
    // Collect allocas from ALL blocks. The promotion filter in mem2reg() uses
    // dominance to decide which non-entry allocas are safe to promote.
    for (auto &B : F.blocks)
        for (auto &I : B.instructions)
            if (I.op == Opcode::Alloca && I.result)
                infos[*I.result] = AllocaInfo{&B, *I.result, Type{}, false, false, true, true, {}};

    for (auto &B : F.blocks)
        for (auto &I : B.instructions)
            for (std::size_t oi = 0; oi < I.operands.size(); ++oi) {
                Value &Op = I.operands[oi];
                if (Op.kind != Value::Kind::Temp)
                    continue;
                auto it = infos.find(Op.id);
                if (it == infos.end())
                    continue;
                AllocaInfo &AI = it->second;
                if (&B != AI.block) {
                    AI.singleBlock = false;
                    AI.useBlocks.push_back(&B);
                }
                if (I.op == Opcode::Store && oi == 0) {
                    AI.hasStore = true;
                    // Check type consistency: if type was already set and differs, mark
                    // inconsistent
                    if (AI.type.kind != Type::Kind::Void && AI.type.kind != I.type.kind)
                        AI.typeConsistent = false;
                    AI.type = I.type;
                } else if (I.op == Opcode::Load && oi == 0) {
                    // Check type consistency: if type was already set and differs, mark
                    // inconsistent
                    if (AI.type.kind != Type::Kind::Void && AI.type.kind != I.type.kind)
                        AI.typeConsistent = false;
                    AI.type = I.type;
                } else {
                    AI.addressTaken = true;
                }
            }
    for (auto &B : F.blocks) {
        for (auto &I : B.instructions) {
            for (const auto &bundle : I.brArgs) {
                for (const auto &arg : bundle) {
                    if (arg.kind != Value::Kind::Temp)
                        continue;
                    auto it = infos.find(arg.id);
                    if (it == infos.end())
                        continue;
                    AllocaInfo &AI = it->second;
                    AI.addressTaken = true;
                    if (&B != AI.block) {
                        AI.singleBlock = false;
                        AI.useBlocks.push_back(&B);
                    }
                }
            }
        }
    }
    return infos;
}

/// @brief Ensure that a block parameter exists for a promoted variable.
///
/// @details Looks up the parameter slot assigned to @p varId in block @p B and
/// creates one when missing.  Newly created parameters receive a fresh
/// temporary identifier and are registered in the @p blocks table so future
/// lookups are constant time.
///
/// @param B Block receiving the parameter.
/// @param varId Identifier of the promoted variable.
/// @param vars State map for variables.
/// @param blocks Per-block state including parameter indices.
/// @param nextId Counter used to generate unique temp ids.
/// @return Index of the block parameter.
/// @sideeffect May append to @p B->params and update @p blocks.
static unsigned ensureParam(
    BasicBlock *B, unsigned varId, VarMap &vars, BlockMap &blocks, unsigned &nextId) {
    BlockState &BS = blocks[B];
    auto it = BS.params.find(varId);
    if (it != BS.params.end())
        return it->second;
    Param p;
    p.id = nextId++;
    p.type = vars[varId].type;
    p.name = "t" + std::to_string(p.id);
    unsigned idx = B->params.size();
    B->params.push_back(p);
    BS.params[varId] = idx;
    return idx;
}

/// @brief Add an incoming value for a block parameter from a predecessor edge.
///
/// @details Extends the predecessor terminator's branch arguments so that the
/// edge targeting @p B forwards @p val in the slot associated with @p varId.
/// If the parameter does not yet exist the helper creates it via
/// @ref ensureParam.
///
/// @param B Destination block that owns the parameter.
/// @param varId Variable identifier for the promoted alloca.
/// @param Pred Predecessor block supplying the value.
/// @param val Value to pass along the edge.
/// @param vars Variable state table.
/// @param blocks Block state table used to lookup parameter indices.
/// @param nextId Counter used when new parameters must be created.
/// @param edgeIndex Concrete successor slot in @p Pred that targets @p B.
/// @return `true` after populating the edge, or `false` if the indexed edge is stale.
/// @sideeffect Mutates branch arguments in @p Pred and may add block params.
static bool addIncoming(BasicBlock *B,
                        unsigned varId,
                        BasicBlock *Pred,
                        const Value &val,
                        VarMap &vars,
                        BlockMap &blocks,
                        unsigned &nextId,
                        std::size_t edgeIndex) {
    unsigned pIdx = ensureParam(B, varId, vars, blocks, nextId);
    Instr &term = Pred->instructions.back();
    if (edgeIndex >= term.labels.size() || term.labels[edgeIndex] != B->label)
        return false;

    if (term.brArgs.size() < term.labels.size())
        term.brArgs.resize(term.labels.size());
    auto &args = term.brArgs[edgeIndex];
    if (args.size() <= pIdx)
        args.resize(pIdx + 1);
    args[pIdx] = val;
    return true;
}

/// Forward declaration for recursive SSA renaming.
static Value renameUses(Function &F,
                        BasicBlock *B,
                        unsigned varId,
                        VarMap &vars,
                        BlockMap &blocks,
                        unsigned &nextId,
                        const IncomingEdgeMap &incoming,
                        const analysis::CFGContext &ctx,
                        bool &ok);

/// @brief Resolve a promoted variable's value at the start of a block.
///
/// @details When @p B has predecessors, the helper ensures a block parameter is
/// present and recursively renames the variable along each incoming edge,
/// wiring the results into the terminator arguments.  For entry blocks with no
/// predecessors, a zero constant of the variable's type is synthesised.
///
/// @param F Function containing the CFG.
/// @param B Block whose incoming value is requested.
/// @param varId Variable identifier.
/// @param vars Variable state table.
/// @param blocks Block state table.
/// @param nextId Counter for generating temp ids.
/// @param incoming Edge-specific predecessor index for @p F.
/// @param ctx CFG context retained by recursive rename operations.
/// @param ok Shared success flag cleared when an incoming edge cannot be repaired.
/// @return SSA value representing the variable at block entry.
/// @sideeffect May mutate the CFG by adding parameters and arguments.
static Value readFromPreds(Function &F,
                           BasicBlock *B,
                           unsigned varId,
                           VarMap &vars,
                           BlockMap &blocks,
                           unsigned &nextId,
                           const IncomingEdgeMap &incoming,
                           const analysis::CFGContext &ctx,
                           bool &ok) {
    const auto &preds = incomingEdges(incoming, B);
    if (preds.empty()) {
        return vars[varId].initialValue;
    }
    unsigned pIdx = ensureParam(B, varId, vars, blocks, nextId);
    Value paramVal = Value::temp(B->params[pIdx].id);
    for (const auto &edge : preds) {
        Value arg = renameUses(F, edge.pred, varId, vars, blocks, nextId, incoming, ctx, ok);
        if (!ok || !addIncoming(B, varId, edge.pred, arg, vars, blocks, nextId, edge.edgeIndex)) {
            ok = false;
            return paramVal;
        }
    }
    return paramVal;
}

/// @brief Determine the SSA value of a promoted variable within a block.
///
/// @details Consults existing definitions recorded in @p vars.  If the block is
/// not yet sealed, the helper creates a placeholder parameter and records the
/// variable as incomplete so it can be finalised once all predecessors are
/// known.  Otherwise, it merges incoming values via @ref readFromPreds.
///
/// @param F Function being rewritten.
/// @param B Current block.
/// @param varId Variable identifier.
/// @param vars Variable state table.
/// @param blocks Block state table indicating seal status.
/// @param nextId Counter for generating temp ids.
/// @param incoming Edge-specific predecessor index for @p F.
/// @param ctx CFG context used to traverse successor and predecessor relationships.
/// @param ok Shared success flag; false stops recursive processing and triggers rollback.
/// @return SSA value for the variable within @p B.
/// @sideeffect May add block parameters and update definition maps.
static Value renameUses(Function &F,
                        BasicBlock *B,
                        unsigned varId,
                        VarMap &vars,
                        BlockMap &blocks,
                        unsigned &nextId,
                        const IncomingEdgeMap &incoming,
                        const analysis::CFGContext &ctx,
                        bool &ok) {
    if (!ok)
        return vars[varId].initialValue;
    VarState &VS = vars[varId];
    if (auto it = VS.defs.find(B); it != VS.defs.end())
        return it->second;
    BlockState &BS = blocks[B];
    if (!BS.sealed) {
        unsigned pIdx = ensureParam(B, varId, vars, blocks, nextId);
        Value v = Value::temp(B->params[pIdx].id);
        VS.defs[B] = v;
        addIncomplete(BS, varId);
        return v;
    }
    const auto &preds = incomingEdges(incoming, B);
    if (preds.empty()) {
        Value v = VS.initialValue;
        VS.defs[B] = v;
        return v;
    }
    if (preds.size() == 1) {
        Value v = renameUses(F, preds.front().pred, varId, vars, blocks, nextId, incoming, ctx, ok);
        VS.defs[B] = v;
        return v;
    }

    // Create a placeholder param BEFORE recursing to break cycles in true
    // join blocks. Entry blocks and single-predecessor blocks are handled
    // directly above so we do not invent synthetic block params that cannot
    // be bound by incoming edges.
    unsigned pIdx = ensureParam(B, varId, vars, blocks, nextId);
    Value placeholder = Value::temp(B->params[pIdx].id);
    VS.defs[B] = placeholder;
    Value v = readFromPreds(F, B, varId, vars, blocks, nextId, incoming, ctx, ok);
    if (!valueEquals(v, placeholder))
        VS.defs[B] = v;
    return VS.defs[B];
}

/// @brief Finalise a block once all of its predecessors are known.
///
/// @details Completes the SSA value for every variable recorded in the
/// block's @c incomplete set by merging incoming values via @ref readFromPreds
/// and marking the block as sealed.  Subsequent queries can therefore rely on
/// the existing definitions without creating new placeholders.
///
/// @param F Function containing the block.
/// @param B Block to seal.
/// @param vars Variable state table.
/// @param blocks Block state table.
/// @param nextId Counter for generating temp ids.
/// @param incoming Edge-specific predecessor index for @p F.
/// @param ctx CFG context forwarded to recursive rename queries.
/// @param ok Shared success flag cleared if edge wiring fails.
/// @sideeffect May mutate the CFG with additional parameters and arguments.
static void sealBlocks(Function &F,
                       BasicBlock *B,
                       VarMap &vars,
                       BlockMap &blocks,
                       unsigned &nextId,
                       const IncomingEdgeMap &incoming,
                       const analysis::CFGContext &ctx,
                       bool &ok) {
    BlockState &BS = blocks[B];
    if (BS.sealed)
        return;
    std::sort(BS.incomplete.begin(), BS.incomplete.end());
    for (unsigned varId : BS.incomplete) {
        Value v = readFromPreds(F, B, varId, vars, blocks, nextId, incoming, ctx, ok);
        if (!ok)
            break;
        if (!vars[varId].defs.contains(B))
            vars[varId].defs[B] = v;
    }
    BS.incomplete.clear();
    BS.sealed = true;
}

/// @brief Decide whether a branch-argument slot still needs a promoted value.
/// @details Resizing a branch-argument vector default-constructs missing slots
///          as null pointer values.  For non-pointer block parameters that null
///          is never a valid argument, so repair must overwrite it instead of
///          treating vector size alone as proof that the slot is populated.
/// @param args Existing argument bundle for one CFG edge.
/// @param paramIdx Destination block parameter index.
/// @param paramType Type of the destination block parameter.
/// @return True when the promoted value should be written into the slot.
static bool needsPromotedBranchArgRepair(const std::vector<Value> &args,
                                         unsigned paramIdx,
                                         Type paramType) {
    if (args.size() <= paramIdx)
        return true;
    return args[paramIdx].kind == Value::Kind::NullPtr && paramType.kind != Type::Kind::Ptr;
}

/// @brief Fill branch arguments for parameters introduced by mem2reg.
/// @details The rename algorithm wires most edges while reading predecessor
///          values.  A final deterministic repair pass closes any remaining
///          gaps for promoted variables, which prevents partially populated
///          branch-argument vectors when a block parameter is created before all
///          incoming edges have been visited.
/// @param F Function whose terminator argument bundles are repaired.
/// @param vars Promoted-variable state used to recover predecessor values.
/// @param blocks Block parameter assignments and sealing state.
/// @param nextId Counter used if repair discovers a missing parameter.
/// @param incoming Stable edge index for the pre-rewrite CFG.
/// @param ctx CFG context used by recursive value lookup.
/// @param ok Shared success flag cleared on an unrecoverable edge.
static void repairPromotedBranchArgs(Function &F,
                                     VarMap &vars,
                                     BlockMap &blocks,
                                     unsigned &nextId,
                                     const IncomingEdgeMap &incoming,
                                     const analysis::CFGContext &ctx,
                                     bool &ok) {
    std::unordered_map<std::string, BasicBlock *> labels;
    labels.reserve(F.blocks.size());
    for (auto &B : F.blocks)
        labels.emplace(B.label, &B);

    for (auto &Pred : F.blocks) {
        if (Pred.instructions.empty())
            continue;
        auto predStateIt = blocks.find(&Pred);
        if (predStateIt == blocks.end() || !predStateIt->second.sealed)
            continue;

        Instr &term = Pred.instructions.back();
        if (term.labels.empty())
            continue;
        if (term.brArgs.size() < term.labels.size())
            term.brArgs.resize(term.labels.size());

        for (std::size_t targetIndex = 0; targetIndex < term.labels.size(); ++targetIndex) {
            auto labelIt = labels.find(term.labels[targetIndex]);
            if (labelIt == labels.end())
                continue;

            BasicBlock *target = labelIt->second;
            auto targetStateIt = blocks.find(target);
            if (targetStateIt == blocks.end() || targetStateIt->second.params.empty())
                continue;

            std::vector<std::pair<unsigned, unsigned>> slots;
            slots.reserve(targetStateIt->second.params.size());
            for (const auto &[varId, paramIdx] : targetStateIt->second.params)
                slots.emplace_back(paramIdx, varId);
            std::sort(slots.begin(), slots.end());

            auto &args = term.brArgs[targetIndex];
            for (const auto &[paramIdx, varId] : slots) {
                if (!needsPromotedBranchArgRepair(args, paramIdx, target->params[paramIdx].type))
                    continue;
                Value arg = renameUses(F, &Pred, varId, vars, blocks, nextId, incoming, ctx, ok);
                if (!ok)
                    return;
                if (args.size() <= paramIdx)
                    args.resize(paramIdx + 1);
                args[paramIdx] = arg;
            }
        }
    }
}

/// @brief Promote eligible allocas within a function to SSA registers.
///
/// @details Executes the seal-and-rename algorithm, deleting loads, stores, and
/// the allocas themselves.  Optional statistics are incremented to record how
/// many variables were promoted and how many memory operations were removed.
///
/// @param F Function to optimize.
/// @param infos Metadata about allocas gathered by @ref collectAllocas.
/// @param stats Optional statistics accumulator.
/// @param ctx CFG context used for edge traversal during sealed-SSA construction.
/// @return True if promotion completed and all branch arguments were repaired;
///         false if the caller should discard the partially rewritten function.
/// @sideeffect Mutates blocks and instructions in @p F and updates @p stats only
///             for a completed promotion.
static bool promoteVariables(Function &F,
                             const AllocaMap &infos,
                             Mem2RegStats *stats,
                             const analysis::CFGContext &ctx) {
    VarMap vars;
    vars.reserve(infos.size());
    std::vector<unsigned> orderedAllocaIds;
    orderedAllocaIds.reserve(infos.size());
    for (const auto &entry : infos)
        orderedAllocaIds.push_back(entry.first);
    std::sort(orderedAllocaIds.begin(), orderedAllocaIds.end());

    for (unsigned id : orderedAllocaIds) {
        const AllocaInfo &AI = infos.at(id);
        if (AI.addressTaken || !AI.hasStore || !AI.typeConsistent)
            continue;
        if (!isPromotableScalarType(AI.type))
            continue;
        VarState state;
        state.type = AI.type;
        state.allocaBlock = AI.block;
        state.initialValue = zeroValueForType(AI.type);
        state.defs[AI.block] = state.initialValue;
        vars[id] = std::move(state);
    }

    if (stats)
        stats->promotedVars += vars.size();

    if (vars.empty())
        return true;

    unsigned nextId = zanna::il::nextTempId(F);
    const IncomingEdgeMap incoming = buildIncomingEdgeMap(F);

    if (std::getenv("ZANNA_MEM2REG_TRACE")) {
        std::cerr << "[mem2reg] " << F.name << ": promoting " << vars.size()
                  << " vars, nextId=" << nextId << "\n";
        std::vector<unsigned> traceIds;
        traceIds.reserve(vars.size());
        for (const auto &entry : vars)
            traceIds.push_back(entry.first);
        std::sort(traceIds.begin(), traceIds.end());
        for (unsigned id : traceIds)
            std::cerr << "[mem2reg]   var %" << id << " type=" << vars[id].type.toString() << "\n";
    }

    BlockMap blocks;
    blocks.reserve(F.blocks.size());
    for (auto &B : F.blocks) {
        BlockState bs;
        bs.totalPreds = incomingEdges(incoming, &B).size();
        bs.sealed = bs.totalPreds == 0;
        blocks[&B] = bs;
    }

    std::queue<BasicBlock *> work;
    std::unordered_set<BasicBlock *> queued;
    queued.reserve(F.blocks.size());
    if (!F.blocks.empty()) {
        work.push(&F.blocks.front());
        queued.insert(&F.blocks.front());
    }

    ReplacementMap replacements;
    bool ok = true;

    while (!work.empty()) {
        if (!ok)
            return false;
        BasicBlock *B = work.front();
        work.pop();

        std::vector<Instr> rewritten;
        rewritten.reserve(B->instructions.size());
        for (auto &I : B->instructions) {
            rewriteInstructionUses(I, replacements);
            if (I.op == Opcode::Alloca && I.result && vars.contains(*I.result)) {
                continue;
            }
            if (I.op == Opcode::Load && I.operands.size() &&
                I.operands[0].kind == Value::Kind::Temp && vars.contains(I.operands[0].id)) {
                unsigned varId = I.operands[0].id;
                Value v = renameUses(F, B, varId, vars, blocks, nextId, incoming, ctx, ok);
                if (!ok)
                    return false;
                if (I.result)
                    replacements[*I.result] = resolveReplacementValue(replacements, v);
                if (stats)
                    stats->removedLoads++;
                continue;
            }
            if (I.op == Opcode::Store && I.operands.size() > 1 &&
                I.operands[0].kind == Value::Kind::Temp && vars.contains(I.operands[0].id)) {
                unsigned varId = I.operands[0].id;
                vars[varId].defs[B] = resolveReplacementValue(replacements, I.operands[1]);
                if (stats)
                    stats->removedStores++;
                continue;
            }
            rewritten.push_back(std::move(I));
        }
        B->instructions = std::move(rewritten);

        const auto &succs = analysis::successors(ctx, *B);
        for (auto *S : succs) {
            BlockState &SS = blocks[S];
            SS.seenPreds++;
            if (!queued.contains(S)) {
                work.push(S);
                queued.insert(S);
            }
            if (SS.seenPreds == SS.totalPreds)
                sealBlocks(F, S, vars, blocks, nextId, incoming, ctx, ok);
            if (!ok)
                return false;
        }
    }

    repairPromotedBranchArgs(F, vars, blocks, nextId, incoming, ctx, ok);
    if (!ok)
        return false;
    applyReplacements(F, replacements);
    return true;
}

/// @brief Return true when @p block can be reached from a predecessor it dominates.
/// @details For a non-entry alloca with cross-block uses, such an edge means the
///          allocation site is re-executed by loop control flow. Promoting that
///          alloca as one long-lived SSA variable would incorrectly carry stores
///          from a previous dynamic allocation into the next execution.
/// @param domTree Dominator tree for the containing function.
/// @param ctx CFG context providing the allocation block's predecessors.
/// @param block Candidate allocation block.
/// @return `true` when a backedge-like predecessor is dominated by @p block.
static bool hasDominatedPredecessor(const zanna::analysis::DomTree &domTree,
                                    const analysis::CFGContext &ctx,
                                    BasicBlock *block) {
    if (!block)
        return false;
    for (BasicBlock *pred : analysis::predecessors(ctx, *block))
        if (pred && domTree.dominates(block, pred))
            return true;
    return false;
}

/// @brief Detect instructions whose exceptional control flow mem2reg does not model.
/// @param F Function to scan.
/// @return `true` when any exception-handler stack or resume opcode is present.
static bool hasExceptionHandling(const Function &F) {
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

} // namespace

/// @brief Split small aggregate allocas into independent scalar allocations.
/// @param F Function whose constant-offset aggregate accesses are rewritten.
/// @return `true` when at least one aggregate allocation is replaced.
/// @details Candidates are limited by size and field-count budgets. Every
///          derived pointer must use a constant in-bounds GEP, and every access
///          at a given offset must agree on a promotable scalar type. Commit
///          rewrites loads/stores to fresh field allocas and removes the
///          original allocation and its derived GEP instructions.
static bool runSROA(Function &F) {
    std::unordered_map<unsigned, SROACandidate> candidates;
    // Map temp id directly to candidate pointer to avoid two-step lookup.
    // Previously: owner[temp] -> baseId, then candidates[baseId] -> candidate
    // Now: owner[temp] -> candidate pointer (single lookup)
    std::unordered_map<unsigned, SROACandidate *> owner;

    std::size_t allocaCount = 0;
    for (const auto &B : F.blocks)
        for (const auto &I : B.instructions)
            if (I.op == Opcode::Alloca && I.result)
                ++allocaCount;
    candidates.reserve(allocaCount);
    owner.reserve(allocaCount);

    for (auto &B : F.blocks) {
        for (std::size_t idx = 0; idx < B.instructions.size(); ++idx) {
            Instr &I = B.instructions[idx];
            if (I.op != Opcode::Alloca || !I.result || I.operands.empty())
                continue;

            auto sizeOpt = constOffset(I.operands[0]);
            if (!sizeOpt || *sizeOpt == 0 || *sizeOpt > kMaxSROAAllocaSize)
                continue;

            SROACandidate cand;
            cand.block = &B;
            cand.allocaIndex = idx;
            cand.baseId = *I.result;
            cand.allocSize = *sizeOpt;
            cand.ok = true;
            cand.offsets.emplace(*I.result, 0);

            auto [it, inserted] = candidates.emplace(*I.result, std::move(cand));
            owner.emplace(*I.result, &it->second);
        }
    }

    if (candidates.empty())
        return false;

    for (auto &B : F.blocks) {
        for (auto &I : B.instructions) {
            if (I.op == Opcode::GEP && I.operands.size() >= 2 &&
                I.operands[0].kind == Value::Kind::Temp) {
                auto ownIt = owner.find(I.operands[0].id);
                if (ownIt != owner.end()) {
                    SROACandidate *cand = ownIt->second;
                    if (cand->ok) {
                        auto offOpt = constOffset(I.operands[1]);
                        if (!offOpt || !I.result) {
                            cand->ok = false;
                        } else {
                            // Get the base offset of the source operand to handle chained GEPs
                            // e.g., gep %3, %2, 4 where %2 = gep %1, 8 should have offset 8+4=12
                            auto baseOffIt = cand->offsets.find(I.operands[0].id);
                            unsigned baseOffset =
                                (baseOffIt != cand->offsets.end()) ? baseOffIt->second : 0;
                            if (baseOffset >= cand->allocSize ||
                                *offOpt >= cand->allocSize - baseOffset) {
                                cand->ok = false;
                            } else {
                                unsigned totalOffset = baseOffset + *offOpt;
                                owner[*I.result] = cand;
                                cand->offsets[*I.result] = totalOffset;
                            }
                        }
                    }
                }
            }

            /// @brief Classify one use and invalidate its SROA owner on escape or mismatch.
            /// @param v Operand or branch argument being classified.
            /// @param Inst Instruction containing the use.
            /// @param operandIdx Operand position within @p Inst.
            auto classifyUse = [&](const Value &v, Instr &Inst, std::size_t operandIdx) {
                if (v.kind != Value::Kind::Temp)
                    return;
                auto ownIt = owner.find(v.id);
                if (ownIt == owner.end())
                    return;
                SROACandidate &cand = *ownIt->second;
                if (!cand.ok)
                    return;

                if (Inst.op == Opcode::Load || Inst.op == Opcode::Store) {
                    if (Inst.operands.empty() || operandIdx != 0) {
                        cand.ok = false;
                        return;
                    }
                    auto offIt = cand.offsets.find(v.id);
                    if (offIt == cand.offsets.end()) {
                        cand.ok = false;
                        return;
                    }
                    Type accessType = Inst.type;
                    if (!isPromotableScalarType(accessType)) {
                        cand.ok = false;
                        return;
                    }
                    auto szOpt = scalarSize(accessType);
                    if (!szOpt || offIt->second + *szOpt > cand.allocSize) {
                        cand.ok = false;
                        return;
                    }

                    SROAField &field = cand.fields[offIt->second];
                    if (field.size == 0) {
                        field.type = accessType;
                        field.size = *szOpt;
                    } else if (field.type.kind != accessType.kind) {
                        cand.ok = false;
                    }
                    return;
                }

                if (Inst.op == Opcode::GEP && operandIdx == 0)
                    return;

                cand.ok = false;
            };

            for (std::size_t oi = 0; oi < I.operands.size(); ++oi)
                classifyUse(I.operands[oi], I, oi);

            for (auto &argList : I.brArgs)
                for (const auto &arg : argList)
                    classifyUse(arg, I, 0);
        }
    }

    for (auto &[id, cand] : candidates) {
        if (!cand.ok || cand.fields.empty() || cand.fields.size() > kMaxSROAFields) {
            cand.ok = false;
            continue;
        }

        std::vector<std::pair<unsigned, SROAField *>> ordered;
        ordered.reserve(cand.fields.size());
        for (auto &[off, field] : cand.fields)
            ordered.emplace_back(off, &field);
        /// @brief Compare field records by ascending byte offset.
        /// @param a First offset/field pair.
        /// @param b Second offset/field pair.
        /// @return True when @p a precedes @p b.
        std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        unsigned end = 0;
        for (const auto &[off, field] : ordered) {
            if (field->size == 0 || off < end || off + field->size > cand.allocSize) {
                cand.ok = false;
                break;
            }
            end = off + field->size;
        }
    }

    bool changed = false;
    unsigned nextId = zanna::il::nextTempId(F);

    for (auto &[id, cand] : candidates) {
        if (!cand.ok)
            continue;

        BasicBlock &B = *cand.block;
        /// @brief Relocate the aggregate alloca after earlier rewrites shifted indices.
        /// @return Current instruction index, or the block size when absent.
        auto findAllocaIndex = [&]() -> std::size_t {
            for (std::size_t i = 0; i < B.instructions.size(); ++i) {
                Instr &I = B.instructions[i];
                if (I.op == Opcode::Alloca && I.result && *I.result == cand.baseId)
                    return i;
            }
            return B.instructions.size();
        };

        std::size_t insertPos = findAllocaIndex();
        if (insertPos == B.instructions.size())
            continue;

        std::unordered_map<unsigned, unsigned> offsetToAlloca;
        offsetToAlloca.reserve(cand.fields.size());

        std::vector<std::pair<unsigned, SROAField *>> ordered;
        ordered.reserve(cand.fields.size());
        for (auto &[off, field] : cand.fields)
            ordered.emplace_back(off, &field);
        /// @brief Compare replacement fields by ascending byte offset.
        /// @param a First offset/field pair.
        /// @param b Second offset/field pair.
        /// @return True when @p a precedes @p b.
        std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        std::size_t fieldIdx = 0;
        for (auto &[offset, field] : ordered) {
            Instr alloc;
            alloc.op = Opcode::Alloca;
            alloc.type = Type(Type::Kind::Ptr);
            alloc.result = nextId;
            alloc.operands.push_back(Value::constInt(field->size));
            offsetToAlloca[offset] = nextId;
            field->allocaId = nextId;

            std::string baseName;
            if (cand.baseId < F.valueNames.size())
                baseName = F.valueNames[cand.baseId];
            if (baseName.empty())
                baseName = "sroa." + std::to_string(cand.baseId);
            ensureValueName(F, nextId, baseName + ".f" + std::to_string(fieldIdx++));

            B.instructions.insert(B.instructions.begin() + static_cast<long>(insertPos),
                                  std::move(alloc));
            ++insertPos;
            ++nextId;
        }

        for (auto &Blk : F.blocks) {
            for (auto &I : Blk.instructions) {
                if ((I.op != Opcode::Load && I.op != Opcode::Store) || I.operands.empty())
                    continue;

                if (I.operands[0].kind != Value::Kind::Temp)
                    continue;

                auto offIt = cand.offsets.find(I.operands[0].id);
                if (offIt == cand.offsets.end())
                    continue;

                auto fieldIt = offsetToAlloca.find(offIt->second);
                if (fieldIt == offsetToAlloca.end())
                    continue;

                I.operands[0] = Value::temp(fieldIt->second);
            }
        }

        for (auto &Blk : F.blocks) {
            for (std::size_t i = 0; i < Blk.instructions.size();) {
                Instr &I = Blk.instructions[i];
                bool erase = false;

                if (I.op == Opcode::GEP && I.result && cand.offsets.contains(*I.result) &&
                    *I.result != cand.baseId) {
                    erase = true;
                } else if (I.op == Opcode::Alloca && I.result && *I.result == cand.baseId) {
                    erase = true;
                }

                if (erase) {
                    Blk.instructions.erase(Blk.instructions.begin() + static_cast<long>(i));
                    changed = true;
                    continue;
                }
                ++i;
            }
        }
    }

    return changed;
}

/// @brief Run memory-to-register promotion across all functions in a module.
///
/// @details Scans each function for promotable allocas, filters out variables
/// whose addresses escape or whose element types are unsupported, and then
/// invokes @ref promoteVariables to perform the transformation.  When provided,
/// @p stats accumulates totals for promoted variables and removed memory
/// operations.
///
/// @param M Module to transform.
/// @param stats Optional statistics collector receiving totals for promoted
///              variables and removed loads/stores.
/// @param enableParallel Allow per-function workers for callers that explicitly
///                       accept parallel transform semantics.
/// @sideeffect Mutates functions within the module.
void mem2reg(Module &M, Mem2RegStats *stats, bool enableParallel) {
    for (auto &F : M.functions) {
        if (!hasExceptionHandling(F))
            runSROA(F);
    }

    analysis::CFGContext cfg(M);
    /// @brief Promote one EH-free function, rolling back if SSA edge repair fails.
    /// @param F Function to transform in place.
    /// @param localStats Optional per-worker statistics accumulator.
    auto processFunction = [&](Function &F, Mem2RegStats *localStats) {
        if (hasExceptionHandling(F))
            return;

        AllocaMap infos = collectAllocas(F);

        // Lazily compute the dominator tree only when there are non-entry-block
        // allocas with cross-block uses, to avoid the overhead for simple cases.
        BasicBlock *entryBlock = F.blocks.empty() ? nullptr : &F.blocks.front();
        bool needDomTree = false;
        for (const auto &[id, info] : infos) {
            if (!info.singleBlock && info.block != entryBlock) {
                needDomTree = true;
                break;
            }
        }
        std::optional<zanna::analysis::DomTree> domTree;
        if (needDomTree)
            domTree = zanna::analysis::computeDominatorTree(cfg, F);

        AllocaMap promotable;
        for (auto &[id, info] : infos) {
            if (info.addressTaken || !info.hasStore || !info.typeConsistent)
                continue;
            if (!isPromotableScalarType(info.type))
                continue;
            // For non-entry-block allocas with multi-block uses: only promote if
            // the defining block dominates every block that contains a use.
            // Single-block allocas are always safe (no cross-block SSA needed).
            if (!info.singleBlock && info.block != entryBlock && domTree) {
                if (hasDominatedPredecessor(*domTree, cfg, info.block))
                    continue;

                bool dominated = true;
                for (BasicBlock *useBlk : info.useBlocks) {
                    if (!domTree->dominates(info.block, useBlk)) {
                        dominated = false;
                        break;
                    }
                }
                if (!dominated)
                    continue;
            }
            promotable.emplace(id, info);
        }
        Function beforePromotion = F;
        Mem2RegStats promotionStats;
        if (!promoteVariables(F, promotable, &promotionStats, cfg)) {
            F = std::move(beforePromotion);
            return;
        }
        if (localStats) {
            localStats->promotedVars += promotionStats.promotedVars;
            localStats->removedLoads += promotionStats.removedLoads;
            localStats->removedStores += promotionStats.removedStores;
        }
    };

    const std::size_t functionCount = M.functions.size();
    const std::size_t hardwareThreads =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    const std::size_t workerCount = enableParallel ? std::min(functionCount, hardwareThreads) : 1;

    if (workerCount <= 1) {
        for (auto &F : M.functions)
            processFunction(F, stats);
        M.internOwnedIdentifiers();
        return;
    }

    std::vector<Mem2RegStats> localStats(workerCount);
    std::atomic_size_t nextIndex{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        /// @brief Claim function indices atomically and accumulate per-worker statistics.
        workers.emplace_back([&, workerIndex]() {
            for (;;) {
                const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= functionCount)
                    break;
                processFunction(M.functions[index], stats ? &localStats[workerIndex] : nullptr);
            }
        });
    }
    for (auto &worker : workers)
        worker.join();

    if (stats) {
        for (const auto &local : localStats) {
            stats->promotedVars += local.promotedVars;
            stats->removedLoads += local.removedLoads;
            stats->removedStores += local.removedStores;
        }
    }
    M.internOwnedIdentifiers();
}

} // namespace zanna::passes
