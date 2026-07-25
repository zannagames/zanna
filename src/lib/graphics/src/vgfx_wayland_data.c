//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_data.c
// Purpose: Implement bounded nonblocking wl_data_device clipboard and DND transfers.
// Key invariants: See vgfx_wayland_data.h.
// Ownership/Lifetime: See vgfx_wayland_data.h.
// Links: src/lib/graphics/src/vgfx_wayland_data.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Bounded nonblocking Wayland clipboard and DND implementation.

#define _POSIX_C_SOURCE 200809L

#include "vgfx_wayland_data.h"

#include "vgfx_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    WL_DATA_MANAGER_CREATE_SOURCE = 0,
    WL_DATA_MANAGER_GET_DEVICE = 1,
    WL_DATA_SOURCE_OFFER = 0,
    WL_DATA_DEVICE_SET_SELECTION = 1,
    WL_DATA_OFFER_ACCEPT = 0,
    WL_DATA_OFFER_RECEIVE = 1,
    WL_DATA_OFFER_DESTROY = 2,
    WL_DATA_OFFER_FINISH = 3,
    WL_DATA_OFFER_SET_ACTIONS = 4,
    WL_DATA_DEVICE_RELEASE = 2,
    WL_DATA_SOURCE_DESTROY = 1,
    WL_MARSHAL_FLAG_DESTROY = 1,
    WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY = 1,
    VGFX_WAYLAND_TRANSFER_LIMIT = 8 * 1024 * 1024,
};

static const char *const VGFX_MIME_UTF8 = "text/plain;charset=utf-8";
static const char *const VGFX_MIME_TEXT = "text/plain";
static const char *const VGFX_MIME_URI = "text/uri-list";

/// @brief Local ABI for wl_data_offer listener callbacks.
typedef struct {
    void (*offer)(void *, struct wl_proxy *, const char *);
    void (*source_actions)(void *, struct wl_proxy *, uint32_t);
    void (*action)(void *, struct wl_proxy *, uint32_t);
} vgfx_wl_data_offer_listener_t;

/// @brief Local ABI for wl_data_source listener callbacks.
typedef struct {
    void (*target)(void *, struct wl_proxy *, const char *);
    void (*send)(void *, struct wl_proxy *, const char *, int32_t);
    void (*cancelled)(void *, struct wl_proxy *);
    void (*dnd_drop_performed)(void *, struct wl_proxy *);
    void (*dnd_finished)(void *, struct wl_proxy *);
    void (*action)(void *, struct wl_proxy *, uint32_t);
} vgfx_wl_data_source_listener_t;

/// @brief Local ABI for wl_data_device listener callbacks.
typedef struct {
    void (*data_offer)(void *, struct wl_proxy *, struct wl_proxy *);
    void (*enter)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *, wl_fixed_t, wl_fixed_t,
                  struct wl_proxy *);
    void (*leave)(void *, struct wl_proxy *);
    void (*motion)(void *, struct wl_proxy *, uint32_t, wl_fixed_t, wl_fixed_t);
    void (*drop)(void *, struct wl_proxy *);
    void (*selection)(void *, struct wl_proxy *, struct wl_proxy *);
} vgfx_wl_data_device_listener_t;

/// @brief Put a transfer descriptor into nonblocking mode.
/// @param fd Open descriptor to update.
/// @return 1 when both fcntl operations succeed, otherwise 0.
static int vgfx_data_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// @brief Find the first unused slot in the fixed transfer pool.
/// @param data Data-device state containing initialized transfer slots.
/// @return Borrowed free slot, or NULL when all eight are active.
static vgfx_wayland_transfer_t *vgfx_data_transfer(vgfx_wayland_data_t *data) {
    for (size_t i = 0; i < sizeof(data->transfers) / sizeof(data->transfers[0]); ++i)
        if (data->transfers[i].fd < 0)
            return &data->transfers[i];
    return NULL;
}

/// @brief Find an owned offer by protocol proxy identity.
/// @param data Data-device state whose offer list should be searched.
/// @param proxy wl_data_offer proxy to match.
/// @return Borrowed offer record, or NULL.
static vgfx_wayland_offer_t *vgfx_data_find_offer(vgfx_wayland_data_t *data,
                                                  struct wl_proxy *proxy) {
    for (vgfx_wayland_offer_t *offer = data->offers; offer; offer = offer->next)
        if (offer->proxy == proxy)
            return offer;
    return NULL;
}

/// @brief Unlink, destroy, and free an owned data offer.
/// @details Clears selection/drag role pointers that reference the offer before
///          issuing its destructor request.
/// @param data Owning data-device state.
/// @param offer Offer to destroy; invalid input is ignored.
static void vgfx_data_destroy_offer(vgfx_wayland_data_t *data, vgfx_wayland_offer_t *offer) {
    if (!data || !offer)
        return;
    vgfx_wayland_offer_t **link = &data->offers;
    while (*link && *link != offer)
        link = &(*link)->next;
    if (*link)
        *link = offer->next;
    if (data->selection == offer)
        data->selection = NULL;
    if (data->drag == offer)
        data->drag = NULL;
    if (offer->proxy)
        (void)data->connection->api.proxy_marshal_flags(
            offer->proxy,
            WL_DATA_OFFER_DESTROY,
            NULL,
            data->connection->api.proxy_get_version(offer->proxy),
            WL_MARSHAL_FLAG_DESTROY);
    free(offer);
}

/// @brief Choose the preferred textual MIME type advertised by an offer.
/// @param offer Offer to inspect.
/// @return UTF-8 MIME first, plain text second, or NULL.
static const char *vgfx_data_text_mime(const vgfx_wayland_offer_t *offer) {
    if (!offer)
        return NULL;
    if (offer->text_utf8)
        return VGFX_MIME_UTF8;
    return offer->text_plain ? VGFX_MIME_TEXT : NULL;
}

/// @brief Start a compositor-to-client offer transfer through a pipe.
/// @details Reserves a transfer slot, makes the read end nonblocking, sends the
///          write end with wl_data_offer.receive, closes the local write copy,
///          marks URI-list mode, and flushes the display.
/// @param data Data-device state owning the transfer.
/// @param offer Offer to receive.
/// @param mime Selected MIME string.
/// @param uri_list Non-zero when completion should emit file-drop events.
/// @return 1 when the receive request was started, otherwise 0.
static int vgfx_data_receive(vgfx_wayland_data_t *data,
                             vgfx_wayland_offer_t *offer,
                             const char *mime,
                             int uri_list) {
    int descriptors[2];
    vgfx_wayland_transfer_t *transfer = vgfx_data_transfer(data);
    if (!transfer || !offer || !mime || pipe(descriptors) != 0)
        return 0;
    if (!vgfx_data_nonblocking(descriptors[0])) {
        close(descriptors[0]);
        close(descriptors[1]);
        return 0;
    }
    (void)data->connection->api.proxy_marshal_flags(
        offer->proxy,
        WL_DATA_OFFER_RECEIVE,
        NULL,
        data->connection->api.proxy_get_version(offer->proxy),
        0,
        mime,
        descriptors[1]);
    close(descriptors[1]);
    transfer->fd = descriptors[0];
    transfer->uri_list = uri_list;
    (void)data->connection->api.display_flush(data->connection->display);
    return 1;
}

/// @brief Convert one hexadecimal digit to an integer.
/// @param c Candidate ASCII byte.
/// @return Value in [0, 15], or -1 for a non-hex digit.
static int vgfx_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// @brief Parse a completed `text/uri-list` transfer into file-drop events.
/// @details Splits CR/LF records, ignores comments/non-local authorities,
///          decodes valid percent escapes while rejecting NUL, enforces the
///          fixed public path capacity, and counts overflowed paths.
/// @param data Data-device state whose window receives events.
/// @param text Mutable NUL-terminated URI-list payload.
static void vgfx_data_emit_uris(vgfx_wayland_data_t *data, char *text) {
    char *line = text;
    while (line && *line) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end = '\0';
        if (line[0] != '#' && strncmp(line, "file://", 7) == 0) {
            const char *source = line + 7;
            if (strncmp(source, "localhost/", 10) == 0)
                source += 9;
            else if (source[0] != '/')
                source = "";
            char path[260];
            size_t out = 0;
            int valid = source[0] != '\0';
            while (*source && out + 1 < sizeof(path)) {
                if (*source == '%' && source[1] && source[2]) {
                    int hi = vgfx_hex(source[1]);
                    int lo = vgfx_hex(source[2]);
                    if (hi >= 0 && lo >= 0) {
                        int decoded = (hi << 4) | lo;
                        if (decoded == 0) {
                            valid = 0;
                            break;
                        }
                        path[out++] = (char)decoded;
                        source += 3;
                        continue;
                    }
                }
                path[out++] = *source++;
            }
            if (valid && !*source && out > 0) {
                path[out] = '\0';
                vgfx_event_t event = {.type = VGFX_EVENT_FILE_DROP,
                                      .time_ms = vgfx_platform_now_ms()};
                memcpy(event.data.file_drop.path, path, out + 1);
                vgfx_internal_enqueue_event(data->window, &event);
            } else if (valid) {
                vgfx_internal_note_event_overflow(data->window);
            }
        }
        if (!end) break;
        line = end + 1;
        while (*line == '\r' || *line == '\n') line++;
    }
}

/// @brief Complete one transfer and apply its payload ownership rules.
/// @details Closes the descriptor, terminates inbound bytes, emits URI drops
///          when appropriate, and fully resets outbound/URI slots.  Inbound
///          clipboard text remains in the slot for synchronous retrieval.
/// @param data Owning data-device state.
/// @param transfer Transfer to finish.
static void vgfx_data_finish_transfer(vgfx_wayland_data_t *data,
                                      vgfx_wayland_transfer_t *transfer) {
    if (transfer->fd >= 0)
        close(transfer->fd);
    transfer->fd = -1;
    if (!transfer->outbound && transfer->bytes) {
        transfer->bytes[transfer->size] = '\0';
        if (transfer->uri_list)
            vgfx_data_emit_uris(data, transfer->bytes);
    }
    if (transfer->uri_list || transfer->outbound) {
        free(transfer->bytes);
        memset(transfer, 0, sizeof(*transfer));
        transfer->fd = -1;
    }
}

/// @brief Mark a transfer failed, publish an error, and finalize it.
/// @param data Owning data-device state.
/// @param transfer Transfer to fail.
/// @param message Stable platform error message.
static void vgfx_data_fail_transfer(vgfx_wayland_data_t *data,
                                    vgfx_wayland_transfer_t *transfer,
                                    const char *message) {
    transfer->failed = 1;
    vgfx_internal_set_error(VGFX_ERR_PLATFORM, message);
    vgfx_data_finish_transfer(data, transfer);
}

/// @copydoc vgfx_wayland_data_tick
void vgfx_wayland_data_tick(vgfx_wayland_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < sizeof(data->transfers) / sizeof(data->transfers[0]); ++i) {
        vgfx_wayland_transfer_t *transfer = &data->transfers[i];
        if (transfer->fd < 0)
            continue;
        if (transfer->outbound) {
            ssize_t count = write(transfer->fd,
                                  transfer->bytes + transfer->offset,
                                  transfer->size - transfer->offset);
            if (count > 0) transfer->offset += (size_t)count;
            if (transfer->offset == transfer->size ||
                (count < 0 && errno != EAGAIN && errno != EINTR))
                vgfx_data_finish_transfer(data, transfer);
            continue;
        }
        if (transfer->capacity == 0) {
            transfer->capacity = 4096;
            transfer->bytes = malloc(transfer->capacity + 1);
            if (!transfer->bytes) {
                vgfx_data_fail_transfer(
                    data, transfer, "Wayland clipboard transfer allocation failed");
                continue;
            }
        }
        if (transfer->size == transfer->capacity && transfer->capacity < VGFX_WAYLAND_TRANSFER_LIMIT) {
            size_t next = transfer->capacity * 2;
            if (next > VGFX_WAYLAND_TRANSFER_LIMIT) next = VGFX_WAYLAND_TRANSFER_LIMIT;
            char *grown = realloc(transfer->bytes, next + 1);
            if (!grown) {
                vgfx_data_fail_transfer(
                    data, transfer, "Wayland clipboard transfer allocation failed");
                continue;
            }
            transfer->bytes = grown;
            transfer->capacity = next;
        }
        if (transfer->size == transfer->capacity) {
            vgfx_data_fail_transfer(data, transfer, "Wayland clipboard transfer exceeded 8 MiB");
            continue;
        }
        ssize_t count = read(transfer->fd,
                             transfer->bytes + transfer->size,
                             transfer->capacity - transfer->size);
        if (count > 0) transfer->size += (size_t)count;
        else if (count == 0)
            vgfx_data_finish_transfer(data, transfer);
        else if (errno != EAGAIN && errno != EINTR)
            vgfx_data_fail_transfer(data, transfer, "Wayland clipboard transfer read failed");
    }
    if (data->pending_selection &&
        (data->input->keyboard_serial || data->input->pointer_serial)) {
        char *text = data->local_text ? strdup(data->local_text) : NULL;
        if (!data->local_text || text) {
            data->pending_selection = 0;
            (void)vgfx_wayland_data_set_text(data, text);
            free(text);
        }
    }
}

/// @brief Record one MIME type advertised by a data offer.
/// @param opaque Borrowed data-device listener context.
/// @param proxy Offer proxy advertising the MIME.
/// @param mime Borrowed MIME string.
static void vgfx_offer_mime(void *opaque, struct wl_proxy *proxy, const char *mime) {
    vgfx_wayland_data_t *data = opaque;
    vgfx_wayland_offer_t *offer = vgfx_data_find_offer(data, proxy);
    if (!offer || !mime) return;
    if (strcmp(mime, VGFX_MIME_UTF8) == 0 || strcmp(mime, "UTF8_STRING") == 0)
        offer->text_utf8 = 1;
    else if (strcmp(mime, VGFX_MIME_TEXT) == 0)
        offer->text_plain = 1;
    else if (strcmp(mime, VGFX_MIME_URI) == 0)
        offer->uri_list = 1;
}

/// @brief Ignore source-action capability advertisements not needed for copy-only DND.
/// @param d Unused listener context.
/// @param p Unused offer proxy.
/// @param a Unused source-action mask.
static void vgfx_offer_source_actions(void *d, struct wl_proxy *p, uint32_t a) {
    (void)d;
    (void)p;
    (void)a;
}

/// @brief Record the compositor-selected DND action for an offer.
/// @param opaque Borrowed data-device listener context.
/// @param proxy Offer proxy whose action changed.
/// @param action Selected action mask.
static void vgfx_offer_action(void *opaque, struct wl_proxy *proxy, uint32_t action) {
    vgfx_wayland_data_t *data = opaque;
    vgfx_wayland_offer_t *offer = vgfx_data_find_offer(data, proxy);
    if (offer)
        offer->action = action;
}
static const vgfx_wl_data_offer_listener_t g_offer_listener = {
    vgfx_offer_mime, vgfx_offer_source_actions, vgfx_offer_action};

/// @brief Ignore target notifications for the fixed text-only clipboard source.
/// @param d Unused listener context.
/// @param p Unused source proxy.
/// @param m Unused selected MIME.
static void vgfx_source_target(void *d, struct wl_proxy *p, const char *m) { (void)d; (void)p; (void)m; }

/// @brief Begin a nonblocking client-to-consumer clipboard transfer.
/// @details Validates the local 8 MiB bound, reserves a transfer slot, copies
///          current text, and adopts the compositor-provided descriptor.
/// @param opaque Borrowed data-device listener context.
/// @param proxy Source proxy requesting data; unused.
/// @param mime Requested MIME; both advertised text types share the UTF-8 bytes.
/// @param fd Owned transfer descriptor, closed on rejection or completion.
static void vgfx_source_send(void *opaque, struct wl_proxy *proxy, const char *mime, int32_t fd) {
    (void)proxy; (void)mime;
    vgfx_wayland_data_t *data = opaque;
    vgfx_wayland_transfer_t *transfer = vgfx_data_transfer(data);
    if (!transfer || !data->local_text || !vgfx_data_nonblocking(fd)) { close(fd); return; }
    if (strnlen(data->local_text, VGFX_WAYLAND_TRANSFER_LIMIT + 1u) >
        VGFX_WAYLAND_TRANSFER_LIMIT) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Wayland clipboard text exceeded 8 MiB");
        close(fd);
        return;
    }
    transfer->bytes = strdup(data->local_text);
    if (!transfer->bytes) { close(fd); return; }
    transfer->fd = fd;
    transfer->outbound = 1;
    transfer->size = strlen(transfer->bytes);
}

/// @brief Handle compositor cancellation of the active clipboard source.
/// @param opaque Borrowed data-device listener context.
/// @param proxy Cancelled source proxy to destroy.
static void vgfx_source_cancelled(void *opaque, struct wl_proxy *proxy) {
    vgfx_wayland_data_t *data = opaque;
    if (data && data->source == proxy) {
        data->source = NULL;
        data->owns_selection = 0;
    }
    if (data && proxy) data->connection->api.proxy_destroy(proxy);
}

/// @brief Ignore a source lifecycle callback that carries no required state.
/// @param d Unused listener context.
/// @param p Unused source proxy.
static void vgfx_source_empty(void *d, struct wl_proxy *p) { (void)d; (void)p; }

/// @brief Ignore a source DND-action callback for clipboard-only sources.
/// @param d Unused listener context.
/// @param p Unused source proxy.
/// @param a Unused action value.
static void vgfx_source_action(void *d, struct wl_proxy *p, uint32_t a) { (void)d; (void)p; (void)a; }
static const vgfx_wl_data_source_listener_t g_source_listener = {
    vgfx_source_target, vgfx_source_send, vgfx_source_cancelled,
    vgfx_source_empty, vgfx_source_empty, vgfx_source_action};

/// @brief Adopt a newly announced wl_data_offer and install its listener.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device announcing the offer; unused.
/// @param proxy Newly owned offer proxy.
static void vgfx_device_offer(void *opaque, struct wl_proxy *device, struct wl_proxy *proxy) {
    (void)device;
    vgfx_wayland_data_t *data = opaque;
    vgfx_wayland_offer_t *offer = calloc(1, sizeof(*offer));
    if (!offer) { data->connection->api.proxy_destroy(proxy); return; }
    offer->proxy = proxy;
    offer->next = data->offers;
    data->offers = offer;
    data->connection->api.proxy_add_listener(
        proxy, (void (**)(void))(void *)&g_offer_listener, data);
}

/// @brief Begin a drag entering the application surface.
/// @details Associates the announced offer, records logical position, advertises
///          copy-only support on protocol version 3+, and accepts URI-list data
///          using the triggering seat serial.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device delivering the event; unused.
/// @param serial Seat serial authorizing acceptance.
/// @param surface Entered surface.
/// @param x Wayland fixed-point X coordinate.
/// @param y Wayland fixed-point Y coordinate.
/// @param proxy Offer proxy associated with the drag.
static void vgfx_device_enter(void *opaque, struct wl_proxy *device, uint32_t serial,
                              struct wl_proxy *surface, wl_fixed_t x, wl_fixed_t y,
                              struct wl_proxy *proxy) {
    (void)device;
    vgfx_wayland_data_t *data = opaque;
    if (!data || surface != data->input->surface) return;
    data->drag = vgfx_data_find_offer(data, proxy);
    data->drag_x = vgfx_wayland_fixed_to_pixel(x);
    data->drag_y = vgfx_wayland_fixed_to_pixel(y);
    const char *mime = data->drag && data->drag->uri_list ? VGFX_MIME_URI : NULL;
    if (data->drag) {
        uint32_t version = data->connection->api.proxy_get_version(data->drag->proxy);
        if (version >= 3)
            (void)data->connection->api.proxy_marshal_flags(
                data->drag->proxy,
                WL_DATA_OFFER_SET_ACTIONS,
                NULL,
                version,
                0,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        (void)data->connection->api.proxy_marshal_flags(data->drag->proxy,
            WL_DATA_OFFER_ACCEPT, NULL,
            version, 0, serial, mime);
    }
}

/// @brief End the current drag and destroy its offer.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device delivering the event; unused.
static void vgfx_device_leave(void *opaque, struct wl_proxy *device) {
    (void)device;
    vgfx_wayland_data_t *data = opaque;
    if (data && data->drag) vgfx_data_destroy_offer(data, data->drag);
}

/// @brief Update the latest logical drag position.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device delivering the event; unused.
/// @param time Native timestamp; unused.
/// @param x Wayland fixed-point X coordinate.
/// @param y Wayland fixed-point Y coordinate.
static void vgfx_device_motion(void *opaque, struct wl_proxy *device, uint32_t time,
                               wl_fixed_t x, wl_fixed_t y) {
    (void)device; (void)time;
    vgfx_wayland_data_t *data = opaque;
    if (data) { data->drag_x = vgfx_wayland_fixed_to_pixel(x); data->drag_y = vgfx_wayland_fixed_to_pixel(y); }
}

/// @brief Receive a dropped URI list and finish an accepted copy action.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device delivering the event; unused.
static void vgfx_device_drop(void *opaque, struct wl_proxy *device) {
    (void)device;
    vgfx_wayland_data_t *data = opaque;
    if (data && data->drag && data->drag->uri_list) {
        (void)vgfx_data_receive(data, data->drag, VGFX_MIME_URI, 1);
        if (data->connection->api.proxy_get_version(data->drag->proxy) >= 3 &&
            data->drag->action != 0)
            (void)data->connection->api.proxy_marshal_flags(data->drag->proxy,
                WL_DATA_OFFER_FINISH, NULL,
                data->connection->api.proxy_get_version(data->drag->proxy), 0);
    }
}

/// @brief Replace the external clipboard selection offer.
/// @details Clears local ownership when a new external offer exists and
///          destroys the prior selection unless it is still the active drag.
/// @param opaque Borrowed data-device listener context.
/// @param device Data device delivering the event; unused.
/// @param proxy New selection offer proxy, or NULL.
static void vgfx_device_selection(void *opaque, struct wl_proxy *device, struct wl_proxy *proxy) {
    (void)device;
    vgfx_wayland_data_t *data = opaque;
    vgfx_wayland_offer_t *old = data->selection;
    data->selection = vgfx_data_find_offer(data, proxy);
    if (data->selection)
        data->owns_selection = 0;
    if (old && old != data->drag) vgfx_data_destroy_offer(data, old);
}
static const vgfx_wl_data_device_listener_t g_device_listener = {
    vgfx_device_offer, vgfx_device_enter, vgfx_device_leave,
    vgfx_device_motion, vgfx_device_drop, vgfx_device_selection};

/// @copydoc vgfx_wayland_data_open
int vgfx_wayland_data_open(vgfx_wayland_data_t *data,
                           vgfx_wayland_connection_t *connection,
                           vgfx_wayland_input_t *input,
                           struct vgfx_window *window) {
    if (!data || !connection || !input || !window) return 0;
    memset(data, 0, sizeof(*data));
    for (size_t i = 0; i < sizeof(data->transfers) / sizeof(data->transfers[0]); ++i)
        data->transfers[i].fd = -1;
    data->connection = connection;
    data->input = input;
    data->window = window;
    if (!connection->data_device_manager) return 1;
    data->device = connection->api.proxy_marshal_flags(connection->data_device_manager,
        WL_DATA_MANAGER_GET_DEVICE, connection->api.data_device_interface,
        connection->api.proxy_get_version(connection->data_device_manager), 0, NULL,
        connection->seat);
    return data->device && connection->api.proxy_add_listener(data->device,
        (void (**)(void))(void *)&g_device_listener, data) == 0;
}

/// @copydoc vgfx_wayland_data_close
void vgfx_wayland_data_close(vgfx_wayland_data_t *data) {
    if (!data)
        return;
    if (!data->connection) {
        memset(data, 0, sizeof(*data));
        return;
    }
    for (size_t i = 0; i < sizeof(data->transfers) / sizeof(data->transfers[0]); ++i) {
        if (data->transfers[i].fd >= 0) close(data->transfers[i].fd);
        free(data->transfers[i].bytes);
    }
    while (data->offers) vgfx_data_destroy_offer(data, data->offers);
    if (data->source)
        (void)data->connection->api.proxy_marshal_flags(data->source, WL_DATA_SOURCE_DESTROY,
            NULL, data->connection->api.proxy_get_version(data->source), WL_MARSHAL_FLAG_DESTROY);
    if (data->device) {
        if (data->connection->api.proxy_get_version(data->device) >= 2)
            (void)data->connection->api.proxy_marshal_flags(data->device, WL_DATA_DEVICE_RELEASE,
                NULL, data->connection->api.proxy_get_version(data->device), WL_MARSHAL_FLAG_DESTROY);
        else data->connection->api.proxy_destroy(data->device);
    }
    free(data->local_text);
    memset(data, 0, sizeof(*data));
}

/// @copydoc vgfx_wayland_data_has_text
int vgfx_wayland_data_has_text(const vgfx_wayland_data_t *data) {
    return data && (((data->owns_selection || data->pending_selection) && data->local_text &&
                     data->local_text[0]) ||
                    vgfx_data_text_mime(data->selection));
}

/// @copydoc vgfx_wayland_data_get_text
char *vgfx_wayland_data_get_text(vgfx_wayland_data_t *data) {
    if (!data) return NULL;
    if ((data->owns_selection || data->pending_selection) && data->local_text)
        return strdup(data->local_text);
    const char *mime = vgfx_data_text_mime(data->selection);
    if (!mime || !vgfx_data_receive(data, data->selection, mime, 0)) return NULL;
    vgfx_wayland_transfer_t *transfer = NULL;
    for (size_t i = 0; i < sizeof(data->transfers) / sizeof(data->transfers[0]); ++i)
        if (data->transfers[i].fd >= 0 && !data->transfers[i].outbound && !data->transfers[i].uri_list)
            transfer = &data->transfers[i];
    if (!transfer) return NULL;
    int64_t started_ms = vgfx_platform_now_ms();
    while (transfer->fd >= 0) {
        int64_t now_ms = vgfx_platform_now_ms();
        if (now_ms < started_ms || now_ms - started_ms >= 1000)
            break;
        vgfx_wayland_data_tick(data);
        if (transfer->fd < 0) break;
        struct pollfd pollfd = {.fd = transfer->fd, .events = POLLIN | POLLHUP};
        int poll_result = poll(&pollfd, 1, 10);
        if (poll_result < 0 && errno == EINTR)
            continue;
        if (poll_result < 0 || (poll_result > 0 && (pollfd.revents & (POLLERR | POLLNVAL)))) {
            vgfx_data_fail_transfer(data, transfer, "Wayland clipboard transfer poll failed");
            break;
        }
    }
    if (transfer->fd >= 0)
        vgfx_data_fail_transfer(data, transfer, "Wayland clipboard transfer timed out");
    int failed = transfer->failed;
    char *result = transfer->bytes;
    memset(transfer, 0, sizeof(*transfer));
    transfer->fd = -1;
    if (failed) {
        free(result);
        return NULL;
    }
    return result;
}

/// @copydoc vgfx_wayland_data_set_text
int vgfx_wayland_data_set_text(vgfx_wayland_data_t *data, const char *text) {
    if (!data) return 0;
    if (text && strnlen(text, VGFX_WAYLAND_TRANSFER_LIMIT + 1u) > VGFX_WAYLAND_TRANSFER_LIMIT) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Wayland clipboard text exceeded 8 MiB");
        return 0;
    }
    char *copy = text ? strdup(text) : NULL;
    if (text && !copy) return 0;
    free(data->local_text);
    data->local_text = copy;
    if (!data->device || !data->connection->data_device_manager) {
        data->pending_selection = 0;
        data->owns_selection = text != NULL;
        return text == NULL;
    }
    uint32_t serial = data->input->keyboard_serial ? data->input->keyboard_serial
                                                   : data->input->pointer_serial;
    if (!serial) {
        data->pending_selection = 1;
        data->owns_selection = text != NULL;
        return 1;
    }
    if (data->source)
        (void)data->connection->api.proxy_marshal_flags(data->source, WL_DATA_SOURCE_DESTROY,
            NULL, data->connection->api.proxy_get_version(data->source), WL_MARSHAL_FLAG_DESTROY);
    data->source = NULL;
    if (text) {
        data->source = data->connection->api.proxy_marshal_flags(
            data->connection->data_device_manager, WL_DATA_MANAGER_CREATE_SOURCE,
            data->connection->api.data_source_interface,
            data->connection->api.proxy_get_version(data->connection->data_device_manager), 0, NULL);
        if (!data->source || data->connection->api.proxy_add_listener(data->source,
            (void (**)(void))(void *)&g_source_listener, data) != 0) return 0;
        (void)data->connection->api.proxy_marshal_flags(data->source, WL_DATA_SOURCE_OFFER,
            NULL, data->connection->api.proxy_get_version(data->source), 0, VGFX_MIME_UTF8);
        (void)data->connection->api.proxy_marshal_flags(data->source, WL_DATA_SOURCE_OFFER,
            NULL, data->connection->api.proxy_get_version(data->source), 0, VGFX_MIME_TEXT);
    }
    (void)data->connection->api.proxy_marshal_flags(data->device, WL_DATA_DEVICE_SET_SELECTION,
        NULL, data->connection->api.proxy_get_version(data->device), 0, data->source, serial);
    int result = data->connection->api.display_flush(data->connection->display);
    if (result >= 0 || errno == EAGAIN) {
        data->pending_selection = 0;
        data->owns_selection = text != NULL;
        return 1;
    }
    data->pending_selection = 1;
    return 0;
}
