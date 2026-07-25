//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/NativeEHLowering.cpp
// Purpose: Rewrite structured EH into setjmp-backed IL that native backends can
// lower like ordinary control flow.
//
//===----------------------------------------------------------------------===//

#include "codegen/common/NativeEHLowering.hpp"

#include "codegen/common/ICE.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Param.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/verify/ControlFlowChecker.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/// @file
/// @brief Implements setjmp-backed lowering of structured IL exception handling.

namespace zanna::codegen::common {
namespace {

using il::core::BasicBlock;
using il::core::Extern;
using il::core::Function;
using il::core::Instr;
using il::core::Module;
using il::core::Opcode;
using il::core::Param;
using il::core::Type;
using il::core::Value;

constexpr const char *kFrameAlloc = "rt_native_eh_frame_alloc";
constexpr const char *kFrameFree = "rt_native_eh_frame_free";
constexpr const char *kFramePush = "rt_native_eh_push";
constexpr const char *kFramePop = "rt_native_eh_pop";
constexpr const char *kFrameSetSite = "rt_native_eh_set_site";
constexpr const char *kFrameGetSite = "rt_native_eh_get_site";
constexpr const char *kSetjmpSymbol = "setjmp";
constexpr int32_t kErrInvalidOperation = 8;

/// @brief Identifies one `eh.push` by its original function position.
struct PushKey {
    std::size_t blockIndex = 0;
    std::size_t instrIndex = 0;

    /// @brief Compare original block/instruction coordinates.
    /// @param other Key to compare.
    /// @return `true` when both indices match.
    bool operator==(const PushKey &other) const noexcept {
        return blockIndex == other.blockIndex && instrIndex == other.instrIndex;
    }
};

/// @brief Hashes an original `eh.push` coordinate for unordered maps.
struct PushKeyHash {
    /// @param key Push coordinate to hash.
    /// @return Deterministic mixed block/instruction hash.
    std::size_t operator()(const PushKey &key) const noexcept {
        return (key.blockIndex * 1315423911u) ^ key.instrIndex;
    }
};

/// @brief Synthetic resume metadata for one potentially trapping instruction.
struct SiteInfo {
    int64_t siteId = 0;
    std::string sameLabel;
    std::string nextLabel;
};

/// @brief Lowering state for one dynamic native EH frame scope.
struct ScopeInfo {
    int id = -1;
    std::string handlerLabel;
    unsigned slotTemp = 0;
    std::vector<int> outerStack;
    bool hasOuterStack = false;
    std::vector<SiteInfo> sites;
};

/// @brief Candidate replacement blocks and mutation flag for one function.
struct RewrittenFunction {
    std::vector<BasicBlock> blocks;
    bool changed = false;
};

/// @brief Construct the IL `void` type used by injected calls.
/// @return Fresh primitive type value.
static Type voidTy() {
    return Type(Type::Kind::Void);
}

/// @brief Construct the IL pointer type used for native EH frames.
/// @return Fresh primitive type value.
static Type ptrTy() {
    return Type(Type::Kind::Ptr);
}

/// @brief Construct the IL boolean type used by synthetic comparisons.
/// @return Fresh primitive type value.
static Type i1Ty() {
    return Type(Type::Kind::I1);
}

/// @brief Construct the IL 32-bit integer type used by error traps.
/// @return Fresh primitive type value.
static Type i32Ty() {
    return Type(Type::Kind::I32);
}

/// @brief Construct the IL 64-bit integer type used by setjmp/site tokens.
/// @return Fresh primitive type value.
static Type i64Ty() {
    return Type(Type::Kind::I64);
}

/// @brief Compare two IL primitive types for exact native-EH ABI equality.
/// @details `Type` intentionally has a tiny value representation, so native EH
///          signature checks only need to compare the primitive kind. Keeping
///          this in one helper makes the extern validation path explicit.
/// @param lhs First type.
/// @param rhs Second type.
/// @return `true` when primitive kinds match.
static bool sameType(const Type &lhs, const Type &rhs) {
    return lhs.kind == rhs.kind;
}

/// @brief Validate that an existing extern declaration matches an expected signature.
/// @details Native EH lowering injects calls to setjmp/runtime helpers. Reusing
///          a same-named extern with a different return type or parameter list
///          would produce invalid call sites, so this helper performs an exact
///          arity and primitive-type comparison before `ensureExtern` accepts
///          an existing declaration.
/// @param ext Existing module declaration.
/// @param retType Expected return type.
/// @param params Expected parameter types in ABI order.
/// @return `true` for exact primitive return/parameter shape equality.
static bool externSignatureMatches(const Extern &ext,
                                   const Type &retType,
                                   const std::vector<Type> &params) {
    if (!sameType(ext.retType, retType) || ext.params.size() != params.size())
        return false;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!sameType(ext.params[i], params[i]))
            return false;
    }
    return true;
}

/// @brief Reserve the next function value identifier and assign its debug name.
/// @param[in,out] fn Function value-name table to extend.
/// @param name Synthetic value name.
/// @return Newly reserved temporary identifier.
static unsigned reserveTemp(Function &fn, const std::string &name) {
    const unsigned id = static_cast<unsigned>(fn.valueNames.size());
    fn.valueNames.push_back(name);
    return id;
}

/// @brief Test whether an opcode is a structured EH marker handled here.
/// @param op IL opcode to classify.
/// @return `true` for push/pop/entry and resume marker families.
static bool isEhOpcode(Opcode op) {
    switch (op) {
        case Opcode::EhPush:
        case Opcode::EhPop:
        case Opcode::EhEntry:
        case Opcode::ResumeSame:
        case Opcode::ResumeNext:
        case Opcode::ResumeLabel:
            return true;
        default:
            return false;
    }
}

/// @brief Test whether a block begins with structured error/resume parameters.
/// @param bb Basic block to inspect.
/// @return `true` when the first two parameters are `error` and `resume.token`.
static bool hasStructuredHandlerParams(const BasicBlock &bb) {
    return bb.params.size() >= 2 && bb.params[0].type.kind == Type::Kind::Error &&
           bb.params[1].type.kind == Type::Kind::ResumeTok;
}

/// @brief Test whether an IL opcode requires an active-scope resume site.
/// @param op IL opcode to classify.
/// @return `true` for calls, explicit traps, checked arithmetic/memory, and
///         checked numeric casts modeled by native EH.
static bool mayTrap(Opcode op) {
    switch (op) {
        case Opcode::Call:
        case Opcode::CallIndirect:
        case Opcode::Trap:
        case Opcode::TrapFromErr:
        case Opcode::IdxChk:
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf:
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0:
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk:
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk:
            return true;
        default:
            return false;
    }
}

/// @brief Ensure one exact native helper declaration exists in a module.
/// @param[in,out] module Module extern and identifier tables to inspect/extend.
/// @param name Required symbol name.
/// @param retType Required return type.
/// @param params Required parameter types.
/// @note A conflicting existing declaration is an internal compiler error.
static void ensureExtern(Module &module, std::string name, Type retType, std::vector<Type> params) {
    for (const auto &ext : module.externs) {
        if (ext.name != name)
            continue;
        if (!externSignatureMatches(ext, retType, params)) {
            ZANNA_ICE("native EH runtime extern '" + name +
                      "' already exists with a different "
                      "signature");
        }
        return;
    }
    module.externs.push_back(Extern{std::move(name), retType, std::move(params)});
    module.externs.back().nameSymbol = module.internIdentifier(module.externs.back().name);
}

/// @brief Ensure declarations for all runtime frame helpers and `setjmp`.
/// @param[in,out] module Module extern table to extend.
static void ensureRuntimeExterns(Module &module) {
    ensureExtern(module, kFrameAlloc, ptrTy(), {});
    ensureExtern(module, kFrameFree, voidTy(), {ptrTy()});
    ensureExtern(module, kFramePush, voidTy(), {ptrTy()});
    ensureExtern(module, kFramePop, voidTy(), {ptrTy()});
    ensureExtern(module, kFrameSetSite, voidTy(), {ptrTy(), i64Ty()});
    ensureExtern(module, kFrameGetSite, i64Ty(), {ptrTy()});
    ensureExtern(module, kSetjmpSymbol, i64Ty(), {ptrTy()});
}

/// @brief Build a direct void call to an injected helper.
/// @param callee Static helper symbol.
/// @param operands Call arguments in source order.
/// @return Fully initialized call instruction without a result.
static Instr makeCallVoid(const char *callee, std::vector<Value> operands = {}) {
    Instr instr;
    instr.op = Opcode::Call;
    instr.type = voidTy();
    instr.setDirectCallee(callee);
    instr.operands = std::move(operands);
    return instr;
}

/// @brief Build a direct helper call producing a temporary.
/// @param result Destination temporary identifier.
/// @param type Call result type.
/// @param callee Static helper symbol.
/// @param operands Call arguments in source order.
/// @return Fully initialized result-producing call instruction.
static Instr makeCallResult(unsigned result,
                            Type type,
                            const char *callee,
                            std::vector<Value> operands = {}) {
    Instr instr;
    instr.result = result;
    instr.op = Opcode::Call;
    instr.type = type;
    instr.setDirectCallee(callee);
    instr.operands = std::move(operands);
    return instr;
}

/// @brief Build a pointer-typed load used for an EH frame slot.
/// @param result Destination temporary identifier.
/// @param ptr Address value to load.
/// @return Fully initialized load instruction.
static Instr makeLoad(unsigned result, Value ptr) {
    Instr instr;
    instr.result = result;
    instr.op = Opcode::Load;
    instr.type = ptrTy();
    instr.operands.push_back(std::move(ptr));
    return instr;
}

/// @brief Build a store to a synthetic EH slot.
/// @param ptr Destination address.
/// @param value Value to write.
/// @param storedType IL value type recorded on the store.
/// @return Fully initialized store instruction.
static Instr makeStore(Value ptr, Value value, Type storedType = ptrTy()) {
    Instr instr;
    instr.op = Opcode::Store;
    instr.operands.push_back(std::move(ptr));
    instr.operands.push_back(std::move(value));
    instr.type = storedType;
    return instr;
}

/// @brief Build a fixed-size stack allocation for an EH frame pointer slot.
/// @param result Destination pointer temporary.
/// @param sizeBytes Allocation size encoded as a constant operand.
/// @return Fully initialized `alloca`.
static Instr makeAlloca(unsigned result, int64_t sizeBytes) {
    Instr instr;
    instr.result = result;
    instr.op = Opcode::Alloca;
    instr.type = ptrTy();
    instr.operands.push_back(Value::constInt(sizeBytes));
    return instr;
}

/// @brief Build an unconditional branch with optional block arguments.
/// @param label Destination block label.
/// @param args Values forwarded to destination parameters.
/// @return Fully initialized branch instruction.
static Instr makeBr(const std::string &label, std::vector<Value> args = {}) {
    Instr instr;
    instr.op = Opcode::Br;
    instr.type = voidTy();
    instr.addBranchTarget(label, std::move(args));
    return instr;
}

/// @brief Build a conditional branch without edge arguments.
/// @param cond Boolean condition.
/// @param trueLabel Taken-edge destination.
/// @param falseLabel Not-taken-edge destination.
/// @return Fully initialized conditional branch.
static Instr makeCBr(Value cond, const std::string &trueLabel, const std::string &falseLabel) {
    Instr instr;
    instr.op = Opcode::CBr;
    instr.type = voidTy();
    instr.operands.push_back(std::move(cond));
    instr.addBranchTarget(trueLabel);
    instr.addBranchTarget(falseLabel);
    return instr;
}

/// @brief Build a conditional branch whose true edge forwards block arguments.
/// @details Native EH resume validation may need to guard a `resume.label`
///          target that itself expects block parameters. The false edge always
///          transfers to a synthetic dispatch/failure block without arguments.
/// @param cond Condition value deciding whether validation succeeded.
/// @param trueLabel Destination reached when @p cond is true.
/// @param trueArgs Branch arguments forwarded to @p trueLabel.
/// @param falseLabel Destination reached when validation fails.
/// @return Conditional branch instruction with explicit branch argument bundles.
static Instr makeCBrWithTrueArgs(Value cond,
                                 const std::string &trueLabel,
                                 std::vector<Value> trueArgs,
                                 const std::string &falseLabel) {
    Instr instr;
    instr.op = Opcode::CBr;
    instr.type = voidTy();
    instr.operands.push_back(std::move(cond));
    instr.addBranchTarget(trueLabel, std::move(trueArgs));
    instr.addBranchTarget(falseLabel);
    return instr;
}

/// @brief Build a terminal trap from a runtime error code.
/// @param code Numeric Zanna runtime error code.
/// @return Fully initialized `trap.from.err` instruction.
static Instr makeTrapFromErr(int32_t code) {
    Instr instr;
    instr.op = Opcode::TrapFromErr;
    instr.type = i32Ty();
    instr.operands.push_back(Value::constInt(code));
    return instr;
}

/// @brief Resolve ordinary branch successors of one terminator.
/// @param terminator Final instruction of a block.
/// @param blockIndex Label-to-index map for the original function.
/// @return Known target indices for `br`, `cbr`, or `switch.i32`; empty for
///         other opcodes and unknown labels.
static std::vector<std::size_t> normalSuccessors(
    const Instr &terminator, const std::unordered_map<std::string, std::size_t> &blockIndex) {
    std::vector<std::size_t> successors;
    switch (terminator.op) {
        case Opcode::Br:
            if (!terminator.labels.empty()) {
                auto it = blockIndex.find(terminator.labels[0]);
                if (it != blockIndex.end())
                    successors.push_back(it->second);
            }
            break;
        case Opcode::CBr:
        case Opcode::SwitchI32:
            for (const auto &label : terminator.labels) {
                auto it = blockIndex.find(label);
                if (it != blockIndex.end())
                    successors.push_back(it->second);
            }
            break;
        default:
            break;
    }
    return successors;
}

/// @brief Remove an obsolete error-token operand from handler getter operations.
///
/// After handler error parameters become native pointers, the runtime error
/// getter opcodes use implicit current-error state rather than consuming that
/// parameter explicitly.
///
/// @param handlerErrParam Handler label to converted error-parameter id map.
/// @param block Block containing @p instr.
/// @param[in,out] instr Candidate getter instruction.
/// @return `true` when a matching explicit token operand was removed.
static bool rewriteErrGetterForHandlerToken(
    const std::unordered_map<std::string, unsigned> &handlerErrParam,
    const BasicBlock &block,
    Instr &instr) {
    switch (instr.op) {
        case Opcode::TrapKind:
        case Opcode::ErrGetKind:
        case Opcode::ErrGetCode:
        case Opcode::ErrGetIp:
        case Opcode::ErrGetLine:
        case Opcode::ErrGetMsg:
            break;
        default:
            return false;
    }

    auto it = handlerErrParam.find(block.label);
    if (it == handlerErrParam.end() || instr.operands.empty())
        return false;
    if (instr.operands[0].kind != Value::Kind::Temp || instr.operands[0].id != it->second)
        return false;
    instr.operands.clear();
    return true;
}

/// @brief Find the captured outer EH stack for a handler label.
/// @param scopes Discovered push scopes.
/// @param handlerLabel Handler block label to match.
/// @return First initialized outer stack for that handler, or an empty vector.
static std::vector<int> handlerEntryStackFor(const std::vector<ScopeInfo> &scopes,
                                             const std::string &handlerLabel) {
    for (const auto &scope : scopes) {
        if (scope.handlerLabel == handlerLabel && scope.hasOuterStack)
            return scope.outerStack;
    }
    return {};
}

/// @brief Seed a block with the first EH stack known at entry.
/// @details The native lowering currently represents one EH stack per block.
///          Valid structured IL reaches ordinary joins with the same stack, and
///          handler helper blocks are seeded through the handler that dispatched
///          to them. If an already-seeded block is seen with a different stack,
///          the structured EH graph is malformed and lowering stops immediately.
/// @param entryStacks Per-block entry stack table being populated.
/// @param worklist Blocks whose outgoing edges still need propagation.
/// @param blockIndex Index of the block being seeded.
/// @param stack EH stack at the beginning of the block.
static void seedEntryStack(std::vector<std::optional<std::vector<int>>> &entryStacks,
                           std::deque<std::size_t> &worklist,
                           std::size_t blockIndex,
                           const std::vector<int> &stack) {
    if (entryStacks[blockIndex].has_value()) {
        if (*entryStacks[blockIndex] != stack) {
            ZANNA_ICE("native EH stack mismatch at CFG join for block index " +
                      std::to_string(blockIndex));
        }
        return;
    }
    entryStacks[blockIndex] = stack;
    worklist.push_back(blockIndex);
}

/// @brief Propagate native EH stack state across ordinary CFG edges.
/// @details The first pass starts at function entry and records each `eh.push`
///          scope's outer stack. A second pass seeds pushed handlers with that
///          post-dispatch stack, then carries it through typed-catch, finally,
///          and rethrow helper blocks. This gives trapping instructions inside
///          handler helper blocks a concrete outer native frame site.
/// @param fn Function whose blocks are being analysed.
/// @param blockIndex Label-to-index map for successor resolution.
/// @param pushToScope Map from concrete `eh.push` instructions to scope ids.
/// @param scopes Mutable native EH scopes; outer stacks are filled in place.
/// @param entryStacks Per-block entry stacks populated by the traversal.
/// @param worklist Initial blocks to process, consumed by the traversal.
static void propagateEntryStacks(const Function &fn,
                                 const std::unordered_map<std::string, std::size_t> &blockIndex,
                                 const std::unordered_map<PushKey, int, PushKeyHash> &pushToScope,
                                 std::vector<ScopeInfo> &scopes,
                                 std::vector<std::optional<std::vector<int>>> &entryStacks,
                                 std::deque<std::size_t> &worklist) {
    while (!worklist.empty()) {
        const std::size_t bi = worklist.front();
        worklist.pop_front();
        if (!entryStacks[bi].has_value())
            continue;

        auto state = *entryStacks[bi];
        const auto &bb = fn.blocks[bi];
        for (std::size_t ii = 0; ii < bb.instructions.size(); ++ii) {
            const auto &instr = bb.instructions[ii];
            if (instr.op == Opcode::EhPush) {
                auto it = pushToScope.find(PushKey{bi, ii});
                if (it != pushToScope.end()) {
                    auto &scope = scopes[static_cast<std::size_t>(it->second)];
                    if (!scope.hasOuterStack) {
                        scope.outerStack = state;
                        scope.hasOuterStack = true;
                    }
                    state.push_back(it->second);
                }
            } else if (instr.op == Opcode::EhPop) {
                if (!state.empty())
                    state.pop_back();
            }
        }

        if (bb.instructions.empty())
            continue;
        const auto &term = bb.instructions.back();
        for (const std::size_t succ : normalSuccessors(term, blockIndex))
            seedEntryStack(entryStacks, worklist, succ, state);
    }
}

/// @brief Lower structured EH in one function into runtime calls and ordinary CFG.
///
/// The analysis discovers push scopes, propagates active-scope stacks, assigns
/// resume sites to trapping operations, converts handler parameter types, then
/// rebuilds blocks with setjmp dispatch, frame cleanup, and validated resume
/// tables. The original block vector is replaced only when EH was present.
///
/// @param[in,out] module Owning module whose runtime externs may be inserted.
/// @param[in,out] fn Function whose value names, parameters, and blocks may change.
/// @return Replacement block snapshot and whether a rewrite occurred.
static RewrittenFunction rewriteFunction(Module &module, Function &fn) {
    RewrittenFunction rewritten{};

    std::unordered_map<std::string, std::size_t> blockIndex;
    for (std::size_t i = 0; i < fn.blocks.size(); ++i)
        blockIndex.emplace(fn.blocks[i].label, i);

    std::vector<ScopeInfo> scopes;
    std::unordered_map<PushKey, int, PushKeyHash> pushToScope;
    std::unordered_map<std::string, std::vector<int>> handlerScopes;
    std::unordered_map<PushKey, SiteInfo, PushKeyHash> siteForInstr;
    std::unordered_map<std::string, std::vector<SiteInfo>> handlerSites;
    std::vector<SiteInfo> allSites;

    bool hasEh = false;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (std::size_t ii = 0; ii < bb.instructions.size(); ++ii) {
            const auto &instr = bb.instructions[ii];
            if (!isEhOpcode(instr.op))
                continue;
            hasEh = true;
            if (instr.op == Opcode::EhPush && instr.labels.empty()) {
                ZANNA_ICE(
                    "native EH lowering encountered eh.push without a handler label in block '" +
                    bb.label + "'");
            }
            if (instr.op == Opcode::EhPush) {
                ScopeInfo scope;
                scope.id = static_cast<int>(scopes.size());
                scope.handlerLabel = instr.labels[0];
                scope.slotTemp = reserveTemp(fn, "__neh.slot." + std::to_string(scope.id));
                pushToScope.emplace(PushKey{bi, ii}, scope.id);
                handlerScopes[scope.handlerLabel].push_back(scope.id);
                scopes.push_back(std::move(scope));
            }
        }
    }
    if (!hasEh)
        return rewritten;

    ensureRuntimeExterns(module);

    std::vector<std::optional<std::vector<int>>> entryStacks(fn.blocks.size());
    std::deque<std::size_t> worklist;
    seedEntryStack(entryStacks, worklist, 0, {});
    propagateEntryStacks(fn, blockIndex, pushToScope, scopes, entryStacks, worklist);

    for (const auto &scope : scopes) {
        if (!scope.hasOuterStack)
            continue;
        auto handlerIt = blockIndex.find(scope.handlerLabel);
        if (handlerIt != blockIndex.end())
            seedEntryStack(entryStacks, worklist, handlerIt->second, scope.outerStack);
    }
    propagateEntryStacks(fn, blockIndex, pushToScope, scopes, entryStacks, worklist);

    const std::string invalidResumeLabel = fn.name + ".__neh.invalid_resume";
    int64_t nextSyntheticSiteId = 1;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        std::vector<int> active;
        if (entryStacks[bi].has_value()) {
            active = *entryStacks[bi];
        } else {
            active = handlerEntryStackFor(scopes, bb.label);
        }

        for (std::size_t ii = 0; ii < bb.instructions.size(); ++ii) {
            const auto &instr = bb.instructions[ii];
            if (instr.op == Opcode::EhPush) {
                const int scopeId = pushToScope.at(PushKey{bi, ii});
                active.push_back(scopeId);
                continue;
            }
            if (instr.op == Opcode::EhPop) {
                if (!active.empty())
                    active.pop_back();
                continue;
            }
            if (!mayTrap(instr.op) || active.empty())
                continue;

            const int scopeId = active.back();
            auto &scope = scopes[static_cast<std::size_t>(scopeId)];
            const int64_t siteId = nextSyntheticSiteId++;
            const bool isTerm = il::verify::isTerminator(instr.op);
            SiteInfo site;
            site.siteId = siteId;
            site.sameLabel = fn.name + ".__neh.site." + std::to_string(siteId);
            site.nextLabel =
                isTerm ? invalidResumeLabel : fn.name + ".__neh.cont." + std::to_string(siteId);
            siteForInstr.emplace(PushKey{bi, ii}, site);
            scope.sites.push_back(site);
            handlerSites[scope.handlerLabel].push_back(site);
            allSites.push_back(site);
        }
    }

    std::unordered_map<std::string, unsigned> handlerErrParam;
    std::unordered_map<std::string, unsigned> handlerSiteParam;
    for (auto &bb : fn.blocks) {
        const bool isPushedHandler = handlerScopes.find(bb.label) != handlerScopes.end();
        if (!isPushedHandler && !hasStructuredHandlerParams(bb))
            continue;
        if (!bb.params.empty()) {
            bb.params[0].type = ptrTy();
            handlerErrParam.emplace(bb.label, bb.params[0].id);
        }
        if (bb.params.size() > 1) {
            bb.params[1].type = i64Ty();
            handlerSiteParam.emplace(bb.label, bb.params[1].id);
        }
    }

    bool needsInvalidResume = false;
    int synthCounter = 0;

    /// Generate a function-scoped unique native-EH block label.
    auto newLabel = [&](const std::string &base) {
        return fn.name + ".__neh." + base + "." + std::to_string(synthCounter++);
    };

    /// Finalize termination metadata and append one rebuilt block.
    auto appendBlock = [&](BasicBlock &&bb) {
        if (!bb.instructions.empty())
            bb.terminated = il::verify::isTerminator(bb.instructions.back().op);
        rewritten.blocks.push_back(std::move(bb));
    };

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &orig = fn.blocks[bi];
        std::vector<int> active;
        if (entryStacks[bi].has_value()) {
            active = *entryStacks[bi];
        } else {
            active = handlerEntryStackFor(scopes, orig.label);
        }

        BasicBlock current;
        current.label = orig.label;
        current.params = orig.params;
        if (bi == 0) {
            for (const auto &scope : scopes)
                current.instructions.push_back(makeAlloca(scope.slotTemp, 8));
        }

        for (std::size_t ii = 0; ii < orig.instructions.size(); ++ii) {
            Instr instr = orig.instructions[ii];
            rewriteErrGetterForHandlerToken(handlerErrParam, orig, instr);

            if (instr.op == Opcode::EhEntry) {
                rewritten.changed = true;
                continue;
            }

            if (instr.op == Opcode::EhPush) {
                rewritten.changed = true;
                const int scopeId = pushToScope.at(PushKey{bi, ii});
                const auto &scope = scopes[static_cast<std::size_t>(scopeId)];

                const unsigned frameTemp =
                    reserveTemp(fn, "__neh.frame.alloc." + std::to_string(scopeId));
                const unsigned setjmpTemp = reserveTemp(fn, "__neh.sj." + std::to_string(scopeId));
                const unsigned caughtTemp =
                    reserveTemp(fn, "__neh.caught." + std::to_string(scopeId));
                const std::string afterLabel = newLabel("after_push");
                const std::string handlerEntryLabel = newLabel("handler_entry");

                current.instructions.push_back(makeCallResult(frameTemp, ptrTy(), kFrameAlloc, {}));
                current.instructions.push_back(
                    makeStore(Value::temp(scope.slotTemp), Value::temp(frameTemp)));
                current.instructions.push_back(makeCallVoid(kFramePush, {Value::temp(frameTemp)}));
                current.instructions.push_back(
                    makeCallResult(setjmpTemp, i64Ty(), kSetjmpSymbol, {Value::temp(frameTemp)}));
                {
                    Instr cmp;
                    cmp.result = caughtTemp;
                    cmp.op = Opcode::ICmpNe;
                    cmp.type = i1Ty();
                    cmp.operands.push_back(Value::temp(setjmpTemp));
                    cmp.operands.push_back(Value::constInt(0));
                    current.instructions.push_back(std::move(cmp));
                }
                current.instructions.push_back(
                    makeCBr(Value::temp(caughtTemp), handlerEntryLabel, afterLabel));
                current.terminated = true;
                appendBlock(std::move(current));

                BasicBlock handlerEntry;
                handlerEntry.label = handlerEntryLabel;
                const unsigned frameLoad =
                    reserveTemp(fn, "__neh.frame.load." + std::to_string(scopeId));
                const unsigned siteTemp = reserveTemp(fn, "__neh.site." + std::to_string(scopeId));
                handlerEntry.instructions.push_back(
                    makeLoad(frameLoad, Value::temp(scope.slotTemp)));
                handlerEntry.instructions.push_back(
                    makeCallVoid(kFramePop, {Value::temp(frameLoad)}));
                handlerEntry.instructions.push_back(
                    makeCallResult(siteTemp, i64Ty(), kFrameGetSite, {Value::temp(frameLoad)}));
                handlerEntry.instructions.push_back(
                    makeCallVoid(kFrameFree, {Value::temp(frameLoad)}));
                handlerEntry.instructions.push_back(
                    makeStore(Value::temp(scope.slotTemp), Value::null()));

                std::vector<Value> handlerArgs;
                auto hIt = blockIndex.find(scope.handlerLabel);
                if (hIt != blockIndex.end()) {
                    const auto &handlerBlock = fn.blocks[hIt->second];
                    if (!handlerBlock.params.empty())
                        handlerArgs.push_back(Value::null());
                    if (handlerBlock.params.size() > 1)
                        handlerArgs.push_back(Value::temp(siteTemp));
                }
                handlerEntry.instructions.push_back(
                    makeBr(scope.handlerLabel, std::move(handlerArgs)));
                handlerEntry.terminated = true;
                appendBlock(std::move(handlerEntry));

                active.push_back(scopeId);
                current = {};
                current.label = afterLabel;
                continue;
            }

            if (instr.op == Opcode::EhPop) {
                rewritten.changed = true;
                if (active.empty())
                    continue;
                const int scopeId = active.back();
                active.pop_back();
                const auto &scope = scopes[static_cast<std::size_t>(scopeId)];
                const unsigned frameLoad = reserveTemp(
                    fn, "__neh.pop.frame." + std::to_string(scopeId) + "." + std::to_string(ii));
                current.instructions.push_back(makeLoad(frameLoad, Value::temp(scope.slotTemp)));
                current.instructions.push_back(makeCallVoid(kFramePop, {Value::temp(frameLoad)}));
                current.instructions.push_back(makeCallVoid(kFrameFree, {Value::temp(frameLoad)}));
                current.instructions.push_back(
                    makeStore(Value::temp(scope.slotTemp), Value::null()));
                continue;
            }

            if ((instr.op == Opcode::ResumeLabel || instr.op == Opcode::ResumeSame ||
                 instr.op == Opcode::ResumeNext) &&
                handlerSiteParam.find(orig.label) != handlerSiteParam.end()) {
                if (ii + 1 != orig.instructions.size()) {
                    ZANNA_ICE("native EH lowering encountered resume before the end of block '" +
                              orig.label + "'");
                }
                rewritten.changed = true;
                if (instr.op == Opcode::ResumeLabel) {
                    const auto tokIt = handlerSiteParam.find(orig.label);
                    const std::string targetLabel =
                        instr.labels.empty() ? invalidResumeLabel : instr.labels[0];
                    const std::vector<Value> targetArgs =
                        (!instr.labels.empty() && !instr.brArgs.empty()) ? instr.brArgs[0]
                                                                         : std::vector<Value>{};
                    const auto siteListIt = handlerSites.find(orig.label);
                    const std::vector<SiteInfo> &siteList =
                        (siteListIt != handlerSites.end() && !siteListIt->second.empty())
                            ? siteListIt->second
                            : allSites;
                    needsInvalidResume = true;
                    if (tokIt == handlerSiteParam.end() || siteList.empty()) {
                        current.instructions.push_back(makeBr(invalidResumeLabel));
                        current.terminated = true;
                        appendBlock(std::move(current));
                        current = {};
                    } else {
                        std::string nextDispatchLabel = invalidResumeLabel;
                        for (std::size_t si = 0; si < siteList.size(); ++si) {
                            const auto &site = siteList[si];
                            const std::string fallback = (si + 1 < siteList.size())
                                                             ? newLabel("resume_label_dispatch")
                                                             : invalidResumeLabel;
                            BasicBlock dispatch;
                            if (si == 0) {
                                dispatch = std::move(current);
                            } else {
                                dispatch.label = nextDispatchLabel;
                            }
                            const unsigned cmpTemp =
                                reserveTemp(fn, "__neh.resume.label.cmp." + std::to_string(si));
                            Instr cmp;
                            cmp.result = cmpTemp;
                            cmp.op = Opcode::ICmpEq;
                            cmp.type = i1Ty();
                            cmp.operands.push_back(Value::temp(tokIt->second));
                            cmp.operands.push_back(Value::constInt(site.siteId));
                            dispatch.instructions.push_back(std::move(cmp));
                            dispatch.instructions.push_back(makeCBrWithTrueArgs(
                                Value::temp(cmpTemp), targetLabel, targetArgs, fallback));
                            dispatch.terminated = true;
                            appendBlock(std::move(dispatch));
                            nextDispatchLabel = fallback;
                        }
                        current = {};
                    }
                } else {
                    const auto tokIt = handlerSiteParam.find(orig.label);
                    needsInvalidResume = true;
                    std::string nextDispatchLabel = invalidResumeLabel;
                    const auto siteListIt = handlerSites.find(orig.label);
                    const std::vector<SiteInfo> &siteList =
                        (siteListIt != handlerSites.end() && !siteListIt->second.empty())
                            ? siteListIt->second
                            : allSites;
                    if (tokIt == handlerSiteParam.end() || siteList.empty()) {
                        current.instructions.push_back(makeBr(invalidResumeLabel));
                        current.terminated = true;
                        appendBlock(std::move(current));
                        current = {};
                    } else {
                        for (std::size_t si = 0; si < siteList.size(); ++si) {
                            const auto &site = siteList[si];
                            const std::string fallback = (si + 1 < siteList.size())
                                                             ? newLabel("resume_dispatch")
                                                             : invalidResumeLabel;
                            BasicBlock dispatch;
                            if (si == 0) {
                                dispatch = std::move(current);
                            } else {
                                dispatch.label = nextDispatchLabel;
                            }
                            const unsigned cmpTemp =
                                reserveTemp(fn, "__neh.resume.cmp." + std::to_string(si));
                            Instr cmp;
                            cmp.result = cmpTemp;
                            cmp.op = Opcode::ICmpEq;
                            cmp.type = i1Ty();
                            cmp.operands.push_back(Value::temp(tokIt->second));
                            cmp.operands.push_back(Value::constInt(site.siteId));
                            dispatch.instructions.push_back(std::move(cmp));
                            dispatch.instructions.push_back(makeCBr(
                                Value::temp(cmpTemp),
                                instr.op == Opcode::ResumeSame ? site.sameLabel : site.nextLabel,
                                fallback));
                            dispatch.terminated = true;
                            appendBlock(std::move(dispatch));
                            nextDispatchLabel = fallback;
                        }
                        current = {};
                    }
                }
                break;
            }

            if (mayTrap(instr.op) && !active.empty()) {
                rewritten.changed = true;
                const int scopeId = active.back();
                auto &scope = scopes[static_cast<std::size_t>(scopeId)];
                const auto &site = siteForInstr.at(PushKey{bi, ii});
                const int64_t siteId = site.siteId;
                const std::string &siteLabel = site.sameLabel;
                const bool isTerm = il::verify::isTerminator(instr.op);
                if (isTerm && ii + 1 != orig.instructions.size()) {
                    ZANNA_ICE(
                        "native EH lowering encountered terminator before the end of block '" +
                        orig.label + "'");
                }
                const std::string &nextLabel = site.nextLabel;

                current.instructions.push_back(makeBr(siteLabel));
                current.terminated = true;
                appendBlock(std::move(current));

                BasicBlock siteBlock;
                siteBlock.label = siteLabel;
                const unsigned frameLoad =
                    reserveTemp(fn, "__neh.site.frame." + std::to_string(siteId));
                siteBlock.instructions.push_back(makeLoad(frameLoad, Value::temp(scope.slotTemp)));
                siteBlock.instructions.push_back(
                    makeCallVoid(kFrameSetSite, {Value::temp(frameLoad), Value::constInt(siteId)}));
                siteBlock.instructions.push_back(std::move(instr));
                if (!isTerm)
                    siteBlock.instructions.push_back(makeBr(nextLabel));
                siteBlock.terminated = true;
                appendBlock(std::move(siteBlock));

                if (isTerm) {
                    current = {};
                    break;
                }
                current = {};
                current.label = nextLabel;
                continue;
            }

            current.instructions.push_back(std::move(instr));
        }

        if (!current.label.empty())
            appendBlock(std::move(current));
    }

    if (needsInvalidResume) {
        BasicBlock invalid;
        invalid.label = invalidResumeLabel;
        invalid.instructions.push_back(makeTrapFromErr(kErrInvalidOperation));
        invalid.terminated = true;
        appendBlock(std::move(invalid));
    }

    if (rewritten.changed)
        fn.blocks = rewritten.blocks;
    return rewritten;
}

} // namespace

/// @copydoc lowerNativeEh
bool lowerNativeEh(Module &module) {
    bool changed = false;
    for (auto &fn : module.functions) {
        auto rewritten = rewriteFunction(module, fn);
        changed |= rewritten.changed;
    }
    return changed;
}

/// @copydoc findResidualStructuredEh
std::optional<std::string> findResidualStructuredEh(const Module &module) {
    for (const auto &fn : module.functions) {
        for (const auto &bb : fn.blocks) {
            if (bb.params.size() >= 2 && bb.params[0].type.kind == Type::Kind::Error &&
                bb.params[1].type.kind == Type::Kind::ResumeTok) {
                return fn.name + ":" + bb.label +
                       ": residual handler error/resume-token block parameters after "
                       "NativeEHLowering";
            }

            for (const auto &instr : bb.instructions) {
                if (!isEhOpcode(instr.op))
                    continue;
                return fn.name + ":" + bb.label + ": residual " +
                       std::string(il::core::toString(instr.op)) + " after NativeEHLowering";
            }
        }
    }
    return std::nullopt;
}

} // namespace zanna::codegen::common
