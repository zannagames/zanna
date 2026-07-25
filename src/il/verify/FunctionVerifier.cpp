//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/verify/FunctionVerifier.cpp
// Purpose: Coordinate function-level IL verification by combining block checks
//          with opcode-specific instruction strategies.
// Key invariants: Each function must expose a valid entry block, maintain
//                 unique labels, and respect extern/runtime signatures and
//                 handler semantics.
// Ownership/Lifetime: Operates on module-provided IR structures without
//                     retaining ownership beyond the call scope.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements signature, body, SSA, effect, and ownership verification.
/// @details Function verification proceeds through definition precollection,
///          block and instruction checks, effect-attribute validation,
///          component-aware dominance and release analyses, and alloca-derived
///          pointer escape checks.

#include "il/verify/FunctionVerifier.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeOwnership.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/verify/ControlFlowChecker.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/DiagSink.hpp"
#include "il/verify/ExceptionHandlerAnalysis.hpp"
#include "il/verify/InstructionChecker.hpp"
#include "il/verify/InstructionStrategies.hpp"
#include "il/verify/TypeInference.hpp"
#include "il/verify/VerifyCtx.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace il::core;

namespace il::verify {
using il::support::Diag;
using il::support::Expected;
using il::support::makeError;

namespace {

/// @brief Identify whether an opcode belongs to the resume-family terminators.
///
/// @details Resume opcodes have additional verifier requirements: they are only
///          legal inside handler blocks and must forward the `%tok` parameter.
///          Recognising them allows the verifier to enforce those constraints
///          uniformly.
///
/// @param op Opcode under classification.
/// @return @c true when the opcode is resume.same/next/label.
bool isResumeOpcode(Opcode op) {
    return op == Opcode::ResumeSame || op == Opcode::ResumeNext || op == Opcode::ResumeLabel;
}

/// @brief Detect opcodes that read fields from an error value.
///
/// @details Used to prevent explicit-error-operand `err.get_*` opcodes from
///          appearing outside handler blocks. Native EH lowering can leave the
///          operandless form, which reads the platform's current exception.
///
/// @param op Opcode under inspection.
/// @return @c true when @p op accesses error metadata.
bool isErrAccessOpcode(Opcode op) {
    switch (op) {
        case Opcode::ErrGetKind:
        case Opcode::ErrGetCode:
        case Opcode::ErrGetIp:
        case Opcode::ErrGetLine:
        case Opcode::ErrGetMsg:
            return true;
        default:
            return false;
    }
}

/// @brief Compare two ASCII-oriented symbol fragments without case sensitivity.
/// @param lhs First character sequence.
/// @param rhs Second character sequence.
/// @return `true` when lengths and lowercased bytes match.
bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char lc = static_cast<unsigned char>(lhs[i]);
        const unsigned char rc = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(lc) != std::tolower(rc))
            return false;
    }
    return true;
}

/// @brief Test whether a symbol ends with a case-insensitive suffix.
/// @param value Complete symbol name.
/// @param suffix Candidate trailing fragment.
/// @return `true` when @p suffix matches the end of @p value ignoring case.
bool endsWithIgnoreCase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size())
        return false;
    return equalsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

/// @brief Recognise runtime helper calls that explicitly release handles.
///
/// @details The verifier tracks releases so it can flag double-free and
///          use-after-release errors on SSA temporaries that reference runtime
///          strings, arrays, or objects. Runtime helpers such as string concat
///          may consume raw C handles internally, but the IL/VM ABI preserves
///          the SSA operand's lifetime, so those calls are not modeled here.
///
/// @param instr Instruction being analysed.
/// @return @c true when the instruction is an explicit runtime release helper.
bool isRuntimeExplicitRelease(const Instr &instr) {
    if (instr.op != Opcode::Call)
        return false;
    return instr.callee == "rt_str_release" || instr.callee == "rt_str_release_maybe" ||
           instr.callee == "Zanna.String.ReleaseMaybe" || instr.callee == "rt_arr_i32_release" ||
           instr.callee == "rt_arr_i64_release" || instr.callee == "rt_arr_f64_release" ||
           instr.callee == "rt_arr_str_release" || instr.callee == "rt_arr_obj_release" ||
           instr.callee == "rt_obj_release_check0" || instr.callee == "rt_obj_release_known_check0";
}

/// @brief Detect calls whose first operand is retained by runtime ownership metadata.
/// @param instr Candidate call instruction.
/// @return `true` for a direct call whose retained-argument mask includes bit zero.
bool isRuntimeArrayRetain(const Instr &instr) {
    if (instr.op != Opcode::Call)
        return false;
    const auto ownership = il::runtime::classifyRuntimeOwnership(instr.callee);
    return (ownership.retainedArgMask & 0b1u) != 0;
}

/// @brief Recognize object destruction and final-free calls.
/// @param instr Candidate instruction.
/// @return `true` when @p instr names a known or conventionally named finalizer.
bool isRuntimeObjectFinalizerCall(const Instr &instr) {
    return instr.op == Opcode::Call &&
           (instr.callee == "rt_obj_free" || instr.callee == "__zia_dtor_dispatch" ||
            endsWithIgnoreCase(instr.callee, ".__dtor") ||
            endsWithIgnoreCase(instr.callee, ".destroy"));
}

/// @brief Classify side-effecting terminators permitted in pure functions.
/// @param op Opcode to inspect.
/// @return `true` for branch, conditional branch, switch, or return.
bool isPureControlTerminator(Opcode op) {
    return op == Opcode::Br || op == Opcode::CBr || op == Opcode::SwitchI32 || op == Opcode::Ret;
}

/// @brief Classify branch-like opcodes that may forward block parameters.
/// @details Resume tokens are allowed to move across these instructions only as
///          branch arguments. The EH verifier separately proves that any
///          forwarded token is the currently active handler token.
/// @param op Opcode under inspection.
/// @return True when @p op owns branch argument bundles.
bool mayForwardBlockArguments(Opcode op) {
    return op == Opcode::Br || op == Opcode::CBr || op == Opcode::SwitchI32 ||
           op == Opcode::ResumeLabel;
}

/// @brief Determine whether a value is statically known to be `resumetok`.
/// @details Missing temporaries are ignored here because the normal operand
///          definition checks emit clearer diagnostics for that condition.
/// @param value Operand or branch argument to inspect.
/// @param types Current type inference environment.
/// @return True when @p value has the `ResumeTok` type.
bool isResumeTokenValue(const Value &value, TypeInference &types) {
    bool missing = false;
    const Type type = types.valueType(value, &missing);
    return !missing && type.kind == Type::Kind::ResumeTok;
}

/// @brief Enforce the non-ordinary-value rule for `resumetok` operands.
/// @details A resume token may be consumed by `resume.*`, or forwarded through
///          block-argument bundles for EH continuations. It must not be used as
///          a call argument, stored to memory, returned, compared, or otherwise
///          treated as data. `resume.label` consumes its token before entering
///          the destination, so it may not forward a `resumetok` branch argument.
/// @param fn Function containing @p instr.
/// @param bb Block containing @p instr.
/// @param instr Instruction whose value uses are checked.
/// @param blockMap Label map used to inspect branch destination parameters.
/// @param types Type inference environment for operand type queries.
/// @return Success when all token uses are legal; diagnostic otherwise.
Expected<void> verifyResumeTokenValueUses(const Function &fn,
                                          const BasicBlock &bb,
                                          const Instr &instr,
                                          const BlockMap &blockMap,
                                          TypeInference &types) {
    /// @brief Construct the standard resume-token escape diagnostic.
    /// @return Failure anchored to @p instr.
    const auto fail = [&]() -> Expected<void> {
        return Expected<void>{makeVerifierError(
            VerifyDiagCode::EhResumeTokenEscape,
            instr.loc,
            formatInstrDiag(
                fn,
                bb,
                instr,
                "resumetok may only be forwarded as active EH state or consumed by resume.*"))};
    };

    for (std::size_t i = 0; i < instr.operands.size(); ++i) {
        if (!isResumeTokenValue(instr.operands[i], types))
            continue;
        if (isResumeOpcode(instr.op) && i == 0)
            continue;
        return fail();
    }

    if (!mayForwardBlockArguments(instr.op)) {
        for (const auto &bundle : instr.brArgs) {
            for (const Value &arg : bundle) {
                if (isResumeTokenValue(arg, types))
                    return fail();
            }
        }
        return {};
    }

    for (std::size_t edge = 0; edge < instr.brArgs.size(); ++edge) {
        const std::vector<Value> &args = instr.brArgs[edge];
        const BasicBlock *target = nullptr;
        if (edge < instr.labels.size()) {
            auto targetIt = blockMap.find(instr.labels[edge]);
            if (targetIt != blockMap.end())
                target = targetIt->second;
        }
        for (std::size_t argIndex = 0; argIndex < args.size(); ++argIndex) {
            if (!isResumeTokenValue(args[argIndex], types))
                continue;
            if (instr.op == Opcode::ResumeLabel)
                return fail();
            if (!target || argIndex >= target->params.size() ||
                target->params[argIndex].type.kind != Type::Kind::ResumeTok) {
                return fail();
            }
        }
    }

    return {};
}

/// @brief Classify built-in opcodes that can throw or trap directly.
/// @details Direct and indirect calls are checked separately through callee
///          effect metadata.
/// @param op Opcode to classify.
/// @return `true` when executing @p op may produce a language fault or trap.
bool opcodeMayThrowOrTrap(Opcode op) {
    switch (op) {
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf:
        case Opcode::SDiv:
        case Opcode::UDiv:
        case Opcode::SRem:
        case Opcode::URem:
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0:
        case Opcode::IdxChk:
        case Opcode::Alloca:
        case Opcode::Load:
        case Opcode::Store:
        case Opcode::GEP:
        case Opcode::ConstStr:
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk:
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk:
        case Opcode::Trap:
        case Opcode::TrapErr:
        case Opcode::TrapFromErr:
            return true;
        default:
            return false;
    }
}

/// @brief Test whether an opcode is a checked integer binary operation.
/// @param op Opcode to classify.
/// @return `true` for overflow-checking arithmetic and zero-checking division
///         or remainder operations.
bool isCheckedIntegerBinaryOpcode(Opcode op) {
    switch (op) {
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf:
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0:
            return true;
        default:
            return false;
    }
}

/// @brief Test whether an integer kind is supported by checked binary inference.
/// @param kind Type kind to classify.
/// @return `true` for i16, i32, or i64.
bool isSupportedIntegerWidth(Type::Kind kind) {
    return kind == Type::Kind::I16 || kind == Type::Kind::I32 || kind == Type::Kind::I64;
}

/// @brief Effect guarantees known for a direct callee.
struct EffectFacts {
    /// @brief Whether a trustworthy source described the callee.
    bool known = false;

    /// @brief Whether the callee is free of observable side effects.
    bool pure = false;

    /// @brief Whether the callee avoids externally visible memory writes.
    bool readonly = false;

    /// @brief Whether the callee is guaranteed not to throw or trap.
    bool nothrow = false;
};

/// @brief Resolve direct-call effect guarantees from available symbol metadata.
/// @details Consults the runtime registry, module functions, explicitly
///          attributed externs, and helper classification in that order.
///          Unattributed unknown externs produce unknown facts.
/// @param callee Direct-call symbol name.
/// @param functionMap Module function declarations indexed by name.
/// @param externMap Module extern declarations indexed by name.
/// @return Resolved effect facts, with `known` false when no authoritative
///         guarantees are available.
EffectFacts directCalleeEffects(
    std::string_view callee,
    const std::unordered_map<std::string, const Function *> &functionMap,
    const FunctionVerifier::ExternMap &externMap) {
    if (const auto *runtimeSig = il::runtime::findRuntimeSignature(callee)) {
        return EffectFacts{true, runtimeSig->pure, runtimeSig->readonly, runtimeSig->nothrow};
    }

    if (auto it = functionMap.find(std::string(callee)); it != functionMap.end()) {
        const FunctionAttrs &attrs = it->second->attrs();
        return EffectFacts{true, attrs.pure, attrs.readonly, attrs.nothrow};
    }

    if (auto it = externMap.find(std::string(callee)); it != externMap.end()) {
        const auto &attrs = it->second->attrs();
        if (attrs.pure || attrs.readonly || attrs.nothrow)
            return EffectFacts{true, attrs.pure, attrs.readonly, attrs.nothrow};
    }

    if (auto helper = il::runtime::classifyHelperEffects(callee); helper.known)
        return EffectFacts{true, helper.pure, helper.readonly, helper.nothrow};

    return {};
}

/// @brief Canonical byte location rooted at an `alloca` temporary.
struct StackLocationKey {
    /// @brief Temporary identifier produced by the root `alloca`.
    unsigned root = 0;

    /// @brief Constant byte offset accumulated through GEP instructions.
    int64_t offset = 0;

    /// @brief Compare stack locations by root allocation and byte offset.
    /// @param other Location to compare.
    /// @return `true` when both components match.
    bool operator==(const StackLocationKey &other) const {
        return root == other.root && offset == other.offset;
    }
};

/// @brief Hash functor for canonical stack locations.
struct StackLocationKeyHash {
    /// @brief Combine hashes of an allocation root and constant offset.
    /// @param key Stack location to hash.
    /// @return Hash suitable for `std::unordered_set`.
    std::size_t operator()(const StackLocationKey &key) const {
        std::size_t rootHash = std::hash<unsigned>{}(key.root);
        std::size_t offsetHash = std::hash<int64_t>{}(key.offset);
        return rootHash ^ (offsetHash + 0x9e3779b97f4a7c15ULL + (rootHash << 6) + (rootHash >> 2));
    }
};

/// @brief Extract a signed integer literal from a value.
/// @param value Value to inspect.
/// @return Literal payload for `ConstInt`, or no value for other kinds.
std::optional<int64_t> constIntValue(const Value &value) {
    if (value.kind == Value::Kind::ConstInt)
        return value.i64;
    return std::nullopt;
}

/// @brief Add two signed offsets without invoking overflow.
/// @param lhs Base offset.
/// @param rhs Increment.
/// @return Sum when representable as int64_t; otherwise no value.
std::optional<int64_t> checkedAdd(int64_t lhs, int64_t rhs) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs))
        return std::nullopt;
    return lhs + rhs;
}

/// @brief Resolve a pointer to an alloca root and constant GEP offset.
/// @details Recursively follows temporary definitions through GEP chains and
///          abandons paths with missing definitions, nonconstant offsets,
///          signed overflow, or more than 32 links.
/// @param ptr Pointer value to resolve.
/// @param defs Instruction definition map for temporary values.
/// @param depth Current recursion depth used to cap malformed chains.
/// @return Canonical stack location, or no value when resolution is unsafe.
std::optional<StackLocationKey> stackLocationKey(
    const Value &ptr, const std::unordered_map<unsigned, const Instr *> &defs, unsigned depth = 0) {
    if (depth > 32 || ptr.kind != Value::Kind::Temp)
        return std::nullopt;

    auto defIt = defs.find(ptr.id);
    if (defIt == defs.end())
        return std::nullopt;

    const Instr &def = *defIt->second;
    if (def.op == Opcode::Alloca)
        return StackLocationKey{ptr.id, 0};

    if (def.op != Opcode::GEP || def.operands.size() < 2)
        return std::nullopt;

    auto base = stackLocationKey(def.operands[0], defs, depth + 1);
    if (!base)
        return std::nullopt;

    auto offset = constIntValue(def.operands[1]);
    if (!offset)
        return std::nullopt;

    auto combined = checkedAdd(base->offset, *offset);
    if (!combined)
        return std::nullopt;

    base->offset = *combined;
    return base;
}

/// @brief Compute the fixed point of temporaries derived from stack allocations.
/// @details Seeds `alloca` results, propagates through GEP, tracks stack slots
///          that store derived pointers and pointer loads from those slots, and
///          forwards derived values into resolved successor block parameters.
/// @param fn Function whose definitions and uses are scanned.
/// @param blockMap Resolved labels used to pair branch arguments with params.
/// @return Set of temporary identifiers that may hold an alloca-derived pointer.
std::unordered_set<unsigned> computeStackDerivedTemps(const Function &fn,
                                                      const BlockMap &blockMap) {
    std::unordered_map<unsigned, const Instr *> defs;
    std::unordered_set<unsigned> stackDerived;
    std::unordered_set<StackLocationKey, StackLocationKeyHash> stackPtrLocations;

    for (const auto &blk : fn.blocks) {
        for (const auto &instr : blk.instructions) {
            if (instr.result)
                defs.emplace(*instr.result, &instr);
            if (instr.op == Opcode::Alloca && instr.result)
                stackDerived.insert(*instr.result);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &instr : blk.instructions) {
                if (instr.op == Opcode::GEP && instr.result && !instr.operands.empty() &&
                    instr.operands[0].kind == Value::Kind::Temp &&
                    stackDerived.contains(instr.operands[0].id)) {
                    changed |= stackDerived.insert(*instr.result).second;
                }

                if (instr.op == Opcode::Store && instr.operands.size() >= 2) {
                    const Value &dst = instr.operands[0];
                    const Value &stored = instr.operands[1];
                    if (stored.kind == Value::Kind::Temp && stackDerived.contains(stored.id)) {
                        if (auto key = stackLocationKey(dst, defs))
                            changed |= stackPtrLocations.insert(*key).second;
                    }
                }

                if (instr.op == Opcode::Load && instr.result && !instr.operands.empty() &&
                    instr.type.kind == Type::Kind::Ptr) {
                    if (auto key = stackLocationKey(instr.operands[0], defs);
                        key && stackPtrLocations.contains(*key)) {
                        changed |= stackDerived.insert(*instr.result).second;
                    }
                }

                if (isTerminator(instr.op)) {
                    for (size_t edge = 0; edge < instr.labels.size() && edge < instr.brArgs.size();
                         ++edge) {
                        auto targetIt = blockMap.find(instr.labels[edge]);
                        if (targetIt == blockMap.end())
                            continue;
                        const BasicBlock *target = targetIt->second;
                        const auto &args = instr.brArgs[edge];
                        const size_t count = std::min(args.size(), target->params.size());
                        for (size_t idx = 0; idx < count; ++idx) {
                            const Value &arg = args[idx];
                            if (arg.kind == Value::Kind::Temp && stackDerived.contains(arg.id))
                                changed |= stackDerived.insert(target->params[idx].id).second;
                        }
                    }
                }
            }
        }
    }

    return stackDerived;
}

/// @brief Determine whether a memory operation touches private stack-derived storage.
/// @param instr Instruction to classify.
/// @param stackDerived Known alloca-derived pointer temporaries.
/// @return `true` for `alloca` and for loads or stores through a derived pointer.
bool isPrivateStackMemoryAccess(const Instr &instr,
                                const std::unordered_set<unsigned> &stackDerived) {
    if (instr.op == Opcode::Alloca)
        return true;
    if ((instr.op == Opcode::Load || instr.op == Opcode::Store) && !instr.operands.empty()) {
        const Value &ptr = instr.operands[0];
        return ptr.kind == Value::Kind::Temp && stackDerived.contains(ptr.id);
    }
    return false;
}

/// @brief Validate names, IDs, and types of function parameters.
/// @param fn Function whose parameter declarations are inspected.
/// @return Success, or a diagnostic for the first malformed/duplicate name,
///         duplicate identifier, or void type.
Expected<void> validateFunctionParams(const Function &fn) {
    std::unordered_set<std::string> paramNames;
    std::unordered_set<unsigned> paramIds;
    for (const auto &param : fn.params) {
        if (!il::io::isValidILIdentifier(param.name))
            return Expected<void>{makeError({}, fn.name + ": malformed param name %" + param.name)};

        if (!paramNames.insert(param.name).second)
            return Expected<void>{makeError({}, fn.name + ": duplicate param %" + param.name)};

        if (!paramIds.insert(param.id).second) {
            std::ostringstream message;
            message << fn.name << ": duplicate param id %" << param.id;
            return Expected<void>{makeError({}, message.str())};
        }

        if (param.type.kind == Type::Kind::Void)
            return Expected<void>{
                makeError({}, fn.name + ": param %" + param.name + " has void type")};
    }
    return {};
}

/// @brief Compare an extern signature with a function declaration.
/// @param ext Extern declaration.
/// @param fn Function declaration with the same symbol.
/// @return `true` when return kind, arity, and parameter kinds match.
bool signaturesMatch(const Extern &ext, const Function &fn) {
    if (ext.retType.kind != fn.retType.kind || ext.params.size() != fn.params.size())
        return false;
    for (size_t i = 0; i < ext.params.size(); ++i)
        if (ext.params[i].kind != fn.params[i].type.kind)
            return false;
    return true;
}

} // namespace

/// @brief Structured block-parameter validator implemented by the CFG checker.
Expected<void> validateBlockParams_E(const Function &fn,
                                     const BasicBlock &bb,
                                     TypeInference &types,
                                     std::vector<unsigned> &paramIds);

/// @brief Structured terminator-placement validator implemented by the CFG checker.
Expected<void> checkBlockTerminators_E(const Function &fn, const BasicBlock &bb);

/// @brief Validate registry-defined operand and result signature constraints.
Expected<void> verifyOpcodeSignature_E(const VerifyCtx &ctx);

/// @brief Legacy structured instruction checker used by strategy adapters.
Expected<void> verifyInstruction_E(const Function &fn,
                                   const BasicBlock &bb,
                                   const Instr &instr,
                                   const std::unordered_map<std::string, const Extern *> &externs,
                                   const std::unordered_map<std::string, const Function *> &funcs,
                                   TypeInference &types,
                                   DiagSink &sink);

/// @brief Construct a verifier with knowledge of extern signatures.
///
/// @details The extern map is cached so that call instructions can be checked
///          against known signatures.  Instruction strategies are seeded with
///          the default collection used to validate every opcode.
///
/// @param externs Map from extern names to their declarations.
/// @param globals Map from global names to their declarations.
FunctionVerifier::FunctionVerifier(const ExternMap &externs, const GlobalMap &globals)
    : externs_(externs), globals_(globals), strategies_(makeDefaultInstructionStrategies()) {}

/// @brief Verify every function in a module for structural correctness.
///
/// @details Builds a name-to-function map to detect duplicates before invoking
///          @ref verifyFunction on each function. Function-body verification
///          keeps scanning independent functions so tooling can report several
///          actionable failures from one verifier run.
///
/// @param module Module containing functions to verify.
/// @param sink Diagnostic sink receiving instruction-level messages.
/// @return Empty Expected on success or the first failure diagnostic.
Expected<void> FunctionVerifier::run(const Module &module, DiagSink &sink) {
    functionMap_.clear();

    for (const auto &fn : module.functions) {
        if (fn.name.empty() || !il::io::isValidILIdentifier(fn.name))
            return Expected<void>{makeError({}, "malformed function @" + fn.name)};
        if (!functionMap_.emplace(fn.name, &fn).second)
            return Expected<void>{makeError({}, "duplicate function @" + fn.name)};
    }

    std::unordered_set<std::string> globalNames;
    for (const auto &global : module.globals) {
        globalNames.insert(global.name);
        if (externs_.contains(global.name))
            return Expected<void>{
                makeError({}, "global @" + global.name + " collides with extern @" + global.name)};
    }

    for (const auto &fn : module.functions) {
        if (globalNames.contains(fn.name))
            return Expected<void>{
                makeError({}, "function @" + fn.name + " collides with global @" + fn.name)};
    }

    std::optional<Diag> firstFailure;
    for (const auto &fn : module.functions) {
        auto result = verifyFunction(fn, sink);
        if (result)
            continue;

        Diag diag = result.error();
        if (!firstFailure)
            firstFailure = diag;
        sink.report(std::move(diag));
    }

    if (firstFailure)
        return Expected<void>{std::move(*firstFailure)};

    return {};
}

/// @brief Validate a single function's blocks, labels, and handler metadata.
///
/// @details Ensures the first block is an entry block, checks for extern
///          signature parity, records handler signatures, and validates that all
///          referenced labels exist. Block-level checks are delegated to
///          @ref verifyBlock.
///
/// @param fn Function being verified.
/// @param sink Diagnostic sink for detailed messages.
/// @return Success or a diagnostic describing the first failure.
Expected<void> FunctionVerifier::verifyFunction(const Function &fn, DiagSink &sink) {
    if (auto result = validateFunctionParams(fn); !result)
        return result;

    if (fn.moduleInitializer && (fn.linkage == Linkage::Import || fn.isVarArg ||
                                 !fn.params.empty() || fn.retType.kind != Type::Kind::Void)) {
        return Expected<void>{makeError(
            {},
            formatFunctionDiag(fn, "module initializer must be a non-import () -> void function"))};
    }

    if (auto it = externs_.find(fn.name); it != externs_.end()) {
        const Extern *ext = it->second;
        if (!signaturesMatch(*ext, fn))
            return Expected<void>{
                makeError({}, "function @" + fn.name + " signature mismatch with extern")};
        if (fn.linkage != Linkage::Import)
            return Expected<void>{
                makeError({}, "function @" + fn.name + " collides with extern @" + fn.name)};
    }

    // Import-linkage functions are declarations with no body; skip body verification.
    if (fn.linkage == Linkage::Import) {
        if (!fn.blocks.empty())
            return Expected<void>{
                makeError({}, formatFunctionDiag(fn, "import function must not have a body"))};
        return {};
    }

    if (fn.blocks.empty())
        return Expected<void>{makeError({}, formatFunctionDiag(fn, "function has no blocks"))};

    const std::string &firstLabel = fn.blocks.front().label;
    const bool isEntry = firstLabel == "entry" || firstLabel.rfind("entry_", 0) == 0;
    if (!isEntry)
        return Expected<void>{makeError({}, formatFunctionDiag(fn, "first block must be entry"))};

    std::unordered_set<std::string> labels;
    BlockMap blockMap;
    blockMap.reserve(fn.blocks.size());
    for (const auto &bb : fn.blocks) {
        if (bb.label.empty() || !il::io::isValidILIdentifier(bb.label))
            return Expected<void>{
                makeError({}, formatFunctionDiag(fn, "malformed label " + bb.label))};
        if (!labels.insert(bb.label).second)
            return Expected<void>{
                makeError({}, formatFunctionDiag(fn, "duplicate label " + bb.label))};
        // Use string_view key referencing bb.label; the Function outlives this map.
        blockMap.emplace(std::string_view{bb.label}, &bb);
    }

    for (const auto &bb : fn.blocks) {
        if (auto result = checkBlockTerminators_E(fn, bb); !result)
            return result;
    }

    handlerInfo_.clear();

    std::unordered_map<unsigned, Type> temps;
    std::unordered_map<unsigned, std::string> tempDefinitions;
    /// @brief Register one SSA definition while enforcing global ID uniqueness.
    /// @param id Temporary identifier being defined.
    /// @param type Type associated with the definition.
    /// @param description Human-readable origin retained for duplicate errors.
    /// @return Success when inserted, or a duplicate-definition diagnostic.
    auto defineTemp = [&](unsigned id, Type type, std::string description) -> Expected<void> {
        auto [it, inserted] = tempDefinitions.emplace(id, std::move(description));
        if (!inserted) {
            std::ostringstream message;
            message << "duplicate temp %" << id << " defined by " << it->second;
            return Expected<void>{makeError({}, formatFunctionDiag(fn, message.str()))};
        }
        temps[id] = type;
        return {};
    };

    std::unordered_map<unsigned, const Param *> functionParamsById;
    functionParamsById.reserve(fn.params.size());
    for (const auto &param : fn.params) {
        functionParamsById.emplace(param.id, &param);
        if (auto result = defineTemp(param.id, param.type, "function param %" + param.name);
            !result)
            return result;
    }

    /// @brief Recognize an entry block parameter that aliases a function parameter.
    /// @details The alias is accepted only when ID and type kind match.
    /// @param bb Block owning @p param.
    /// @param param Candidate block parameter.
    /// @return `true` when no second SSA definition should be registered.
    auto isEntryFunctionParamAlias = [&](const BasicBlock &bb, const Param &param) {
        if (&bb != &fn.blocks.front())
            return false;
        auto it = functionParamsById.find(param.id);
        if (it == functionParamsById.end())
            return false;
        const Param &functionParam = *it->second;
        return param.type.kind == functionParam.type.kind;
    };

    // ===== PASS 1: Pre-collect all definitions for type information =====
    // This is necessary because SimplifyCFG and other transforms may reorder blocks
    // such that definitions appear later in declaration order but still dominate uses.
    // By collecting all definitions first, we have complete type information for
    // cross-block operand references.
    //
    // We also track which block each definition comes from so that verifyBlock can
    // still detect within-block use-before-def errors.
    std::unordered_map<unsigned, const BasicBlock *> definingBlock;
    /// @brief Infer the type registered during result precollection.
    /// @details Checked integer operations lacking an explicit type infer a
    ///          consistent supported operand width or conservatively use i64.
    ///          Direct and signature-bearing indirect calls use callee return
    ///          metadata; all other instructions use their stored result type.
    /// @param instr Result-producing instruction to inspect.
    /// @return Type to associate with the precollected result identifier.
    auto precollectedResultType = [&](const Instr &instr) {
        if (!instr.result)
            return instr.type;
        if (isCheckedIntegerBinaryOpcode(instr.op) && instr.type.kind == Type::Kind::Void) {
            std::optional<Type::Kind> inferred;
            for (const Value &operand : instr.operands) {
                if (operand.kind != Value::Kind::Temp)
                    continue;
                auto tempIt = temps.find(operand.id);
                if (tempIt == temps.end() || !isSupportedIntegerWidth(tempIt->second.kind))
                    return Type(Type::Kind::I64);
                if (inferred && *inferred != tempIt->second.kind)
                    return Type(Type::Kind::I64);
                inferred = tempIt->second.kind;
            }
            return Type(inferred.value_or(Type::Kind::I64));
        }
        if (instr.op == Opcode::Call) {
            if (auto extIt = externs_.find(instr.callee); extIt != externs_.end())
                return extIt->second->retType;
            if (auto fnIt = functionMap_.find(instr.callee); fnIt != functionMap_.end())
                return fnIt->second->retType;
            if (const auto *runtimeSig = il::runtime::findRuntimeSignature(instr.callee))
                return runtimeSig->retType;
        }
        if (instr.op == Opcode::CallIndirect && instr.hasIndirectSignature)
            return instr.indirectRetType;
        return instr.type;
    };

    for (const auto &bb : fn.blocks) {
        // Block parameters define temporaries
        for (const auto &param : bb.params) {
            if (isEntryFunctionParamAlias(bb, param))
                continue;
            if (auto result = defineTemp(
                    param.id, param.type, "block param %" + param.name + " in ^" + bb.label);
                !result)
                return result;
            definingBlock[param.id] = &bb;
        }

        // Instructions with results define temporaries
        for (const auto &instr : bb.instructions) {
            if (instr.result.has_value()) {
                std::ostringstream description;
                description << "instruction result in ^" << bb.label;
                if (auto result =
                        defineTemp(*instr.result, precollectedResultType(instr), description.str());
                    !result)
                    return result;
                definingBlock[*instr.result] = &bb;
            }
        }
    }

    // ===== PASS 2: Full verification with complete type info =====
    // Collect EhPush targets and label references during single pass over blocks.
    // This avoids two additional O(blocks × instructions) traversals.
    /// @brief Deferred validation record for one labelled `eh.push`.
    struct EhPushCheck {
        /// @brief Block containing the push.
        const BasicBlock *bb{nullptr};

        /// @brief Concrete push instruction.
        const Instr *instr{nullptr};

        /// @brief Referenced handler label.
        std::string target;
    };

    std::vector<EhPushCheck> ehPushChecks;
    std::vector<std::string> labelRefs;

    for (const auto &bb : fn.blocks) {
        if (auto result = verifyBlock(fn, bb, blockMap, temps, definingBlock, sink); !result)
            return result;

        // Collect EhPush targets and all label references in single pass
        for (const auto &instr : bb.instructions) {
            if (instr.op == Opcode::EhPush && !instr.labels.empty())
                ehPushChecks.push_back({&bb, &instr, instr.labels.front()});

            for (const auto &label : instr.labels)
                labelRefs.push_back(label);
        }
    }

    // Validate EhPush targets exist in handlerInfo_ (populated during verifyBlock)
    for (const auto &check : ehPushChecks) {
        if (handlerInfo_.find(check.target) == handlerInfo_.end()) {
            std::ostringstream message;
            message << "eh.push target ^" << check.target << " must name a handler block";
            return Expected<void>{makeError(
                check.instr->loc, formatInstrDiag(fn, *check.bb, *check.instr, message.str()))};
        }
    }

    // Validate all label references exist
    for (const auto &label : labelRefs) {
        if (!labels.contains(label))
            return Expected<void>{makeError({}, formatFunctionDiag(fn, "unknown label " + label))};
    }

    // ===== PASS 2b: Function attribute body verification =====
    // Function-level effect attributes are consumed by optimisation passes, so
    // definitions must prove their own annotations instead of relying on call
    // sites or hand-written metadata being honest.
    if (fn.attrs().pure || fn.attrs().readonly || fn.attrs().nothrow) {
        const FunctionAttrs &attrs = fn.attrs();
        const auto stackDerived = computeStackDerivedTemps(fn, blockMap);
        for (const auto &bb : fn.blocks) {
            for (const auto &instr : bb.instructions) {
                /// @brief Construct an effect-attribute diagnostic at @p instr.
                /// @param message Attribute violation text.
                /// @return Failure anchored to the current instruction.
                const auto failAttr = [&](std::string_view message) -> Expected<void> {
                    return Expected<void>{
                        makeError(instr.loc, formatInstrDiag(fn, bb, instr, message))};
                };

                if (instr.op == Opcode::Call) {
                    const EffectFacts effects =
                        directCalleeEffects(instr.callee, functionMap_, externs_);
                    if (attrs.pure && !(effects.known && effects.pure))
                        return failAttr("pure function calls non-pure callee");
                    if (attrs.readonly && !(effects.known && (effects.readonly || effects.pure)))
                        return failAttr("readonly function calls memory-writing callee");
                    if (attrs.nothrow && !(effects.known && effects.nothrow))
                        return failAttr("nothrow function calls throwing callee");
                    continue;
                }

                if (instr.op == Opcode::CallIndirect) {
                    if (attrs.pure)
                        return failAttr("pure function contains indirect call");
                    if (attrs.readonly)
                        return failAttr("readonly function contains indirect call");
                    if (attrs.nothrow)
                        return failAttr("nothrow function contains indirect call");
                    continue;
                }

                const MemoryEffects memory = memoryEffects(instr.op);
                const bool privateStackAccess = isPrivateStackMemoryAccess(instr, stackDerived);
                if (attrs.pure && memory != MemoryEffects::None && !privateStackAccess)
                    return failAttr("pure function contains memory access");
                if (attrs.readonly &&
                    (memory == MemoryEffects::Write || memory == MemoryEffects::ReadWrite ||
                     memory == MemoryEffects::Unknown) &&
                    !privateStackAccess)
                    return failAttr("readonly function contains memory write");

                const OpcodeInfo &info = getOpcodeInfo(instr.op);
                if (attrs.pure && info.hasSideEffects && !isPureControlTerminator(instr.op) &&
                    !privateStackAccess)
                    return failAttr("pure function contains side-effecting instruction");
                if (attrs.nothrow && opcodeMayThrowOrTrap(instr.op))
                    return failAttr("nothrow function contains trapping instruction");
            }
        }
    }

    if (auto result = verifyDominanceAndEscapes(fn, blockMap, temps, definingBlock); !result)
        return result;

    return {};
}

/// @brief Verify cross-block SSA dominance, release lifetime, and stack escapes.
/// @details Builds component-aware dominator sets so unreachable CFG components
///          are checked independently, validates managed-handle release ordering
///          both by dominance and forward data flow, and rejects alloca-derived
///          pointers returned, stored outside stack storage, or passed through
///          indirect calls.
/// @param fn Function whose CFG and value uses are analysed.
/// @param blockMap Resolved label map used to construct CFG edges.
/// @param temps Precollected type for every known temporary.
/// @param definingBlock Defining block for instruction and block-param values.
/// @return Success when all three analyses pass, or the first diagnostic.
Expected<void> FunctionVerifier::verifyDominanceAndEscapes(
    const Function &fn,
    const BlockMap &blockMap,
    const std::unordered_map<unsigned, Type> &temps,
    const std::unordered_map<unsigned, const BasicBlock *> &definingBlock) {
    // ===== PASS 3: Dominance verification =====
    // Compute dominators using iterative dataflow (Cooper-Harvey-Kennedy) and
    // verify that every use of a temp is dominated by its definition.
    {
        const BasicBlock *entry = &fn.blocks.front();

        // Build CFG predecessor map from the block map.
        std::unordered_map<const BasicBlock *, std::vector<const BasicBlock *>> preds;
        for (const auto &blk : fn.blocks) {
            for (const auto &instr : blk.instructions) {
                if (!isTerminator(instr.op))
                    continue;
                for (const auto &label : instr.labels) {
                    if (auto it = blockMap.find(label); it != blockMap.end())
                        preds[it->second].push_back(&blk);
                }
                break;
            }
        }

        // Compute reverse post-order via DFS over every CFG component. The real
        // entry remains the first root; unreachable components get independent
        // roots so malformed SSA inside dead code is still checked.
        std::vector<const BasicBlock *> rpo;
        std::vector<const BasicBlock *> roots;
        std::unordered_set<const BasicBlock *> entryReachable;
        {
            std::unordered_set<const BasicBlock *> visited;

            /// @brief Explicit DFS frame used to avoid native recursion.
            struct Frame {
                /// @brief Block whose successors are visited.
                const BasicBlock *bb;

                /// @brief Whether successor frames have already been pushed.
                bool childrenPushed;
            };

            /// @brief Traverse one not-yet-visited CFG component in postorder.
            /// @details Records the component root, uses an explicit DFS stack,
            ///          and appends that component's reverse postorder to @ref rpo.
            /// @param root Candidate component root.
            auto visitRoot = [&](const BasicBlock *root) {
                if (!root || !visited.insert(root).second)
                    return;
                roots.push_back(root);
                std::vector<const BasicBlock *> componentPostorder;
                std::vector<Frame> stack;
                stack.push_back({root, false});
                while (!stack.empty()) {
                    auto &top = stack.back();
                    if (!top.childrenPushed) {
                        top.childrenPushed = true;
                        for (const auto &instr : top.bb->instructions) {
                            if (!isTerminator(instr.op))
                                continue;
                            for (const auto &label : instr.labels) {
                                if (auto it = blockMap.find(label); it != blockMap.end()) {
                                    if (visited.insert(it->second).second)
                                        stack.push_back({it->second, false});
                                }
                            }
                            break;
                        }
                    } else {
                        componentPostorder.push_back(top.bb);
                        stack.pop_back();
                    }
                }
                rpo.insert(rpo.end(), componentPostorder.rbegin(), componentPostorder.rend());
            };

            visitRoot(entry);
            entryReachable = visited;
            for (const auto &bb : fn.blocks) {
                if (visited.contains(&bb))
                    continue;
                bool hasUnvisitedPred = false;
                if (auto predIt = preds.find(&bb); predIt != preds.end()) {
                    for (const auto *pred : predIt->second) {
                        if (!visited.contains(pred)) {
                            hasUnvisitedPred = true;
                            break;
                        }
                    }
                }
                if (!hasUnvisitedPred)
                    visitRoot(&bb);
            }
            for (const auto &bb : fn.blocks) {
                if (!visited.contains(&bb))
                    visitRoot(&bb);
            }
        }

        std::unordered_set<const BasicBlock *> rootSet(roots.begin(), roots.end());

        std::vector<const BasicBlock *> blocks;
        blocks.reserve(fn.blocks.size());
        std::unordered_map<const BasicBlock *, size_t> blockIndex;
        for (const auto &bb : fn.blocks) {
            blockIndex[&bb] = blocks.size();
            blocks.push_back(&bb);
        }

        std::vector<std::vector<size_t>> predIndices(blocks.size());
        for (const auto &[bb, predList] : preds) {
            auto bbIt = blockIndex.find(bb);
            if (bbIt == blockIndex.end())
                continue;
            auto &indices = predIndices[bbIt->second];
            indices.reserve(predList.size());
            const bool targetReachable = entryReachable.contains(bb);
            for (const auto *pred : predList) {
                if (entryReachable.contains(pred) != targetReachable)
                    continue;
                auto predIdx = blockIndex.find(pred);
                if (predIdx != blockIndex.end())
                    indices.push_back(predIdx->second);
            }
        }

        std::vector<std::vector<unsigned char>> dom(blocks.size(),
                                                    std::vector<unsigned char>(blocks.size(), 1));

        /// @brief Initialize one component root to dominate only itself.
        /// @param index Dense block index of the root.
        auto setRootDom = [&](size_t index) {
            std::fill(dom[index].begin(), dom[index].end(), 0);
            dom[index][index] = 1;
        };

        for (const auto *root : roots) {
            if (auto it = blockIndex.find(root); it != blockIndex.end())
                setRootDom(it->second);
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto *bb : rpo) {
                if (rootSet.contains(bb))
                    continue;

                const size_t index = blockIndex[bb];
                if (predIndices[index].empty()) {
                    std::vector<unsigned char> newDom(blocks.size(), 0);
                    newDom[index] = 1;
                    if (dom[index] != newDom) {
                        dom[index] = std::move(newDom);
                        changed = true;
                    }
                    continue;
                }

                std::vector<unsigned char> newDom(blocks.size(), 1);
                for (size_t pred : predIndices[index]) {
                    for (size_t bit = 0; bit < newDom.size(); ++bit)
                        newDom[bit] = newDom[bit] && dom[pred][bit];
                }
                newDom[index] = 1;

                if (dom[index] != newDom) {
                    dom[index] = std::move(newDom);
                    changed = true;
                }
            }
        }

        /// @brief Query the converged dense dominance relation.
        /// @param A Candidate dominating block.
        /// @param B Candidate dominated block.
        /// @return `true` when both are indexed and A dominates B.
        auto dominates = [&](const BasicBlock *A, const BasicBlock *B) -> bool {
            auto aIt = blockIndex.find(A);
            auto bIt = blockIndex.find(B);
            if (aIt == blockIndex.end() || bIt == blockIndex.end())
                return false;
            return dom[bIt->second][aIt->second] != 0;
        };

        const auto stackDerivedForDominance = computeStackDerivedTemps(fn, blockMap);
        /// @brief Identify an entry-defined stack pointer exempted from CFG checks.
        /// @details Entry allocation addresses are treated as function-scoped
        ///          values even when referenced by separately rooted dead CFG
        ///          components.
        /// @param op Operand to classify.
        /// @return `true` for a pointer-typed stack-derived temp defined in entry.
        auto isEntryStackAddress = [&](const Value &op) {
            if (op.kind != Value::Kind::Temp || !stackDerivedForDominance.contains(op.id))
                return false;
            auto defIt = definingBlock.find(op.id);
            auto typeIt = temps.find(op.id);
            return defIt != definingBlock.end() && defIt->second == entry &&
                   typeIt != temps.end() && typeIt->second.kind == Type::Kind::Ptr;
        };

        // Check every operand use: the defining block must dominate the using block.
        for (const auto &blk : fn.blocks) {
            for (const auto &instr : blk.instructions) {
                /// @brief Check dominance for one temporary use at @p instr.
                /// @param op Operand or branch argument to inspect.
                /// @return Success for non-temporaries, untracked values, and
                ///         dominated uses; otherwise a diagnostic.
                auto checkValue = [&](const Value &op) -> Expected<void> {
                    if (op.kind != Value::Kind::Temp)
                        return {};
                    auto defIt = definingBlock.find(op.id);
                    if (defIt == definingBlock.end())
                        return {};

                    if (defIt->second != &blk && !isEntryStackAddress(op) &&
                        !dominates(defIt->second, &blk)) {
                        std::ostringstream msg;
                        msg << "use of %" << op.id << " in ^" << blk.label
                            << " not dominated by definition in ^" << defIt->second->label;
                        return Expected<void>{
                            makeError(instr.loc, formatInstrDiag(fn, blk, instr, msg.str()))};
                    }
                    return {};
                };

                for (const auto &op : instr.operands)
                    if (auto result = checkValue(op); !result)
                        return result;
                for (const auto &bundle : instr.brArgs)
                    for (const auto &arg : bundle)
                        if (auto result = checkValue(arg); !result)
                            return result;
            }
        }

        /// @brief One direct runtime release of a managed temporary.
        struct ReleaseSite {
            /// @brief Block containing the release.
            const BasicBlock *block;

            /// @brief Release call instruction.
            const Instr *instr;

            /// @brief Instruction index within @ref block.
            size_t index;

            /// @brief Released temporary identifier.
            unsigned id;
        };

        /// @brief One non-finalizer use that may follow a release.
        struct ReleasedUse {
            /// @brief Block containing the use.
            const BasicBlock *block;

            /// @brief Instruction consuming the value.
            const Instr *instr;

            /// @brief Instruction index within @ref block.
            size_t index;

            /// @brief Consumed temporary identifier.
            unsigned id;
        };

        /// @brief One runtime retain that restores a released handle's lifetime.
        struct RetainSite {
            /// @brief Block containing the retain.
            const BasicBlock *block;

            /// @brief Retain call instruction.
            const Instr *instr;

            /// @brief Instruction index within @ref block.
            size_t index;

            /// @brief Retained temporary identifier.
            unsigned id;
        };

        std::vector<ReleaseSite> releaseSites;
        std::vector<ReleasedUse> releasedUses;
        std::vector<RetainSite> retainSites;
        for (const auto &blk : fn.blocks) {
            for (size_t index = 0; index < blk.instructions.size(); ++index) {
                const Instr &instr = blk.instructions[index];
                if (isRuntimeExplicitRelease(instr)) {
                    if (!instr.operands.empty() && instr.operands[0].kind == Value::Kind::Temp)
                        releaseSites.push_back({&blk, &instr, index, instr.operands[0].id});
                    continue;
                }
                if (isRuntimeArrayRetain(instr) && !instr.operands.empty() &&
                    instr.operands[0].kind == Value::Kind::Temp) {
                    retainSites.push_back({&blk, &instr, index, instr.operands[0].id});
                }

                if (isRuntimeObjectFinalizerCall(instr))
                    continue;

                /// @brief Record one temporary-valued use for release analysis.
                /// @param value Operand or branch argument to inspect.
                auto recordUse = [&](const Value &value) {
                    if (value.kind == Value::Kind::Temp)
                        releasedUses.push_back({&blk, &instr, index, value.id});
                };
                for (const auto &operand : instr.operands)
                    recordUse(operand);
                for (const auto &bundle : instr.brArgs)
                    for (const auto &argument : bundle)
                        recordUse(argument);
            }
        }

        /// @brief Test whether one instruction site strictly precedes another.
        /// @param releaseBlock Block containing the earlier candidate.
        /// @param releaseIndex Index of the earlier candidate.
        /// @param useBlock Block containing the later candidate.
        /// @param useIndex Index of the later candidate.
        /// @return Same-block order or cross-block dominance result.
        auto siteDominates = [&](const BasicBlock *releaseBlock,
                                 size_t releaseIndex,
                                 const BasicBlock *useBlock,
                                 size_t useIndex) {
            if (releaseBlock == useBlock)
                return releaseIndex < useIndex;
            return dominates(releaseBlock, useBlock);
        };
        /// @brief Find a retain of @p id between two ordered instruction sites.
        /// @param id Managed temporary identifier.
        /// @param releaseBlock Block containing the release.
        /// @param releaseIndex Release instruction index.
        /// @param useBlock Block containing the later use or release.
        /// @param useIndex Later instruction index.
        /// @return `true` when a retain is dominated by the release and itself
        ///         dominates the later site.
        auto retainBetween = [&](unsigned id,
                                 const BasicBlock *releaseBlock,
                                 size_t releaseIndex,
                                 const BasicBlock *useBlock,
                                 size_t useIndex) {
            for (const RetainSite &retain : retainSites) {
                if (retain.id != id)
                    continue;
                if (!siteDominates(releaseBlock, releaseIndex, retain.block, retain.index))
                    continue;
                if (!siteDominates(retain.block, retain.index, useBlock, useIndex))
                    continue;
                return true;
            }
            return false;
        };

        for (size_t i = 0; i < releaseSites.size(); ++i) {
            const ReleaseSite &first = releaseSites[i];
            for (size_t j = 0; j < releaseSites.size(); ++j) {
                if (i == j)
                    continue;
                const ReleaseSite &second = releaseSites[j];
                if (first.id != second.id)
                    continue;
                if (!siteDominates(first.block, first.index, second.block, second.index))
                    continue;
                if (retainBetween(first.id, first.block, first.index, second.block, second.index))
                    continue;

                std::ostringstream message;
                message << "double release of %" << second.id;
                return Expected<void>{
                    makeError(second.instr->loc,
                              formatInstrDiag(fn, *second.block, *second.instr, message.str()))};
            }

            for (const ReleasedUse &use : releasedUses) {
                if (first.id != use.id)
                    continue;
                if (!siteDominates(first.block, first.index, use.block, use.index))
                    continue;
                if (retainBetween(first.id, first.block, first.index, use.block, use.index))
                    continue;

                std::ostringstream message;
                message << "use after release of %" << use.id;
                return Expected<void>{makeError(
                    use.instr->loc, formatInstrDiag(fn, *use.block, *use.instr, message.str()))};
            }
        }

        std::vector<std::vector<size_t>> successors(blocks.size());
        for (size_t index = 0; index < blocks.size(); ++index) {
            const BasicBlock *block = blocks[index];
            for (const Instr &instr : block->instructions) {
                if (!isTerminator(instr.op))
                    continue;
                for (const std::string &label : instr.labels) {
                    auto blockIt = blockMap.find(label);
                    if (blockIt == blockMap.end())
                        continue;
                    auto indexIt = blockIndex.find(blockIt->second);
                    if (indexIt != blockIndex.end())
                        successors[index].push_back(indexIt->second);
                }
                break;
            }
        }

        std::vector<std::unordered_set<unsigned>> releaseIn(blocks.size());
        std::vector<std::unordered_set<unsigned>> releaseOut(blocks.size());
        std::queue<size_t> worklist;
        std::vector<unsigned char> queued(blocks.size(), 1);
        for (size_t index = 0; index < blocks.size(); ++index)
            worklist.push(index);

        /// @brief Format a managed-handle release diagnostic.
        /// @param block Block containing @p instr.
        /// @param instr Instruction at which the invalid action occurs.
        /// @param id Managed temporary identifier.
        /// @param action Short action description such as double release.
        /// @return Structured error anchored to @p instr.
        auto makeReleaseDiag =
            [&](const BasicBlock &block, const Instr &instr, unsigned id, std::string_view action) {
                std::ostringstream message;
                message << action << " of %" << id;
                return makeError(instr.loc, formatInstrDiag(fn, block, instr, message.str()));
            };

        /// @brief Apply one block's ownership operations to an incoming release set.
        /// @details Releases add IDs, retains remove them, ordinary uses reject
        ///          released IDs, and result definitions kill loop-carried state
        ///          for their fresh SSA value.
        /// @param block Block whose instructions are transferred in order.
        /// @param released Set of possibly released IDs on block entry.
        /// @return Updated exit set, or a double-release/use-after-release error.
        auto transferReleases =
            [&](const BasicBlock &block,
                std::unordered_set<unsigned> released) -> Expected<std::unordered_set<unsigned>> {
            for (const Instr &instr : block.instructions) {
                if (isRuntimeExplicitRelease(instr)) {
                    if (!instr.operands.empty() && instr.operands[0].kind == Value::Kind::Temp) {
                        const unsigned id = instr.operands[0].id;
                        if (released.contains(id))
                            return Expected<std::unordered_set<unsigned>>{
                                makeReleaseDiag(block, instr, id, "double release")};
                        released.insert(id);
                    }
                } else if (isRuntimeArrayRetain(instr) && !instr.operands.empty() &&
                           instr.operands[0].kind == Value::Kind::Temp) {
                    released.erase(instr.operands[0].id);
                } else if (!isRuntimeObjectFinalizerCall(instr)) {
                    /// @brief Reject one use of a possibly released temporary.
                    /// @param value Operand or branch argument to inspect.
                    /// @return Success when live or non-temporary; otherwise an error.
                    auto checkUse = [&](const Value &value) -> Expected<void> {
                        if (value.kind == Value::Kind::Temp && released.contains(value.id))
                            return Expected<void>{
                                makeReleaseDiag(block, instr, value.id, "use after release")};
                        return {};
                    };
                    for (const Value &operand : instr.operands)
                        if (auto result = checkUse(operand); !result)
                            return Expected<std::unordered_set<unsigned>>{result.error()};
                    for (const auto &bundle : instr.brArgs)
                        for (const Value &arg : bundle)
                            if (auto result = checkUse(arg); !result)
                                return Expected<std::unordered_set<unsigned>>{result.error()};
                }

                // A result temp denotes a fresh dynamic value each time the
                // instruction executes. If a loop backedge carries a prior
                // release state for the same SSA id, the new definition
                // supersedes that state after its operands have been checked.
                // Apply this kill to every call classification: a helper can
                // both retain an argument and return a fresh managed object.
                if (instr.result)
                    released.erase(*instr.result);

                if (isTerminator(instr.op))
                    break;
            }
            return released;
        };

        /// @brief Union one release-state set into another.
        /// @param dst Destination set updated in place.
        /// @param src IDs to merge.
        /// @return `true` when at least one ID was newly inserted.
        auto mergeReleaseSet = [](std::unordered_set<unsigned> &dst,
                                  const std::unordered_set<unsigned> &src) {
            bool changed = false;
            for (unsigned id : src)
                changed |= dst.insert(id).second;
            return changed;
        };

        while (!worklist.empty()) {
            const size_t index = worklist.front();
            worklist.pop();
            queued[index] = 0;

            auto transferred = transferReleases(*blocks[index], releaseIn[index]);
            if (!transferred)
                return Expected<void>{transferred.error()};
            if (transferred.value() == releaseOut[index])
                continue;

            releaseOut[index] = std::move(transferred.value());
            for (size_t succ : successors[index]) {
                if (mergeReleaseSet(releaseIn[succ], releaseOut[index]) && !queued[succ]) {
                    worklist.push(succ);
                    queued[succ] = 1;
                }
            }
        }
    }

    // ===== PASS 4: Alloca escape verification =====
    // Detect obvious alloca-derived pointer escapes. Returning a stack address
    // or storing one into non-stack storage can leave dangling addresses after
    // the function returns. Direct calls may borrow stack addresses under the
    // current IL ABI; memory analyses still conservatively treat such calls as
    // escapes for optimization purposes.
    {
        const auto stackDerived = computeStackDerivedTemps(fn, blockMap);

        if (!stackDerived.empty()) {
            for (const auto &blk : fn.blocks) {
                for (const auto &instr : blk.instructions) {
                    /// @brief Diagnose a prohibited escape of one derived pointer.
                    /// @param op Candidate escaping value.
                    /// @param action Verb describing the escape operation.
                    /// @return Success for ordinary values; otherwise an error
                    ///         when @p op is alloca-derived.
                    auto failEscape = [&](const Value &op,
                                          std::string_view action) -> Expected<void> {
                        if (op.kind != Value::Kind::Temp || !stackDerived.contains(op.id))
                            return {};
                        std::ostringstream msg;
                        msg << action << " alloca-derived pointer %" << op.id;
                        return Expected<void>{
                            makeError(instr.loc, formatInstrDiag(fn, blk, instr, msg.str()))};
                    };

                    if (instr.op == Opcode::Ret) {
                        for (const auto &op : instr.operands)
                            if (auto result = failEscape(op, "returning"); !result)
                                return result;
                    }

                    if (instr.op == Opcode::Store && instr.operands.size() >= 2) {
                        const Value &dst = instr.operands[0];
                        const Value &stored = instr.operands[1];
                        const bool dstIsStackDerived =
                            dst.kind == Value::Kind::Temp && stackDerived.contains(dst.id);
                        if (!dstIsStackDerived) {
                            if (auto result = failEscape(stored, "storing"); !result)
                                return result;
                        }
                    }

                    if (instr.op == Opcode::CallIndirect) {
                        for (const auto &op : instr.operands)
                            if (auto result = failEscape(op, "passing"); !result)
                                return result;
                    }
                }
            }
        }
    }

    return {};
}

/// @brief Run block-level verification including handler semantics.
///
/// @details Establishes a type inference context seeded with incoming
///          temporaries, validates block parameters, records handler metadata,
///          enforces resume and error accessor placement rules, tracks runtime
///          array releases, dispatches opcode-specific verification, and finally
///          ensures terminators are well-formed. Parameter temporaries are
///          removed from the inference context after the block is processed.
///
/// @param fn Enclosing function definition.
/// @param bb Block under inspection.
/// @param blockMap Mapping from labels to block pointers for CFG lookups.
/// @param temps Table of SSA temporaries and their known types.
/// @param definingBlock Maps temp IDs to their defining blocks for within-block ordering.
/// @param sink Diagnostic sink receiving instruction-level messages.
/// @return Success or a diagnostic describing the failure.
Expected<void> FunctionVerifier::verifyBlock(
    const Function &fn,
    const BasicBlock &bb,
    const BlockMap &blockMap,
    std::unordered_map<unsigned, Type> &temps,
    const std::unordered_map<unsigned, const BasicBlock *> &definingBlock,
    DiagSink &sink) {
    // Initialize defined set with definitions from OTHER blocks.
    // This allows cross-block uses to pass verification even when the defining
    // block appears later in declaration order (which is valid after SimplifyCFG).
    // Within-block definitions are added incrementally to detect within-block
    // use-before-def errors.
    std::unordered_set<unsigned> defined;
    for (const auto &entry : temps) {
        auto it = definingBlock.find(entry.first);
        if (it == definingBlock.end() || it->second != &bb) {
            // Definition is from another block or a function parameter
            defined.insert(entry.first);
        }
    }

    TypeInference types(temps, defined);

    std::vector<unsigned> paramIds;
    if (auto result = validateBlockParams_E(fn, bb, types, paramIds); !result)
        return result;

    std::optional<HandlerSignature> handlerSignature;
    auto handlerCheck = analyzeHandlerBlock(fn, bb);
    if (!handlerCheck)
        return Expected<void>{handlerCheck.error()};
    handlerSignature = handlerCheck.value();
    if (handlerSignature)
        handlerInfo_[bb.label] = *handlerSignature;

    std::unordered_set<unsigned> released;

    for (const auto &instr : bb.instructions) {
        if (auto result = types.ensureOperandsDefined_E(fn, bb, instr); !result)
            return result;

        if (instr.op == Opcode::EhEntry && &instr != &bb.instructions.front())
            return Expected<void>{makeError(
                instr.loc,
                formatInstrDiag(
                    fn, bb, instr, "eh.entry only allowed as first instruction of handler block"))};

        if (isResumeOpcode(instr.op)) {
            if (!handlerSignature)
                return Expected<void>{makeError(
                    instr.loc,
                    formatInstrDiag(fn, bb, instr, "resume.* only allowed in handler block"))};
            if (instr.operands.empty() || instr.operands[0].kind != Value::Kind::Temp ||
                instr.operands[0].id != handlerSignature->resumeTokenParam) {
                return Expected<void>{makeError(
                    instr.loc,
                    formatInstrDiag(fn, bb, instr, "resume.* must use handler %tok parameter"))};
            }
        }

        if (isErrAccessOpcode(instr.op) && !instr.operands.empty()) {
            if (!handlerSignature) {
                return Expected<void>{makeError(
                    instr.loc,
                    formatInstrDiag(fn, bb, instr, "err.get_* only allowed in handler block"))};
            }
        }

        if (auto result = verifyResumeTokenValueUses(fn, bb, instr, blockMap, types); !result)
            return result;

        if (isRuntimeExplicitRelease(instr)) {
            if (!instr.operands.empty() && instr.operands[0].kind == Value::Kind::Temp) {
                const unsigned id = instr.operands[0].id;
                if (released.contains(id)) {
                    std::ostringstream message;
                    message << "double release of %" << id;
                    return Expected<void>{
                        makeError(instr.loc, formatInstrDiag(fn, bb, instr, message.str()))};
                }
            }
        } else if (isRuntimeArrayRetain(instr)) {
            if (!instr.operands.empty() && instr.operands[0].kind == Value::Kind::Temp) {
                const unsigned id = instr.operands[0].id;
                if (released.contains(id)) {
                    std::ostringstream message;
                    message << "use after release of %" << id;
                    return Expected<void>{
                        makeError(instr.loc, formatInstrDiag(fn, bb, instr, message.str()))};
                }
                released.erase(id);
            }
        } else if (isRuntimeObjectFinalizerCall(instr)) {
            // rt_obj_release_check0 reports that the object's refcount reached
            // zero; destructor dispatch and rt_obj_free are finalization steps
            // for that same handle and must be allowed after the release check.
        } else {
            /// @brief Reject one same-block use of a previously released value.
            /// @param value Operand or branch argument to inspect.
            /// @return Success when live or non-temporary; otherwise a diagnostic.
            const auto checkValue = [&](const Value &value) -> Expected<void> {
                if (value.kind != Value::Kind::Temp)
                    return Expected<void>{};
                const unsigned id = value.id;
                if (!released.contains(id))
                    return Expected<void>{};
                std::ostringstream message;
                message << "use after release of %" << id;
                return Expected<void>{
                    makeError(instr.loc, formatInstrDiag(fn, bb, instr, message.str()))};
            };

            for (const auto &operand : instr.operands)
                if (auto result = checkValue(operand); !result)
                    return result;

            for (const auto &bundle : instr.brArgs)
                for (const auto &argument : bundle)
                    if (auto result = checkValue(argument); !result)
                        return result;
        }

        if (auto result = verifyInstruction(fn, bb, instr, blockMap, types, sink); !result)
            return result;

        if (isRuntimeExplicitRelease(instr) && !instr.operands.empty() &&
            instr.operands[0].kind == Value::Kind::Temp) {
            released.insert(instr.operands[0].id);
        }

        if (isTerminator(instr.op))
            break;
    }

    if (auto result = checkBlockTerminators_E(fn, bb); !result)
        return result;

    // Block params persist in the type map for cross-block use.  The pre-collection
    // pass already registers all definitions so successor blocks can reference them.
    // (Removed per-block removeTemp that prevented valid cross-block references
    //  after inlining.)
    (void)paramIds;

    return {};
}

/// @brief Dispatch verification logic for a single instruction.
///
/// @details Constructs a @ref VerifyCtx populated with the surrounding context,
///          validates operand/result signature contracts, and iterates the
///          registered strategy list until one claims the opcode.  The selected
///          strategy performs opcode-specific checks and returns its result.
///
/// @param fn Function containing the instruction.
/// @param bb Block containing the instruction.
/// @param instr Instruction to verify.
/// @param blockMap Mapping from labels to block pointers for CFG queries.
/// @param types Type inference state for SSA values.
/// @param sink Diagnostic sink receiving verification messages.
/// @return Success or a diagnostic describing the error.
Expected<void> FunctionVerifier::verifyInstruction(const Function &fn,
                                                   const BasicBlock &bb,
                                                   const Instr &instr,
                                                   const BlockMap &blockMap,
                                                   TypeInference &types,
                                                   DiagSink &sink) {
    VerifyCtx ctx{sink, types, externs_, functionMap_, globals_, fn, bb, instr};
    if (auto result = verifyOpcodeSignature_E(ctx); !result)
        return result;

    for (const auto &strategy : strategies_) {
        if (!strategy->matches(instr))
            continue;
        return strategy->verify(
            fn, bb, instr, blockMap, externs_, functionMap_, globals_, types, sink);
    }

    return Expected<void>{makeError({}, formatFunctionDiag(fn, "no instruction strategy for op"))};
}

/// @brief Compose a function-scoped diagnostic prefix.
///
/// @details Formats the function name and appends an optional suffix so callers
///          can reuse the string as a consistent diagnostic prefix when no
///          specific instruction location is available.
///
/// @param fn Function associated with the diagnostic.
/// @param message Additional context appended after the name.
/// @return Human-readable string describing the function context.
std::string FunctionVerifier::formatFunctionDiag(const Function &fn,
                                                 std::string_view message) const {
    std::ostringstream oss;
    oss << fn.name;
    if (!message.empty())
        oss << ": " << message;
    return oss.str();
}

} // namespace il::verify
