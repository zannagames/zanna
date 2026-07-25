//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_activation.c
// Purpose: Implement the xdg-activation-v1 token handshake.
// Key invariants: See vgfx_wayland_activation.h.
// Ownership/Lifetime: See vgfx_wayland_activation.h.
// Links: src/lib/graphics/src/vgfx_wayland_activation.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implementation of the xdg-activation-v1 token handshake.

#include "vgfx_wayland_activation.h"

#include <string.h>

enum {
    XDG_ACTIVATION_GET_TOKEN = 1,
    XDG_ACTIVATION_ACTIVATE = 2,
    XDG_TOKEN_SET_SERIAL = 0,
    XDG_TOKEN_SET_APP_ID = 1,
    XDG_TOKEN_SET_SURFACE = 2,
    XDG_TOKEN_COMMIT = 3,
    XDG_TOKEN_DESTROY = 4,
};

/// @brief Listener ABI for an xdg activation-token completion callback.
typedef struct {
    void (*done)(void *, struct xdg_activation_token_v1 *, const char *token);
} vgfx_activation_token_listener_t;

/// @copydoc vgfx_wayland_activation_serial
uint32_t vgfx_wayland_activation_serial(const vgfx_wayland_input_t *input) {
    if (!input)
        return 0;
    return input->keyboard_serial ? input->keyboard_serial : input->pointer_serial;
}

/// @brief Destroy and clear the currently owned activation-token proxy.
/// @param activation Activation state whose token should be released.
static void vgfx_activation_destroy_token(vgfx_wayland_activation_t *activation) {
    if (!activation || !activation->token || !activation->connection)
        return;
    (void)activation->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)activation->token,
        XDG_TOKEN_DESTROY,
        NULL,
        1,
        VGFX_WL_MARSHAL_FLAG_DESTROY);
    activation->token = NULL;
}

/// @brief Submit a compositor-issued token and finish its request lifecycle.
/// @details Activates the configured surface when a non-empty token and all
///          borrowed dependencies remain valid, increments the completion
///          counter, and destroys the one-shot token proxy in every case.
/// @param data Borrowed `vgfx_wayland_activation_t` listener context.
/// @param token_proxy Token proxy that produced the callback.
/// @param token Borrowed compositor-issued token string.
static void vgfx_activation_done(void *data,
                                 struct xdg_activation_token_v1 *token_proxy,
                                 const char *token) {
    (void)token_proxy;
    vgfx_wayland_activation_t *activation = (vgfx_wayland_activation_t *)data;
    if (!activation || !activation->connection)
        return;
    if (token && token[0] && activation->connection->activation_v1 && activation->surface) {
        (void)activation->connection->api.proxy_marshal_flags(
            (struct wl_proxy *)activation->connection->activation_v1,
            XDG_ACTIVATION_ACTIVATE,
            NULL,
            1,
            0,
            token,
            activation->surface);
        activation->completed++;
    }
    vgfx_activation_destroy_token(activation);
}

static const vgfx_activation_token_listener_t g_activation_listener = {
    .done = vgfx_activation_done,
};

/// @copydoc vgfx_wayland_activation_init
void vgfx_wayland_activation_init(vgfx_wayland_activation_t *activation,
                                  vgfx_wayland_connection_t *connection,
                                  vgfx_wayland_input_t *input,
                                  struct wl_proxy *surface,
                                  const char *app_id) {
    if (!activation)
        return;
    memset(activation, 0, sizeof(*activation));
    activation->connection = connection;
    activation->input = input;
    activation->surface = surface;
    activation->app_id = app_id && app_id[0] ? app_id : "org.zanna.app";
}

/// @copydoc vgfx_wayland_activation_close
void vgfx_wayland_activation_close(vgfx_wayland_activation_t *activation) {
    if (!activation)
        return;
    vgfx_activation_destroy_token(activation);
    memset(activation, 0, sizeof(*activation));
}

/// @copydoc vgfx_wayland_activation_request
int vgfx_wayland_activation_request(vgfx_wayland_activation_t *activation) {
    if (!activation || !activation->connection || !activation->input || activation->token ||
        !activation->connection->activation_v1 || !activation->surface)
        return 0;
    uint32_t serial = vgfx_wayland_activation_serial(activation->input);
    if (!serial || !activation->connection->seat)
        return 0;
    activation->token =
        (struct xdg_activation_token_v1 *)activation->connection->api.proxy_marshal_flags(
            (struct wl_proxy *)activation->connection->activation_v1,
            XDG_ACTIVATION_GET_TOKEN,
            &vgfx_xdg_activation_token_v1_interface,
            1,
            0,
            NULL);
    if (!activation->token ||
        activation->connection->api.proxy_add_listener(
            (struct wl_proxy *)activation->token,
            (void (**)(void))(void *)&g_activation_listener,
            activation) != 0) {
        if (activation->token)
            activation->connection->api.proxy_destroy((struct wl_proxy *)activation->token);
        activation->token = NULL;
        return 0;
    }
    (void)activation->connection->api.proxy_marshal_flags((struct wl_proxy *)activation->token,
                                                          XDG_TOKEN_SET_SERIAL,
                                                          NULL,
                                                          1,
                                                          0,
                                                          serial,
                                                          activation->connection->seat);
    (void)activation->connection->api.proxy_marshal_flags((struct wl_proxy *)activation->token,
                                                          XDG_TOKEN_SET_APP_ID,
                                                          NULL,
                                                          1,
                                                          0,
                                                          activation->app_id);
    (void)activation->connection->api.proxy_marshal_flags((struct wl_proxy *)activation->token,
                                                          XDG_TOKEN_SET_SURFACE,
                                                          NULL,
                                                          1,
                                                          0,
                                                          activation->surface);
    (void)activation->connection->api.proxy_marshal_flags((struct wl_proxy *)activation->token,
                                                          XDG_TOKEN_COMMIT,
                                                          NULL,
                                                          1,
                                                          0);
    return 1;
}
