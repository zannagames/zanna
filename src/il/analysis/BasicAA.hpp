//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/BasicAA.hpp
// Purpose: Lightweight alias analysis for IL functions. Provides conservative
//          memory disambiguation (NoAlias/MayAlias/MustAlias) using SSA-based
//          reasoning about allocas, noalias parameters, global addresses, and
//          GEP offsets. Also classifies call-site ModRef behaviour via function
//          attributes and the canonical runtime signature registry.
// Key invariants:
//   - Unknown base kinds conservatively return MayAlias.
//   - Distinct allocas never alias each other.
//   - Non-escaping allocas never alias with function parameters.
//   - noalias parameters never alias other pointers.
//   - ModRef queries apply to Opcode::Call and Opcode::CallIndirect instructions.
// Ownership/Lifetime: BasicAA is constructed per-function, collecting alloca
//          and parameter info from the function body. Holds const pointers to
//          the function and optional module; both must outlive the analysis.
// Links: il/core/Function.hpp, il/core/Instr.hpp, il/core/Module.hpp,
//        il/runtime/RuntimeSignatures.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/analysis/AllocaRoots.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/signatures/Registry.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::analysis {

namespace detail {

/// @brief Add two signed offsets without invoking integer overflow.
/// @param lhs First addend.
/// @param rhs Second addend.
/// @param out Receives the sum when it is representable.
/// @return True when addition succeeded; false leaves @p out unchanged.
inline bool checkedAdd(long long lhs, long long rhs, long long &out) {
    if ((rhs > 0 && lhs > (std::numeric_limits<long long>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<long long>::min)() - rhs)) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

} // namespace detail

/// @brief Describe the relationship between two pointer-like values.
enum class AliasResult { NoAlias, MayAlias, MustAlias };

/// @brief Summarise how a call interacts with memory.
enum class ModRefResult { NoModRef, Ref, Mod, ModRef };

/// @brief Lightweight alias analysis for IL functions.
class BasicAA {
  public:
    /// @brief Build analysis state for @p function optionally referencing @p module.
    /// @param function Function whose SSA definitions and parameters are indexed.
    /// @param module Optional containing module used to resolve direct callees.
    /// @pre @p function and any supplied @p module outlive this analysis object.
    explicit BasicAA(const il::core::Function &function, const il::core::Module *module = nullptr);

    /// @brief Convenience constructor when both module and function are known.
    /// @param module Module containing direct function and extern declarations.
    /// @param function Function whose memory locations are analyzed.
    /// @pre Both arguments outlive this analysis object.
    BasicAA(const il::core::Module &module, const il::core::Function &function);

    /// @brief Query aliasing behaviour for two pointer-like values.
    /// @param lhs First pointer-like value.
    /// @param rhs Second pointer-like value.
    /// @param lhsSize Optional byte width of the lhs access when known.
    /// @param rhsSize Optional byte width of the rhs access when known.
    /// @return Proven no-alias/must-alias result, or MayAlias when evidence is insufficient.
    [[nodiscard]] AliasResult alias(const il::core::Value &lhs,
                                    const il::core::Value &rhs,
                                    std::optional<unsigned> lhsSize = std::nullopt,
                                    std::optional<unsigned> rhsSize = std::nullopt) const;

    /// @brief Classify the ModRef behaviour for a call instruction.
    /// @param instr Instruction to classify.
    /// @return NoModRef for pure direct calls, Ref for readonly direct calls,
    ///         and conservative ModRef otherwise.
    [[nodiscard]] ModRefResult modRef(const il::core::Instr &instr) const;

    /// @brief Compute the byte width of a scalar IL type when known.
    /// @param type IL type whose storage representation is queried.
    /// @return Storage width in bytes, or no value for unsized/unknown types.
    [[nodiscard]] static std::optional<unsigned> typeSizeBytes(const il::core::Type &type);

    /// @brief Check whether a temp id is a non-escaping alloca.
    /// @details Non-escaping allocas are stack slots whose addresses never leave
    ///          the function (not passed to calls, not stored as values). Loads
    ///          from such allocas are safe to hoist past calls.
    /// @param id SSA temporary identifier of a candidate alloca.
    /// @return True when construction proved the alloca address does not escape.
    [[nodiscard]] bool isNonEscapingAlloca(unsigned id) const;

  private:
    enum class BaseKind { Unknown, Alloca, Param, NoAliasParam, Global, ConstStr, Null };

    /// @brief Canonical base identity and optional byte offset for a pointer value.
    struct Location {
        BaseKind kind = BaseKind::Unknown;
        /// @brief Identity of the base (alloca id, param id).
        unsigned id = 0;
        /// @brief Global symbol backing the location.
        std::string_view global;
        /// @brief Byte offset from the base when known.
        long long offset = 0;
        /// @brief Whether @ref offset remained statically computable without overflow.
        bool hasOffset = true;
    };

    const il::core::Function *function_;
    const il::core::Module *module_;
    std::unordered_set<unsigned> allocas_;
    std::unordered_set<unsigned> nonEscapingAllocas_;
    std::unordered_set<unsigned> noaliasParams_;
    std::unordered_set<unsigned> params_;
    std::unordered_map<std::string_view, const il::core::Function *> functionIndex_;
    std::unordered_map<std::string_view, const il::core::Extern *> externIndex_;

    using DefInfo = AllocaRootDefInfo;

    std::unordered_map<unsigned, DefInfo> defs_;

    /// @brief Memory-effect facts known for a potential direct callee.
    struct CallEffect {
        bool pure = false;
        bool readonly = false;
        bool known = false;
    };

    /// @brief Populate module-level callee indexes from the borrowed module.
    /// @details The index avoids repeated linear scans when many call sites ask
    ///          for ModRef information in the same function.
    void collectModuleInfo();

    /// @brief Populate the internal alloca, parameter, and no-alias sets from @p function.
    /// @details Scans the entry block for @c Alloca instructions and records each
    ///          parameter, building the data structures that drive pointer-identity
    ///          queries later.  Called once during construction.
    void collectFunctionInfo(const il::core::Function &function);

    /// @brief Compare two IL value records for exact semantic identity.
    /// @param lhs First value.
    /// @param rhs Second value.
    /// @return True when both values have the same kind and payload.
    [[nodiscard]] static bool equalValues(const il::core::Value &lhs, const il::core::Value &rhs);

    /// @brief Test whether an SSA temporary is defined by alloca.
    /// @param id Temporary identifier.
    /// @return True when @p id belongs to the collected alloca set.
    [[nodiscard]] bool isAlloca(unsigned id) const;

    /// @brief Test whether an SSA temporary is a noalias parameter.
    /// @param id Temporary identifier.
    /// @return True when the corresponding parameter carries the noalias attribute.
    [[nodiscard]] bool isNoAliasParam(unsigned id) const;

    /// @brief Test whether an SSA temporary is any function parameter.
    /// @param id Temporary identifier.
    /// @return True when @p id belongs to the collected parameter set.
    [[nodiscard]] bool isParam(unsigned id) const;

    /// @brief Resolve a direct callee name to a function definition.
    /// @param name Callee name.
    /// @return Borrowed function pointer, or nullptr when it is not defined locally.
    [[nodiscard]] const il::core::Function *findFunction(std::string_view name) const;

    /// @brief Read memory-effect attributes from a defined function.
    /// @param fn Function definition.
    /// @return Known effect facts derived from its attributes.
    [[nodiscard]] CallEffect queryFunctionEffect(const il::core::Function &fn) const;

    /// @brief Read explicit memory-effect attributes from an extern declaration.
    /// @param name Extern symbol name.
    /// @return Known facts for pure/readonly declarations, otherwise unknown.
    [[nodiscard]] CallEffect queryExternEffect(std::string_view name) const;

    /// @brief Query generated, dynamic, and classified runtime helper effects.
    /// @param name Runtime helper symbol.
    /// @return First known effect description, or an unknown effect.
    [[nodiscard]] CallEffect queryRuntimeEffect(std::string_view name) const;

    /// @brief Query dynamically registered runtime test signatures by name.
    /// @details The generated runtime registry handles production helpers with
    ///          allocation-free binary search. Unit tests and debug validation
    ///          can also append signatures at runtime; this cache is rebuilt only
    ///          when that append-only registry's version changes.
    /// @param name Runtime helper name to inspect.
    /// @return Effect metadata when a dynamic signature is registered.
    [[nodiscard]] static CallEffect queryRegisteredSignatureEffect(std::string_view name);

    /// @brief Resolve direct-callee effects in local, extern, then runtime order.
    /// @param name Direct call target.
    /// @return Best known effect metadata.
    [[nodiscard]] CallEffect computeCalleeEffect(std::string_view name) const;

    /// @brief Reduce a pointer-like value to a canonical base and offset.
    /// @param value Value to trace through GEP and address definitions.
    /// @return Location description; BaseKind::Unknown represents insufficient evidence.
    [[nodiscard]] Location describe(const il::core::Value &value) const;

    /// @brief Extract a signed constant integer offset.
    /// @param v Candidate IL value.
    /// @param out Receives the constant payload on success.
    /// @return True only for ConstInt values.
    [[nodiscard]] static bool constOffset(const il::core::Value &v, long long &out);
};

/// @brief Construct per-function alias facts with optional module context.
/// @param function Function to analyze.
/// @param module Optional containing module used for callee-effect resolution.
inline BasicAA::BasicAA(const il::core::Function &function, const il::core::Module *module)
    : function_(&function), module_(module) {
    collectModuleInfo();
    collectFunctionInfo(function);
}

/// @brief Construct alias facts with an explicit containing module.
/// @param module Module used for function and extern lookup.
/// @param function Function to analyze.
inline BasicAA::BasicAA(const il::core::Module &module, const il::core::Function &function)
    : BasicAA(function, &module) {}

/// @brief Index defined functions and extern declarations by borrowed names.
/// @details Does nothing when construction did not receive module context.
inline void BasicAA::collectModuleInfo() {
    if (!module_)
        return;

    functionIndex_.reserve(module_->functions.size());
    for (const auto &fn : module_->functions)
        functionIndex_.emplace(std::string_view(fn.name), &fn);

    externIndex_.reserve(module_->externs.size());
    for (const auto &ext : module_->externs)
        externIndex_.emplace(std::string_view(ext.name), &ext);
}

/// @brief Collect pointer parameters, SSA definitions, allocas, and escape facts.
/// @param function Function whose complete body is scanned once.
inline void BasicAA::collectFunctionInfo(const il::core::Function &function) {
    for (const auto &param : function.params) {
        if (param.isNoAlias())
            noaliasParams_.insert(param.id);
        params_.insert(param.id);
    }

    for (const auto &block : function.blocks)
        for (const auto &instr : block.instructions) {
            if (instr.result)
                defs_[*instr.result] = DefInfo{instr.op, instr.operands};
            if (instr.op == il::core::Opcode::Alloca && instr.result)
                allocas_.insert(*instr.result);
        }

    const AllocaRootMap allocaRoots = computeAllocaRoots(function, defs_);

    for (unsigned allocaId : allocas_)
        if (!allocaEscapes(function, allocaId, allocaRoots))
            nonEscapingAllocas_.insert(allocaId);
}

/// @brief Compare two IL values for exact identity.
/// @param lhs First value.
/// @param rhs Second value.
/// @return True when value kind and kind-specific payload match.
inline bool BasicAA::equalValues(const il::core::Value &lhs, const il::core::Value &rhs) {
    if (lhs.kind != rhs.kind)
        return false;

    using Kind = il::core::Value::Kind;
    switch (lhs.kind) {
        case Kind::Temp:
            return lhs.id == rhs.id;
        case Kind::ConstInt:
            return lhs.i64 == rhs.i64 && lhs.isBool == rhs.isBool;
        case Kind::ConstFloat:
            return il::core::valueEquals(lhs, rhs);
        case Kind::ConstStr:
        case Kind::GlobalAddr:
            return lhs.str == rhs.str;
        case Kind::NullPtr:
            return true;
    }
    return false;
}

/// @brief Test membership in the collected alloca-result set.
/// @param id SSA temporary identifier.
/// @return True when @p id is an alloca result.
inline bool BasicAA::isAlloca(unsigned id) const {
    return allocas_.count(id) != 0;
}

/// @brief Test membership in the collected noalias-parameter set.
/// @param id SSA temporary identifier.
/// @return True when @p id names a noalias parameter.
inline bool BasicAA::isNoAliasParam(unsigned id) const {
    return noaliasParams_.count(id) != 0;
}

/// @brief Test membership in the collected parameter set.
/// @param id SSA temporary identifier.
/// @return True when @p id names any parameter.
inline bool BasicAA::isParam(unsigned id) const {
    return params_.count(id) != 0;
}

/// @brief Test membership in the proven non-escaping alloca set.
/// @param id SSA temporary identifier.
/// @return True when the corresponding alloca address cannot escape.
inline bool BasicAA::isNonEscapingAlloca(unsigned id) const {
    return nonEscapingAllocas_.count(id) != 0;
}

/// @brief Find a local function definition by exact name.
/// @param name Direct callee name.
/// @return Borrowed function pointer, or nullptr when absent.
inline const il::core::Function *BasicAA::findFunction(std::string_view name) const {
    if (name.empty())
        return nullptr;

    if (function_ && function_->name == name)
        return function_;

    auto it = functionIndex_.find(name);
    return it == functionIndex_.end() ? nullptr : it->second;
}

/// @brief Translate function attributes into known memory-effect facts.
/// @param fn Defined function.
/// @return Known pure and readonly flags.
inline BasicAA::CallEffect BasicAA::queryFunctionEffect(const il::core::Function &fn) const {
    CallEffect effect;
    effect.pure = fn.attrs().pure;
    effect.readonly = fn.attrs().readonly;
    effect.known = true;
    return effect;
}

/// @brief Query built-in and dynamically classified runtime helper effects.
/// @param name Runtime helper name.
/// @return Known facts from the first matching registry, or an unknown effect.
inline BasicAA::CallEffect BasicAA::queryRuntimeEffect(std::string_view name) const {
    if (const auto *sig = il::runtime::findRuntimeSignature(name))
        return CallEffect{sig->pure, sig->readonly, true};

    if (const auto dynamic = queryRegisteredSignatureEffect(name); dynamic.known)
        return dynamic;

    const auto helper = il::runtime::classifyHelperEffects(name);
    if (helper.known)
        return CallEffect{helper.pure, helper.readonly, true};
    return {};
}

/// @brief Query append-only runtime signature registrations.
/// @param name Runtime helper name.
/// @return Known registered pure/readonly facts, or an unknown effect.
inline BasicAA::CallEffect BasicAA::queryRegisteredSignatureEffect(std::string_view name) {
    bool pure = false;
    bool readonly = false;
    if (il::runtime::signatures::find_signature_effects(name, pure, readonly))
        return CallEffect{pure, readonly, true};
    return {};
}

/// @brief Query explicit memory-effect attributes on an extern declaration.
/// @param name Extern symbol name.
/// @return Known pure/readonly facts, or unknown when absent/unannotated.
inline BasicAA::CallEffect BasicAA::queryExternEffect(std::string_view name) const {
    auto it = externIndex_.find(name);
    if (it == externIndex_.end())
        return {};

    const auto &ext = *it->second;
    // `nothrow` is control-flow metadata, not a memory-effect promise.
    // Only pure/readonly extern attributes can refine ModRef.
    if (ext.attrs().pure || ext.attrs().readonly)
        return CallEffect{ext.attrs().pure, ext.attrs().readonly, true};
    return {};
}

/// @brief Resolve direct-callee effects using the most authoritative source.
/// @param name Direct call target name.
/// @return Local definition, extern declaration, or runtime-registry effects.
inline BasicAA::CallEffect BasicAA::computeCalleeEffect(std::string_view name) const {
    // Module-level analysis is authoritative when the callee is defined locally.
    // Fall back to the runtime signature table only for external functions.
    if (const auto *fn = findFunction(name))
        return queryFunctionEffect(*fn);

    if (const auto effect = queryExternEffect(name); effect.known)
        return effect;

    return queryRuntimeEffect(name);
}

/// @brief Extract a signed constant used as a GEP byte offset.
/// @param v Candidate IL value.
/// @param out Receives the integer payload on success.
/// @return True when @p v is a ConstInt.
inline bool BasicAA::constOffset(const il::core::Value &v, long long &out) {
    if (v.kind == il::core::Value::Kind::ConstInt) {
        out = v.i64;
        return true;
    }
    return false;
}

/// @brief Trace a pointer through definitions to its base identity and offset.
/// @param value Pointer-like IL value.
/// @return Canonical location, or an unknown location for cycles/unsupported definitions.
inline BasicAA::Location BasicAA::describe(const il::core::Value &value) const {
    using Kind = il::core::Value::Kind;
    Location loc;
    const il::core::Value *current = &value;
    long long accumulatedOffset = 0;
    bool hasOffset = true;
    std::unordered_set<unsigned> visited;

    while (true) {
        switch (current->kind) {
        case Kind::Temp: {
            if (!visited.insert(current->id).second)
                return {};
            if (isAlloca(current->id)) {
                loc.kind = BaseKind::Alloca;
                loc.id = current->id;
                loc.offset = accumulatedOffset;
                loc.hasOffset = hasOffset;
                return loc;
            }
            if (isNoAliasParam(current->id)) {
                loc.kind = BaseKind::NoAliasParam;
                loc.id = current->id;
                loc.offset = accumulatedOffset;
                loc.hasOffset = hasOffset;
                return loc;
            }
            if (isParam(current->id)) {
                loc.kind = BaseKind::Param;
                loc.id = current->id;
                loc.offset = accumulatedOffset;
                loc.hasOffset = hasOffset;
                return loc;
            }

            auto defIt = defs_.find(current->id);
            if (defIt == defs_.end())
                return loc;
            const DefInfo &def = defIt->second;

            if (def.op == il::core::Opcode::GEP && def.operands.size() >= 2) {
                long long off = 0;
                if (constOffset(def.operands[1], off) && hasOffset) {
                    long long adjusted = 0;
                    if (detail::checkedAdd(accumulatedOffset, off, adjusted))
                        accumulatedOffset = adjusted;
                    else
                        hasOffset = false;
                } else {
                    hasOffset = false;
                }
                current = &def.operands[0];
                continue;
            }

            if ((def.op == il::core::Opcode::AddrOf || def.op == il::core::Opcode::GAddr) &&
                !def.operands.empty() && def.operands[0].kind == Kind::GlobalAddr) {
                loc.kind = BaseKind::Global;
                loc.global = def.operands[0].str;
                loc.offset = accumulatedOffset;
                loc.hasOffset = hasOffset;
                return loc;
            }

            return loc;
        }
        case Kind::GlobalAddr:
            loc.kind = BaseKind::Global;
            loc.global = current->str;
            loc.offset = accumulatedOffset;
            loc.hasOffset = hasOffset;
            return loc;
        case Kind::ConstStr:
            loc.kind = BaseKind::ConstStr;
            loc.global = current->str;
            loc.offset = accumulatedOffset;
            loc.hasOffset = hasOffset;
            return loc;
        case Kind::NullPtr:
            loc.kind = BaseKind::Null;
            return loc;
        case Kind::ConstInt:
        case Kind::ConstFloat:
            return loc;
        }
        return loc;
    }
}

/// @brief Compute storage width using the canonical IL layout helper.
/// @param type IL type to inspect.
/// @return Byte width when the type has a statically known storage size.
inline std::optional<unsigned> BasicAA::typeSizeBytes(const il::core::Type &type) {
    return il::core::storageSizeBytes(type);
}

/// @brief Determine whether two pointer-like values can denote overlapping storage.
/// @param lhs First pointer-like value.
/// @param rhs Second pointer-like value.
/// @param lhsSize Optional byte width accessed through @p lhs.
/// @param rhsSize Optional byte width accessed through @p rhs.
/// @return NoAlias or MustAlias when proven, otherwise conservative MayAlias.
inline AliasResult BasicAA::alias(const il::core::Value &lhs,
                                  const il::core::Value &rhs,
                                  std::optional<unsigned> lhsSize,
                                  std::optional<unsigned> rhsSize) const {
    if (equalValues(lhs, rhs))
        return AliasResult::MustAlias;

    Location l = describe(lhs);
    Location r = describe(rhs);

    auto basesEqual = [](const Location &a, const Location &b) {
        if (a.kind == BaseKind::Global || a.kind == BaseKind::ConstStr)
            return a.kind == b.kind && a.global == b.global;
        return a.kind == b.kind && a.id == b.id;
    };

    if (l.kind == BaseKind::Unknown || r.kind == BaseKind::Unknown)
        return AliasResult::MayAlias;

    if (l.kind == BaseKind::Null || r.kind == BaseKind::Null)
        return basesEqual(l, r) ? AliasResult::MustAlias : AliasResult::NoAlias;

    auto isParamLike = [](BaseKind k) {
        return k == BaseKind::Param || k == BaseKind::NoAliasParam;
    };

    if (l.kind == BaseKind::NoAliasParam && isParamLike(r.kind) && !basesEqual(l, r))
        return AliasResult::NoAlias;
    if (r.kind == BaseKind::NoAliasParam && isParamLike(l.kind) && !basesEqual(l, r))
        return AliasResult::NoAlias;

    auto isGlobalLike = [](BaseKind k) { return k == BaseKind::Global || k == BaseKind::ConstStr; };

    if ((l.kind == BaseKind::Alloca && isGlobalLike(r.kind)) ||
        (r.kind == BaseKind::Alloca && isGlobalLike(l.kind)))
        return AliasResult::NoAlias;

    if (l.kind == BaseKind::Alloca && r.kind == BaseKind::Alloca && l.id != r.id)
        return AliasResult::NoAlias;

    // Non-escaping alloca cannot alias with parameters (address never leaves function)
    if (l.kind == BaseKind::Alloca && r.kind == BaseKind::Param && nonEscapingAllocas_.count(l.id))
        return AliasResult::NoAlias;
    if (r.kind == BaseKind::Alloca && l.kind == BaseKind::Param && nonEscapingAllocas_.count(r.id))
        return AliasResult::NoAlias;

    if (l.kind == BaseKind::Global && r.kind == BaseKind::Global && l.global != r.global)
        return AliasResult::NoAlias;

    if (isGlobalLike(l.kind) && isGlobalLike(r.kind) && l.global != r.global)
        return AliasResult::NoAlias;

    if (l.kind == BaseKind::Param && r.kind == BaseKind::Param && l.id != r.id)
        return AliasResult::MayAlias;

    if (basesEqual(l, r)) {
        if (l.hasOffset && r.hasOffset) {
            if (l.offset == r.offset)
                return AliasResult::MustAlias;

            if (lhsSize && rhsSize) {
                long long lEnd = 0;
                long long rEnd = 0;
                if (!detail::checkedAdd(l.offset, static_cast<long long>(*lhsSize), lEnd) ||
                    !detail::checkedAdd(r.offset, static_cast<long long>(*rhsSize), rEnd))
                    return AliasResult::MayAlias;
                if (lEnd <= r.offset || rEnd <= l.offset)
                    return AliasResult::NoAlias;
            }
        }
        return AliasResult::MayAlias;
    }

    return AliasResult::MayAlias;
}

/// @brief Classify memory reads and writes performed by a call instruction.
/// @param instr Direct or indirect call candidate.
/// @return NoModRef for known pure calls, Ref for known readonly calls,
///         and ModRef for indirect, unknown, or non-call instructions.
inline ModRefResult BasicAA::modRef(const il::core::Instr &instr) const {
    if (instr.op != il::core::Opcode::Call && instr.op != il::core::Opcode::CallIndirect)
        return ModRefResult::ModRef;

    if (instr.op == il::core::Opcode::CallIndirect)
        return ModRefResult::ModRef;

    const CallEffect callee = computeCalleeEffect(instr.callee);
    const bool pure = callee.known && callee.pure;
    const bool readonly = callee.known && callee.readonly;

    if (pure)
        return ModRefResult::NoModRef;
    if (readonly)
        return ModRefResult::Ref;
    return ModRefResult::ModRef;
}

} // namespace zanna::analysis
