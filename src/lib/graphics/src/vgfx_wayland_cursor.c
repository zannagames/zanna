//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_cursor.c
// Purpose: Implement standard Wayland cursor themes through dynamic ABI loading.
// Key invariants: See vgfx_wayland_cursor.h.
// Ownership/Lifetime: See vgfx_wayland_cursor.h.
// Links: src/lib/graphics/src/vgfx_wayland_cursor.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Dynamic Wayland cursor-theme loading and surface application.

#include "vgfx_wayland_cursor.h"

#include "vgfx.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

enum {
    WL_COMPOSITOR_CREATE_SURFACE = 0,
    WL_POINTER_SET_CURSOR = 0,
    WL_SURFACE_ATTACH = 1,
    WL_SURFACE_DAMAGE = 2,
    WL_SURFACE_COMMIT = 6,
};

/// @brief Local ABI-compatible cursor image metadata from libwayland-cursor.
struct wl_cursor_image {
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t delay;
};

/// @brief Local ABI-compatible themed cursor descriptor.
struct wl_cursor {
    unsigned int image_count;
    struct wl_cursor_image **images;
    char *name;
};

/// @copydoc vgfx_wayland_cursor_name
const char *vgfx_wayland_cursor_name(int32_t type) {
    switch (type) {
    case VGFX_CURSOR_POINTER: return "pointer";
    case VGFX_CURSOR_TEXT: return "text";
    case VGFX_CURSOR_RESIZE_H: return "ew-resize";
    case VGFX_CURSOR_RESIZE_V: return "ns-resize";
    case VGFX_CURSOR_WAIT: return "wait";
    case VGFX_CURSOR_RESIZE_NWSE: return "nwse-resize";
    case VGFX_CURSOR_RESIZE_NESW: return "nesw-resize";
    case VGFX_CURSOR_GRAB: return "grab";
    case VGFX_CURSOR_GRABBING: return "grabbing";
    case VGFX_CURSOR_CROSSHAIR: return "crosshair";
    case VGFX_CURSOR_HELP: return "help";
    case VGFX_CURSOR_NOT_ALLOWED: return "not-allowed";
    default: return "default";
    }
}

/// @brief Look up a themed cursor with legacy-name fallbacks.
/// @param cursor Initialized cursor-theme state.
/// @param type Public cursor type.
/// @return Borrowed themed cursor descriptor, or NULL when unavailable.
static struct wl_cursor *vgfx_cursor_lookup(vgfx_wayland_cursor_t *cursor, int32_t type) {
    struct wl_cursor *result =
        cursor->theme_get_cursor(cursor->theme, vgfx_wayland_cursor_name(type));
    if (!result && type == VGFX_CURSOR_DEFAULT)
        result = cursor->theme_get_cursor(cursor->theme, "left_ptr");
    if (!result && type == VGFX_CURSOR_POINTER)
        result = cursor->theme_get_cursor(cursor->theme, "hand2");
    if (!result && type == VGFX_CURSOR_TEXT)
        result = cursor->theme_get_cursor(cursor->theme, "xterm");
    return result;
}

/// @brief Apply cursor visibility and image for a pointer-enter serial.
/// @details A hidden cursor sends a NULL surface.  A visible cursor selects the
///          first theme image, assigns its hotspot, attaches/damages its buffer,
///          and commits the cursor surface.
/// @param opaque Borrowed `vgfx_wayland_cursor_t` callback context.
/// @param serial Pointer-enter serial authorizing `wl_pointer.set_cursor`.
static void vgfx_cursor_apply(void *opaque, uint32_t serial) {
    vgfx_wayland_cursor_t *cursor = opaque;
    if (!cursor || !cursor->input || !cursor->input->pointer)
        return;
    vgfx_wayland_client_api_t *api = &cursor->connection->api;
    if (!cursor->visible) {
        (void)api->proxy_marshal_flags(cursor->input->pointer,
                                      WL_POINTER_SET_CURSOR,
                                      NULL,
                                      api->proxy_get_version(cursor->input->pointer),
                                      0,
                                      serial,
                                      NULL,
                                      0,
                                      0);
        return;
    }
    if (!cursor->theme || !cursor->surface)
        return;
    struct wl_cursor *theme_cursor = vgfx_cursor_lookup(cursor, cursor->type);
    if (!theme_cursor || theme_cursor->image_count == 0 || !theme_cursor->images[0])
        return;
    struct wl_cursor_image *image = theme_cursor->images[0];
    struct wl_proxy *buffer = cursor->image_get_buffer(image);
    if (!buffer)
        return;
    (void)api->proxy_marshal_flags(cursor->input->pointer,
                                  WL_POINTER_SET_CURSOR,
                                  NULL,
                                  api->proxy_get_version(cursor->input->pointer),
                                  0,
                                  serial,
                                  cursor->surface,
                                  (int32_t)image->hotspot_x,
                                  (int32_t)image->hotspot_y);
    (void)api->proxy_marshal_flags(cursor->surface,
                                  WL_SURFACE_ATTACH,
                                  NULL,
                                  api->proxy_get_version(cursor->surface),
                                  0,
                                  buffer,
                                  0,
                                  0);
    (void)api->proxy_marshal_flags(cursor->surface,
                                  WL_SURFACE_DAMAGE,
                                  NULL,
                                  api->proxy_get_version(cursor->surface),
                                  0,
                                  0,
                                  0,
                                  (int32_t)image->width,
                                  (int32_t)image->height);
    (void)api->proxy_marshal_flags(cursor->surface,
                                  WL_SURFACE_COMMIT,
                                  NULL,
                                  api->proxy_get_version(cursor->surface),
                                  0);
}

/// @copydoc vgfx_wayland_cursor_open
int vgfx_wayland_cursor_open(vgfx_wayland_cursor_t *cursor,
                             vgfx_wayland_connection_t *connection,
                             vgfx_wayland_input_t *input) {
    if (!cursor || !connection || !input)
        return 0;
    memset(cursor, 0, sizeof(*cursor));
    cursor->connection = connection;
    cursor->input = input;
    cursor->visible = 1;
    cursor->library = dlopen("libwayland-cursor.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!cursor->library)
        return 1;
#define LOAD(field, symbol)                                                                        \
    do {                                                                                           \
        *(void **)(&cursor->field) = dlsym(cursor->library, symbol);                               \
        if (!cursor->field) {                                                                      \
            vgfx_wayland_cursor_close(cursor);                                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
    LOAD(theme_load, "wl_cursor_theme_load");
    LOAD(theme_destroy, "wl_cursor_theme_destroy");
    LOAD(theme_get_cursor, "wl_cursor_theme_get_cursor");
    LOAD(image_get_buffer, "wl_cursor_image_get_buffer");
#undef LOAD
    int size = 24;
    const char *size_text = getenv("XCURSOR_SIZE");
    if (size_text) {
        char *end = NULL;
        long parsed = strtol(size_text, &end, 10);
        if (end != size_text && *end == '\0' && parsed >= 8 && parsed <= 256)
            size = (int)parsed;
    }
    cursor->theme = cursor->theme_load(getenv("XCURSOR_THEME"), size, connection->shm);
    cursor->surface = connection->api.proxy_marshal_flags(
        connection->compositor,
        WL_COMPOSITOR_CREATE_SURFACE,
        connection->api.surface_interface,
        connection->api.proxy_get_version(connection->compositor),
        0,
        NULL);
    if (cursor->theme && cursor->surface) {
        input->pointer_entered = vgfx_cursor_apply;
        input->pointer_entered_data = cursor;
    }
    return 1;
}

/// @copydoc vgfx_wayland_cursor_close
void vgfx_wayland_cursor_close(vgfx_wayland_cursor_t *cursor) {
    if (!cursor)
        return;
    if (cursor->input && cursor->input->pointer_entered_data == cursor) {
        cursor->input->pointer_entered = NULL;
        cursor->input->pointer_entered_data = NULL;
    }
    if (cursor->surface && cursor->connection)
        cursor->connection->api.proxy_destroy(cursor->surface);
    if (cursor->theme && cursor->theme_destroy)
        cursor->theme_destroy(cursor->theme);
    if (cursor->library)
        dlclose(cursor->library);
    memset(cursor, 0, sizeof(*cursor));
}

/// @copydoc vgfx_wayland_cursor_set
void vgfx_wayland_cursor_set(vgfx_wayland_cursor_t *cursor, int32_t type, int32_t visible) {
    if (!cursor)
        return;
    cursor->type = type;
    cursor->visible = visible ? 1 : 0;
    if (cursor->input && cursor->input->pointer_serial)
        vgfx_cursor_apply(cursor, cursor->input->pointer_serial);
}
