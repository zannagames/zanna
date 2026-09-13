//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_service_hooks.c
// Purpose: Implements the Base-level frame-pump slot and GPU-presenter flag
//          used by the optional Zanna.Services platform layer.
// Key invariants:
//   - All state is atomic, so frame loops and the services layer never need a
//     lock to publish or read it.
//   - An empty frame-pump slot costs one atomic load per poll.
// Ownership/Lifetime:
//   - No heap ownership; state lasts for the process lifetime.
// Links: src/runtime/core/rt_service_hooks.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_service_hooks.c
 * @brief Implements lock-free hook slots shared by frame loops and services.
 * @details The frame-pump callback pointer and the sticky GPU-presenter flag
 *          are stored with C11 atomics so the graphics component can call
 *          into the services component (and the services component can read
 *          graphics state) without either archive referencing the other.
 */

#include "rt_service_hooks.h"

#include <stdatomic.h>
#include <stdbool.h>

/// @brief Installed frame-pump callback, or NULL when no service is started.
static _Atomic(rt_service_hooks_frame_pump_fn) g_frame_pump;

/// @brief Sticky flag set once a window has presented through a GPU backend.
static atomic_bool g_gpu_presenter_created;

/// @brief Publish or detach the frame-pump callback.
/// @details Replaces the slot atomically; NULL detaches. The services layer
///          installs its pump when a provider starts and detaches it on
///          shutdown.
/// @param fn Callback to install, or NULL.
void rt_service_hooks_set_frame_pump(rt_service_hooks_frame_pump_fn fn) {
    atomic_store_explicit(&g_frame_pump, fn, memory_order_release);
}

/// @brief Invoke the installed frame-pump callback, if any.
/// @details Loads the slot once and calls the observed callback. A concurrent
///          detach may still see the previous callback run once, which the
///          services pump tolerates because it re-checks its own started state.
void rt_service_hooks_run_frame_pump(void) {
    rt_service_hooks_frame_pump_fn fn = atomic_load_explicit(&g_frame_pump, memory_order_acquire);
    if (fn)
        fn();
}

/// @brief Record that a window presents through a GPU backend.
/// @details Idempotent; the flag is never cleared.
void rt_service_hooks_note_gpu_presenter(void) {
    atomic_store_explicit(&g_gpu_presenter_created, true, memory_order_release);
}

/// @brief Report whether a GPU-presented window has been created.
/// @return 1 after @ref rt_service_hooks_note_gpu_presenter, otherwise 0.
int8_t rt_service_hooks_gpu_presenter_created(void) {
    return atomic_load_explicit(&g_gpu_presenter_created, memory_order_acquire) ? 1 : 0;
}
