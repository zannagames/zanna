//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_data.h
// Purpose: Implement Wayland clipboard selections and file drag-and-drop.
// Key invariants:
//   - All transfer descriptors are nonblocking and payloads are size bounded.
//   - Offer proxies remain alive until their selection or drag role ends.
// Ownership/Lifetime: One data object owns its device, sources, offers, and transfer buffers.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Wayland clipboard selection and file drag-and-drop state.
/// @details Defines bounded offer inventories and nonblocking transfer slots
///          used by the wl_data_device implementation.

#pragma once

#include "vgfx_wayland_input.h"

#include <stddef.h>
#include <stdint.h>

/// @brief One advertised Wayland data offer and its recognized MIME types.
typedef struct vgfx_wayland_offer {
    /// @brief Owned wl_data_offer proxy and intrusive-list link.
    struct wl_proxy *proxy;
    struct vgfx_wayland_offer *next;
    /// @brief Supported text/URI MIME flags learned from offer callbacks.
    int32_t text_utf8;
    int32_t text_plain;
    int32_t uri_list;
    uint32_t action;
} vgfx_wayland_offer_t;

/// @brief One bounded asynchronous pipe transfer.
typedef struct vgfx_wayland_transfer {
    /// @brief Nonblocking descriptor, or -1 when this slot is free.
    int32_t fd;
    /// @brief Direction/type/failure flags.
    int32_t outbound;
    int32_t uri_list;
    int32_t failed;
    /// @brief Owned bytes plus live size, allocation capacity, and write offset.
    char *bytes;
    size_t size;
    size_t capacity;
    size_t offset;
} vgfx_wayland_transfer_t;

/// @brief Per-window Wayland data-device, selection, DND, and transfer state.
typedef struct vgfx_wayland_data {
    /// @brief Borrowed integration objects.
    vgfx_wayland_connection_t *connection;
    vgfx_wayland_input_t *input;
    struct vgfx_window *window;
    /// @brief Owned data-device and current local data-source proxies.
    struct wl_proxy *device;
    struct wl_proxy *source;
    /// @brief Owned offer list and borrowed role pointers into that list.
    vgfx_wayland_offer_t *offers;
    vgfx_wayland_offer_t *selection;
    vgfx_wayland_offer_t *drag;
    /// @brief Owned local UTF-8 clipboard text.
    char *local_text;
    /// @brief Latest logical DND position.
    int32_t drag_x;
    int32_t drag_y;
    /// @brief Local selection publication state.
    int32_t pending_selection;
    int32_t owns_selection;
    /// @brief Fixed pool of concurrent nonblocking transfers.
    vgfx_wayland_transfer_t transfers[8];
} vgfx_wayland_data_t;

/// @brief Initialize optional wl_data_device integration for one window.
/// @details Always initializes transfer slots; when the compositor lacks a
///          data-device manager it succeeds in degraded local-only mode.
/// @param data Destination state.
/// @param connection Borrowed initialized connection.
/// @param input Borrowed seat-input state.
/// @param window Borrowed owning core window.
/// @return 1 on success, otherwise 0.
int vgfx_wayland_data_open(vgfx_wayland_data_t *data,
                           vgfx_wayland_connection_t *connection,
                           vgfx_wayland_input_t *input,
                           struct vgfx_window *window);

/// @brief Cancel transfers and release sources, offers, device, and local text.
/// @param data State to close; NULL is ignored.
void vgfx_wayland_data_close(vgfx_wayland_data_t *data);

/// @brief Advance all nonblocking transfers and deferred selection publication.
/// @param data Initialized data-device state; NULL is ignored.
void vgfx_wayland_data_tick(vgfx_wayland_data_t *data);

/// @brief Test whether local or offered non-empty text is available.
/// @param data Data-device state to inspect.
/// @return 1 when text can be retrieved, otherwise 0.
int vgfx_wayland_data_has_text(const vgfx_wayland_data_t *data);

/// @brief Return a caller-owned copy of local or compositor-offered text.
/// @details External selection retrieval pumps its bounded nonblocking transfer
///          for at most one second.
/// @param data Data-device state to query.
/// @return Heap-allocated NUL-terminated text, or NULL on absence/failure.
char *vgfx_wayland_data_get_text(vgfx_wayland_data_t *data);

/// @brief Publish or clear UTF-8 clipboard text.
/// @details Text is limited to 8 MiB.  Publication is deferred until a recent
///          seat serial exists and retained locally in the meantime.
/// @param data Data-device state to update.
/// @param text UTF-8 text to copy, or NULL to clear.
/// @return 1 when accepted/published, otherwise 0.
int vgfx_wayland_data_set_text(vgfx_wayland_data_t *data, const char *text);
