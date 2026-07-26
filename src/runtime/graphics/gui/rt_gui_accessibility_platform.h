//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_accessibility_platform.h
/// @brief Declares the internal native accessibility adapter contract.
///
/// @details
/// Platform implementations normalize desktop appearance preferences and
/// project the toolkit's authoritative semantic tree into native accessibility
/// APIs. All handles are borrowed, unsupported integrations remain safe no-ops,
/// and the runtime's headless accessibility snapshot remains available.
///
// File: src/runtime/graphics/gui/rt_gui_accessibility_platform.h
// Purpose: Internal platform-adapter contract for native GUI accessibility preferences.
//
// Key invariants:
//   - Queries are read-only and never create a native window or event loop.
//   - Every adapter returns normalized zero/one values.
//   - Missing native settings have the deterministic fallback value zero.
//
// Ownership/Lifetime:
//   - Window handles are borrowed for the duration of each call.
//   - The adapter allocates no caller-owned memory and retains no window handle.
//
// Links: src/runtime/graphics/gui/rt_gui_accessibility.c,
//        docs/adr/0107-gui-theme-accessibility-input-and-render-policy.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef ZANNA_ENABLE_GRAPHICS

#include "vg_widget.h"
#include "vgfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Query the host desktop's high-contrast preference.
/// @details The selected platform adapter uses only operating-system/toolkit facilities already
///          available to Zanna. A missing display, unavailable setting, malformed platform value,
///          or NULL window produces the deterministic fallback unless the platform exposes a
///          process-wide preference that does not require a window.
/// @param window Borrowed ZannaGFX window used to reach a native display when required; may be
/// NULL.
/// @return One when the host currently requests high contrast, otherwise zero.
int32_t rt_gui_accessibility_platform_high_contrast(vgfx_window_t window);

/// @brief Query the host desktop's reduced-motion preference.
/// @details A native setting that disables interface animations maps to one. Missing displays,
///          unavailable settings, and malformed values map to zero. The function performs no
///          mutation and does not retain @p window.
/// @param window Borrowed ZannaGFX window used to reach a native display when required; may be
/// NULL.
/// @return One when the host currently requests reduced motion, otherwise zero.
int32_t rt_gui_accessibility_platform_reduced_motion(vgfx_window_t window);

/// @brief Query whether the host desktop currently prefers a dark application appearance.
/// @details The platform adapter observes the native application-theme preference without
///          creating a window or retaining @p window. Missing settings, malformed values, and
///          unavailable desktop services use the documented light fallback (zero). Callers poll
///          this only while `Zanna.GUI.Theme` is in System mode.
/// @param window Borrowed ZannaGFX window used to reach a native view or display when useful; may
///          be NULL when the platform exposes a process-wide setting.
/// @return One for a dark preference and zero for light or unavailable state.
int32_t rt_gui_accessibility_platform_prefers_dark(vgfx_window_t window);

/// @brief Attach the native accessibility projection to one live GUI window.
/// @details The adapter exposes @p root through the platform's accessibility hierarchy when the
///          platform supports a native bridge. Unsupported environments safely keep only the
///          always-available headless snapshot. Repeated attachment replaces the previous root.
/// @param window Borrowed live ZannaGFX window; NULL is a no-op.
/// @param root Borrowed live semantic-tree root; NULL removes any existing projection.
void rt_gui_accessibility_platform_attach(vgfx_window_t window, vg_widget_t *root);

/// @brief Detach any native accessibility projection before window/root destruction.
/// @details After return the adapter retains no semantic element that can query the old widget
///          tree. Calling this for a window without an attached bridge is harmless.
/// @param window Borrowed ZannaGFX window being detached; NULL is a no-op.
void rt_gui_accessibility_platform_detach(vgfx_window_t window);

/// @brief Notify native assistive technology that one live semantic node changed.
/// @details The headless record is already authoritative when this function runs. Adapters may
///          coalesce notifications; unavailable native bridges treat the call as a no-op.
/// @param window Borrowed ZannaGFX window owning @p widget; may be NULL.
/// @param widget Borrowed changed widget; NULL/stale handles are ignored.
void rt_gui_accessibility_platform_notify(vgfx_window_t window, vg_widget_t *widget);

/// @brief Publish coalesced native accessibility-tree changes after layout is current.
/// @param window Borrowed ZannaGFX window owning the semantic tree; may be NULL.
/// @param root Borrowed live semantic-tree root; NULL removes stale native descendants.
void rt_gui_accessibility_platform_sync(vgfx_window_t window, vg_widget_t *root);

/// @brief Project one semantic live-region announcement through the native bridge.
/// @details The text remains stored in the headless widget record independently of projection.
///          Invalid UTF-8, absent bridges, stale widgets, and NULL windows are ignored safely.
/// @param window Borrowed owning ZannaGFX window; may be NULL.
/// @param widget Borrowed announcement source widget; may be NULL or stale.
/// @param text UTF-8 announcement text borrowed for the call; may be NULL to announce empty text.
/// @param mode Live-region urgency (`VG_LIVE_REGION_POLITE` or `VG_LIVE_REGION_ASSERTIVE`).
void rt_gui_accessibility_platform_announce(vgfx_window_t window,
                                            vg_widget_t *widget,
                                            const char *text,
                                            vg_live_region_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif // ZANNA_ENABLE_GRAPHICS
