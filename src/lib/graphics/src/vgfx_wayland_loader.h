//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_loader.h
// Purpose: Header-free dynamic dispatch surface for the Wayland client ABI.
// Key invariants:
//   - No Wayland development headers or link-time client dependency is required.
//   - A dispatch table is either completely initialized or completely zeroed.
// Ownership/Lifetime:
//   - The table owns its shared-object handle until vgfx_wayland_loader_close().
//   - Interface descriptors are borrowed from the loaded client library.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>
#include <stdint.h>

/// @file
/// @brief Declares the dynamically loaded Wayland client ABI used by the graphics backend.
/// @details This header reproduces only the Wayland declarations needed by Zanna so the
/// backend can be compiled without the Wayland development package. Callers populate
/// @ref vgfx_wayland_client_api_t with @ref vgfx_wayland_loader_open and release it with
/// @ref vgfx_wayland_loader_close.

/// @brief Opaque connection to a Wayland display server.
struct wl_display;

/// @brief Opaque queue used to route Wayland protocol events.
struct wl_event_queue;

/// @brief Opaque base object shared by Wayland protocol proxies.
struct wl_proxy;

/// @brief Fixed-point scalar type used by the Wayland wire protocol.
typedef int32_t wl_fixed_t;

/// @brief Wayland ABI-compatible dynamically sized byte array.
/// @details The loader does not allocate or resize this structure; it is declared here so
/// listener callbacks can consume arrays supplied by the Wayland client library.
struct wl_array {
    /// @brief Number of initialized bytes in @ref data.
    size_t size;

    /// @brief Number of bytes currently allocated for @ref data.
    size_t alloc;

    /// @brief Pointer to the array's byte storage.
    void *data;
};

/// @brief Describes one request or event in a Wayland protocol interface.
struct wl_message {
    /// @brief Protocol message name.
    const char *name;

    /// @brief Wayland wire signature for the message arguments.
    const char *signature;

    /// @brief Interface descriptors for object-valued arguments.
    const struct wl_interface **types;
};

/// @brief Wayland ABI-compatible description of a protocol interface.
struct wl_interface {
    /// @brief Protocol interface name advertised on the wire.
    const char *name;

    /// @brief Highest protocol version described by this interface.
    int version;

    /// @brief Number of entries in @ref methods.
    int method_count;

    /// @brief Array describing requests accepted by the interface.
    const struct wl_message *methods;

    /// @brief Number of entries in @ref events.
    int event_count;

    /// @brief Array describing events emitted by the interface.
    const struct wl_message *events;
};

/// @brief Receives diagnostics emitted by the Wayland client library.
/// @param format printf-style format string followed by its format arguments.
typedef void (*vgfx_wayland_log_handler_t)(const char *format, ...);

/// @brief Complete dynamically resolved subset of the Wayland client ABI.
/// @details A successfully opened table contains every function and interface pointer below.
/// On failure or after @ref vgfx_wayland_loader_close, every member is zero. The table owns
/// @ref library but only borrows the exported interface descriptors.
typedef struct vgfx_wayland_client_api {
    /// @brief Native dynamic-library handle owned by this table.
    void *library;

    /// @brief Connect to a Wayland display by name, or to the default display when null.
    struct wl_display *(*display_connect)(const char *name);

    /// @brief Disconnect and destroy a display connection.
    void (*display_disconnect)(struct wl_display *display);

    /// @brief Read, queue, and dispatch events from a display connection.
    int (*display_dispatch)(struct wl_display *display);

    /// @brief Dispatch events already queued for a display connection.
    int (*display_dispatch_pending)(struct wl_display *display);

    /// @brief Perform a synchronous display round trip.
    int (*display_roundtrip)(struct wl_display *display);

    /// @brief Flush buffered protocol requests to the compositor.
    int (*display_flush)(struct wl_display *display);

    /// @brief Return the file descriptor backing a display connection.
    int (*display_get_fd)(struct wl_display *display);

    /// @brief Begin the coordinated read sequence for display events.
    int (*display_prepare_read)(struct wl_display *display);

    /// @brief Complete a prepared read and queue the received events.
    int (*display_read_events)(struct wl_display *display);

    /// @brief Cancel a previously prepared display read.
    void (*display_cancel_read)(struct wl_display *display);

    /// @brief Return the fatal protocol or transport error recorded by the display.
    int (*display_get_error)(struct wl_display *display);

    /// @brief Destroy a protocol proxy object.
    void (*proxy_destroy)(struct wl_proxy *proxy);

    /// @brief Attach an event-listener implementation and user data to a proxy.
    int (*proxy_add_listener)(struct wl_proxy *proxy, void (**implementation)(void), void *data);

    /// @brief Marshal a versioned request and optionally construct its returned proxy.
    struct wl_proxy *(*proxy_marshal_flags)(struct wl_proxy *proxy,
                                            uint32_t opcode,
                                            const struct wl_interface *interface,
                                            uint32_t version,
                                            uint32_t flags,
                                            ...);

    /// @brief Return the protocol version negotiated for a proxy.
    uint32_t (*proxy_get_version)(struct wl_proxy *proxy);

    /// @brief Install the process-wide Wayland client diagnostic handler.
    void (*log_set_handler_client)(vgfx_wayland_log_handler_t handler);

    /// @brief Descriptor for the core `wl_display` interface.
    const struct wl_interface *display_interface;

    /// @brief Descriptor for the core `wl_registry` interface.
    const struct wl_interface *registry_interface;

    /// @brief Descriptor for the core `wl_callback` interface.
    const struct wl_interface *callback_interface;

    /// @brief Descriptor for the core `wl_compositor` interface.
    const struct wl_interface *compositor_interface;

    /// @brief Descriptor for the core `wl_subcompositor` interface.
    const struct wl_interface *subcompositor_interface;

    /// @brief Descriptor for the core `wl_subsurface` interface.
    const struct wl_interface *subsurface_interface;

    /// @brief Descriptor for the core `wl_surface` interface.
    const struct wl_interface *surface_interface;

    /// @brief Descriptor for the core `wl_region` interface.
    const struct wl_interface *region_interface;

    /// @brief Descriptor for the core `wl_shm` interface.
    const struct wl_interface *shm_interface;

    /// @brief Descriptor for the core `wl_shm_pool` interface.
    const struct wl_interface *shm_pool_interface;

    /// @brief Descriptor for the core `wl_buffer` interface.
    const struct wl_interface *buffer_interface;

    /// @brief Descriptor for the core `wl_seat` interface.
    const struct wl_interface *seat_interface;

    /// @brief Descriptor for the core `wl_pointer` interface.
    const struct wl_interface *pointer_interface;

    /// @brief Descriptor for the core `wl_keyboard` interface.
    const struct wl_interface *keyboard_interface;

    /// @brief Descriptor for the core `wl_touch` interface.
    const struct wl_interface *touch_interface;

    /// @brief Descriptor for the core `wl_output` interface.
    const struct wl_interface *output_interface;

    /// @brief Descriptor for the core `wl_data_device_manager` interface.
    const struct wl_interface *data_device_manager_interface;

    /// @brief Descriptor for the core `wl_data_device` interface.
    const struct wl_interface *data_device_interface;

    /// @brief Descriptor for the core `wl_data_source` interface.
    const struct wl_interface *data_source_interface;

    /// @brief Descriptor for the core `wl_data_offer` interface.
    const struct wl_interface *data_offer_interface;
} vgfx_wayland_client_api_t;

/// @brief Load the system Wayland client ABI.
/// @details Resolves every required function and interface descriptor from @p library_name,
/// or from the supported system sonames when no explicit name is supplied. Resolution is
/// atomic: any missing symbol closes the library and restores @p api to an all-zero state.
/// @param api Destination table, zeroed on every failure.
/// @param library_name Optional explicit shared-object name; NULL tries supported system names.
/// @param error Destination for a stable diagnostic when non-NULL and error_size is non-zero.
/// @param error_size Capacity of error including its terminator.
/// @return 1 only when every required symbol was resolved.
int vgfx_wayland_loader_open(vgfx_wayland_client_api_t *api,
                             const char *library_name,
                             char *error,
                             uint32_t error_size);

/// @brief Close a loaded Wayland client ABI and zero the table.
/// @details Passing NULL is harmless. After this call, no function or interface pointer from
/// the table may be used.
/// @param api Table to close and clear, or NULL.
void vgfx_wayland_loader_close(vgfx_wayland_client_api_t *api);
