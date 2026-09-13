//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_service_hooks.h
// Purpose: Base-level hook slots that let optional runtime components (the
//          Zanna.Services platform layer) observe frame polls and graphics
//          device creation without a link dependency in either direction.
// Key invariants:
//   - The frame-pump slot holds at most one callback; publishing replaces it
//     atomically and NULL detaches it.
//   - Running the slot with no callback installed is a cheap no-op, so frame
//     loops may call it unconditionally.
//   - The GPU-presenter note is a sticky process-wide flag; it is never cleared.
// Ownership/Lifetime:
//   - The module owns only atomic process state; no heap allocation.
//   - Callbacks must remain valid until they are detached.
// Links: src/runtime/core/rt_service_hooks.c,
//        src/runtime/services/rt_services.c,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_service_hooks.h
 * @brief Declares Base-level slots used by optional runtime services.
 * @details Graphics frame loops (Canvas.Poll, Canvas3D.Poll) call
 *          @ref rt_service_hooks_run_frame_pump once per frame, and Canvas3D
 *          reports the first GPU-presented window through
 *          @ref rt_service_hooks_note_gpu_presenter. The platform services layer
 *          installs its pump while a provider is started and reads the
 *          presenter flag to warn about overlay ordering. Neither side links
 *          against the other; both only depend on Base.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Callback invoked once per polled frame while installed.
/// @details Called on the thread that polls the canvas (the main thread in
///          every supported frame loop). The callback must not re-enter a
///          canvas poll.
typedef void (*rt_service_hooks_frame_pump_fn)(void);

/// @brief Publish or detach the frame-pump callback.
/// @details Atomically replaces any previously installed callback. Frame loops
///          that load the slot concurrently observe either the previous or the
///          new value.
/// @param fn Callback to install, or NULL to detach the current one.
void rt_service_hooks_set_frame_pump(rt_service_hooks_frame_pump_fn fn);

/// @brief Invoke the installed frame-pump callback, if any.
/// @details Frame loops call this once per poll. With no callback installed the
///          call performs one atomic load and returns.
void rt_service_hooks_run_frame_pump(void);

/// @brief Record that a window now presents through a GPU backend.
/// @details Sticky and idempotent. Canvas3D calls this after a hardware backend
///          context is created for a window, which is the point where a
///          platform overlay must already be hooked.
void rt_service_hooks_note_gpu_presenter(void);

/// @brief Report whether any window has presented through a GPU backend.
/// @return 1 once @ref rt_service_hooks_note_gpu_presenter has been called in
///         this process, otherwise 0.
int8_t rt_service_hooks_gpu_presenter_created(void);

#ifdef __cplusplus
}
#endif
