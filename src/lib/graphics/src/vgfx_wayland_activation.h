//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_activation.h
// Purpose: Request compositor-authorized activation from recent seat interaction.
// Key invariants: Requests are issued only with xdg-activation and a non-zero seat serial.
// Ownership/Lifetime: Owns an optional pending token proxy and borrows all other objects.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Wayland xdg-activation-v1 request state and lifecycle API.
/// @details Defines a per-window activation handshake that borrows connection,
///          input, surface, and application-ID state while owning at most one
///          pending compositor token proxy.

#pragma once

#include "vgfx_wayland_input.h"

/// @brief State for one compositor-authorized Wayland activation requester.
/// @invariant At most one token request is pending at a time.
typedef struct vgfx_wayland_activation {
    /// @brief Borrowed connection and protocol API.
    vgfx_wayland_connection_t *connection;
    /// @brief Borrowed source of recent seat serials.
    vgfx_wayland_input_t *input;
    /// @brief Borrowed surface to activate.
    struct wl_proxy *surface;
    /// @brief Owned pending token proxy, or NULL.
    struct xdg_activation_token_v1 *token;
    /// @brief Borrowed stable application identifier.
    const char *app_id;
    /// @brief Successfully submitted activation count.
    uint64_t completed;
} vgfx_wayland_activation_t;

/// @brief Initialize activation request state around borrowed Wayland objects.
/// @details Clears prior bytes, stores borrowed dependencies, and substitutes
///          `org.zanna.app` for a missing or empty application ID.
/// @param activation State to initialize; NULL is ignored.
/// @param connection Borrowed live Wayland connection.
/// @param input Borrowed input state used to obtain an interaction serial.
/// @param surface Borrowed xdg-associated surface to activate.
/// @param app_id Borrowed stable application ID, or NULL/empty for the default.
void vgfx_wayland_activation_init(vgfx_wayland_activation_t *activation,
                                  vgfx_wayland_connection_t *connection,
                                  vgfx_wayland_input_t *input,
                                  struct wl_proxy *surface,
                                  const char *app_id);

/// @brief Begin an xdg-activation token handshake.
/// @details Requires the advertised protocol, a surface, seat, recent keyboard
///          or pointer serial, and no request already pending.  It configures
///          serial, app ID, and surface before committing the asynchronous token.
/// @param activation Initialized activation state.
/// @return 1 when a token request was committed, otherwise 0.
int vgfx_wayland_activation_request(vgfx_wayland_activation_t *activation);

/// @brief Cancel any pending token and clear activation state.
/// @param activation State to close; NULL is ignored.
void vgfx_wayland_activation_close(vgfx_wayland_activation_t *activation);

/// @brief Select the most recent usable input serial for activation.
/// @details Prefers a keyboard serial and falls back to the pointer serial.
/// @param input Input state to inspect.
/// @return Recent serial, or zero when unavailable.
uint32_t vgfx_wayland_activation_serial(const vgfx_wayland_input_t *input);
