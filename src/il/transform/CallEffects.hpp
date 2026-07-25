//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/CallEffects.hpp
// Purpose: Provide a unified API for querying call instruction side effects
//          to support optimization passes that need to determine whether calls
//          can be safely eliminated, reordered, or hoisted.
// Key invariants: Effect classification is conservative—when in doubt, assume
//                 the call may have side effects. The API integrates runtime
//                 signature metadata and deliberately does not trust raw
//                 instruction attributes without verified callee metadata.
// Ownership/Lifetime: Header-only utilities; no global state.
// Links: docs/il/il-guide.md#reference, docs/internals/architecture.md#runtime-signatures
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines conservative call effect and ownership classification helpers.

#pragma once

#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeOwnership.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <cstdint>
#include <string_view>

namespace il::transform {

/// @brief Describe the side-effect classification of a call instruction.
/// @details Used by optimization passes to determine what transformations
///          are safe. Known callee metadata is authoritative.
struct CallEffects {
    bool pure = false;     ///< Call has no observable side effects (can eliminate if unused).
    bool readonly = false; ///< Call may read memory but performs no writes (can reorder).
    bool nothrow = false;  ///< Call cannot throw or trap (can hoist across exception boundaries).
    std::uint64_t consumedArgMask = 0; ///< IL-visible args whose ownership is consumed.
    std::uint64_t retainedArgMask = 0; ///< IL-visible args whose reference count is retained.
    std::uint64_t ownedOutArgMask = 0; ///< Pointer args that receive an owned reference.
    bool returnsOwned = false;         ///< Call returns an owned reference/string handle.
    bool mayAllocate = false;          ///< Call may allocate runtime-managed storage.
    bool knownNeutral = false;         ///< Callee borrows all args, touches no reference
                                       ///< counts, and cannot re-enter user code.

    /// @brief Returns true if the call can be safely eliminated when its result is unused.
    /// @return True only for pure calls guaranteed not to throw.
    [[nodiscard]] constexpr bool canEliminateIfUnused() const noexcept {
        return pure && nothrow;
    }

    /// @brief Returns true if the call can be safely reordered with memory operations.
    /// @return True for pure or readonly calls.
    [[nodiscard]] constexpr bool canReorderWithMemory() const noexcept {
        return pure || readonly;
    }

    /// @brief True when argument @p index has ownership consumed by this call.
    /// @param index Zero-based IL-visible argument index.
    /// @return True when the corresponding consumed bit is set.
    [[nodiscard]] constexpr bool consumesArg(unsigned index) const noexcept {
        return index < 64 && (consumedArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief True when argument @p index has its reference count retained by this call.
    /// @param index Zero-based IL-visible argument index.
    /// @return True when the corresponding retained bit is set.
    [[nodiscard]] constexpr bool retainsArg(unsigned index) const noexcept {
        return index < 64 && (retainedArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief True when pointer argument @p index receives an owned reference.
    /// @param index Zero-based IL-visible argument index.
    /// @return True when the corresponding owned-output bit is set.
    [[nodiscard]] constexpr bool writesOwnedOutArg(unsigned index) const noexcept {
        return index < 64 && (ownedOutArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief True when any known ownership effect is attached to this call.
    /// @return True when a mask, owned result, or allocation property is set.
    [[nodiscard]] constexpr bool hasOwnershipEffects() const noexcept {
        return consumedArgMask != 0 || retainedArgMask != 0 || ownedOutArgMask != 0 ||
               returnsOwned || mayAllocate;
    }
};

/// @brief Merge runtime ownership metadata into a call classification.
/// @param effects Classification updated in place.
/// @param ownership Ownership facts to merge without clearing existing facts.
inline void applyRuntimeOwnership(CallEffects &effects,
                                  const il::runtime::RuntimeOwnershipEffects &ownership) {
    effects.consumedArgMask |= ownership.consumedArgMask;
    effects.retainedArgMask |= ownership.retainedArgMask;
    effects.ownedOutArgMask |= ownership.ownedOutArgMask;
    effects.returnsOwned = effects.returnsOwned || ownership.returnsOwned;
    effects.mayAllocate = effects.mayAllocate || ownership.mayAllocate;
    effects.knownNeutral = effects.knownNeutral || ownership.knownNeutral;
}

/// @brief Copy verified core effect attributes into a call classification.
/// @param effects Classification updated in place.
/// @param attrs Authoritative declaration attributes.
inline void applyEffectAttrs(CallEffects &effects, const il::core::EffectAttrs &attrs) {
    effects.pure = attrs.pure;
    effects.readonly = attrs.readonly;
    effects.nothrow = attrs.nothrow;
}

/// @brief Populate call effects from a registered runtime signature.
/// @param effects Classification overwritten with signature facts.
/// @param signature Runtime ABI and effect metadata.
/// @param callee Symbol used to merge name-based ownership facts.
inline void applyRuntimeSignature(CallEffects &effects,
                                  const il::runtime::RuntimeSignature &signature,
                                  std::string_view callee) {
    effects.pure = signature.pure;
    effects.readonly = signature.readonly;
    effects.nothrow = signature.nothrow;
    effects.consumedArgMask = signature.consumedArgMask;
    effects.retainedArgMask = signature.retainedArgMask;
    effects.ownedOutArgMask = signature.ownedOutArgMask;
    effects.returnsOwned = signature.returnsOwned;
    effects.mayAllocate = signature.mayAllocate;
    applyRuntimeOwnership(effects, il::runtime::classifyRuntimeOwnership(callee));
}

/// @brief Find an extern declaration by callee name.
/// @param module Optional module to search.
/// @param callee Exact symbol name.
/// @return Borrowed declaration pointer, or null.
inline const il::core::Extern *findExternDecl(const il::core::Module *module,
                                              std::string_view callee) {
    if (!module)
        return nullptr;
    for (const auto &ext : module->externs)
        if (ext.name == callee)
            return &ext;
    return nullptr;
}

/// @brief Find a function declaration or definition by callee name.
/// @param module Optional module to search.
/// @param callee Exact symbol name.
/// @return Borrowed function pointer, or null.
inline const il::core::Function *findFunctionDecl(const il::core::Module *module,
                                                  std::string_view callee) {
    if (!module)
        return nullptr;
    for (const auto &fn : module->functions)
        if (fn.name == callee)
            return &fn;
    return nullptr;
}

/// @brief Query side-effect metadata for a call instruction.
/// @details Runtime metadata is authoritative when available. Unknown callees
///          are classified conservatively; the verifier rejects call-site
///          attributes unless the callee has effect metadata.
///
/// @param instr The call instruction to classify.
/// @param module Optional module supplying declaration metadata.
/// @return Effect classification for the call.
inline CallEffects classifyCallEffects(const il::core::Instr &instr,
                                       const il::core::Module *module = nullptr) {
    CallEffects effects;

    if (instr.op != il::core::Opcode::Call)
        return effects; // Conservative: unknown effect for non-call instructions

    const auto *fn = findFunctionDecl(module, instr.callee);
    if (fn && fn->linkage != il::core::Linkage::Import) {
        applyEffectAttrs(effects, fn->attrs());
    } else if (const auto *sig = il::runtime::findRuntimeSignature(instr.callee)) {
        applyRuntimeSignature(effects, *sig, instr.callee);
        return effects;
    } else if (const auto *ext = findExternDecl(module, instr.callee)) {
        applyEffectAttrs(effects, ext->attrs());
    } else if (fn) {
        applyEffectAttrs(effects, fn->attrs());
    } else {
        const auto helperEffects = il::runtime::classifyHelperEffects(instr.callee);
        if (helperEffects.known) {
            effects.pure = helperEffects.pure;
            effects.readonly = helperEffects.readonly;
            effects.nothrow = helperEffects.nothrow;
        }
    }
    applyRuntimeOwnership(effects, il::runtime::classifyRuntimeOwnership(instr.callee));

    return effects;
}

/// @brief Query side-effect metadata for a callee by name.
/// @details Useful when the full instruction is not available.
/// @param callee The callee name to classify.
/// @param module Optional module supplying declaration metadata.
/// @return Effect classification for the callee.
inline CallEffects classifyCalleeEffects(std::string_view callee,
                                         const il::core::Module *module = nullptr) {
    CallEffects effects;

    const auto *fn = findFunctionDecl(module, callee);
    if (fn && fn->linkage != il::core::Linkage::Import) {
        applyEffectAttrs(effects, fn->attrs());
    } else if (const auto *sig = il::runtime::findRuntimeSignature(callee)) {
        applyRuntimeSignature(effects, *sig, callee);
        return effects;
    } else if (const auto *ext = findExternDecl(module, callee)) {
        applyEffectAttrs(effects, ext->attrs());
    } else if (fn) {
        applyEffectAttrs(effects, fn->attrs());
    }

    if (!findExternDecl(module, callee) && !findFunctionDecl(module, callee)) {
        const auto helperEffects = il::runtime::classifyHelperEffects(callee);
        effects.pure = helperEffects.pure;
        effects.readonly = helperEffects.readonly;
        effects.nothrow = helperEffects.nothrow;
    }
    applyRuntimeOwnership(effects, il::runtime::classifyRuntimeOwnership(callee));

    return effects;
}

} // namespace il::transform
