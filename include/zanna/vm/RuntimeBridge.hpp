//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/RuntimeBridge.hpp
// Purpose: Public-facing extern registration surface for the VM runtime bridge.
//          Supports process-global and per-VM extern registries with optional
//          strict mode for signature mismatch detection.
// Key invariants: Registry lookup and mutation are mutex-protected.
//                 In strict mode, re-registration with a different signature
//                 returns SignatureMismatch instead of silently overwriting.
// Ownership/Lifetime: ExternRegistryPtr owns an initial registry reference via
//                     custom deleter. VMs and worker payloads retain additional
//                     references while they hold raw registry pointers.
// Links: src/vm/RuntimeBridge.cpp, docs/internals/codemap/vm-runtime.md
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/RuntimeBridge.hpp
 * @brief Declares the public registry used to connect VM extern calls to native
 *        handlers.
 *
 * @details
 * Embedders describe a native callback with `ExternDesc`, register it in the
 * process-global registry or an isolated registry, and attach isolated
 * registries to individual VMs. Names are matched case-insensitively for ASCII
 * letters by canonicalizing them to lowercase.
 *
 * Registry operations synchronize their internal state. Registry handles use
 * intrusive reference counting behind an opaque type so VMs and worker
 * payloads can safely extend the lifetime established by `ExternRegistryPtr`.
 */

#pragma once

#include "il/runtime/signatures/Registry.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace il::vm
{

/// @brief Public signature description used by VM extern declarations.
using Signature = il::runtime::signatures::Signature;

/**
 * @brief Describes one native function exposed to IL execution.
 *
 * The descriptor owns its symbolic name and signature metadata. `fn` remains an
 * opaque pointer until registration converts it to the runtime handler ABI
 * `void(void **args, void *result)`.
 *
 * @ownership The descriptor does not own the native function. The code
 *            containing that function must remain loaded while it is registered
 *            or callable by a VM.
 */
struct ExternDesc
{
    std::string name;    ///< Symbolic name used in IL (e.g., "rt_abs_i64").
    Signature signature; ///< Expected parameter and return kinds.
    void *fn = nullptr;  ///< Function pointer matching the runtime handler ABI.
};

/**
 * @brief Canonicalize an extern name for case-insensitive registry matching.
 * @param n Borrowed name bytes to transform.
 * @return An owning copy with each ASCII letter lowercased and every non-ASCII
 *         or non-alphabetic byte preserved.
 */
std::string canonicalizeExternName(std::string_view n);

//===----------------------------------------------------------------------===//
// ExternRegistry API
//===----------------------------------------------------------------------===//

/// @brief Outcomes produced by `registerExternIn`.
enum class ExternRegisterResult
{
    Success,          ///< The new descriptor was inserted or replaced the old one.
    SignatureMismatch ///< Strict mode preserved an existing incompatible entry.
};

/**
 * @brief Opaque, reference-counted registry of native extern descriptions.
 *
 * The concrete mapping, synchronization primitive, strict-mode flag, and
 * intrusive reference count are private to `RuntimeBridge.cpp`.
 */
struct ExternRegistry;

/// @brief Releases the owning reference held by `ExternRegistryPtr`.
struct ExternRegistryDeleter
{
    /**
     * @brief Release one intrusive registry reference.
     * @param reg Registry to release, or null for a no-op.
     */
    void operator()(ExternRegistry *reg) const noexcept;
};

/**
 * @brief Movable owner of one intrusive registry reference.
 *
 * VMs that retain the registry maintain their own references even though their
 * public attachment APIs may expose raw pointers.
 */
using ExternRegistryPtr = std::unique_ptr<ExternRegistry, ExternRegistryDeleter>;

/**
 * @brief Create an empty registry independent of the global singleton.
 * @return A non-null owning handle with strict mode disabled and an initial
 *         intrusive reference count of one.
 *
 * @throws std::bad_alloc If registry allocation fails.
 */
[[nodiscard]] ExternRegistryPtr createExternRegistry();

/**
 * @brief Access the lazily initialized process-global registry.
 * @return A reference that remains valid until process shutdown.
 *
 * @note The caller must not attempt to delete the returned singleton.
 */
ExternRegistry &processGlobalExternRegistry();

/**
 * @brief Insert or replace an extern descriptor in a registry.
 *
 * The key is obtained with `canonicalizeExternName(ext.name)`. Re-registration
 * with structurally equal parameter and return kinds replaces the descriptor
 * even in strict mode. Signature attributes and signature names do not
 * participate in that structural comparison.
 *
 * @param registry Registry to mutate.
 * @param ext Descriptor copied into registry-owned storage.
 * @return `Success` after insertion or replacement. Returns
 *         `SignatureMismatch` only when strict mode rejects an existing key
 *         with different parameter or return kinds.
 *
 * @note Non-strict mode always replaces an existing canonical-name match.
 */
ExternRegisterResult registerExternIn(ExternRegistry &registry, const ExternDesc &ext);

/**
 * @brief Configure incompatible re-registration handling.
 * @param registry Registry whose synchronized strict-mode flag is updated.
 * @param enabled `true` to preserve an existing entry when a replacement has
 *                different parameter or return kinds; `false` to overwrite it.
 *
 * @note Strict mode is disabled for newly created registries.
 */
void setExternRegistryStrictMode(ExternRegistry &registry, bool enabled);

/**
 * @brief Query the synchronized strict-mode setting.
 * @param registry Registry to inspect.
 * @return `true` when incompatible replacements are rejected.
 */
bool isExternRegistryStrictMode(const ExternRegistry &registry);

/**
 * @brief Remove an extern by case-insensitive name.
 * @param registry Registry to mutate.
 * @param name Borrowed name; canonicalization is performed by the function.
 * @return `true` when an entry was erased; `false` when no key matched.
 */
bool unregisterExternIn(ExternRegistry &registry, std::string_view name);

/**
 * @brief Look up an extern by case-insensitive name.
 * @param registry Registry to inspect.
 * @param name Borrowed name; canonicalization is performed by the function.
 * @return Pointer to a thread-local descriptor copy, or null when no key
 *         matches.
 *
 * @warning The returned pointer is valid only on the calling thread and may be
 *          overwritten after eight later successful `findExternIn` lookups on
 *          that thread. Copy the descriptor when a longer lifetime is needed.
 */
const ExternDesc *findExternIn(ExternRegistry &registry, std::string_view name);

class RuntimeBridge;

} // namespace il::vm
