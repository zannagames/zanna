//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares runtime tracking and declaration helpers for BASIC
// lowering, managing the interface between generated IL and the Zanna runtime
// library.
//
// Runtime Library Integration:
// Many BASIC operations cannot be expressed directly in IL and require calls
// to runtime library functions:
//
// - String operations: Concatenation, comparison, substring extraction
// - I/O operations: PRINT, INPUT, file operations
// - Array operations: Dynamic allocation, bounds checking, element access
// - Built-in functions: Mathematical functions (SIN, COS), string functions
//   (LEFT$, MID$), type conversions
//
// Runtime Declaration Management:
// The LowerRuntime helper ensures that:
// - Runtime functions are declared in IL before use
// - Each runtime function is declared exactly once per IL module
// - Runtime function signatures match the actual runtime library exports
// - Calls to runtime functions use correct calling conventions
//
// Runtime Function Categories:
// - String manipulation (concat, compare, substring, etc.)
// - Terminal and file-channel I/O
// - Array allocation, access, resizing, and bounds handling
// - Mathematical functions and type conversions
//
// Declaration Tracking:
// To ensure each runtime function is declared exactly once:
// - Scanning and emission record requested features and exact call spellings
// - A final declaration pass resolves registry signatures and aliases
// - Bitsets track feature requirements, while declaration names are deduplicated
//
// Integration:
// - Used by: Lowering helpers (LowerExprBuiltin, LowerStmt_IO, etc.)
// - Operates on: Lowerer state (IRBuilder, IL module)
// - Coordinates with: RuntimeSignatures for function signatures
//
// Design Notes:
// - Runtime declarations are module-scoped, not per-function
// - Tracking prevents duplicate declarations
// - Signature validation ensures ABI compatibility
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerRuntime.hpp
 * @brief Declares per-lowering-run runtime helper requirement tracking.
 *
 * RuntimeHelperTracker records feature bits, first-use order for descriptors
 * that request it, and exact call-site spellings used for alias-aware extern
 * selection. It owns bookkeeping only; the IR builder and runtime registry
 * remain external.
 */

#pragma once

#include "il/runtime/RuntimeSignatures.hpp"
#include <bitset>
#include <unordered_set>
#include <vector>

namespace il::build {
class IRBuilder;
} // namespace il::build

namespace il::frontends::basic {

/// @brief Tracks runtime helper usage across scanning and lowering.
/// @details Owned by Lowerer and reset between programs. Ordered descriptors
///          retain first-use order, while request bits and callee spellings are
///          deduplicated.
class RuntimeHelperTracker {
  public:
    /// Runtime feature enumeration shared with the canonical descriptor registry.
    using RuntimeFeature = il::runtime::RuntimeFeature;

    /// @brief Reset helper tracking to an empty state.
    /// @post No feature, ordered entry, tracked feature, or callee name remains.
    void reset();

    /// @brief Mark a runtime helper as required.
    /// @param feature Feature whose request bit should be set.
    /// @pre @p feature is below RuntimeFeature::Count.
    void requestHelper(RuntimeFeature feature);

    /// @brief Query whether a runtime helper has been requested.
    /// @param feature Feature whose request bit should be inspected.
    /// @return True exactly when @ref requestHelper has marked @p feature since
    ///         the most recent reset.
    [[nodiscard]] bool isHelperNeeded(RuntimeFeature feature) const;

    /// @brief Record an ordered runtime helper requirement.
    /// @details Marks the feature requested and records its first-use position
    ///          only when the registry descriptor opts into ordered lowering.
    /// @param feature Feature observed during lowering.
    void trackRuntime(RuntimeFeature feature);

    /// @brief Declare all helpers requested during the current lowering run.
    /// @details Applies registry lowering modes, exact call-site alias choices,
    ///          and ordered replay while deduplicating emitted symbol names.
    /// @param b Builder receiving extern declarations.
    /// @param boundsChecks Whether BoundsChecked registry entries are eligible.
    void declareRequiredRuntime(build::IRBuilder &b, bool boundsChecks) const;

  private:
    /// Hashes RuntimeFeature by its underlying ordinal.
    struct RuntimeFeatureHash {
        /// @param f Feature value to hash.
        /// @return Ordinal converted to `std::size_t`.
        std::size_t operator()(RuntimeFeature f) const;
    };

    /// Number of requestable runtime features, excluding the Count sentinel.
    static constexpr std::size_t kRuntimeFeatureCount =
        static_cast<std::size_t>(RuntimeFeature::Count);

    /// Feature requirement bits.
    std::bitset<kRuntimeFeatureCount> requested_;
    /// Ordered features retained in first-use order.
    std::vector<RuntimeFeature> ordered_;
    /// Features already encountered by trackRuntime().
    std::unordered_set<RuntimeFeature, RuntimeFeatureHash> tracked_;
    /// Exact callee spellings emitted into IL.
    std::unordered_set<std::string> usedNames_;

  public:
    /// @brief Record a runtime callee name observed during lowering.
    /// @details When a call targets a known runtime helper, remember the exact
    ///          symbol spelling so extern declarations can match call sites.
    ///          The name is copied and need not be registry-known.
    /// @param name Exact callee spelling to record.
    void trackCalleeName(std::string_view name);

    /// @brief Enumerate callee names recorded so far.
    /// @return Const reference valid until this tracker is modified or destroyed.
    const std::unordered_set<std::string> &usedNames() const {
        return usedNames_;
    }
};

} // namespace il::frontends::basic
