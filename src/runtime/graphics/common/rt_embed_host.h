//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_embed_host.h
// Purpose: Managed host/producer handles over the embed frame channel
//          (Zanna.System.EmbedHost, ADR 0225).
// Key invariants:
//   - Handles are rt_obj_new_i64-allocated and GC-managed; Close releases
//     the OS mapping and is idempotent.
//   - Every operation is non-trapping: failures report 0/null so an
//     editor pane can render truthful state instead of dying.
// Ownership/Lifetime:
//   - The HOST side owns the named OS object; producers only attach.
// Links: src/lib/graphics/include/vgfx_embed_channel.h,
//        src/runtime/graphics/common/rt_embed_host.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_EMBED_HOST_CLASS_ID INT64_C(-0x440204)

void *rt_embed_host_create(rt_string name, int64_t max_width, int64_t max_height);
void *rt_embed_host_attach(rt_string name);
void rt_embed_host_close(void *handle);
int64_t rt_embed_host_is_valid(void *handle);
int64_t rt_embed_host_frame_width(void *handle);
int64_t rt_embed_host_frame_height(void *handle);
int64_t rt_embed_host_acquire_frame_into(void *handle, void *pixels);
int64_t rt_embed_host_acquire_frame_to_image(void *handle, void *image);
int64_t rt_embed_host_publish_frame(void *handle, void *pixels);
int64_t rt_embed_host_push_event(void *handle, int64_t kind, int64_t a, int64_t b, int64_t c);
int64_t rt_embed_host_poll_event(void *handle);
int64_t rt_embed_host_event_a(void *handle);
int64_t rt_embed_host_event_b(void *handle);
int64_t rt_embed_host_event_c(void *handle);
int64_t rt_embed_host_set_size(void *handle, int64_t width, int64_t height);
int64_t rt_embed_host_producer_attached(void *handle);
int64_t rt_embed_host_producer_exited(void *handle);

#ifdef __cplusplus
}
#endif
