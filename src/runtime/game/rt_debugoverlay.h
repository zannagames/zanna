//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_debugoverlay.h
/// @file
/// @brief Declares the runtime's canvas-rendered game debug overlay.
//
// Purpose: Debug overlay for displaying FPS, delta time, and custom watched
//   variables during game development. Renders as a semi-transparent panel
//   in the top-right corner of the canvas.
//
// Key invariants:
//   - Maximum of RT_DEBUG_MAX_WATCHES (16) custom watch entries.
//   - FPS is computed as a rolling average over RT_DEBUG_FPS_HISTORY (16) frames.
//   - The overlay is disabled by default; call rt_debugoverlay_enable() to show.
//   - Draw must be called after all other rendering (renders on top).
//
// Ownership/Lifetime:
//   - rt_debugoverlay_new returns one runtime-object reference. Release it with
//     rt_debugoverlay_destroy.
//   - Watch labels are copied into the overlay and are never retained.
//
// Links: src/runtime/game/rt_debugoverlay.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Fixed number of integer-watch slots in each overlay.
#define RT_DEBUG_MAX_WATCHES 16

/// @brief Number of frame deltas included in the rolling FPS calculation.
#define RT_DEBUG_FPS_HISTORY 16

/// @brief Runtime class ID used to validate DebugOverlay handles.
#define RT_DEBUGOVERLAY_CLASS_ID INT64_C(-0x510216)

/// @brief Opaque handle to a mutable DebugOverlay instance.
typedef struct rt_debugoverlay_impl *rt_debugoverlay;

/// @brief Create a new DebugOverlay (disabled by default).
/// @return A new DebugOverlay reference, or `NULL` if allocation fails.
/// @details Frame history and all watch slots begin empty.
rt_debugoverlay rt_debugoverlay_new(void);

/// @brief Release one DebugOverlay reference.
/// @param dbg Overlay to release; `NULL` is a no-op.
/// @details Frees the object when its reference count reaches zero. A non-null
///          handle of another runtime class raises a trap.
void rt_debugoverlay_destroy(rt_debugoverlay dbg);

/// @brief Enable the overlay (visible when Draw is called).
/// @param dbg Overlay to enable; `NULL` is a no-op.
/// @details A non-null handle of another runtime class raises a trap.
void rt_debugoverlay_enable(rt_debugoverlay dbg);

/// @brief Disable the overlay (hidden).
/// @param dbg Overlay to disable; `NULL` is a no-op.
/// @details Frame history and watches may still be updated while disabled. A
///          non-null handle of another runtime class raises a trap.
void rt_debugoverlay_disable(rt_debugoverlay dbg);

/// @brief Toggle the overlay on/off.
/// @param dbg Overlay to toggle; `NULL` is a no-op.
/// @details A non-null handle of another runtime class raises a trap.
void rt_debugoverlay_toggle(rt_debugoverlay dbg);

/// @brief Check if the overlay is enabled.
/// @param dbg Overlay to inspect.
/// @return `1` when enabled; otherwise `0`.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_debugoverlay_is_enabled(rt_debugoverlay dbg);

/// @brief Update the overlay with current frame delta time (ms).
/// @param dbg Overlay whose frame history is updated; `NULL` is a no-op.
/// @param dt_ms Frame duration in milliseconds; negative values become zero.
/// @details Call once per frame to maintain the rolling FPS sample. The newest
///          value replaces the oldest after RT_DEBUG_FPS_HISTORY updates. The
///          lifetime frame count saturates at `INT64_MAX`. A non-null invalid
///          handle raises a runtime trap.
void rt_debugoverlay_update(rt_debugoverlay dbg, int64_t dt_ms);

/// @brief Register or update a named integer watch variable.
/// @param dbg Overlay to mutate; `NULL` is a no-op.
/// @param name Nonempty label of at most 31 bytes with no embedded NUL.
/// @param value The integer value to display.
/// @details Updates an exact-name match or copies the label into the first free
///          slot. Invalid names and additions beyond RT_DEBUG_MAX_WATCHES are
///          silently ignored. The input string is not retained. A non-null
///          invalid overlay handle raises a runtime trap.
void rt_debugoverlay_watch(rt_debugoverlay dbg, rt_string name, int64_t value);

/// @brief Remove a named watch variable.
/// @param dbg Overlay to mutate.
/// @param name Complete label of the watch to remove.
/// @return 1 if found and removed, 0 if not found.
/// @details Null arguments and invalid labels return zero. The input string is
///          not retained or released. A non-null invalid overlay handle raises
///          a runtime trap.
int8_t rt_debugoverlay_unwatch(rt_debugoverlay dbg, rt_string name);

/// @brief Clear all watch variables.
/// @param dbg Overlay to clear; `NULL` is a no-op.
/// @details Frame history and enabled state are unchanged. A non-null invalid
///          handle raises a runtime trap.
void rt_debugoverlay_clear(rt_debugoverlay dbg);

/// @brief Get the current computed FPS (rolling average).
/// @param dbg Overlay whose history is queried.
/// @return `1000 * sample_count / sum(dt_ms)` using integer division, or zero
///         before any positive total delta has been recorded.
/// @details The delta sum saturates at `INT64_MAX`. A non-null invalid handle
///          raises a runtime trap.
int64_t rt_debugoverlay_get_fps(rt_debugoverlay dbg);

/// @brief Draw the overlay onto a canvas. Must be called after all other
///        rendering so the overlay appears on top.
/// @param dbg Overlay to render.
/// @param canvas_ptr Opaque pointer to an rt_canvas.
/// @details Null arguments and disabled overlays return without drawing. The
///          panel contains color-coded FPS, the latest frame delta, and active
///          watches in slot order. Watch labels are shortened to 28 display
///          bytes at a UTF-8 continuation-byte boundary. A non-null invalid
///          overlay handle raises a runtime trap.
void rt_debugoverlay_draw(rt_debugoverlay dbg, void *canvas_ptr);

#ifdef __cplusplus
}
#endif
