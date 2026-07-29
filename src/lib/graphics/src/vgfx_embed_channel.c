//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_embed_channel.c
// Purpose: Cross-platform shared-memory implementation of the embed
//          frame/input channel (ADR 0225).
// Key invariants:
//   - The shared header is a flat POD block of C11 atomics; both sides map
//     the identical layout and never take locks.
//   - Frame publication is a two-slot seqlock: odd sequence = slot being
//     written; the consumer retries torn copies and always keeps the last
//     complete frame it saw.
//   - The input ring holds a power-of-two record count; the writer drops
//     the oldest record on overflow so the game never back-pressures the
//     editor UI thread.
// Ownership/Lifetime:
//   - POSIX: the host shm_unlink()s on close; mappings live per handle.
//     Windows: the section dies with the last handle automatically.
// Links: src/lib/graphics/src/vgfx_embed_channel.h
//
//===----------------------------------------------------------------------===//

#include "vgfx_embed_channel.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define EMBED_MAGIC UINT32_C(0x5A454D42) /* "ZEMB" */
#define EMBED_VERSION UINT32_C(1)
#define EMBED_INPUT_CAPACITY 256u /* power of two */
#define EMBED_MAX_DIMENSION 8192

/// @brief Flat shared header both processes map at offset zero.
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t max_width;
    int32_t max_height;
    _Atomic uint32_t producer_attached;
    _Atomic uint32_t producer_exited;
    _Atomic int32_t requested_width;
    _Atomic int32_t requested_height;
    _Atomic uint64_t frame_seq;    ///< Even = slot (seq/2)&1 complete; odd = write in progress.
    _Atomic int32_t frame_width;   ///< Dimensions of the most recent published frame.
    _Atomic int32_t frame_height;
    _Atomic uint64_t input_head;   ///< Next write index (host).
    _Atomic uint64_t input_tail;   ///< Next read index (game).
    vgfx_embed_event_t input_ring[EMBED_INPUT_CAPACITY];
} vgfx_embed_header_t;

struct vgfx_embed_channel {
    vgfx_embed_header_t *header;
    uint8_t *slots[2];
    size_t slot_bytes;
    size_t map_bytes;
    int is_host;
    uint64_t last_seen_seq;
#if defined(_WIN32)
    HANDLE mapping;
#else
    char shm_name[128];
#endif
};

/// @brief Validate a portable channel name (letters, digits, '-', '_').
static int embed_name_ok(const char *name) {
    if (!name || !name[0])
        return 0;
    size_t len = strlen(name);
    if (len > 96)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

/// @brief Total mapping size for one channel configuration.
static size_t embed_map_bytes(int32_t max_w, int32_t max_h, size_t *slot_bytes) {
    size_t frame = (size_t)max_w * (size_t)max_h * 4u;
    *slot_bytes = frame;
    return sizeof(vgfx_embed_header_t) + frame * 2u;
}

/// @brief Wire the per-handle pointers into an established mapping.
static void embed_wire(vgfx_embed_channel_t *ch, void *base) {
    ch->header = (vgfx_embed_header_t *)base;
    ch->slots[0] = (uint8_t *)base + sizeof(vgfx_embed_header_t);
    ch->slots[1] = ch->slots[0] + ch->slot_bytes;
}

int vgfx_embed_channel_create(const char *name, int32_t max_w, int32_t max_h,
                              vgfx_embed_channel_t **out) {
    if (out)
        *out = NULL;
    if (!out || !embed_name_ok(name))
        return 0;
    if (max_w <= 0 || max_h <= 0 || max_w > EMBED_MAX_DIMENSION || max_h > EMBED_MAX_DIMENSION)
        return 0;
    vgfx_embed_channel_t *ch = (vgfx_embed_channel_t *)calloc(1, sizeof(*ch));
    if (!ch)
        return 0;
    ch->is_host = 1;
    ch->map_bytes = embed_map_bytes(max_w, max_h, &ch->slot_bytes);
#if defined(_WIN32)
    char object_name[160];
    snprintf(object_name, sizeof(object_name), "Local\\zanna-embed-%s", name);
    ch->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     (DWORD)((uint64_t)ch->map_bytes >> 32),
                                     (DWORD)(ch->map_bytes & 0xFFFFFFFFu), object_name);
    if (!ch->mapping) {
        free(ch);
        return 0;
    }
    void *base = MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0, ch->map_bytes);
    if (!base) {
        CloseHandle(ch->mapping);
        free(ch);
        return 0;
    }
#else
    snprintf(ch->shm_name, sizeof(ch->shm_name), "/zanna-embed-%s", name);
    shm_unlink(ch->shm_name); /* stale object from a crashed prior host */
    int fd = shm_open(ch->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        free(ch);
        return 0;
    }
    if (ftruncate(fd, (off_t)ch->map_bytes) != 0) {
        close(fd);
        shm_unlink(ch->shm_name);
        free(ch);
        return 0;
    }
    void *base = mmap(NULL, ch->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        shm_unlink(ch->shm_name);
        free(ch);
        return 0;
    }
#endif
    memset(base, 0, sizeof(vgfx_embed_header_t));
    embed_wire(ch, base);
    ch->header->magic = EMBED_MAGIC;
    ch->header->version = EMBED_VERSION;
    ch->header->max_width = max_w;
    ch->header->max_height = max_h;
    *out = ch;
    return 1;
}

int vgfx_embed_channel_attach(const char *name, vgfx_embed_channel_t **out) {
    if (out)
        *out = NULL;
    if (!out || !embed_name_ok(name))
        return 0;
    vgfx_embed_channel_t *ch = (vgfx_embed_channel_t *)calloc(1, sizeof(*ch));
    if (!ch)
        return 0;
#if defined(_WIN32)
    char object_name[160];
    snprintf(object_name, sizeof(object_name), "Local\\zanna-embed-%s", name);
    ch->mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, object_name);
    if (!ch->mapping) {
        free(ch);
        return 0;
    }
    void *probe = MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                sizeof(vgfx_embed_header_t));
    if (!probe) {
        CloseHandle(ch->mapping);
        free(ch);
        return 0;
    }
#else
    snprintf(ch->shm_name, sizeof(ch->shm_name), "/zanna-embed-%s", name);
    int fd = shm_open(ch->shm_name, O_RDWR, 0600);
    if (fd < 0) {
        free(ch);
        return 0;
    }
    void *probe = mmap(NULL, sizeof(vgfx_embed_header_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       0);
    if (probe == MAP_FAILED) {
        close(fd);
        free(ch);
        return 0;
    }
#endif
    vgfx_embed_header_t *header = (vgfx_embed_header_t *)probe;
    if (header->magic != EMBED_MAGIC || header->version != EMBED_VERSION ||
        header->max_width <= 0 || header->max_height <= 0 ||
        header->max_width > EMBED_MAX_DIMENSION || header->max_height > EMBED_MAX_DIMENSION) {
#if defined(_WIN32)
        UnmapViewOfFile(probe);
        CloseHandle(ch->mapping);
#else
        munmap(probe, sizeof(vgfx_embed_header_t));
        close(fd);
#endif
        free(ch);
        return 0;
    }
    int32_t max_w = header->max_width;
    int32_t max_h = header->max_height;
    ch->map_bytes = embed_map_bytes(max_w, max_h, &ch->slot_bytes);
#if defined(_WIN32)
    UnmapViewOfFile(probe);
    void *base = MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0, ch->map_bytes);
    if (!base) {
        CloseHandle(ch->mapping);
        free(ch);
        return 0;
    }
#else
    munmap(probe, sizeof(vgfx_embed_header_t));
    void *base = mmap(NULL, ch->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        free(ch);
        return 0;
    }
#endif
    embed_wire(ch, base);
    atomic_store(&ch->header->producer_attached, 1u);
    *out = ch;
    return 1;
}

void vgfx_embed_channel_close(vgfx_embed_channel_t *channel) {
    if (!channel)
        return;
    if (channel->header) {
        if (!channel->is_host)
            atomic_store(&channel->header->producer_exited, 1u);
#if defined(_WIN32)
        UnmapViewOfFile(channel->header);
#else
        munmap(channel->header, channel->map_bytes);
#endif
    }
#if defined(_WIN32)
    if (channel->mapping)
        CloseHandle(channel->mapping);
#else
    if (channel->is_host)
        shm_unlink(channel->shm_name);
#endif
    free(channel);
}

int vgfx_embed_channel_publish_frame(vgfx_embed_channel_t *channel, const uint8_t *rgba,
                                     int32_t width, int32_t height) {
    if (!channel || !channel->header || !rgba || width <= 0 || height <= 0)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    if (width > h->max_width || height > h->max_height)
        return 0;
    uint64_t seq = atomic_load(&h->frame_seq);
    uint64_t writing = seq + 1u; /* odd = in progress */
    atomic_store_explicit(&h->frame_seq, writing, memory_order_release);
    /* Index by the sequence the reader will observe (writing + 1). */
    uint8_t *slot = channel->slots[((writing + 1u) / 2u) & 1u];
    memcpy(slot, rgba, (size_t)width * (size_t)height * 4u);
    atomic_store_explicit(&h->frame_width, width, memory_order_release);
    atomic_store_explicit(&h->frame_height, height, memory_order_release);
    atomic_store_explicit(&h->frame_seq, writing + 1u, memory_order_release);
    atomic_store(&h->producer_attached, 1u);
    return 1;
}

int vgfx_embed_channel_acquire_frame(vgfx_embed_channel_t *channel, uint8_t *dst, int32_t *width,
                                     int32_t *height) {
    if (!channel || !channel->header || !dst || !width || !height)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t seq = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
        if (seq == 0 || (seq & 1u) || seq == channel->last_seen_seq)
            return 0;
        int32_t w = atomic_load_explicit(&h->frame_width, memory_order_acquire);
        int32_t hgt = atomic_load_explicit(&h->frame_height, memory_order_acquire);
        if (w <= 0 || hgt <= 0 || w > h->max_width || hgt > h->max_height)
            return 0;
        const uint8_t *slot = channel->slots[(seq / 2u) & 1u];
        memcpy(dst, slot, (size_t)w * (size_t)hgt * 4u);
        uint64_t reread = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
        if (reread == seq) {
            channel->last_seen_seq = seq;
            *width = w;
            *height = hgt;
            return 1;
        }
        /* torn copy: the producer republished mid-read — retry */
    }
    return 0;
}

int vgfx_embed_channel_push_event(vgfx_embed_channel_t *channel, const vgfx_embed_event_t *event) {
    if (!channel || !channel->header || !event)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    uint64_t head = atomic_load_explicit(&h->input_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&h->input_tail, memory_order_acquire);
    if (head - tail >= EMBED_INPUT_CAPACITY) {
        /* Ring full: drop the oldest record instead of stalling the UI. */
        atomic_store_explicit(&h->input_tail, tail + 1u, memory_order_release);
    }
    h->input_ring[head & (EMBED_INPUT_CAPACITY - 1u)] = *event;
    atomic_store_explicit(&h->input_head, head + 1u, memory_order_release);
    return 1;
}

int vgfx_embed_channel_poll_event(vgfx_embed_channel_t *channel, vgfx_embed_event_t *event) {
    if (!channel || !channel->header || !event)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    uint64_t tail = atomic_load_explicit(&h->input_tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&h->input_head, memory_order_acquire);
    if (tail >= head)
        return 0;
    *event = h->input_ring[tail & (EMBED_INPUT_CAPACITY - 1u)];
    atomic_store_explicit(&h->input_tail, tail + 1u, memory_order_release);
    return 1;
}

int vgfx_embed_channel_set_size(vgfx_embed_channel_t *channel, int32_t width, int32_t height) {
    if (!channel || !channel->header || width <= 0 || height <= 0)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    if (width > h->max_width)
        width = h->max_width;
    if (height > h->max_height)
        height = h->max_height;
    atomic_store(&h->requested_width, width);
    atomic_store(&h->requested_height, height);
    vgfx_embed_event_t resize = {VGFX_EMBED_EVENT_RESIZE, width, height, 0};
    return vgfx_embed_channel_push_event(channel, &resize);
}

void vgfx_embed_channel_mark_exited(vgfx_embed_channel_t *channel) {
    if (channel && channel->header)
        atomic_store(&channel->header->producer_exited, 1u);
}

int vgfx_embed_channel_producer_exited(const vgfx_embed_channel_t *channel) {
    if (!channel || !channel->header)
        return 0;
    return atomic_load(&((vgfx_embed_channel_t *)channel)->header->producer_exited) != 0u;
}

int vgfx_embed_channel_producer_attached(const vgfx_embed_channel_t *channel) {
    if (!channel || !channel->header)
        return 0;
    return atomic_load(&((vgfx_embed_channel_t *)channel)->header->producer_attached) != 0u;
}

int vgfx_embed_channel_frame_size(const vgfx_embed_channel_t *channel, int32_t *width,
                                  int32_t *height) {
    if (width)
        *width = 0;
    if (height)
        *height = 0;
    if (!channel || !channel->header)
        return 0;
    vgfx_embed_header_t *h = ((vgfx_embed_channel_t *)channel)->header;
    uint64_t seq = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
    if (seq == 0)
        return 0;
    if (width)
        *width = atomic_load_explicit(&h->frame_width, memory_order_acquire);
    if (height)
        *height = atomic_load_explicit(&h->frame_height, memory_order_acquire);
    return 1;
}

void vgfx_embed_channel_capacity(const vgfx_embed_channel_t *channel, int32_t *max_w,
                                 int32_t *max_h) {
    if (max_w)
        *max_w = 0;
    if (max_h)
        *max_h = 0;
    if (!channel || !channel->header)
        return;
    if (max_w)
        *max_w = channel->header->max_width;
    if (max_h)
        *max_h = channel->header->max_height;
}
