//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_atspi_linux.h
/// @brief Declares the optional Linux AT-SPI accessibility projection.
///
/// @details
/// These internal entry points attach immutable widget snapshots to the
/// accessibility bus, synchronize mutations back onto the GUI thread, and
/// expose narrow diagnostic seams. All window and widget handles are borrowed;
/// unavailable GIO or bus services make the bridge a safe no-op.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_atspi_linux.h
// Purpose: Optional native Linux AT-SPI projection entry points.
// Key invariants:
//   - A missing accessibility bus or GIO runtime silently preserves the in-process tree.
//   - Every successfully attached application is unregistered and joined before destruction.
// Ownership/Lifetime: Attach borrows the window/root; detach ends all native bridge activity.
// Links: org.a11y.atspi.Socket, rt_gui_accessibility_platform.h
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_gui_accessibility_platform.h"

#include <stddef.h>
#include <stdint.h>

/// @brief Snapshot and publish one GUI window on the AT-SPI bus.
/// @param window Borrowed live window used as the bridge key.
/// @param root Borrowed live semantic root.
void rt_gui_atspi_linux_attach(vgfx_window_t window, vg_widget_t *root);

/// @brief Stop and release the AT-SPI bridge for a window.
/// @param window Borrowed window whose projection is removed.
void rt_gui_atspi_linux_detach(vgfx_window_t window);

/// @brief Notify the bridge of a changed semantic node.
/// @param window Borrowed window whose bridge is searched.
/// @param widget Borrowed changed widget; may be NULL for rebuild-only invalidation.
void rt_gui_atspi_linux_notify(vgfx_window_t window, vg_widget_t *widget);

/// @brief Service queued actions and rebuild dirty snapshot state.
/// @param window Borrowed window whose bridge is synchronized.
/// @param root Borrowed current semantic root.
void rt_gui_atspi_linux_sync(vgfx_window_t window, vg_widget_t *root);

/// @brief Return the number of registered snapshot nodes for internal Linux tests/diagnostics.
/// @param window Borrowed window used as the bridge key.
/// @return Snapshot node count, or 0 when no bridge is attached.
size_t rt_gui_atspi_linux_snapshot_count(vgfx_window_t window);

/// @brief Materialize the AT-SPI Cache payload and return its item count for internal tests.
/// @param window Borrowed window used as the bridge key.
/// @return Cache item count, or 0 when unavailable.
size_t rt_gui_atspi_linux_cache_item_count(vgfx_window_t window);

enum {
    RT_GUI_ATSPI_TEST_ACTION = 1,
    RT_GUI_ATSPI_TEST_CARET = 2,
    RT_GUI_ATSPI_TEST_VALUE = 3,
};

/// @brief Exercise the same bounded worker-to-GUI mutation queue used by D-Bus callbacks.
/// @param window Borrowed window used as the bridge key.
/// @param widget_id Stable snapshot widget ID.
/// @param kind One of the internal AT-SPI test request kinds.
/// @param value Numeric request payload.
/// @return GUI-thread request result, or 0 when invalid/unavailable.
int rt_gui_atspi_linux_test_request(vgfx_window_t window,
                                    uint64_t widget_id,
                                    int kind,
                                    double value);

/// @brief Emit an AT-SPI live-region announcement for a snapshotted widget.
/// @param window Borrowed window whose bridge owns the widget snapshot.
/// @param widget Borrowed announcement source widget.
/// @param text Borrowed non-empty UTF-8 announcement text.
/// @param mode Polite or assertive live-region urgency.
void rt_gui_atspi_linux_announce(vgfx_window_t window,
                                 vg_widget_t *widget,
                                 const char *text,
                                 vg_live_region_mode_t mode);
