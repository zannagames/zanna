//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/pass/PassManager.hpp
// Purpose: Declare a lightweight, instrumentation-friendly pass manager façade.
// Key invariants: Pass callbacks are invoked in registration order; instrumentation
// hooks run around each pass invocation when provided.
// Ownership/Lifetime: The façade stores pass callbacks by value and does not own
//                     captured state referenced by those callbacks.
// Links: docs/internals/architecture.md#passes, src/pass/PassManager.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/pass/PassManager.hpp
 * @brief Declares a callback-based pass sequencer with optional instrumentation.
 *
 * @details
 * The shared pass manager maps caller-selected string identifiers to type-erased
 * callbacks and runs identifiers in pipeline order. Optional hooks observe the
 * state immediately before and after successful passes or verify state between
 * a pass and its after-hook.
 *
 * Callables are owned by value, but any state captured by reference remains the
 * caller's responsibility. The manager performs no synchronization and does
 * not catch exceptions thrown by callbacks.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zanna::pass
{

/**
 * @brief Sequences named transformation callbacks across Zanna subsystems.
 *
 * Subsystems register callbacks that close over their own modules, diagnostics,
 * or target state. `PassManager` owns those callbacks and their identifiers,
 * but deliberately knows nothing about the captured execution context.
 *
 * @invariant Each identifier maps to at most one active callback.
 * @invariant `runPipeline()` visits identifiers in their vector order and stops
 *            at the first missing pass, failed pass, or failed verification.
 *
 * @ownership Registered callbacks and hooks are owned by the manager. Resources
 *            referenced by their captures must outlive every invocation.
 */
class PassManager
{
  public:
    /// @brief Owning, ordered list of pass identifiers forming a pipeline.
    using Pipeline = std::vector<std::string>;

    /**
     * @brief Type-erased operation that executes one registered pass.
     * @return `true` when execution succeeds; `false` to stop the pipeline.
     */
    using PassCallback = std::function<bool()>;

    /**
     * @brief Type-erased observer invoked before or after a pass.
     * @param id Borrowed identifier valid for the duration of the hook call.
     */
    using PrintHook = std::function<void(std::string_view id)>;

    /**
     * @brief Type-erased verifier invoked after a successful pass callback.
     * @param id Borrowed identifier of the pass that just completed.
     * @return `true` when state is valid; `false` to stop the pipeline.
     */
    using VerifyHook = std::function<bool(std::string_view id)>;

    /**
     * @brief Register or replace the callback associated with an identifier.
     *
     * Both arguments are moved into manager-owned storage. Reusing an
     * identifier atomically replaces the prior callback from the perspective of
     * later, non-concurrent calls to `runPipeline()`.
     *
     * @param id Stable identifier referenced by pipelines.
     * @param callback Non-empty callable invoked when `id` is encountered.
     *
     * @pre `callback` is invocable; an empty `std::function` would throw when
     *      the pipeline attempts to execute it.
     */
    void registerPass(std::string id, PassCallback callback);

    /**
     * @brief Install or clear the hook invoked before each pipeline entry.
     * @param hook Callback stored by value, or an empty function to disable the
     *             hook.
     *
     * @note The hook runs before pass lookup, so it also observes an identifier
     *       that ultimately proves unregistered.
     */
    void setPrintBeforeHook(PrintHook hook);

    /**
     * @brief Install or clear the hook invoked after successful verification.
     * @param hook Callback stored by value, or an empty function to disable the
     *             hook.
     *
     * @note The hook is skipped when pass execution or verification fails.
     */
    void setPrintAfterHook(PrintHook hook);

    /**
     * @brief Install or clear verification after every successful pass.
     * @param hook Verifier stored by value, or an empty function to disable
     *             per-pass verification.
     *
     * @note Verification runs before the configured after-hook.
     */
    void setVerifyEachHook(VerifyHook hook);

    /**
     * @brief Execute a pipeline in identifier order.
     *
     * For each entry, the manager invokes the before-hook, looks up and runs the
     * pass, invokes the verifier, then invokes the after-hook. Processing stops
     * immediately when lookup fails or a pass or verifier returns `false`.
     *
     * @param pipeline Borrowed ordered sequence; it is not retained after the
     *                 call.
     * @return `true` when every entry executes and verifies successfully,
     *         including when the pipeline is empty; otherwise `false`.
     *
     * @note Exceptions from any callback propagate to the caller.
     */
    bool runPipeline(const Pipeline &pipeline) const;

  private:
    std::unordered_map<std::string, PassCallback> passes_; ///< Registered callbacks by id.
    PrintHook printBefore_{};                              ///< Optional pre-pass observer.
    PrintHook printAfter_{};                               ///< Optional post-verification observer.
    VerifyHook verifyEach_{};                              ///< Optional per-pass verifier.
};

} // namespace zanna::pass
