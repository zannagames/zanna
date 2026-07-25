//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/signatures/Registry.hpp
// Purpose: Declare a lightweight registry describing expected runtime helper
//          signatures for debug cross-checks.
// Key invariants: Entries mirror the runtime ABI symbol names and expose a
//                 simplified type classification for comparison.
// Ownership/Lifetime: Registered signatures are stored with static lifetime
//                     and remain valid for the duration of the process.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the debug runtime-signature expectation registry.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace il::runtime::signatures {

/// @brief Describe a parameter or result using a coarse type bucket.
struct SigParam {
    /// @brief Enumerate supported coarse-grained type categories.
    enum Kind {
        I1,  ///< Boolean value.
        I32, ///< 32-bit integral value.
        I64, ///< 64-bit integral value.
        F32, ///< 32-bit floating-point value.
        F64, ///< 64-bit floating-point value.
        Ptr, ///< Generic pointer value.
        Str  ///< String value (IL str type, runtime rt_string).
    } kind;  ///< Active type classification.
};

/// @brief Capture the expected signature shape for a runtime helper.
struct Signature {
    std::string name;                  ///< Canonical runtime symbol name.
    std::vector<SigParam> params;      ///< Parameter type sequence.
    std::vector<SigParam> rets;        ///< Result type sequence (empty for void).
    bool nothrow = false;              ///< Helper is guaranteed not to throw.
    bool readonly = false;             ///< Helper may read memory but performs no writes.
    bool pure = false;                 ///< Helper is free of side effects and memory access.
    std::uint64_t consumedArgMask = 0; ///< IL-visible args whose ownership is consumed.
    std::uint64_t retainedArgMask = 0; ///< IL-visible args whose reference count is retained.
    std::uint64_t ownedOutArgMask = 0; ///< Pointer args that receive an owned reference.
    bool returnsOwned = false;         ///< Helper returns an owned reference/string handle.
    bool mayAllocate = false;          ///< Helper may allocate runtime-managed storage.
    bool returnsKnownObject = false;   ///< Result supports object-specific RC helpers.
};

/// @brief Register an expected runtime signature in the debug registry.
/// @param signature Signature metadata to append.
void register_signature(const Signature &signature);

/// @brief Access the registered runtime signature expectations.
/// @return Reference to the stable list of registered signatures.
const std::vector<Signature> &all_signatures();

/// @brief Look up normalized memory-effect flags without allocating.
/// @param name Runtime symbol name.
/// @param pure Receives the registered pure flag on success.
/// @param readonly Receives the registered readonly flag on success.
/// @return True when a matching dynamic signature exists.
bool find_signature_effects(std::string_view name, bool &pure, bool &readonly);

/// @brief Return a monotonically increasing registry version.
/// @details The version changes only when a new unique signature is appended.
/// @return Current append-generation counter.
std::size_t registry_version();

/// @brief Helper to construct a signature from initializer lists.
/// @param name Runtime symbol name to register.
/// @param params Ordered parameter kinds.
/// @param returns Ordered result kinds.
/// @param nothrow Whether the helper cannot throw or trap.
/// @param readonly Whether the helper reads but does not write memory.
/// @param pure Whether the helper has no observable side effects.
/// @param consumedArgMask Bits for arguments whose ownership is consumed.
/// @param retainedArgMask Bits for arguments retained by the helper.
/// @param returnsOwned Whether the result is an owned managed reference.
/// @param mayAllocate Whether the helper may allocate managed storage.
/// @param ownedOutArgMask Bits for pointer arguments receiving owned references.
/// @param returnsKnownObject Whether the result supports object-specific
///                           reference counting.
/// @return Materialised @ref Signature for registration.
inline Signature make_signature(std::string name,
                                std::initializer_list<SigParam::Kind> params,
                                std::initializer_list<SigParam::Kind> returns = {},
                                bool nothrow = false,
                                bool readonly = false,
                                bool pure = false,
                                std::uint64_t consumedArgMask = 0,
                                std::uint64_t retainedArgMask = 0,
                                bool returnsOwned = false,
                                bool mayAllocate = false,
                                std::uint64_t ownedOutArgMask = 0,
                                bool returnsKnownObject = false) {
    Signature signature;
    signature.name = std::move(name);
    signature.params.reserve(params.size());
    for (auto kind : params)
        signature.params.push_back(SigParam{kind});
    signature.rets.reserve(returns.size());
    for (auto kind : returns)
        signature.rets.push_back(SigParam{kind});
    signature.nothrow = nothrow;
    signature.readonly = readonly;
    signature.pure = pure;
    signature.consumedArgMask = consumedArgMask;
    signature.retainedArgMask = retainedArgMask;
    signature.ownedOutArgMask = ownedOutArgMask;
    signature.returnsOwned = returnsOwned;
    signature.mayAllocate = mayAllocate;
    signature.returnsKnownObject = returnsKnownObject;
    return signature;
}

} // namespace il::runtime::signatures
