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
//   - The input ring has one host writer and one game reader. A full ring
//     rejects the newest event so neither side ever writes the other's index
//     or races an in-flight record copy.
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

#if !defined(_WIN32)
/// @brief Derive a stable POSIX object name within Darwin's short shm-name limit.
/// @details The public channel name remains portable and may contain up to 96 bytes. Two
///          independent hashes keep the native key bounded without truncating that identity.
static void embed_posix_name(const char *name, char out[128]) {
    uint64_t primary = UINT64_C(14695981039346656037);
    uint32_t secondary = UINT32_C(5381);
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; cursor++) {
        primary ^= (uint64_t)*cursor;
        primary *= UINT64_C(1099511628211);
        secondary = ((secondary << 5u) + secondary) ^ (uint32_t)*cursor;
    }
    snprintf(out, 128, "/ze-%016llx%08x", (unsigned long long)primary, (unsigned int)secondary);
}

/// @brief Accept the requested extent or one native-page rounding of it.
/// @details Darwin reports shm sizes rounded up to the VM page while Linux reports the exact
///          ftruncate size. Larger or non-page-aligned extents still fail closed.
static int embed_posix_extent_matches(off_t actual, size_t expected) {
    off_t requested = (off_t)expected;
    if (requested < 0 || (size_t)requested != expected || actual < requested)
        return 0;
    if (actual == requested)
        return 1;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 0;
    off_t page = (off_t)page_size;
    return actual % page == 0 && actual - requested < page;
}
#endif

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
    _Atomic uint64_t frame_seq;  ///< Even = slot (seq/2)&1 complete; odd = write in progress.
    _Atomic int32_t frame_width; ///< Dimensions of the most recent published frame.
    _Atomic int32_t frame_height;
    _Atomic uint64_t input_head; ///< Next write index (host).
    _Atomic uint64_t input_tail; ///< Next read index (game).
    vgfx_embed_event_t input_ring[EMBED_INPUT_CAPACITY];
} vgfx_embed_header_t;

struct vgfx_embed_channel {
    vgfx_embed_header_t *header;
    uint8_t *slots[2];
    size_t slot_bytes;
    size_t map_bytes;
    int32_t max_width;
    int32_t max_height;
    int is_host;
    int producer_claimed;
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

/// @brief Compute one channel layout without allowing size_t overflow.
static int embed_layout(int32_t max_w, int32_t max_h, size_t *slot_bytes, size_t *map_bytes) {
    if (!slot_bytes || !map_bytes || max_w <= 0 || max_h <= 0 || max_w > EMBED_MAX_DIMENSION ||
        max_h > EMBED_MAX_DIMENSION)
        return 0;
    size_t width = (size_t)max_w;
    size_t height = (size_t)max_h;
    if (height > SIZE_MAX / width || width * height > SIZE_MAX / 4u)
        return 0;
    size_t frame = width * height * 4u;
    if (frame > (SIZE_MAX - sizeof(vgfx_embed_header_t)) / 2u)
        return 0;
    *slot_bytes = frame;
    *map_bytes = sizeof(vgfx_embed_header_t) + frame * 2u;
    return 1;
}

/// @brief Wire the per-handle pointers into an established mapping.
static void embed_wire(vgfx_embed_channel_t *ch, void *base) {
    ch->header = (vgfx_embed_header_t *)base;
    ch->slots[0] = (uint8_t *)base + sizeof(vgfx_embed_header_t);
    ch->slots[1] = ch->slots[0] + ch->slot_bytes;
}

/// @brief Require atomics that are safe to share between processes.
static int embed_atomics_lock_free(vgfx_embed_header_t *header) {
    return header && atomic_is_lock_free(&header->producer_attached) &&
           atomic_is_lock_free(&header->producer_exited) &&
           atomic_is_lock_free(&header->requested_width) &&
           atomic_is_lock_free(&header->requested_height) &&
           atomic_is_lock_free(&header->frame_seq) && atomic_is_lock_free(&header->frame_width) &&
           atomic_is_lock_free(&header->frame_height) && atomic_is_lock_free(&header->input_head) &&
           atomic_is_lock_free(&header->input_tail);
}

/// @brief Validate immutable protocol metadata and derive the mapped layout.
static int embed_header_layout(vgfx_embed_header_t *header, size_t *slot_bytes, size_t *map_bytes) {
    if (!header || header->magic != EMBED_MAGIC || header->version != EMBED_VERSION)
        return 0;
    return embed_layout(header->max_width, header->max_height, slot_bytes, map_bytes) &&
           embed_atomics_lock_free(header);
}

/// @brief Check that shared immutable metadata still matches this handle.
static int embed_channel_valid(const vgfx_embed_channel_t *channel) {
    return channel && channel->header && channel->header->magic == EMBED_MAGIC &&
           channel->header->version == EMBED_VERSION &&
           channel->header->max_width == channel->max_width &&
           channel->header->max_height == channel->max_height;
}

/// @brief Initialize every shared atomic before publishing the magic sentinel.
static int embed_initialize_header(vgfx_embed_header_t *header, int32_t max_w, int32_t max_h) {
    memset(header, 0, sizeof(*header));
    header->version = EMBED_VERSION;
    header->max_width = max_w;
    header->max_height = max_h;
    atomic_init(&header->producer_attached, 0u);
    atomic_init(&header->producer_exited, 0u);
    atomic_init(&header->requested_width, 0);
    atomic_init(&header->requested_height, 0);
    atomic_init(&header->frame_seq, 0u);
    atomic_init(&header->frame_width, 0);
    atomic_init(&header->frame_height, 0);
    atomic_init(&header->input_head, 0u);
    atomic_init(&header->input_tail, 0u);
    if (!embed_atomics_lock_free(header))
        return 0;
    atomic_thread_fence(memory_order_release);
    header->magic = EMBED_MAGIC;
    return 1;
}

int vgfx_embed_channel_create(const char *name,
                              int32_t max_w,
                              int32_t max_h,
                              vgfx_embed_channel_t **out) {
    if (out)
        *out = NULL;
    if (!out || !embed_name_ok(name))
        return 0;
    size_t slot_bytes = 0;
    size_t map_bytes = 0;
    if (!embed_layout(max_w, max_h, &slot_bytes, &map_bytes))
        return 0;
    vgfx_embed_channel_t *ch = (vgfx_embed_channel_t *)calloc(1, sizeof(*ch));
    if (!ch)
        return 0;
    ch->is_host = 1;
    ch->max_width = max_w;
    ch->max_height = max_h;
    ch->slot_bytes = slot_bytes;
    ch->map_bytes = map_bytes;
#if defined(_WIN32)
    char object_name[160];
    snprintf(object_name, sizeof(object_name), "Local\\zanna-embed-%s", name);
    ch->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                     NULL,
                                     PAGE_READWRITE,
                                     (DWORD)((uint64_t)ch->map_bytes >> 32),
                                     (DWORD)(ch->map_bytes & 0xFFFFFFFFu),
                                     object_name);
    DWORD mapping_error = GetLastError();
    if (!ch->mapping) {
        free(ch);
        return 0;
    }
    if (mapping_error == ERROR_ALREADY_EXISTS) {
        CloseHandle(ch->mapping);
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
    embed_posix_name(name, ch->shm_name);
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
    embed_wire(ch, base);
    if (!embed_initialize_header(ch->header, max_w, max_h)) {
        vgfx_embed_channel_close(ch);
        return 0;
    }
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
    void *probe =
        MapViewOfFile(ch->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vgfx_embed_header_t));
    if (!probe) {
        CloseHandle(ch->mapping);
        free(ch);
        return 0;
    }
#else
    embed_posix_name(name, ch->shm_name);
    int fd = shm_open(ch->shm_name, O_RDWR, 0600);
    if (fd < 0) {
        free(ch);
        return 0;
    }
    struct stat mapping_stat;
    if (fstat(fd, &mapping_stat) != 0 ||
        mapping_stat.st_size < (off_t)sizeof(vgfx_embed_header_t)) {
        close(fd);
        free(ch);
        return 0;
    }
    void *probe =
        mmap(NULL, sizeof(vgfx_embed_header_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (probe == MAP_FAILED) {
        close(fd);
        free(ch);
        return 0;
    }
#endif
    vgfx_embed_header_t *header = (vgfx_embed_header_t *)probe;
    size_t slot_bytes = 0;
    size_t map_bytes = 0;
    if (!embed_header_layout(header, &slot_bytes, &map_bytes)) {
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
    ch->max_width = max_w;
    ch->max_height = max_h;
    ch->slot_bytes = slot_bytes;
    ch->map_bytes = map_bytes;
#if defined(_WIN32)
    if (!UnmapViewOfFile(probe)) {
        CloseHandle(ch->mapping);
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
    if (munmap(probe, sizeof(vgfx_embed_header_t)) != 0 || fstat(fd, &mapping_stat) != 0 ||
        !embed_posix_extent_matches(mapping_stat.st_size, ch->map_bytes)) {
        close(fd);
        free(ch);
        return 0;
    }
    void *base = mmap(NULL, ch->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        free(ch);
        return 0;
    }
#endif
    embed_wire(ch, base);
    size_t verified_slot_bytes = 0;
    size_t verified_map_bytes = 0;
    if (!embed_header_layout(ch->header, &verified_slot_bytes, &verified_map_bytes) ||
        ch->header->max_width != max_w || ch->header->max_height != max_h ||
        verified_slot_bytes != ch->slot_bytes || verified_map_bytes != ch->map_bytes) {
#if defined(_WIN32)
        UnmapViewOfFile(ch->header);
        CloseHandle(ch->mapping);
#else
        munmap(ch->header, ch->map_bytes);
#endif
        free(ch);
        return 0;
    }
    uint32_t expected = 0u;
    if (!atomic_compare_exchange_strong_explicit(&ch->header->producer_attached,
                                                 &expected,
                                                 2u,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
#if defined(_WIN32)
        UnmapViewOfFile(ch->header);
        CloseHandle(ch->mapping);
#else
        munmap(ch->header, ch->map_bytes);
#endif
        free(ch);
        return 0;
    }
    ch->producer_claimed = 1;
    atomic_store_explicit(&ch->header->producer_exited, 0u, memory_order_release);
    atomic_store_explicit(&ch->header->producer_attached, 1u, memory_order_release);
    *out = ch;
    return 1;
}

void vgfx_embed_channel_close(vgfx_embed_channel_t *channel) {
    if (!channel)
        return;
    if (channel->header) {
        if (channel->producer_claimed) {
            atomic_store_explicit(&channel->header->producer_exited, 1u, memory_order_release);
            atomic_store_explicit(&channel->header->producer_attached, 0u, memory_order_release);
            channel->producer_claimed = 0;
        }
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

int vgfx_embed_channel_publish_frame(vgfx_embed_channel_t *channel,
                                     const uint8_t *rgba,
                                     int32_t width,
                                     int32_t height) {
    if (!embed_channel_valid(channel) || channel->is_host || !channel->producer_claimed || !rgba ||
        width <= 0 || height <= 0)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    if (width > channel->max_width || height > channel->max_height ||
        atomic_load_explicit(&h->producer_attached, memory_order_acquire) != 1u)
        return 0;
    uint64_t seq = atomic_load(&h->frame_seq);
    uint64_t writing = seq >= UINT64_MAX - 2u ? 1u : seq + ((seq & 1u) ? 2u : 1u);
    atomic_store_explicit(&h->frame_seq, writing, memory_order_release);
    /* Index by the sequence the reader will observe (writing + 1). */
    uint8_t *slot = channel->slots[((writing + 1u) / 2u) & 1u];
    memcpy(slot, rgba, (size_t)width * (size_t)height * 4u);
    atomic_store_explicit(&h->frame_width, width, memory_order_release);
    atomic_store_explicit(&h->frame_height, height, memory_order_release);
    atomic_store_explicit(&h->frame_seq, writing + 1u, memory_order_release);
    return 1;
}

int vgfx_embed_channel_acquire_frame(vgfx_embed_channel_t *channel,
                                     uint8_t *dst,
                                     int32_t *width,
                                     int32_t *height) {
    if (width)
        *width = 0;
    if (height)
        *height = 0;
    if (!embed_channel_valid(channel) || !channel->is_host || !dst || !width || !height)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t seq = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
        if (seq == 0 || (seq & 1u) || seq == channel->last_seen_seq)
            return 0;
        int32_t w = atomic_load_explicit(&h->frame_width, memory_order_acquire);
        int32_t hgt = atomic_load_explicit(&h->frame_height, memory_order_acquire);
        if (w <= 0 || hgt <= 0 || w > channel->max_width || hgt > channel->max_height)
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
    if (!embed_channel_valid(channel) || !channel->is_host || !event ||
        event->kind <= VGFX_EMBED_EVENT_NONE || event->kind > VGFX_EMBED_EVENT_CLOSE)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    uint64_t head = atomic_load_explicit(&h->input_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&h->input_tail, memory_order_acquire);
    if (head - tail >= EMBED_INPUT_CAPACITY)
        return 0;
    h->input_ring[head & (EMBED_INPUT_CAPACITY - 1u)] = *event;
    atomic_store_explicit(&h->input_head, head + 1u, memory_order_release);
    return 1;
}

int vgfx_embed_channel_poll_event(vgfx_embed_channel_t *channel, vgfx_embed_event_t *event) {
    if (event)
        memset(event, 0, sizeof(*event));
    if (!embed_channel_valid(channel) || channel->is_host || !channel->producer_claimed || !event)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    uint64_t tail = atomic_load_explicit(&h->input_tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&h->input_head, memory_order_acquire);
    if (tail == head || head - tail > EMBED_INPUT_CAPACITY)
        return 0;
    *event = h->input_ring[tail & (EMBED_INPUT_CAPACITY - 1u)];
    atomic_store_explicit(&h->input_tail, tail + 1u, memory_order_release);
    return 1;
}

int vgfx_embed_channel_set_size(vgfx_embed_channel_t *channel, int32_t width, int32_t height) {
    if (!embed_channel_valid(channel) || !channel->is_host || width <= 0 || height <= 0)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    if (width > channel->max_width)
        width = channel->max_width;
    if (height > channel->max_height)
        height = channel->max_height;
    vgfx_embed_event_t resize = {VGFX_EMBED_EVENT_RESIZE, width, height, 0};
    if (!vgfx_embed_channel_push_event(channel, &resize))
        return 0;
    atomic_store_explicit(&h->requested_width, width, memory_order_release);
    atomic_store_explicit(&h->requested_height, height, memory_order_release);
    return 1;
}

void vgfx_embed_channel_mark_exited(vgfx_embed_channel_t *channel) {
    if (!embed_channel_valid(channel) || channel->is_host || !channel->producer_claimed)
        return;
    atomic_store_explicit(&channel->header->producer_exited, 1u, memory_order_release);
    atomic_store_explicit(&channel->header->producer_attached, 0u, memory_order_release);
    channel->producer_claimed = 0;
}

int vgfx_embed_channel_producer_exited(const vgfx_embed_channel_t *channel) {
    if (!embed_channel_valid(channel))
        return 0;
    return atomic_load_explicit(&channel->header->producer_exited, memory_order_acquire) != 0u;
}

int vgfx_embed_channel_producer_attached(const vgfx_embed_channel_t *channel) {
    if (!embed_channel_valid(channel))
        return 0;
    return atomic_load_explicit(&channel->header->producer_attached, memory_order_acquire) == 1u;
}

int vgfx_embed_channel_frame_size(const vgfx_embed_channel_t *channel,
                                  int32_t *width,
                                  int32_t *height) {
    if (width)
        *width = 0;
    if (height)
        *height = 0;
    if (!embed_channel_valid(channel) || !channel->is_host)
        return 0;
    vgfx_embed_header_t *h = channel->header;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t seq = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
        if (seq == 0)
            return 0;
        if (seq & 1u)
            continue;
        int32_t frame_w = atomic_load_explicit(&h->frame_width, memory_order_acquire);
        int32_t frame_h = atomic_load_explicit(&h->frame_height, memory_order_acquire);
        uint64_t reread = atomic_load_explicit(&h->frame_seq, memory_order_acquire);
        if (reread != seq)
            continue;
        if (frame_w <= 0 || frame_h <= 0 || frame_w > channel->max_width ||
            frame_h > channel->max_height)
            return 0;
        if (width)
            *width = frame_w;
        if (height)
            *height = frame_h;
        return 1;
    }
    return 0;
}

void vgfx_embed_channel_capacity(const vgfx_embed_channel_t *channel,
                                 int32_t *max_w,
                                 int32_t *max_h) {
    if (max_w)
        *max_w = 0;
    if (max_h)
        *max_h = 0;
    if (!embed_channel_valid(channel))
        return;
    if (max_w)
        *max_w = channel->max_width;
    if (max_h)
        *max_h = channel->max_height;
}
