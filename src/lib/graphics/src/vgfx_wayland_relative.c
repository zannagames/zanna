//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_relative.c
// Purpose: Implement relative-pointer-v1 with a persistent pointer constraint.
// Key invariants: See vgfx_wayland_relative.h.
// Ownership/Lifetime: See vgfx_wayland_relative.h.
// Links: src/lib/graphics/src/vgfx_wayland_relative.h
//
//===----------------------------------------------------------------------===//

#include "vgfx_wayland_relative.h"
#include "vgfx_internal.h"

#include <string.h>

/// @file
/// @brief Implements relative-pointer-v1 motion and persistent pointer constraints.

/// @brief Wire opcodes and lifetime value used by the relative-pointer protocols.
enum {
    /// @brief Opcode for `zwp_relative_pointer_manager_v1.get_relative_pointer`.
    ZWP_RELATIVE_MANAGER_GET_POINTER = 1,

    /// @brief Opcode for `zwp_relative_pointer_v1.destroy`.
    ZWP_RELATIVE_POINTER_DESTROY = 0,

    /// @brief Opcode for `zwp_pointer_constraints_v1.lock_pointer`.
    ZWP_CONSTRAINTS_LOCK_POINTER = 1,

    /// @brief Opcode for `zwp_locked_pointer_v1.destroy`.
    ZWP_LOCKED_POINTER_DESTROY = 0,

    /// @brief Request a constraint that survives temporary compositor unlocks.
    ZWP_POINTER_LIFETIME_PERSISTENT = 2,
};

/// @brief ABI callback table for relative-pointer-v1 events.
typedef struct {
    /// @brief Receive accelerated and unaccelerated pointer displacement.
    void (*relative_motion)(void *, struct zwp_relative_pointer_v1 *, uint32_t, uint32_t,
                            wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t);
} vgfx_relative_pointer_listener_t;

/// @brief ABI callback table for pointer-constraint state changes.
typedef struct {
    /// @brief Report that the compositor activated the pointer lock.
    void (*locked)(void *, struct zwp_locked_pointer_v1 *);

    /// @brief Report that the compositor temporarily released the pointer lock.
    void (*unlocked)(void *, struct zwp_locked_pointer_v1 *);
} vgfx_locked_pointer_listener_t;

/// @copydoc vgfx_wayland_relative_fixed
double vgfx_wayland_relative_fixed(wl_fixed_t value) { return (double)value / 256.0; }

/// @brief Accumulate one unaccelerated relative-motion event on the owning window.
/// @param data Registered @ref vgfx_wayland_relative_t callback context.
/// @param pointer Relative-pointer proxy that emitted the event.
/// @param time_hi High 32 bits of the protocol's 64-bit microsecond timestamp.
/// @param time_lo Low 32 bits of the protocol's 64-bit microsecond timestamp.
/// @param dx Accelerated horizontal displacement, unused by this backend.
/// @param dy Accelerated vertical displacement, unused by this backend.
/// @param dx_unaccelerated Raw horizontal displacement in Wayland fixed-point units.
/// @param dy_unaccelerated Raw vertical displacement in Wayland fixed-point units.
static void vgfx_relative_motion(void *data,
                                 struct zwp_relative_pointer_v1 *pointer,
                                 uint32_t time_hi,
                                 uint32_t time_lo,
                                 wl_fixed_t dx,
                                 wl_fixed_t dy,
                                 wl_fixed_t dx_unaccelerated,
                                 wl_fixed_t dy_unaccelerated) {
    (void)pointer;
    (void)time_hi;
    (void)time_lo;
    (void)dx;
    (void)dy;
    vgfx_wayland_relative_t *relative = (vgfx_wayland_relative_t *)data;
    if (relative && relative->enabled)
        vgfx_internal_add_relative_delta(relative->window,
                                         vgfx_wayland_relative_fixed(dx_unaccelerated),
                                         vgfx_wayland_relative_fixed(dy_unaccelerated));
}

/// @brief Mark the persistent pointer constraint as compositor-active.
/// @param data Registered @ref vgfx_wayland_relative_t callback context.
/// @param pointer Locked-pointer proxy that emitted the event.
static void vgfx_relative_locked(void *data, struct zwp_locked_pointer_v1 *pointer) {
    (void)pointer;
    vgfx_wayland_relative_t *relative = (vgfx_wayland_relative_t *)data;
    if (relative)
        relative->locked = 1;
}

/// @brief Mark the persistent pointer constraint as temporarily inactive.
/// @param data Registered @ref vgfx_wayland_relative_t callback context.
/// @param pointer Locked-pointer proxy that emitted the event.
static void vgfx_relative_unlocked(void *data, struct zwp_locked_pointer_v1 *pointer) {
    (void)pointer;
    vgfx_wayland_relative_t *relative = (vgfx_wayland_relative_t *)data;
    if (relative)
        relative->locked = 0;
}

static const vgfx_relative_pointer_listener_t g_relative_listener = {
    .relative_motion = vgfx_relative_motion,
};
static const vgfx_locked_pointer_listener_t g_locked_listener = {
    .locked = vgfx_relative_locked,
    .unlocked = vgfx_relative_unlocked,
};

/// @brief Marshal a protocol object's destructor when all required state is available.
/// @param relative Relative-pointer state providing the Wayland dispatch table.
/// @param proxy Owned proxy to destroy, or NULL.
/// @param opcode Destructor opcode appropriate for @p proxy.
static void vgfx_relative_destroy(vgfx_wayland_relative_t *relative,
                                  struct wl_proxy *proxy,
                                  uint32_t opcode) {
    if (relative && relative->connection && proxy)
        (void)relative->connection->api.proxy_marshal_flags(
            proxy, opcode, NULL, 1, VGFX_WL_MARSHAL_FLAG_DESTROY);
}

/// @copydoc vgfx_wayland_relative_close
void vgfx_wayland_relative_close(vgfx_wayland_relative_t *relative) {
    if (!relative)
        return;
    vgfx_relative_destroy(relative,
                          (struct wl_proxy *)relative->locked_pointer,
                          ZWP_LOCKED_POINTER_DESTROY);
    vgfx_relative_destroy(relative,
                          (struct wl_proxy *)relative->relative_pointer,
                          ZWP_RELATIVE_POINTER_DESTROY);
    memset(relative, 0, sizeof(*relative));
}

/// @copydoc vgfx_wayland_relative_init
void vgfx_wayland_relative_init(vgfx_wayland_relative_t *relative,
                                vgfx_wayland_connection_t *connection,
                                vgfx_wayland_input_t *input,
                                struct wl_proxy *surface,
                                struct vgfx_window *window) {
    if (!relative)
        return;
    memset(relative, 0, sizeof(*relative));
    relative->connection = connection;
    relative->input = input;
    relative->surface = surface;
    relative->window = window;
}

/// @copydoc vgfx_wayland_relative_set_enabled
int vgfx_wayland_relative_set_enabled(vgfx_wayland_relative_t *relative, int32_t enabled) {
    if (!relative || !relative->connection || !relative->input)
        return 0;
    if (!enabled) {
        vgfx_wayland_connection_t *connection = relative->connection;
        vgfx_wayland_input_t *input = relative->input;
        struct wl_proxy *surface = relative->surface;
        struct vgfx_window *window = relative->window;
        vgfx_wayland_relative_close(relative);
        vgfx_wayland_relative_init(relative, connection, input, surface, window);
        return 0;
    }
    if (relative->enabled)
        return 1;
    if (!relative->connection->relative_pointer_manager_v1 ||
        !relative->connection->pointer_constraints_v1 || !relative->input->pointer)
        return 0;
    relative->relative_pointer =
        (struct zwp_relative_pointer_v1 *)relative->connection->api.proxy_marshal_flags(
            (struct wl_proxy *)relative->connection->relative_pointer_manager_v1,
            ZWP_RELATIVE_MANAGER_GET_POINTER,
            &vgfx_zwp_relative_pointer_v1_interface,
            1,
            0,
            NULL,
            relative->input->pointer);
    relative->locked_pointer =
        (struct zwp_locked_pointer_v1 *)relative->connection->api.proxy_marshal_flags(
            (struct wl_proxy *)relative->connection->pointer_constraints_v1,
            ZWP_CONSTRAINTS_LOCK_POINTER,
            &vgfx_zwp_locked_pointer_v1_interface,
            1,
            0,
            NULL,
            relative->surface,
            relative->input->pointer,
            NULL,
            ZWP_POINTER_LIFETIME_PERSISTENT);
    if (!relative->relative_pointer || !relative->locked_pointer ||
        relative->connection->api.proxy_add_listener(
            (struct wl_proxy *)relative->relative_pointer,
            (void (**)(void))(void *)&g_relative_listener,
            relative) != 0 ||
        relative->connection->api.proxy_add_listener(
            (struct wl_proxy *)relative->locked_pointer,
            (void (**)(void))(void *)&g_locked_listener,
            relative) != 0) {
        vgfx_wayland_connection_t *connection = relative->connection;
        vgfx_wayland_input_t *input = relative->input;
        struct wl_proxy *surface = relative->surface;
        struct vgfx_window *window = relative->window;
        vgfx_wayland_relative_close(relative);
        vgfx_wayland_relative_init(relative, connection, input, surface, window);
        return 0;
    }
    relative->enabled = 1;
    return 1;
}
