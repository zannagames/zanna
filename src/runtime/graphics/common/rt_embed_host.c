//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_embed_host.c
// Purpose: Zanna.System.EmbedHost — managed handles over the embed
//          frame/input channel for the Studio play pane and its loopback
//          tests (ADR 0225).
// Key invariants:
//   - Non-trapping surface: every failure reports 0/null; the pane shows
//     truthful "waiting"/"exited" states instead of dying.
//   - AcquireFrameToImage is the single-copy path: channel slot →
//     borrowed widget RGBA8 buffer, no intermediate Pixels.
//   - Pixels bridges convert between packed-u32 Pixels and channel RGBA8
//     rows without retaining allocations beyond one scratch per handle.
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated and GC-managed; Close frees
//     the mapping and scratch and is idempotent.
// Links: src/lib/graphics/include/vgfx_embed_channel.h,
//        src/runtime/graphics/gui/rt_gui_image.c
//
//===----------------------------------------------------------------------===//

#include "rt_embed_host.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

#include "../2d/rt_pixels_internal.h"
#include "../gui/rt_gui.h"
#include "rt_pixels.h"
#include "vgfx_embed_channel.h"

/// @brief Managed embed-channel handle payload.
typedef struct {
    vgfx_embed_channel_t *channel;
    uint8_t *scratch; ///< Conversion buffer (frame capacity), lazily grown.
    size_t scratch_bytes;
    vgfx_embed_event_t last_event;
    int32_t is_host;
} rt_embed_host_impl;

/// @brief Resolve a handle back to its payload, or NULL.
static rt_embed_host_impl *embed_checked(void *handle) {
    if (!rt_obj_is_instance(handle, RT_EMBED_HOST_CLASS_ID, sizeof(rt_embed_host_impl)))
        return NULL;
    return (rt_embed_host_impl *)handle;
}

/// @brief Copy a runtime string into a bounded C name buffer.
static int embed_copy_name(rt_string name, char *out, size_t out_size) {
    if (!name)
        return 0;
    const char *bytes = rt_string_cstr(name);
    if (!bytes || !bytes[0])
        return 0;
    size_t len = strlen(bytes);
    if (len >= out_size)
        return 0;
    memcpy(out, bytes, len + 1u);
    return 1;
}

/// @brief Allocate one managed handle around an open channel.
static void *embed_wrap(vgfx_embed_channel_t *channel, int32_t is_host) {
    rt_embed_host_impl *impl = (rt_embed_host_impl *)rt_obj_new_i64(
        RT_EMBED_HOST_CLASS_ID, (int64_t)sizeof(rt_embed_host_impl));
    if (!impl) {
        vgfx_embed_channel_close(channel);
        return NULL;
    }
    impl->channel = channel;
    impl->scratch = NULL;
    impl->scratch_bytes = 0;
    impl->is_host = is_host;
    memset(&impl->last_event, 0, sizeof(impl->last_event));
    return impl;
}

/// @brief Ensure the conversion scratch holds at least @p bytes.
static uint8_t *embed_scratch(rt_embed_host_impl *impl, size_t bytes) {
    if (impl->scratch && impl->scratch_bytes >= bytes)
        return impl->scratch;
    uint8_t *grown = (uint8_t *)realloc(impl->scratch, bytes);
    if (!grown)
        return NULL;
    impl->scratch = grown;
    impl->scratch_bytes = bytes;
    return grown;
}

void *rt_embed_host_create(rt_string name, int64_t max_width, int64_t max_height) {
    char cname[97];
    if (!embed_copy_name(name, cname, sizeof(cname)))
        return NULL;
    if (max_width <= 0 || max_height <= 0 || max_width > 8192 || max_height > 8192)
        return NULL;
    vgfx_embed_channel_t *channel = NULL;
    if (!vgfx_embed_channel_create(cname, (int32_t)max_width, (int32_t)max_height, &channel))
        return NULL;
    return embed_wrap(channel, 1);
}

void *rt_embed_host_attach(rt_string name) {
    char cname[97];
    if (!embed_copy_name(name, cname, sizeof(cname)))
        return NULL;
    vgfx_embed_channel_t *channel = NULL;
    if (!vgfx_embed_channel_attach(cname, &channel))
        return NULL;
    return embed_wrap(channel, 0);
}

void rt_embed_host_close(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl)
        return;
    if (impl->channel) {
        vgfx_embed_channel_close(impl->channel);
        impl->channel = NULL;
    }
    free(impl->scratch);
    impl->scratch = NULL;
    impl->scratch_bytes = 0;
}

int64_t rt_embed_host_is_valid(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    return (impl && impl->channel) ? 1 : 0;
}

int64_t rt_embed_host_frame_width(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel)
        return 0;
    int32_t w = 0, h = 0;
    if (!vgfx_embed_channel_frame_size(impl->channel, &w, &h))
        return 0;
    return w;
}

int64_t rt_embed_host_frame_height(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel)
        return 0;
    int32_t w = 0, h = 0;
    if (!vgfx_embed_channel_frame_size(impl->channel, &w, &h))
        return 0;
    return h;
}

int64_t rt_embed_host_acquire_frame_into(void *handle, void *pixels) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || !impl->is_host || !pixels)
        return 0;
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    int32_t expected_w = 0, expected_h = 0;
    if (!pv || !pv->data ||
        !vgfx_embed_channel_frame_size(impl->channel, &expected_w, &expected_h) ||
        pv->width != (int64_t)expected_w || pv->height != (int64_t)expected_h)
        return 0;
    int32_t max_w = 0, max_h = 0;
    vgfx_embed_channel_capacity(impl->channel, &max_w, &max_h);
    if (max_w <= 0 || max_h <= 0)
        return 0;
    uint8_t *scratch = embed_scratch(impl, (size_t)max_w * (size_t)max_h * 4u);
    if (!scratch)
        return 0;
    int32_t w = 0, h = 0;
    if (!vgfx_embed_channel_acquire_frame(impl->channel, scratch, &w, &h))
        return 0;
    if (pv->width != (int64_t)w || pv->height != (int64_t)h)
        return 0;
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *px = &scratch[i * 4u];
        pv->data[i] = ((uint32_t)px[0] << 24) | ((uint32_t)px[1] << 16) | ((uint32_t)px[2] << 8) |
                      (uint32_t)px[3];
    }
    pixels_touch(pv);
    return 1;
}

int64_t rt_embed_host_acquire_frame_to_image(void *handle, void *image) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || !impl->is_host || !image)
        return 0;
    int32_t max_w = 0, max_h = 0;
    vgfx_embed_channel_capacity(impl->channel, &max_w, &max_h);
    if (max_w <= 0 || max_h <= 0)
        return 0;
    uint8_t *dst = rt_gui_image_borrow_rgba(image, max_w, max_h);
    if (!dst)
        return 0;
    int32_t w = 0, h = 0;
    if (!vgfx_embed_channel_acquire_frame(impl->channel, dst, &w, &h))
        return 0;
    rt_gui_image_commit_rgba(image, w, h);
    return 1;
}

int64_t rt_embed_host_publish_frame(void *handle, void *pixels) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || impl->is_host || !pixels)
        return 0;
    int64_t w = rt_pixels_width(pixels);
    int64_t h = rt_pixels_height(pixels);
    const uint32_t *src = rt_pixels_raw_buffer(pixels);
    int32_t max_w = 0, max_h = 0;
    vgfx_embed_channel_capacity(impl->channel, &max_w, &max_h);
    if (w <= 0 || h <= 0 || w > max_w || h > max_h || !src)
        return 0;
    uint8_t *scratch = embed_scratch(impl, (size_t)w * (size_t)h * 4u);
    if (!scratch)
        return 0;
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++) {
        uint32_t px = src[i];
        uint8_t *out = &scratch[i * 4u];
        out[0] = (uint8_t)((px >> 24) & 0xFFu);
        out[1] = (uint8_t)((px >> 16) & 0xFFu);
        out[2] = (uint8_t)((px >> 8) & 0xFFu);
        out[3] = (uint8_t)(px & 0xFFu);
    }
    return vgfx_embed_channel_publish_frame(impl->channel, scratch, (int32_t)w, (int32_t)h);
}

int64_t rt_embed_host_push_event(void *handle, int64_t kind, int64_t a, int64_t b, int64_t c) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || !impl->is_host || kind <= VGFX_EMBED_EVENT_NONE ||
        kind > VGFX_EMBED_EVENT_CLOSE || a < INT32_MIN || a > INT32_MAX || b < INT32_MIN ||
        b > INT32_MAX || c < INT32_MIN || c > INT32_MAX)
        return 0;
    vgfx_embed_event_t event;
    event.kind = (int32_t)kind;
    event.a = (int32_t)a;
    event.b = (int32_t)b;
    event.c = (int32_t)c;
    return vgfx_embed_channel_push_event(impl->channel, &event);
}

int64_t rt_embed_host_poll_event(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || impl->is_host)
        return 0;
    if (!vgfx_embed_channel_poll_event(impl->channel, &impl->last_event))
        return 0;
    return impl->last_event.kind;
}

int64_t rt_embed_host_event_a(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    return impl ? impl->last_event.a : 0;
}

int64_t rt_embed_host_event_b(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    return impl ? impl->last_event.b : 0;
}

int64_t rt_embed_host_event_c(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    return impl ? impl->last_event.c : 0;
}

int64_t rt_embed_host_set_size(void *handle, int64_t width, int64_t height) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel || !impl->is_host || width <= 0 || height <= 0)
        return 0;
    int32_t max_w = 0, max_h = 0;
    vgfx_embed_channel_capacity(impl->channel, &max_w, &max_h);
    if (max_w <= 0 || max_h <= 0)
        return 0;
    if (width > max_w)
        width = max_w;
    if (height > max_h)
        height = max_h;
    return vgfx_embed_channel_set_size(impl->channel, (int32_t)width, (int32_t)height);
}

int64_t rt_embed_host_producer_attached(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel)
        return 0;
    return vgfx_embed_channel_producer_attached(impl->channel);
}

int64_t rt_embed_host_producer_exited(void *handle) {
    rt_embed_host_impl *impl = embed_checked(handle);
    if (!impl || !impl->channel)
        return 0;
    return vgfx_embed_channel_producer_exited(impl->channel);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

void *rt_embed_host_create(rt_string name, int64_t max_width, int64_t max_height) {
    (void)name;
    (void)max_width;
    (void)max_height;
    return NULL;
}

void *rt_embed_host_attach(rt_string name) {
    (void)name;
    return NULL;
}

void rt_embed_host_close(void *handle) {
    (void)handle;
}

int64_t rt_embed_host_is_valid(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_frame_width(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_frame_height(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_acquire_frame_into(void *handle, void *pixels) {
    (void)handle;
    (void)pixels;
    return 0;
}

int64_t rt_embed_host_acquire_frame_to_image(void *handle, void *image) {
    (void)handle;
    (void)image;
    return 0;
}

int64_t rt_embed_host_publish_frame(void *handle, void *pixels) {
    (void)handle;
    (void)pixels;
    return 0;
}

int64_t rt_embed_host_push_event(void *handle, int64_t kind, int64_t a, int64_t b, int64_t c) {
    (void)handle;
    (void)kind;
    (void)a;
    (void)b;
    (void)c;
    return 0;
}

int64_t rt_embed_host_poll_event(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_event_a(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_event_b(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_event_c(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_set_size(void *handle, int64_t width, int64_t height) {
    (void)handle;
    (void)width;
    (void)height;
    return 0;
}

int64_t rt_embed_host_producer_attached(void *handle) {
    (void)handle;
    return 0;
}

int64_t rt_embed_host_producer_exited(void *handle) {
    (void)handle;
    return 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
