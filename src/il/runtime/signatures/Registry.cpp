//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/signatures/Registry.cpp
// Purpose: Implement the lightweight signature registry used for debug
//          validation of runtime helper metadata.
// Key invariants: Registration preserves insertion order, exposes stable
//                 references for the lifetime of the process, and treats
//                 duplicate identical registration as idempotent while rejecting
//                 conflicting metadata for the same helper name.
// Ownership/Lifetime: Stored signatures have static storage managed by this
//                     translation unit, ensuring the registry survives for the
//                     duration of the process and remains accessible from
//                     multiple translation units.
// Links: docs/il/il-guide.md#reference, docs/internals/architecture.md#runtime-signatures
//
//===----------------------------------------------------------------------===//

#include "il/runtime/signatures/Registry.hpp"
#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeOwnership.hpp"

#include <mutex>
#include <stdexcept>

/// @file
/// @brief Defines the backing container for runtime signature metadata.
/// @details Runtime verification utilities rely on a shared table of
///          @ref Signature objects to cross-check compiler emitted calls
///          against the C runtime ABI.  This translation unit provides the
///          canonical storage and mutation helpers for that table, keeping the
///          behaviour uniform across the various registration modules.

namespace il::runtime::signatures {
namespace {
/// @brief Access the process-wide container that owns runtime signatures.
/// @details The helper wraps a function-local static so the vector is lazily
///          constructed upon first use yet guaranteed to survive until program
///          shutdown.  Because the registry returns a reference, callers avoid
///          global variable exposure while still mutating the shared storage.
///          The vector intentionally never shrinks; registration is append-only
///          so previously returned references remain valid for diagnostic tools
///          that snapshot the registry contents.
/// @return Mutable vector storing registered signatures in insertion order.
std::vector<Signature> &registry() {
    static std::vector<Signature> g_signatures;
    return g_signatures;
}

/// @brief Access the mutex serializing registry reads and writes.
/// @return Process-lifetime mutex shared by every registry operation.
std::mutex &registry_mutex() {
    static std::mutex g_mutex;
    return g_mutex;
}

/// @brief Access storage for the append-generation counter.
/// @return Mutable process-lifetime registry version value.
std::size_t &registry_version_storage() {
    static std::size_t g_version = 0;
    return g_version;
}

/// @brief Compare two coarse ABI kind sequences.
/// @param lhs First ordered parameter or result sequence.
/// @param rhs Second ordered parameter or result sequence.
/// @return True when both sequences have identical length and kinds.
bool sameKinds(const std::vector<SigParam> &lhs, const std::vector<SigParam> &rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
        if (lhs[i].kind != rhs[i].kind)
            return false;
    return true;
}

/// @brief Compare complete normalized signature metadata.
/// @param lhs First signature.
/// @param rhs Second signature.
/// @return True when ABI shape and all effect/ownership facts match.
bool sameSignature(const Signature &lhs, const Signature &rhs) {
    return lhs.name == rhs.name && sameKinds(lhs.params, rhs.params) &&
           sameKinds(lhs.rets, rhs.rets) && lhs.nothrow == rhs.nothrow &&
           lhs.readonly == rhs.readonly && lhs.pure == rhs.pure &&
           lhs.consumedArgMask == rhs.consumedArgMask &&
           lhs.retainedArgMask == rhs.retainedArgMask &&
           lhs.ownedOutArgMask == rhs.ownedOutArgMask && lhs.returnsOwned == rhs.returnsOwned &&
           lhs.mayAllocate == rhs.mayAllocate &&
           lhs.returnsKnownObject == rhs.returnsKnownObject;
}
} // namespace

/// @brief Merge shared effect and ownership classifications into a signature.
/// @param signature Signature value to normalize.
/// @return Normalized copy preserving explicit facts and adding table-derived
///         facts for the named helper.
Signature apply_effect_overrides(Signature signature) {
    const auto effects = il::runtime::classifyHelperEffects(signature.name);
    signature.nothrow = signature.nothrow || effects.nothrow;
    signature.readonly = signature.readonly || effects.readonly;
    signature.pure = signature.pure || effects.pure;
    const auto ownership = il::runtime::classifyRuntimeOwnership(signature.name);
    signature.consumedArgMask |= ownership.consumedArgMask;
    signature.retainedArgMask |= ownership.retainedArgMask;
    signature.ownedOutArgMask |= ownership.ownedOutArgMask;
    signature.returnsOwned = signature.returnsOwned || ownership.returnsOwned;
    signature.mayAllocate = signature.mayAllocate || ownership.mayAllocate;
    signature.returnsKnownObject =
        signature.returnsKnownObject || ownership.returnsKnownObject;
    return signature;
}

/// @brief Register a runtime signature in the diagnostic registry.
/// @details Re-registering identical normalized metadata is a no-op. A duplicate
///          name with conflicting ABI or effects throws to prevent order-dependent
///          validation behavior.
/// @param signature Signature metadata describing a runtime helper.
/// @throws std::logic_error When an existing name has different metadata.
void register_signature(const Signature &signature) {
    Signature normalized = apply_effect_overrides(signature);
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &entries = registry();
    for (const auto &entry : entries) {
        if (entry.name != normalized.name)
            continue;
        if (!sameSignature(entry, normalized))
            throw std::logic_error("conflicting runtime signature registration for " +
                                   normalized.name);
        return;
    }
    entries.push_back(std::move(normalized));
    ++registry_version_storage();
}

/// @brief Retrieve a stable view of all registered runtime signatures.
/// @details Returns a reference to the underlying container so callers can
///          iterate without copying.  The reference remains valid because the
///          registry owns static storage for the lifetime of the process.
///          Subsequent registrations may invalidate iterators but never the
///          reference itself, matching standard library container semantics.
/// @return Read-only view of the registered signatures in insertion order.
const std::vector<Signature> &all_signatures() {
    return registry();
}

/// @brief Look up normalized pure and readonly flags by symbol name.
/// @param name Exact registered runtime symbol.
/// @param pure Receives the pure flag on success.
/// @param readonly Receives the readonly flag on success.
/// @return True when a matching signature was found; outputs are unchanged
///         otherwise.
bool find_signature_effects(std::string_view name, bool &pure, bool &readonly) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    for (const auto &signature : registry()) {
        if (signature.name != name)
            continue;
        pure = signature.pure;
        readonly = signature.readonly;
        return true;
    }
    return false;
}

/// @brief Read the current append-generation counter.
/// @return Monotonic version incremented for each newly appended unique entry.
std::size_t registry_version() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry_version_storage();
}

} // namespace il::runtime::signatures
