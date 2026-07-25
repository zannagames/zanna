//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/debug/DebugFrontend.hpp
// Purpose: Interface a host (e.g. the Zanna Studio debug adapter) implements to drive
//          interactive debugging, plus the plain-data stop descriptor the VM hands
//          it at each pause.
// Key invariants:
//   - DebugStopInfo is pure data: the frontend never touches VM internals.
//   - onStop is called on the VM thread while execution is paused; it returns the
//     next DebugAction (continue/step/...) the VM should apply.
// Ownership/Lifetime:
//   - The frontend is owned by the host and must outlive the VM run.
// Links: src/vm/debug/VMDebug.cpp, src/tools/zanna/DebugAdapter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/debug/DebugFrontend.hpp
 * @brief Defines the host interface and stop snapshots for interactive VM
 *        debugging.
 *
 * @details
 * At each debugger stop, the VM constructs an owning `DebugStopInfo` containing
 * source location, stack frames, and top-frame locals, then synchronously calls
 * `DebugFrontend::onStop()` on the VM thread. The host returns the next
 * `DebugAction` before execution can resume.
 *
 * Expandable values use stop-scoped integer handles resolved through a shared
 * `DebugVarExpander`. Those handles and the expander become invalid after the
 * stop snapshot is released.
 */

#pragma once

#include "zanna/vm/debug/Debug.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace il::vm {

/// @brief Owning source-level snapshot of one paused call-stack frame.
struct DebugFrameInfo {
    std::string function; ///< Function name (without leading '@').
    std::string path;     ///< Source file of the frame, or empty when unknown.
    uint32_t line = 0;    ///< 1-based line, or 0 when unknown.
};

/**
 * @brief Owns the name, type, summary, and expansion metadata for one local.
 *
 * Composite values advertise a positive `varRef` and a `childCount`; a host
 * expands them one level at a time through the stop's `DebugVarExpander`.
 * Leaves keep `varRef == 0`, preserving the historical flat representation.
 */
struct DebugLocalInfo {
    std::string name;       ///< Source-level variable name.
    std::string value;      ///< Formatted value (a compact summary for composites).
    std::string type;       ///< Type label (e.g. "i64", "f64", "str", "List").
    int64_t varRef = 0;     ///< >0 when expandable this stop; 0 = leaf. Dies on resume.
    int64_t childCount = 0; ///< Child count when varRef>0; 0 for leaves.
};

/**
 * @brief Provides lazy, one-level children for structured debugger values.
 *
 * The VM creates an implementation for one stop and shares ownership through
 * `DebugStopInfo::vars`. Every reference it advertises is scoped to that
 * expander. Implementations expand exactly one level; recursion depth is driven
 * explicitly by subsequent host requests.
 */
class DebugVarExpander {
  public:
    /// @brief Destroy a concrete stop-scoped expander through the interface.
    virtual ~DebugVarExpander() = default;

    /**
     * @brief Materialize a bounded slice of a structured value's direct children.
     * @param ref Positive handle advertised by this expander during the current
     *            stop.
     * @param start Zero-based index of the first requested child.
     * @param count Maximum number of children to return.
     * @return Owning child snapshots for the requested slice, or an empty vector
     *         when the handle or range is unavailable.
     */
    virtual std::vector<DebugLocalInfo> expand(int64_t ref, int64_t start, int64_t count) = 0;
};

/**
 * @brief Owns the serializable state presented for one debugger pause.
 *
 * Frames are ordered most-recent first and locals describe the top frame.
 * `vars`, when present, keeps all expansion handles alive for exactly the
 * snapshot lifetime.
 */
struct DebugStopInfo {
    std::string reason;                 ///< "breakpoint" | "step" | "step-over" | ...
    std::string path;                   ///< Source file of the top frame.
    uint32_t line = 0;                  ///< 1-based line of the top frame; 0 if unknown.
    uint32_t column = 0;                ///< 1-based column; 0 if unknown.
    std::vector<DebugFrameInfo> frames; ///< Backtrace, most-recent first.
    std::vector<DebugLocalInfo> locals; ///< Named locals of the top frame.
    /// @brief Expander for locals whose varRef>0, or null when nothing is
    ///        expandable at this stop. Lifetime is tied to this DebugStopInfo.
    std::shared_ptr<DebugVarExpander> vars;
};

/**
 * @brief Host-implemented synchronous driver for interactive VM debugging.
 *
 * The VM borrows this interface from its host and invokes it on the VM thread
 * while execution is stopped. Implementations may communicate with an IDE or
 * debugger transport but must return an action before the VM resumes.
 */
class DebugFrontend {
  public:
    /// @brief Destroy a host implementation through the interface.
    virtual ~DebugFrontend() = default;

    /**
     * @brief Present a pause snapshot and choose the VM's next debug action.
     * @param info Borrowed snapshot valid for the duration of the call. Copy any
     *             data needed after return; expansion handles are not reusable
     *             after execution resumes.
     * @return Action applied immediately after the callback returns.
     *
     * @note The callback runs synchronously on the VM thread.
     */
    virtual DebugAction onStop(const DebugStopInfo &info) = 0;
};

} // namespace il::vm
