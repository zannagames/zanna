//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_automation_bridge.h
/// @brief Declares the narrow C/C++ bridge from managed GUI apps to TestHarness automation.
///
/// @details
/// The bridge validates application liveness and returns only opaque, borrowed
/// window/root pointers plus a scheduler-aware event timestamp. Keeping toolkit
/// implementation types out of this boundary lets the C++ harness author input
/// without exposing the private `rt_gui_app_t` layout.
///
// File: src/runtime/graphics/gui/rt_gui_automation_bridge.h
// Purpose: Narrow C/C++ bridge that exposes validated, borrowed GUI App state
//          required by the runtime TestHarness without publishing rt_gui_app_t.
// Key invariants:
//   - App identity and liveness are validated on every snapshot request.
//   - Returned window and root pointers are borrowed and main-thread-only.
//   - The bridge contains no C-only flexible arrays or toolkit implementation types.
// Ownership/Lifetime:
//   - Snapshot values do not retain the App or either borrowed native pointer.
//   - Callers retain the managed App separately and refresh the snapshot per operation.
// Links: src/runtime/graphics/gui/rt_gui_app.c,
//        src/runtime/graphics/gui/rt_gui_ide.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Borrowed automation-facing state for one validated GUI application.
/// @details The opaque window is a `vgfx_window_t` in graphics code and the opaque root is a
///          `vg_widget_t *`. Keeping both fields opaque prevents this C/C++ boundary header from
///          leaking the C-only GUI implementation layout. Values are valid only until the next
///          app operation that can destroy or replace their owning resource.
typedef struct rt_gui_automation_app_view {
    void *window;          ///< Borrowed platform window, or NULL when unavailable.
    void *root;            ///< Borrowed semantic/widget root, or NULL when unavailable.
    int64_t event_time_ms; ///< Non-negative scheduler-aware timestamp for authored input.
} rt_gui_automation_app_view_t;

/// @brief Validate a managed App and capture the borrowed state required by TestHarness.
/// @details Explicitly destroyed, unrelated, and null handles are rejected. The output is cleared
///          before validation so callers never observe stale native pointers after failure. The
///          timestamp follows the app's deterministic scheduler clock once initialized and falls
///          back to the GUI monotonic clock before its first rendered frame.
/// @param app Candidate managed Zanna.GUI.App handle.
/// @param out_view Destination for borrowed window/root state and authored-event timestamp.
/// @return 1 for a live app with a live window, otherwise 0.
int8_t rt_gui_automation_snapshot_app(void *app, rt_gui_automation_app_view_t *out_view);

#ifdef __cplusplus
}
#endif
