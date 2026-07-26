//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements a small direct-call inliner with a simple cost model. The inliner
// targets tiny, non-recursive callees with a handful of blocks and no
// exception-handling constructs. Callee parameters (including block parameters)
// are mapped to call operands, SSA temporaries are remapped into the caller,
// and returns branch to a continuation block at the call site. A hard budget on
// instruction count, block count, and inline depth keeps code growth bounded.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements cost-guided direct-call inlining for IL modules.
 *
 * @details The implementation measures candidate legality and cost, validates
 *          the complete call-site rewrite before mutation, clones and renames
 *          callee CFG/SSA state, threads escaping caller values through a
 *          continuation block, and converts returns into branches. Per-block
 *          depth tracking and module-wide growth accounting bound repeated
 *          aggressive rounds.
 */

#include "il/transform/Inline.hpp"

#include "il/analysis/CallGraph.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/internal/io/ParserUtil.hpp"

#include "il/utils/UseDefInfo.hpp"
#include "il/utils/Utils.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {
/// @brief Maximum number of uses permitted for an otherwise eligible callee.
constexpr unsigned kMaxCallSites = 8;

/// @brief Separator that makes function/block depth keys unambiguous.
constexpr char kDepthKeySep = '\0';

/// @brief Inline nesting depth indexed by a composite function/block key.
using BlockDepthMap = std::unordered_map<std::string, unsigned>;

/// @brief Structural and size metrics used to judge an inline candidate.
/// @details The record separates hard legality properties from adjustable cost
///          inputs so individual call sites can apply constant-argument bonuses.
struct InlineCost {
    /// @brief Total instructions in the candidate callee.
    unsigned instrCount = 0;
    /// @brief Number of basic blocks in the candidate callee.
    unsigned blockCount = 0;
    /// @brief Direct call-site count reported by the module call graph.
    unsigned callSites = 0;
    /// @brief Number of direct or indirect calls inside the candidate.
    unsigned nestedCalls = 0;
    /// @brief Number of return terminators inside the candidate.
    unsigned returnCount = 0;
    /// @brief Whether call-graph analysis found recursive reachability.
    bool recursive = false;
    /// @brief Whether the body contains exception-handling instructions.
    bool hasEH = false;
    /// @brief Whether the body allocates function-scoped stack storage.
    bool hasAlloca = false;
    /// @brief Whether the signature exceeds the current value remapper's type support.
    bool hasNonScalarSignature = false;
    /// @brief Whether the body has malformed or unsupported control flow.
    bool unsupportedCFG = false;
    /// @brief Whether at least one valid return terminator was found.
    bool hasReturn = false;

    /// @brief Check whether the callee satisfies non-negotiable structural constraints.
    /// @return `true` when the function is non-recursive, scalar, supported, and returns.
    bool isInlinable() const {
        return !recursive && !hasEH && !hasAlloca && !hasNonScalarSignature && !unsupportedCFG &&
               hasReturn;
    }

    /// @brief Compute the site-specific cost after applying bonuses and penalties.
    /// @param config Cost-model weights used for the calculation.
    /// @param constArgCount Number of constant operands at the prospective call site.
    /// @return Saturated signed cost, or `INT_MAX` when the callee is not inlinable.
    int adjustedCost(const InlineCostConfig &config, unsigned constArgCount) const {
        if (!isInlinable())
            return INT_MAX;

        long long cost = static_cast<long long>(instrCount);

        // Apply bonuses
        if (callSites == 1)
            cost -= static_cast<long long>(config.singleUseBonus);

        if (instrCount <= 8)
            cost -= static_cast<long long>(config.tinyFunctionBonus);

        // Constant arguments enable optimization
        cost -=
            static_cast<long long>(constArgCount) * static_cast<long long>(config.constArgBonus);

        // Penalty for functions with many nested calls (may cause code explosion)
        cost += static_cast<long long>(nestedCalls) * 2LL;

        // Multiple returns are slightly more expensive to inline
        if (returnCount > 1)
            cost += static_cast<long long>(returnCount - 1) * 2LL;

        if (cost > static_cast<long long>(INT_MAX))
            return INT_MAX;
        if (cost < static_cast<long long>(INT_MIN))
            return INT_MIN;
        return static_cast<int>(cost);
    }

    /// @brief Decide whether a call site is within the inline budget.
    /// @details First checks hard limits (inlinability flag, block count, call-site
    ///          count) and then compares the adjusted instruction cost against the
    ///          configured threshold.  Constant argument bonuses are applied inside
    ///          @ref adjustedCost so specialisable sites are more aggressively inlined.
    /// @param config        Tuning parameters for the inliner.
    /// @param constArgCount Number of call arguments known to be constants.
    /// @return @c true when inlining is legal and within cost budget.
    bool withinBudget(const InlineCostConfig &config, unsigned constArgCount) const {
        if (!isInlinable())
            return false;
        if (blockCount > config.blockBudget)
            return false;
        if (blockCount > 1 && config.requireSingleReturnForMultiBlock && returnCount != 1)
            return false;
        if (callSites > kMaxCallSites)
            return false;

        int cost = adjustedCost(config, constArgCount);
        return cost <= static_cast<int>(config.instrThreshold);
    }
};

/// @brief Build a composite key for the block-depth map.
/// @details Concatenates the function name and block label with a NUL separator
///          so that no valid identifier can collide with the combined key.
/// @param fn Function name prefix.
/// @param label Block label suffix.
/// @return Composite key suitable for BlockDepthMap lookups.
std::string depthKey(const std::string &fn, const std::string &label) {
    return fn + kDepthKeySep + label;
}

/// @brief Query the inline depth recorded for a specific block.
/// @param depths Map of (function+label) → depth values.
/// @param fn Owning function name.
/// @param label Block label within the function.
/// @return Recorded depth, or 0 if no entry exists.
unsigned getBlockDepth(const BlockDepthMap &depths,
                       const std::string &fn,
                       const std::string &label) {
    auto it = depths.find(depthKey(fn, label));
    if (it == depths.end())
        return 0;
    return it->second;
}

/// @brief Record the inline depth for a specific block.
/// @param depths Map of (function+label) → depth values (modified in-place).
/// @param fn Owning function name.
/// @param label Block label within the function.
/// @param depth Inline nesting depth to store.
void setBlockDepth(BlockDepthMap &depths,
                   const std::string &fn,
                   const std::string &label,
                   unsigned depth) {
    depths[depthKey(fn, label)] = depth;
}

/// @brief Test whether an instruction is a direct (non-indirect) call.
/// @param I Instruction to inspect.
/// @return True when the opcode is Call and a callee name is present.
bool isDirectCall(const Instr &I) {
    return I.isDirectCall();
}

/// @brief Test whether an instruction is part of the exception-handling framework.
/// @param I Instruction to inspect.
/// @return True for EhPush, EhPop, EhEntry, ResumeSame, ResumeNext, ResumeLabel.
bool isEHSensitive(const Instr &I) {
    switch (I.op) {
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

/// @brief Test whether a terminator instruction is unsupported for inlining.
/// @details The inliner only handles Ret, Br, CBr, and SwitchI32 terminators.
/// @param I Terminator instruction to check.
/// @return True when the terminator cannot be inlined.
bool hasUnsupportedTerminator(const Instr &I) {
    return !(I.op == Opcode::Ret || I.op == Opcode::Br || I.op == Opcode::CBr ||
             I.op == Opcode::SwitchI32);
}

/// @brief Count the total number of instructions across all blocks of a function.
/// @param F Function to measure.
/// @return Sum of instruction counts in every block.
unsigned countInstructions(const Function &F) {
    unsigned n = 0;
    for (const auto &B : F.blocks)
        n += static_cast<unsigned>(B.instructions.size());
    return n;
}

/// @brief Look up the debug name for an SSA value, falling back to a default.
/// @param F Function whose valueNames table is queried.
/// @param id SSA value identifier.
/// @param fallback String returned when no name is recorded for @p id.
/// @return The stored name if present and non-empty; otherwise @p fallback.
std::string lookupValueName(const Function &F, unsigned id, const std::string &fallback) {
    if (id < F.valueNames.size() && !F.valueNames[id].empty())
        return F.valueNames[id];
    return fallback;
}

/// @brief Collect every explicit temp/param name already used in a function.
/// @details The textual IL parser resolves temps by printed name, so any new
///          names introduced by the inliner must be unique within the whole
///          function, not just within the inlined region.
/// @param F Function whose parameter and value-name namespaces are scanned.
/// @return Set containing every nonempty name already reserved by @p F.
std::unordered_set<std::string> collectUsedValueNames(const Function &F) {
    std::unordered_set<std::string> names;
    names.reserve(F.params.size() + F.blocks.size() * 4 + F.valueNames.size());

    for (const auto &param : F.params)
        if (!param.name.empty())
            names.insert(param.name);

    for (const auto &block : F.blocks)
        for (const auto &param : block.params)
            if (!param.name.empty())
                names.insert(param.name);

    for (const auto &name : F.valueNames)
        if (!name.empty())
            names.insert(name);

    return names;
}

/// @brief Normalize an SSA debug name into an IL identifier fragment.
/// @details Inliner-derived names can originate from user-facing value names
///          that already include the `%` sigil or from intermediate debug names
///          that contain parser delimiters. This helper strips redundant temp
///          sigils, replaces delimiter characters with underscores, and falls
///          back to `tmp` if the result still cannot be parsed as an IL
///          identifier fragment.
/// @param base Candidate name to normalize.
/// @return A verifier-valid identifier fragment suitable for Param::name.
std::string canonicalValueName(std::string base) {
    while (!base.empty() && base.front() == '%')
        base.erase(base.begin());

    if (il::io::isValidILIdentifier(base))
        return base;

    for (size_t index = 0; index < base.size(); ++index) {
        const unsigned char ch = static_cast<unsigned char>(base[index]);
        if (std::isspace(ch)) {
            base[index] = '_';
            continue;
        }
        switch (static_cast<char>(ch)) {
            case '@':
            case '^':
            case '(':
            case ')':
            case '{':
            case '}':
            case '[':
            case ']':
            case ',':
            case ':':
            case ';':
            case '"':
            case '\\':
                base[index] = '_';
                break;
            default:
                break;
        }
        if (index == 0 && (ch == '%' || ch == '#'))
            base[index] = '_';
    }

    if (!il::io::isValidILIdentifier(base))
        return "tmp";
    return base;
}

/// @brief Make a temp/param name unique within a function namespace.
/// @details Canonicalises generated names to the parser/verifier identifier
///          fragment rules before adding a numeric suffix for collisions.
/// @param usedNames Set of names already reserved in the function. Updated in place.
/// @param base Desired name, with an optional leading `%` tolerated.
/// @return A valid unique name derived from @p base.
std::string reserveUniqueValueName(std::unordered_set<std::string> &usedNames, std::string base) {
    base = canonicalValueName(std::move(base));
    if (!usedNames.contains(base)) {
        usedNames.insert(base);
        return base;
    }

    std::string candidate = base;
    unsigned suffix = 0;
    do {
        candidate = base + "_" + std::to_string(++suffix);
    } while (usedNames.contains(candidate));

    usedNames.insert(candidate);
    return candidate;
}

/// @brief Record a debug name for an SSA value, growing the table if needed.
/// @param F Function whose valueNames table is modified.
/// @param id SSA value identifier.
/// @param name Name to associate; empty names are silently ignored.
void ensureValueName(Function &F, unsigned id, const std::string &name) {
    if (name.empty())
        return;
    if (F.valueNames.size() <= id)
        F.valueNames.resize(id + 1);
    F.valueNames[id] = name;
}

/// @brief Measure a function and identify structural reasons it cannot be inlined.
/// @param fn Candidate callee to inspect.
/// @param cg Module call graph supplying call counts and recursion information.
/// @return Populated metrics consumed by the per-call-site cost model.
/// @details This evaluation rejects unsupported signatures, exception handling,
///          stack allocation, malformed returns, and terminators the cloning
///          routine cannot reproduce.
InlineCost evaluateInlineCost(const Function &fn, const zanna::analysis::CallGraph &cg) {
    InlineCost cost;
    cost.instrCount = countInstructions(fn);
    cost.blockCount = static_cast<unsigned>(fn.blocks.size());

    /// @brief Determine whether the current value remapper supports a signature type.
    /// @param type Candidate parameter or return type.
    /// @return True for void, boolean, integer, and floating scalar types.
    auto isScalarType = [](const Type &type) {
        return type.kind == Type::Kind::I64 || type.kind == Type::Kind::I1 ||
               type.kind == Type::Kind::F64 || type.kind == Type::Kind::Void;
    };
    if (!isScalarType(fn.retType))
        cost.hasNonScalarSignature = true;
    for (const auto &param : fn.params)
        if (!isScalarType(param.type))
            cost.hasNonScalarSignature = true;

    auto callIt = cg.callCounts.find(fn.name);
    if (callIt != cg.callCounts.end())
        cost.callSites = callIt->second;

    cost.recursive = cg.isRecursive(fn.name);

    if (fn.blocks.empty()) {
        cost.unsupportedCFG = true;
        return cost;
    }

    // Entry-block params are handled: we pass call arguments as branch args
    // when jumping to the cloned entry block (see inlineCallSite).

    for (const auto &B : fn.blocks) {
        if (!zanna::il::isTerminated(B)) {
            cost.unsupportedCFG = true;
            continue;
        }

        const Instr &term = B.instructions.back();
        if (hasUnsupportedTerminator(term))
            cost.unsupportedCFG = true;

        if (term.op == Opcode::Ret) {
            cost.hasReturn = true;
            ++cost.returnCount;
            bool expectValue = fn.retType.kind != Type::Kind::Void;
            bool hasValue = !term.operands.empty();
            if (expectValue != hasValue)
                cost.unsupportedCFG = true;
        }

        for (const auto &I : B.instructions) {
            if (isEHSensitive(I))
                cost.hasEH = true;

            if (I.op == Opcode::Alloca)
                cost.hasAlloca = true;

            // Count nested calls
            if (I.op == Opcode::Call || I.op == Opcode::CallIndirect)
                ++cost.nestedCalls;
        }
    }

    return cost;
}

/// @brief Count constant arguments in a call instruction.
/// @param callInstr Direct call whose operands are classified.
/// @return Number of integer, floating-point, string, or null-pointer constants.
unsigned countConstantArgs(const Instr &callInstr) {
    unsigned count = 0;
    for (const auto &op : callInstr.operands) {
        if (op.kind == Value::Kind::ConstInt || op.kind == Value::Kind::ConstFloat ||
            op.kind == Value::Kind::NullPtr || op.kind == Value::Kind::ConstStr) {
            ++count;
        }
    }
    return count;
}

/// @brief Generate a block label that does not collide with existing labels.
/// @details Builds an unordered_set of existing labels for O(1) collision checks,
///          then appends increasing numeric suffixes until a unique name is found.
///          This replaces a previous O(n) linear scan per candidate, improving
///          performance from O(n*k) to O(n+k) where n = block count, k = attempts.
/// @param function Function whose blocks define the label namespace.
/// @param base Desired label prefix; returned as-is when no collision occurs.
/// @return A label guaranteed to be unique within @p function.
std::string makeUniqueLabel(const Function &function, const std::string &base) {
    std::unordered_set<std::string> existingLabels;
    existingLabels.reserve(function.blocks.size());
    for (const auto &block : function.blocks)
        existingLabels.insert(block.label);

    std::string candidate = base;
    unsigned suffix = 0;
    while (existingLabels.count(candidate)) {
        candidate = base + "." + std::to_string(++suffix);
    }
    return candidate;
}

/// @brief Remap a temporary value through a substitution map.
/// @param v The value to remap.
/// @param map Mapping from old temporary IDs to replacement values.
/// @return The replacement value if \p v is a temporary found in \p map,
///         otherwise \p v unchanged.
Value remapValue(const Value &v, const std::unordered_map<unsigned, Value> &map) {
    if (v.kind != Value::Kind::Temp)
        return v;
    auto it = map.find(v.id);
    if (it == map.end())
        return v;
    return it->second;
}

/// @brief Replace all uses of a temporary in a basic block.
/// @details Scans every instruction operand and branch argument in \p block,
///          replacing any temporary whose ID matches \p from with \p replacement.
/// @param block The basic block to rewrite.
/// @param from  The temporary ID to search for.
/// @param replacement The value to substitute in place of the old temporary.
void replaceUsesInBlock(BasicBlock &block, unsigned from, const Value &replacement) {
    for (auto &instr : block.instructions) {
        for (auto &op : instr.operands) {
            if (op.kind == Value::Kind::Temp && op.id == from)
                op = replacement;
        }

        for (auto &argList : instr.brArgs) {
            for (auto &arg : argList) {
                if (arg.kind == Value::Kind::Temp && arg.id == from)
                    arg = replacement;
            }
        }
    }
}

/// @brief Map each callee entry-block param to the index of the call operand
///        that supplies it. Prefers exact param-id matches, then falls back to
///        positional type-compatible mapping for canonical entry params.
/// @param callee Function whose formal and entry-block parameters are correlated.
/// @return The per-entry-param call-arg indices, or std::nullopt if any entry
///         param cannot be mapped (the call site must then be left un-inlined).
std::optional<std::vector<size_t>> mapEntryParamsToCallArgs(const Function &callee) {
    std::vector<size_t> entryParamToCallArg;
    if (callee.blocks.empty())
        return entryParamToCallArg;
    const auto &entryParams = callee.blocks.front().params;
    entryParamToCallArg.reserve(entryParams.size());
    for (size_t epIdx = 0; epIdx < entryParams.size(); ++epIdx) {
        const auto &ep = entryParams[epIdx];
        std::optional<size_t> mappedIndex;
        for (size_t fpIdx = 0; fpIdx < callee.params.size(); ++fpIdx) {
            const auto &fp = callee.params[fpIdx];
            if (fp.id == ep.id && fp.type.kind == ep.type.kind) {
                mappedIndex = fpIdx;
                break;
            }
        }

        if (!mappedIndex && entryParams.size() == callee.params.size() &&
            epIdx < callee.params.size() && callee.params[epIdx].type.kind == ep.type.kind) {
            mappedIndex = epIdx;
        }

        if (!mappedIndex)
            return std::nullopt;
        entryParamToCallArg.push_back(*mappedIndex);
    }
    return entryParamToCallArg;
}

/// @brief Map a fixed TypeCategory to a concrete Type, or Void when the category
///        is not a fixed type (None/Any/InstrType/Dynamic).
/// @param cat Opcode metadata category to convert.
/// @return Concrete IL type, or `void` for a category requiring contextual inference.
Type fixedCategoryType(TypeCategory cat) {
    switch (cat) {
        case TypeCategory::Void: return Type(Type::Kind::Void);
        case TypeCategory::I1: return Type(Type::Kind::I1);
        case TypeCategory::I16: return Type(Type::Kind::I16);
        case TypeCategory::I32: return Type(Type::Kind::I32);
        case TypeCategory::I64: return Type(Type::Kind::I64);
        case TypeCategory::F64: return Type(Type::Kind::F64);
        case TypeCategory::Ptr: return Type(Type::Kind::Ptr);
        case TypeCategory::Str: return Type(Type::Kind::Str);
        case TypeCategory::Error: return Type(Type::Kind::Error);
        case TypeCategory::ResumeTok: return Type(Type::Kind::ResumeTok);
        default: return Type(Type::Kind::Void);
    }
}

/// @brief Resolve a caller temporary's static type from parameters or its definition.
/// @param caller Function whose SSA namespace contains the temporary.
/// @param functionLookup Module function map used to type direct-call results.
/// @param id Temporary id to resolve.
/// @param memo Cache of previously resolved ids.
/// @param active Recursion guard for operand-dependent type inference.
/// @return Inferred type, conservatively falling back to `i64`.
Type resolveTempType(const Function &caller,
                     const std::unordered_map<std::string, const Function *> &functionLookup,
                     unsigned id,
                     std::unordered_map<unsigned, Type> &memo,
                     std::unordered_set<unsigned> &active);

/// @brief Static result type of an instruction, mirroring the verifier's
///        inference. Most non-F64 results record Void on the instruction (only
///        F64 is stored, for register-class selection), so we recover the real
///        type from the opcode result-type model and, for `InstrType` ops (the
///        arithmetic / checked / bitwise / shift family), from the operands.
/// @param caller Function containing @p ins and its operand definitions.
/// @param functionLookup Module function map used to type direct-call results.
/// @param ins Instruction whose result type is required.
/// @param memo Temporary-type cache shared with recursive lookups.
/// @param active Temporary ids currently being resolved.
/// @return Recovered result type, with `i64` as the conservative fallback.
Type resolveInstrResultType(const Function &caller,
                            const std::unordered_map<std::string, const Function *> &functionLookup,
                            const Instr &ins,
                            std::unordered_map<unsigned, Type> &memo,
                            std::unordered_set<unsigned> &active) {
    if (ins.type.kind != Type::Kind::Void)
        return ins.type; // concretely recorded (F64 results, casts, typed loads)
    if (ins.op == Opcode::Call) {
        auto it = functionLookup.find(ins.callee);
        if (it != functionLookup.end())
            return it->second->retType;
        return Type(Type::Kind::I64);
    }
    const OpcodeInfo &info = getOpcodeInfo(ins.op);
    Type fixed = fixedCategoryType(info.resultType);
    if (fixed.kind != Type::Kind::Void)
        return fixed;
    if (info.resultType == TypeCategory::InstrType ||
        info.resultType == TypeCategory::Dynamic ||
        info.resultType == TypeCategory::Any) {
        for (const auto &op : ins.operands) {
            if (op.kind == Value::Kind::Temp)
                return resolveTempType(caller, functionLookup, op.id, memo, active);
        }
    }
    return Type(Type::Kind::I64);
}

/// @brief Resolve the static type of caller temporary @p id, recursing through
///        operand types as needed. Memoized, with a cycle guard.
/// @param caller Function whose SSA namespace contains @p id.
/// @param functionLookup Module function map used to type direct-call results.
/// @param id Temporary id to resolve.
/// @param memo Cache updated with the resolved type.
/// @param active Set used to stop malformed recursive definition chains.
/// @return Recovered static type, or `i64` if no more precise type is available.
Type resolveTempType(const Function &caller,
                     const std::unordered_map<std::string, const Function *> &functionLookup,
                     unsigned id,
                     std::unordered_map<unsigned, Type> &memo,
                     std::unordered_set<unsigned> &active) {
    if (auto it = memo.find(id); it != memo.end())
        return it->second;
    if (!active.insert(id).second)
        return Type(Type::Kind::I64); // cycle guard (should not occur in valid SSA)

    Type result(Type::Kind::I64);
    bool found = false;
    for (const auto &fp : caller.params) {
        if (fp.id == id) {
            result = fp.type;
            found = true;
            break;
        }
    }
    if (!found) {
        for (const auto &bb : caller.blocks) {
            for (const auto &bp : bb.params) {
                if (bp.id == id) {
                    result = bp.type;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
            for (const auto &ins : bb.instructions) {
                if (ins.result && *ins.result == id) {
                    result = resolveInstrResultType(caller, functionLookup, ins, memo, active);
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    active.erase(id);
    memo[id] = result;
    return result;
}

/// @brief Replace one direct call with cloned callee blocks and a continuation.
/// @param caller Function containing the call and receiving the cloned CFG.
/// @param callBlockIdx Index of the block containing the call.
/// @param callIndex Instruction index of the call within that block.
/// @param callee Function whose body is cloned.
/// @param callDepth Inline depth associated with the original call block.
/// @param maxDepth Maximum permitted nesting depth.
/// @param depths Per-block depth map updated for cloned and continuation blocks.
/// @param functionLookup Module function map used while recovering escaped-value types.
/// @return `true` after committing the rewrite, or `false` when validation fails.
/// @details All fallible structural and type analysis is completed before the
///          original call block is truncated. The commit phase remaps SSA ids,
///          threads values escaping into the continuation, converts returns to
///          branches, and inserts the cloned region directly after the call block.
bool inlineCallSite(Function &caller,
                    size_t callBlockIdx,
                    size_t callIndex,
                    const Function &callee,
                    unsigned callDepth,
                    unsigned maxDepth,
                    BlockDepthMap &depths,
                    const std::unordered_map<std::string, const Function *> &functionLookup) {
    if (callDepth >= maxDepth)
        return false;

    if (callBlockIdx >= caller.blocks.size())
        return false;

    caller.blocks.reserve(caller.blocks.size() + callee.blocks.size() + 1);

    BasicBlock &callBlock = caller.blocks[callBlockIdx];
    if (callIndex >= callBlock.instructions.size())
        return false;
    // Copy the call instruction — the reference becomes dangling after the
    // block is resized below.
    const Instr callInstr = callBlock.instructions[callIndex];

    if (callInstr.operands.size() != callee.params.size())
        return false;

    // Map entry-block params to call operands. Textual IL commonly names
    // function params and entry block params independently:
    //
    //   func @f(i64 %x) -> i64 {
    //   entry(%x0: i64):
    //
    // Older inliner logic required the ids to match, causing otherwise-valid
    // tiny callees to be skipped. Prefer exact id matches, then fall back to
    // positional type-compatible mapping for canonical entry params.
    auto mappedEntryParams = mapEntryParamsToCallArgs(callee);
    if (!mappedEntryParams)
        return false;
    std::vector<size_t> entryParamToCallArg = std::move(*mappedEntryParams);

    // The parser leaves call instruction type as Void for non-f64 returns
    // (only f64 is recorded for register-class selection).  Use the callee's
    // declared retType instead of the call instruction's type for the match.
    // Bail out only when the callee genuinely returns void but the call has a
    // result, or vice versa.
    if (callee.retType.kind == Type::Kind::Void && callInstr.result)
        return false;
    if (callee.retType.kind != Type::Kind::Void && !callInstr.result)
        return false;

    bool returnsValue = callee.retType.kind != Type::Kind::Void;
    if (!returnsValue && callInstr.result)
        return false;

    unsigned nextId = zanna::il::nextTempId(caller);

    // Value mapping from callee temps/params to caller values.
    std::unordered_map<unsigned, Value> valueMap;
    valueMap.reserve(callee.params.size() + callee.blocks.size() * 2);
    for (size_t i = 0; i < callee.params.size(); ++i)
        valueMap.emplace(callee.params[i].id, callInstr.operands[i]);

    // Build label map for cloned blocks.
    std::unordered_map<std::string, std::string> labelMap;
    labelMap.reserve(callee.blocks.size());
    for (const auto &B : callee.blocks) {
        std::string base = callBlock.label + ".inline." + callee.name + "." + B.label;
        labelMap.emplace(B.label, makeUniqueLabel(caller, base));
    }

    // =========================================================================
    // PHASE 1: Read-only analysis — can safely bail at any point
    // =========================================================================

    // Build continuation block from instructions after the call.
    BasicBlock continuation;
    continuation.label = makeUniqueLabel(caller, callBlock.label + ".inline.cont");
    continuation.instructions.assign(callBlock.instructions.begin() +
                                         static_cast<long>(callIndex + 1),
                                     callBlock.instructions.end());
    continuation.terminated = zanna::il::isTerminated(continuation);

    // Compute return param info (but don't add to continuation yet).
    std::unordered_set<std::string> usedValueNames = collectUsedValueNames(caller);

    Param retParam;
    bool hasRetParam = returnsValue && callInstr.result;
    if (hasRetParam) {
        retParam.name = reserveUniqueValueName(
            usedValueNames,
            lookupValueName(caller, *callInstr.result, "ret" + std::to_string(*callInstr.result)));
        retParam.id = nextId++;
        retParam.type = callee.retType;
    }

    // Compute escaped IDs: temps used in continuation but not defined there.
    // Include both the return param AND the original call result ID in contDefined.
    // The call result will be replaced by retParam in Phase 2 (UseDefInfo::replaceAllUses),
    // but since escape analysis runs in Phase 1 (before replacement), we must exclude
    // the original call result from escapedIds to avoid creating a spurious escaped param.
    std::unordered_set<unsigned> contDefined;
    if (hasRetParam) {
        contDefined.insert(retParam.id);
        contDefined.insert(*callInstr.result); // exclude original result from escape detection
    }
    for (const auto &instr : continuation.instructions) {
        if (instr.result)
            contDefined.insert(*instr.result);
    }

    std::unordered_set<unsigned> contUsed;
    for (const auto &instr : continuation.instructions) {
        for (const auto &op : instr.operands) {
            if (op.kind == Value::Kind::Temp)
                contUsed.insert(op.id);
        }
        for (const auto &argList : instr.brArgs) {
            for (const auto &v : argList) {
                if (v.kind == Value::Kind::Temp)
                    contUsed.insert(v.id);
            }
        }
    }

    // Build set of alloca result IDs — allocas are function-scoped resources
    // that persist after call-block truncation.  They must NOT be threaded
    // through continuation block parameters because:
    //   1. The alloca definition survives in the truncated call block.
    //   2. Escaping an alloca creates a bridge reference (Value::temp(origId))
    //      that DCE can orphan when it removes write-only allocas.
    //   3. The continuation can reference the alloca directly.
    std::unordered_set<unsigned> allocaIds;
    for (const auto &bb : caller.blocks) {
        for (const auto &instr : bb.instructions) {
            if (instr.op == Opcode::Alloca && instr.result)
                allocaIds.insert(*instr.result);
        }
    }

    std::vector<unsigned> escapedIds;
    for (unsigned id : contUsed) {
        if (contDefined.find(id) == contDefined.end() && allocaIds.find(id) == allocaIds.end())
            escapedIds.push_back(id);
    }
    std::sort(escapedIds.begin(), escapedIds.end());

    // Type inference for escaped values — runs on FULL (pre-truncated) caller.
    // This is the key advantage of Phase 1: we see ALL instructions including
    // those in the call block that will be truncated in Phase 2.
    struct EscapedParamInfo {
        /// @brief Continuation-block parameter created for the escaping value.
        Param param;
        /// @brief Whether static type resolution populated @ref param.
        bool typeFound{false};
    };

    std::vector<EscapedParamInfo> escapedParamInfos;
    escapedParamInfos.reserve(escapedIds.size());

    // Shared cache for the recursive value-type resolver.
    std::unordered_map<unsigned, Type> typeMemo;
    std::unordered_set<unsigned> typeActive;

    for (unsigned origId : escapedIds) {
        EscapedParamInfo info;
        Param &p = info.param;
        p.name = reserveUniqueValueName(
            usedValueNames, lookupValueName(caller, origId, "ext" + std::to_string(origId)));
        p.id = nextId++;
        // Resolve the escaped value's real type the way the verifier infers it.
        // Raw Instr.type is Void for most non-F64 results (e.g. the iadd.ovf
        // family), which previously yielded void-typed continuation params and a
        // branch-arg/param type mismatch.
        p.type = resolveTempType(caller, functionLookup, origId, typeMemo, typeActive);
        info.typeFound = true;

        escapedParamInfos.push_back(std::move(info));
    }

    // Note: escaped values without found types use the I64 fallback.
    // This is imprecise but safe — the verifier will catch actual type
    // mismatches, and the inline pass can be retried with better type info
    // after other optimization passes clean up the IL.

    // =========================================================================
    // PHASE 2: Commit — all validation passed, now mutate the caller
    // =========================================================================

    // Truncate call block (POINT OF NO RETURN).
    callBlock.instructions.resize(callIndex);
    callBlock.terminated = false;

    // Add return param to continuation.
    if (hasRetParam) {
        continuation.params.push_back(retParam);
        ensureValueName(caller, retParam.id, retParam.name);

        Value repl = Value::temp(retParam.id);
        zanna::il::UseDefInfo useInfo(caller);
        useInfo.replaceAllUses(*callInstr.result, repl);
        replaceUsesInBlock(continuation, *callInstr.result, repl);
    }

    // Add escaped params to continuation and build the remap.
    std::unordered_map<unsigned, unsigned> escapedMap;
    for (size_t i = 0; i < escapedIds.size(); ++i) {
        continuation.params.push_back(escapedParamInfos[i].param);
        ensureValueName(caller, escapedParamInfos[i].param.id, escapedParamInfos[i].param.name);
        escapedMap[escapedIds[i]] = escapedParamInfos[i].param.id;
    }

    // Remap continuation instructions to use the new params.
    if (!escapedMap.empty()) {
        for (auto &instr : continuation.instructions) {
            for (auto &op : instr.operands) {
                if (op.kind == Value::Kind::Temp) {
                    auto it = escapedMap.find(op.id);
                    if (it != escapedMap.end())
                        op = Value::temp(it->second);
                }
            }
            for (auto &argList : instr.brArgs) {
                for (auto &v : argList) {
                    if (v.kind == Value::Kind::Temp) {
                        auto it = escapedMap.find(v.id);
                        if (it != escapedMap.end())
                            v = Value::temp(it->second);
                    }
                }
            }
        }
    }

    // Clone callee blocks.
    std::vector<BasicBlock> clonedBlocks;
    clonedBlocks.reserve(callee.blocks.size());

    for (const auto &srcBlock : callee.blocks) {
        BasicBlock clone;
        clone.label = labelMap.at(srcBlock.label);

        // Clone block parameters with fresh IDs.
        // Use unique names to avoid collision with caller-scope temps that have
        // the same numeric names but different types (e.g., caller has %t8:ptr,
        // callee has %t8:i64). Without uniquification, the IL verifier may
        // resolve a branch argument to the wrong definition in scope.
        clone.params.reserve(srcBlock.params.size());
        for (const auto &param : srcBlock.params) {
            Param p = param;
            p.id = nextId++;
            valueMap[param.id] = Value::temp(p.id);
            std::string origName = lookupValueName(callee, param.id, param.name);
            std::string uniqueName =
                reserveUniqueValueName(usedValueNames, origName + "_il" + std::to_string(p.id));
            p.name = uniqueName; // Update the Param's own name to avoid collision
            clone.params.push_back(p);
            ensureValueName(caller, p.id, uniqueName);
        }

        for (size_t idx = 0; idx < srcBlock.instructions.size(); ++idx) {
            const Instr &CI = srcBlock.instructions[idx];

            if (idx + 1 == srcBlock.instructions.size() && CI.op == Opcode::Ret) {
                Instr bridge;
                bridge.op = Opcode::Br;
                bridge.type = Type(Type::Kind::Void);
                bridge.addBranchTarget(continuation.label);

                if (!continuation.params.empty()) {
                    auto &bridgeArgs = bridge.brArgs.back();
                    // Pass the return value as the continuation block's first parameter.
                    // Always emit a return value arg when the callee is non-void,
                    // even if this particular Ret has no operands (e.g., unreachable
                    // void-ret in a non-void function after optimization).
                    if (returnsValue) {
                        if (!CI.operands.empty())
                            bridgeArgs.push_back(remapValue(CI.operands.front(), valueMap));
                        else
                            bridgeArgs.push_back(Value::constInt(0));
                    }
                    // Pass escaped caller values as extra parameters.
                    for (unsigned origId : escapedIds)
                        bridgeArgs.push_back(Value::temp(origId));
                }

                clone.instructions.push_back(std::move(bridge));
                clone.terminated = true;
                continue;
            }

            Instr cloned = CI;
            cloned.operands.clear();
            cloned.clearBranchTargets();

            cloned.operands.reserve(CI.operands.size());
            for (const auto &op : CI.operands)
                cloned.operands.push_back(remapValue(op, valueMap));

            std::vector<std::string> remappedLabels;
            remappedLabels.reserve(CI.labels.size());
            for (const auto &lab : CI.labels)
                remappedLabels.push_back(labelMap.at(lab));
            std::vector<std::vector<Value>> remappedArgs;
            remappedArgs.reserve(CI.brArgs.size());
            for (const auto &argList : CI.brArgs) {
                std::vector<Value> remapped;
                remapped.reserve(argList.size());
                for (const auto &arg : argList)
                    remapped.push_back(remapValue(arg, valueMap));
                remappedArgs.push_back(std::move(remapped));
            }
            cloned.setBranchTargets(std::move(remappedLabels), std::move(remappedArgs));

            if (CI.result) {
                cloned.result = nextId;
                valueMap[*CI.result] = Value::temp(nextId);
                std::string origName = lookupValueName(callee, *CI.result, "");
                std::string uniqueName =
                    origName.empty()
                        ? reserveUniqueValueName(usedValueNames, "t" + std::to_string(nextId))
                        : reserveUniqueValueName(usedValueNames,
                                                 origName + "_il" + std::to_string(nextId));
                ensureValueName(caller, nextId, uniqueName);
                ++nextId;
            }

            clone.instructions.push_back(std::move(cloned));
        }

        if (!clone.terminated)
            clone.terminated = zanna::il::isTerminated(clone);

        clonedBlocks.push_back(std::move(clone));
    }

    // Branch from call site to cloned entry block.
    Instr jump;
    jump.op = Opcode::Br;
    jump.type = Type(Type::Kind::Void);
    jump.addBranchTarget(labelMap.at(callee.blocks.front().label));

    // Pass call arguments as branch args when the entry block has params.
    const auto &origEntryParams = callee.blocks.front().params;
    if (!origEntryParams.empty()) {
        std::vector<Value> args;
        args.reserve(origEntryParams.size());
        for (size_t mappedIndex : entryParamToCallArg) {
            if (mappedIndex >= callInstr.operands.size())
                return false;
            args.push_back(callInstr.operands[mappedIndex]);
        }
        jump.brArgs.back() = std::move(args);
    }

    callBlock.instructions.push_back(std::move(jump));
    callBlock.terminated = true;

    // Insert the inlined region immediately after the call block. The textual
    // IL parser expects value definitions to appear before uses, so appending
    // the continuation block at function end can leave its allocas/results
    // referenced by original successor blocks that are serialized earlier.
    setBlockDepth(depths, caller.name, continuation.label, callDepth);
    for (auto &B : clonedBlocks)
        setBlockDepth(depths, caller.name, B.label, callDepth + 1);

    auto insertPos = caller.blocks.begin() + static_cast<std::ptrdiff_t>(callBlockIdx + 1);
    insertPos = caller.blocks.insert(insertPos,
                                     std::make_move_iterator(clonedBlocks.begin()),
                                     std::make_move_iterator(clonedBlocks.end()));
    caller.blocks.insert(insertPos + static_cast<std::ptrdiff_t>(callee.blocks.size()),
                         std::move(continuation));

    return true;
}

} // namespace

/// @copydoc Inliner::id()
std::string_view Inliner::id() const {
    return "inline";
}

/// @copydoc Inliner::run()
PreservedAnalyses Inliner::run(Module &module, AnalysisManager &) {
    unsigned codeGrowth = 0;

    BlockDepthMap depths;
    for (const auto &fn : module.functions)
        for (const auto &B : fn.blocks)
            setBlockDepth(depths, fn.name, B.label, 0);

    bool changed = false;
    std::unordered_set<std::string> changedFunctions;

    const unsigned maxRounds = config_.aggressive ? 8U : 1U;
    for (unsigned round = 0; round < maxRounds; ++round) {
        zanna::analysis::CallGraph cg = zanna::analysis::buildCallGraph(module);

        std::unordered_map<std::string, const Function *> functionLookup;
        std::unordered_map<std::string, InlineCost> costCache;

        functionLookup.reserve(module.functions.size());
        costCache.reserve(module.functions.size());

        for (const auto &fn : module.functions) {
            functionLookup.emplace(fn.name, &fn);
            costCache.emplace(fn.name, evaluateInlineCost(fn, cg));
        }

        bool roundChanged = false;

        for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
            Function &caller = module.functions[fnIdx];

            // Snapshot block count for this round. Newly inserted blocks are
            // picked up on the next aggressive round, giving nested helper
            // chains a bounded fixpoint without recursively editing a block
            // while iterating it.
            const size_t originalBlockCount = caller.blocks.size();
            for (size_t blockIdx = 0; blockIdx < originalBlockCount; ++blockIdx) {
                BasicBlock &block = caller.blocks[blockIdx];
                size_t instIdx = 0;
                while (instIdx < block.instructions.size()) {
                    const Instr &I = block.instructions[instIdx];
                    if (!isDirectCall(I)) {
                        ++instIdx;
                        continue;
                    }

                    auto calleeIt = functionLookup.find(I.callee);
                    if (calleeIt == functionLookup.end()) {
                        ++instIdx;
                        continue;
                    }
                    const Function *callee = calleeIt->second;
                    if (callee->name == caller.name) {
                        ++instIdx;
                        continue;
                    }

                    auto edgeIt = cg.edges.find(callee->name);
                    if (edgeIt != cg.edges.end() &&
                        std::find(edgeIt->second.begin(), edgeIt->second.end(), caller.name) !=
                            edgeIt->second.end()) {
                        ++instIdx;
                        continue;
                    }

                    // Check code growth budget
                    const InlineCost &cost = costCache.at(callee->name);
                    if (cost.instrCount > config_.maxCodeGrowth ||
                        codeGrowth > config_.maxCodeGrowth - cost.instrCount) {
                        ++instIdx;
                        continue;
                    }

                    // Use enhanced cost model with constant argument bonuses
                    unsigned constArgs = countConstantArgs(I);
                    if (!cost.withinBudget(config_, constArgs)) {
                        ++instIdx;
                        continue;
                    }

                    unsigned depth = getBlockDepth(depths, caller.name, block.label);
                    if (!inlineCallSite(caller,
                                        blockIdx,
                                        instIdx,
                                        *callee,
                                        depth,
                                        config_.maxInlineDepth,
                                        depths,
                                        functionLookup)) {
                        ++instIdx;
                        continue;
                    }

                    // Track code growth (callee instructions minus the call itself)
                    if (cost.instrCount > 1)
                        codeGrowth += cost.instrCount - 1;

                    changed = true;
                    roundChanged = true;
                    changedFunctions.insert(caller.name);
                    break; // block reshaped; move to next block
                }
            }
        }

        if (!config_.aggressive || !roundChanged)
            break;
    }

    module.internOwnedIdentifiers();
    if (!changed)
        return PreservedAnalyses::all();
    PreservedAnalyses preserved;
    for (const auto &name : changedFunctions)
        preserved.markChangedFunction(name);
    return preserved;
}

/// @copydoc registerInlinePass()
void registerInlinePass(PassRegistry &registry) {
    /// @brief Run the default-cost inliner over a module.
    /// @param module Module to optimize in place.
    /// @param analysis Pipeline analysis manager forwarded to the pass.
    /// @return Analyses preserved by the inliner run.
    registry.registerModulePass("inline", [](core::Module &module, AnalysisManager &analysis) {
        Inliner inliner;
        return inliner.run(module, analysis);
    });
    /// @brief Run the aggressive O2 inliner with expanded budgets and fixpoint rounds.
    /// @param module Module to optimize in place.
    /// @param analysis Pipeline analysis manager forwarded to the pass.
    /// @return Analyses preserved by the inliner run.
    registry.registerModulePass("inline-o2", [](core::Module &module, AnalysisManager &analysis) {
        InlineCostConfig config;
        config.instrThreshold = 120;
        config.blockBudget = 4;
        config.maxCodeGrowth = 4000;
        config.aggressive = true;
        config.requireSingleReturnForMultiBlock = true;
        Inliner inliner(config);
        return inliner.run(module, analysis);
    });
}

} // namespace il::transform
