//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_csd.c
// Purpose: Render and operate the native Wayland client-side fallback frame.
// Key invariants:
//   - The child surface is below the content and synchronized with its parent.
//   - Move and resize requests use the serial from the triggering pointer press.
// Ownership/Lifetime: See vgfx_wayland_csd.h.
// Links: Wayland wl_subcompositor and stable xdg-shell protocols.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Wayland fallback client-side decoration implementation.

#include "vgfx_wayland_csd.h"

#include "vgfx_internal.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    VGFX_CSD_BORDER = 6,
    VGFX_CSD_TITLE = 30,
    VGFX_CSD_BUTTON = 24,
    VGFX_WL_SUBCOMPOSITOR_GET_SUBSURFACE = 1,
    VGFX_WL_SUBSURFACE_DESTROY = 0,
    VGFX_WL_SUBSURFACE_SET_POSITION = 1,
    VGFX_WL_SUBSURFACE_PLACE_BELOW = 2,
    VGFX_XDG_SURFACE_SET_WINDOW_GEOMETRY = 3,
    VGFX_XDG_TOPLEVEL_MOVE = 5,
    VGFX_XDG_TOPLEVEL_RESIZE = 6,
    VGFX_XDG_RESIZE_EDGE_TOP = 1,
    VGFX_XDG_RESIZE_EDGE_BOTTOM = 2,
    VGFX_XDG_RESIZE_EDGE_LEFT = 4,
    VGFX_XDG_RESIZE_EDGE_TOP_LEFT = 5,
    VGFX_XDG_RESIZE_EDGE_BOTTOM_LEFT = 6,
    VGFX_XDG_RESIZE_EDGE_RIGHT = 8,
    VGFX_XDG_RESIZE_EDGE_TOP_RIGHT = 9,
    VGFX_XDG_RESIZE_EDGE_BOTTOM_RIGHT = 10,
};

/// @brief Format a stable client-decoration setup failure.
/// @param error Destination buffer, or NULL.
/// @param size Destination capacity including the terminator.
/// @param detail Failure-specific detail.
static void vgfx_wayland_csd_error(char *error, uint32_t size, const char *detail) {
    if (error && size > 0)
        (void)snprintf(error, size, "Wayland client decoration setup failed: %s", detail);
}

/// @brief Fill a rectangular region of an RGBA decoration buffer.
/// @param pixels Destination top-left pixel buffer.
/// @param stride Row stride in bytes.
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param width Rectangle width.
/// @param height Rectangle height.
/// @param red Red channel value.
/// @param green Green channel value.
/// @param blue Blue channel value.
static void vgfx_wayland_csd_fill(uint8_t *pixels,
                                  int32_t stride,
                                  int32_t x,
                                  int32_t y,
                                  int32_t width,
                                  int32_t height,
                                  uint8_t red,
                                  uint8_t green,
                                  uint8_t blue) {
    for (int32_t row = y; row < y + height; ++row) {
        for (int32_t column = x; column < x + width; ++column) {
            uint8_t *pixel = pixels + (size_t)row * (size_t)stride + (size_t)column * 4u;
            pixel[0] = red;
            pixel[1] = green;
            pixel[2] = blue;
            pixel[3] = 255;
        }
    }
}

/// @brief Rasterize the fallback frame and its three title-bar controls.
/// @details Draws border/background colors plus allocation-free close,
///          maximize, and minimize glyphs into the owned RGBA source.
/// @param csd Active decoration state with sized pixel storage.
static void vgfx_wayland_csd_draw(vgfx_wayland_csd_t *csd) {
    int32_t frame_width = csd->width + VGFX_CSD_BORDER * 2;
    int32_t frame_height = csd->height + VGFX_CSD_TITLE + VGFX_CSD_BORDER;
    int32_t stride = frame_width * 4;
    vgfx_wayland_csd_fill(csd->pixels, stride, 0, 0, frame_width, frame_height, 42, 45, 52);
    vgfx_wayland_csd_fill(csd->pixels,
                          stride,
                          VGFX_CSD_BORDER,
                          VGFX_CSD_TITLE,
                          csd->width,
                          csd->height,
                          0,
                          0,
                          0);
    int32_t close_x = frame_width - VGFX_CSD_BORDER - VGFX_CSD_BUTTON;
    int32_t max_x = close_x - VGFX_CSD_BUTTON;
    int32_t min_x = max_x - VGFX_CSD_BUTTON;
    if (min_x < VGFX_CSD_BORDER)
        return;
    vgfx_wayland_csd_fill(
        csd->pixels, stride, close_x, 3, VGFX_CSD_BUTTON, VGFX_CSD_BUTTON, 176, 54, 62);
    vgfx_wayland_csd_fill(
        csd->pixels, stride, max_x, 3, VGFX_CSD_BUTTON, VGFX_CSD_BUTTON, 72, 77, 88);
    vgfx_wayland_csd_fill(
        csd->pixels, stride, min_x, 3, VGFX_CSD_BUTTON, VGFX_CSD_BUTTON, 72, 77, 88);

    /* High-contrast button glyphs remain recognizable without a font dependency. */
    for (int32_t i = 8; i < 16; ++i) {
        uint8_t *a = csd->pixels + (size_t)(3 + i) * (size_t)stride +
                     (size_t)(close_x + i) * 4u;
        uint8_t *b = csd->pixels + (size_t)(3 + i) * (size_t)stride +
                     (size_t)(close_x + 23 - i) * 4u;
        a[0] = a[1] = a[2] = b[0] = b[1] = b[2] = 255;
    }
    vgfx_wayland_csd_fill(csd->pixels, stride, max_x + 7, 10, 10, 8, 210, 213, 220);
    vgfx_wayland_csd_fill(csd->pixels, stride, max_x + 9, 12, 6, 4, 42, 45, 52);
    vgfx_wayland_csd_fill(csd->pixels, stride, min_x + 7, 17, 10, 2, 210, 213, 220);
}

/// @brief Map a decoration-surface coordinate to an xdg resize edge.
/// @param csd Decoration state supplying content dimensions.
/// @param x Pointer X coordinate on the frame surface.
/// @param y Pointer Y coordinate on the frame surface.
/// @return `XDG_TOPLEVEL_RESIZE_EDGE_*` value, or zero outside resize borders.
static uint32_t vgfx_wayland_csd_resize_edge(const vgfx_wayland_csd_t *csd, int32_t x, int32_t y) {
    int left = x < VGFX_CSD_BORDER;
    int right = x >= csd->width + VGFX_CSD_BORDER;
    int top = y < VGFX_CSD_BORDER;
    int bottom = y >= csd->height + VGFX_CSD_TITLE;
    if (top && left)
        return VGFX_XDG_RESIZE_EDGE_TOP_LEFT;
    if (top && right)
        return VGFX_XDG_RESIZE_EDGE_TOP_RIGHT;
    if (bottom && left)
        return VGFX_XDG_RESIZE_EDGE_BOTTOM_LEFT;
    if (bottom && right)
        return VGFX_XDG_RESIZE_EDGE_BOTTOM_RIGHT;
    if (top)
        return VGFX_XDG_RESIZE_EDGE_TOP;
    if (bottom)
        return VGFX_XDG_RESIZE_EDGE_BOTTOM;
    if (left)
        return VGFX_XDG_RESIZE_EDGE_LEFT;
    if (right)
        return VGFX_XDG_RESIZE_EDGE_RIGHT;
    return 0;
}

/// @brief Handle pointer activity routed from the decoration child surface.
/// @details Tracks coordinates and, on a left-button press, uses the triggering
///          serial to request edge resize, close, maximize toggle, minimize, or
///          title-bar move according to the hit region.
/// @param data Borrowed `vgfx_wayland_csd_t` callback context.
/// @param surface Surface on which the event occurred.
/// @param serial Seat serial authorizing move/resize.
/// @param time Native event timestamp; unused.
/// @param x Decoration-relative X coordinate.
/// @param y Decoration-relative Y coordinate.
/// @param button Linux input button code.
/// @param pressed Non-zero for press, zero for release.
static void vgfx_wayland_csd_pointer(void *data,
                                     struct wl_proxy *surface,
                                     uint32_t serial,
                                     uint32_t time,
                                     int32_t x,
                                     int32_t y,
                                     uint32_t button,
                                     int32_t pressed) {
    (void)time;
    vgfx_wayland_csd_t *csd = data;
    if (!csd || surface != (struct wl_proxy *)csd->surface)
        return;
    csd->pointer_x = x;
    csd->pointer_y = y;
    if (button != BTN_LEFT || !pressed)
        return;
    vgfx_wayland_connection_t *connection = csd->shell->connection;
    uint32_t version = connection->api.proxy_get_version((struct wl_proxy *)csd->shell->toplevel);
    uint32_t edge = vgfx_wayland_csd_resize_edge(csd, x, y);
    if (edge) {
        (void)connection->api.proxy_marshal_flags((struct wl_proxy *)csd->shell->toplevel,
                                                  VGFX_XDG_TOPLEVEL_RESIZE,
                                                  NULL,
                                                  version,
                                                  0,
                                                  connection->seat,
                                                  serial,
                                                  edge);
        return;
    }
    int32_t frame_width = csd->width + VGFX_CSD_BORDER * 2;
    int32_t close_x = frame_width - VGFX_CSD_BORDER - VGFX_CSD_BUTTON;
    int32_t min_x = close_x - VGFX_CSD_BUTTON * 2;
    if (min_x >= VGFX_CSD_BORDER && y >= 3 && y < 3 + VGFX_CSD_BUTTON && x >= close_x) {
        csd->shell->close_requested = 1;
    } else if (min_x >= VGFX_CSD_BORDER && y >= 3 && y < 3 + VGFX_CSD_BUTTON &&
               x >= close_x - VGFX_CSD_BUTTON) {
        uint32_t opcode = csd->shell->maximized ? VGFX_XDG_TOPLEVEL_UNSET_MAXIMIZED
                                                : VGFX_XDG_TOPLEVEL_SET_MAXIMIZED;
        (void)connection->api.proxy_marshal_flags(
            (struct wl_proxy *)csd->shell->toplevel, opcode, NULL, version, 0);
    } else if (min_x >= VGFX_CSD_BORDER && y >= 3 && y < 3 + VGFX_CSD_BUTTON && x >= min_x) {
        (void)connection->api.proxy_marshal_flags((struct wl_proxy *)csd->shell->toplevel,
                                                  VGFX_XDG_TOPLEVEL_SET_MINIMIZED,
                                                  NULL,
                                                  version,
                                                  0);
    } else if (y < VGFX_CSD_TITLE) {
        (void)connection->api.proxy_marshal_flags((struct wl_proxy *)csd->shell->toplevel,
                                                  VGFX_XDG_TOPLEVEL_MOVE,
                                                  NULL,
                                                  version,
                                                  0,
                                                  connection->seat,
                                                  serial);
    }
}

/// @copydoc vgfx_wayland_csd_close
void vgfx_wayland_csd_close(vgfx_wayland_csd_t *csd) {
    if (!csd)
        return;
    if (csd->input && csd->input->foreign_pointer_data == csd) {
        csd->input->foreign_pointer_event = NULL;
        csd->input->foreign_pointer_data = NULL;
    }
    vgfx_wayland_shm_close(&csd->presenter);
    free(csd->pixels);
    if (csd->subsurface && csd->shell) {
        vgfx_wayland_connection_t *connection = csd->shell->connection;
        (void)connection->api.proxy_marshal_flags(csd->subsurface,
                                                  VGFX_WL_SUBSURFACE_DESTROY,
                                                  NULL,
                                                  connection->api.proxy_get_version(csd->subsurface),
                                                  VGFX_WL_MARSHAL_FLAG_DESTROY);
    }
    if (csd->surface && csd->shell) {
        vgfx_wayland_connection_t *connection = csd->shell->connection;
        (void)connection->api.proxy_marshal_flags((struct wl_proxy *)csd->surface,
                                                  VGFX_WL_SURFACE_DESTROY,
                                                  NULL,
                                                  connection->api.proxy_get_version(
                                                      (struct wl_proxy *)csd->surface),
                                                  VGFX_WL_MARSHAL_FLAG_DESTROY);
    }
    memset(csd, 0, sizeof(*csd));
}

/// @copydoc vgfx_wayland_csd_resize
int vgfx_wayland_csd_resize(vgfx_wayland_csd_t *csd,
                            int32_t width,
                            int32_t height,
                            char *error,
                            uint32_t error_size) {
    if (!csd || !csd->active)
        return 1;
    if (width <= 0 || height <= 0 || width > INT32_MAX - VGFX_CSD_BORDER * 2 ||
        height > INT32_MAX - VGFX_CSD_TITLE - VGFX_CSD_BORDER) {
        vgfx_wayland_csd_error(error, error_size, "invalid frame dimensions");
        return 0;
    }
    int32_t frame_width = width + VGFX_CSD_BORDER * 2;
    int32_t frame_height = height + VGFX_CSD_TITLE + VGFX_CSD_BORDER;
    size_t pixels_size = (size_t)frame_width * (size_t)frame_height * 4u;
    uint8_t *pixels = malloc(pixels_size);
    if (!pixels) {
        vgfx_wayland_csd_error(error, error_size, "could not allocate frame pixels");
        return 0;
    }
    vgfx_wayland_shm_close(&csd->presenter);
    free(csd->pixels);
    csd->pixels = pixels;
    csd->pixels_size = pixels_size;
    csd->width = width;
    csd->height = height;
    vgfx_wayland_connection_t *connection = csd->shell->connection;
    (void)connection->api.proxy_marshal_flags(csd->subsurface,
                                              VGFX_WL_SUBSURFACE_SET_POSITION,
                                              NULL,
                                              1,
                                              0,
                                              -VGFX_CSD_BORDER,
                                              -VGFX_CSD_TITLE);
    (void)connection->api.proxy_marshal_flags((struct wl_proxy *)csd->shell->xdg_surface,
                                              VGFX_XDG_SURFACE_SET_WINDOW_GEOMETRY,
                                              NULL,
                                              connection->api.proxy_get_version(
                                                  (struct wl_proxy *)csd->shell->xdg_surface),
                                              0,
                                              0,
                                              0,
                                              width,
                                              height);
    if (!vgfx_wayland_shm_open_surface(&csd->presenter,
                                       csd->shell,
                                       csd->surface,
                                       frame_width,
                                       frame_height,
                                       error,
                                       error_size))
        return 0;
    vgfx_wayland_csd_draw(csd);
    return vgfx_wayland_shm_present(&csd->presenter, csd->pixels, csd->pixels_size);
}

/// @copydoc vgfx_wayland_csd_open
int vgfx_wayland_csd_open(vgfx_wayland_csd_t *csd,
                          vgfx_wayland_shell_t *shell,
                          vgfx_wayland_input_t *input,
                          struct vgfx_window *window,
                          int32_t width,
                          int32_t height,
                          char *error,
                          uint32_t error_size) {
    if (!csd || !shell || !input || !window)
        return 0;
    memset(csd, 0, sizeof(*csd));
    const char *force_csd = getenv("ZANNA_WAYLAND_FORCE_CSD");
    if (shell->decoration_mode == 2 && (!force_csd || strcmp(force_csd, "1") != 0))
        return 1;
    vgfx_wayland_connection_t *connection = shell->connection;
    if (!connection->subcompositor) {
        vgfx_wayland_csd_error(error, error_size, "compositor does not advertise wl_subcompositor");
        return 0;
    }
    csd->shell = shell;
    csd->input = input;
    csd->window = window;
    csd->surface = (struct wl_surface *)connection->api.proxy_marshal_flags(
        connection->compositor,
        VGFX_WL_COMPOSITOR_CREATE_SURFACE,
        connection->api.surface_interface,
        connection->api.proxy_get_version(connection->compositor),
        0,
        NULL);
    if (csd->surface) {
        csd->subsurface = connection->api.proxy_marshal_flags(
            connection->subcompositor,
            VGFX_WL_SUBCOMPOSITOR_GET_SUBSURFACE,
            connection->api.subsurface_interface,
            connection->api.proxy_get_version(connection->subcompositor),
            0,
            NULL,
            csd->surface,
            shell->surface);
    }
    if (!csd->surface || !csd->subsurface) {
        vgfx_wayland_csd_error(error, error_size, "could not create frame subsurface");
        vgfx_wayland_csd_close(csd);
        return 0;
    }
    (void)connection->api.proxy_marshal_flags(csd->subsurface,
                                              VGFX_WL_SUBSURFACE_PLACE_BELOW,
                                              NULL,
                                              1,
                                              0,
                                              shell->surface);
    csd->active = 1;
    input->foreign_pointer_event = vgfx_wayland_csd_pointer;
    input->foreign_pointer_data = csd;
    if (!vgfx_wayland_csd_resize(csd, width, height, error, error_size)) {
        vgfx_wayland_csd_close(csd);
        return 0;
    }
    return 1;
}
