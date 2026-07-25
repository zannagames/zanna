//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_tempmgr.c
// Purpose: Canvas3D per-frame transient-resource tracking — temp buffers,
//   final-overlay temp buffers, and the GC-managed transient-object hash set.
//   Split out of rt_canvas3d.c; the bookkeeping arrays live on rt_canvas3d
//   (see rt_canvas3d_internal.h).
// Key invariants:
//   - Tracked temp buffers/objects are released at end-of-frame, or on a failed
//     allocation path via the release_* helpers.
//   - The transient-buffer/object hash sets mirror their tracking lists exactly.
// Ownership/Lifetime:
//   - Temp buffers are malloc'd elsewhere; tracking takes ownership for the frame.
//   - Tracked objects are retained on insert and released on removal/frame end.
//   - Mesh snapshot entries may instead own native retained-geometry references;
//     those references are released before the snapshot table is reset.
// Links: rt_canvas3d_internal.h, rt_canvas3d_snapshot.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D frame arenas and transient-resource ownership.
/// @details The module supplies stable bump allocations, a retained late-overlay
///   arena, duplicate-filtered temporary buffer/object lists, rollback helpers,
///   and deterministic end-of-frame cleanup.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "rt_object.h"

#include <stdlib.h>
#include <string.h>

#define CANVAS3D_FINAL_OVERLAY_ARENA_DEFAULT_BYTES (256u * 1024u)
#define CANVAS3D_FINAL_OVERLAY_ARENA_RETAIN_BYTES (4u * 1024u * 1024u)

#define CANVAS3D_FRAME_ARENA_CHUNK_BYTES (1u * 1024u * 1024u)
#define CANVAS3D_FRAME_ARENA_RETAIN_CHUNKS 8
#define CANVAS3D_FRAME_ARENA_ALIGN 16u

/// @brief One frame-arena chunk: bump storage with stable addresses.
/// @details Chunks form a singly linked list; the arena grows by APPENDING
///   chunks (never realloc), so pointers handed to recorded draw commands stay
///   valid until the end-of-frame reset. Oversized single requests get their
///   own exact-size chunk.
typedef struct canvas3d_frame_arena_chunk {
    struct canvas3d_frame_arena_chunk *next;
    size_t used;
    size_t capacity;
    /* payload follows the header, CANVAS3D_FRAME_ARENA_ALIGN-aligned */
} canvas3d_frame_arena_chunk;

/// @brief Payload base address of a chunk (header rounded up to the alignment).
/// @param chunk Non-null arena chunk allocated with its payload trailing the header.
/// @return Aligned first payload byte within @p chunk.
static uint8_t *canvas3d_frame_arena_chunk_payload(canvas3d_frame_arena_chunk *chunk) {
    size_t header = (sizeof(canvas3d_frame_arena_chunk) + (CANVAS3D_FRAME_ARENA_ALIGN - 1u)) &
                    ~(size_t)(CANVAS3D_FRAME_ARENA_ALIGN - 1u);
    return (uint8_t *)chunk + header;
}

/// @brief Allocate a chunk able to serve at least @p payload_bytes.
/// @details Ordinary requests receive the default chunk capacity; oversized
///   requests receive a dedicated capacity equal to their requested size.
/// @param payload_bytes Minimum payload capacity in bytes.
/// @return Newly allocated empty chunk, or `NULL` on size overflow or allocation failure.
static canvas3d_frame_arena_chunk *canvas3d_frame_arena_new_chunk(size_t payload_bytes) {
    size_t header = (sizeof(canvas3d_frame_arena_chunk) + (CANVAS3D_FRAME_ARENA_ALIGN - 1u)) &
                    ~(size_t)(CANVAS3D_FRAME_ARENA_ALIGN - 1u);
    canvas3d_frame_arena_chunk *chunk;
    if (payload_bytes < CANVAS3D_FRAME_ARENA_CHUNK_BYTES)
        payload_bytes = CANVAS3D_FRAME_ARENA_CHUNK_BYTES;
    if (payload_bytes > SIZE_MAX - header)
        return NULL;
    chunk = (canvas3d_frame_arena_chunk *)malloc(header + payload_bytes);
    if (!chunk)
        return NULL;
    chunk->next = NULL;
    chunk->used = 0u;
    chunk->capacity = payload_bytes;
    return chunk;
}

/// @brief Allocate aligned, stable scratch storage from the current frame arena.
/// @details Rounds requests to the arena alignment, advances through retained
///   chunks, and appends a new chunk when necessary. Returned bytes are uninitialized
///   and their addresses remain stable until arena reset or destruction.
/// @param c Canvas owning the frame arena and byte counter.
/// @param bytes Positive payload size requested by the caller.
/// @return Sixteen-byte-aligned borrowed storage, or `NULL` for invalid input,
///   alignment overflow, size overflow, or allocation failure.
void *canvas3d_frame_arena_alloc(rt_canvas3d *c, size_t bytes) {
    canvas3d_frame_arena_chunk *chunk;
    if (!c || bytes == 0u)
        return NULL;
    bytes =
        (bytes + (CANVAS3D_FRAME_ARENA_ALIGN - 1u)) & ~(size_t)(CANVAS3D_FRAME_ARENA_ALIGN - 1u);
    if (bytes == 0u)
        return NULL; /* overflow in round-up */
    chunk = c->frame_arena_current;
    /* Walk forward through retained chunks until one fits. */
    while (chunk && chunk->capacity - chunk->used < bytes) {
        if (!chunk->next)
            break;
        chunk = chunk->next;
        c->frame_arena_current = chunk;
    }
    if (!chunk || chunk->capacity - chunk->used < bytes) {
        canvas3d_frame_arena_chunk *grown = canvas3d_frame_arena_new_chunk(bytes);
        if (!grown)
            return NULL;
        if (chunk)
            chunk->next = grown;
        else
            c->frame_arena_head = grown;
        c->frame_arena_current = grown;
        chunk = grown;
    }
    {
        uint8_t *out = canvas3d_frame_arena_chunk_payload(chunk) + chunk->used;
        chunk->used += bytes;
        c->frame_arena_frame_bytes += bytes;
        return out;
    }
}

/// @brief Reset the frame arena while retaining a bounded working set of chunks.
/// @details Invalidates all outstanding arena allocations, zeroes bump offsets and
///   frame byte accounting, preserves at most the first eight chunks, and frees an
///   unusually large tail so transient spikes do not become permanent retention.
/// @param c Canvas whose arena is reset; `NULL` is ignored.
void canvas3d_frame_arena_reset(rt_canvas3d *c) {
    canvas3d_frame_arena_chunk *chunk;
    int32_t kept = 0;
    if (!c)
        return;
    chunk = c->frame_arena_head;
    while (chunk) {
        chunk->used = 0u;
        kept++;
        if (kept == CANVAS3D_FRAME_ARENA_RETAIN_CHUNKS && chunk->next) {
            /* Unusual frames can chain many chunks; keep a bounded working
             * set and release the tail. */
            canvas3d_frame_arena_chunk *tail = chunk->next;
            chunk->next = NULL;
            while (tail) {
                canvas3d_frame_arena_chunk *next = tail->next;
                free(tail);
                tail = next;
            }
            break;
        }
        chunk = chunk->next;
    }
    c->frame_arena_current = c->frame_arena_head;
    c->frame_arena_frame_bytes = 0u;
}

/// @brief Destroy every frame-arena chunk and clear its canvas state.
/// @param c Canvas whose arena allocations are invalidated and freed; `NULL` is ignored.
void canvas3d_frame_arena_free(rt_canvas3d *c) {
    canvas3d_frame_arena_chunk *chunk;
    if (!c)
        return;
    chunk = c->frame_arena_head;
    while (chunk) {
        canvas3d_frame_arena_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    c->frame_arena_head = NULL;
    c->frame_arena_current = NULL;
    c->frame_arena_frame_bytes = 0u;
}

/// @brief Round @p value up to the next power-of-two size, saturating on overflow.
/// @details Final-overlay arena growth only happens when no recorded command points into the
/// arena, so rounding up amortizes future HUD allocations without risking pointer invalidation.
/// @param value Minimum capacity requested by the caller.
/// @return A power-of-two capacity at least @p value, or @p value when rounding would overflow.
static size_t canvas3d_next_power_of_two_size(size_t value) {
    size_t result = 1u;
    if (value <= 1u)
        return 1u;
    while (result < value) {
        if (result > SIZE_MAX / 2u)
            return value;
        result *= 2u;
    }
    return result;
}

/// @brief Return whether @p alignment is a valid non-zero power-of-two.
/// @param alignment Alignment value to validate.
/// @return Non-zero when @p alignment can be used for arena pointer rounding.
static int canvas3d_valid_power_of_two_alignment(size_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

/// @brief Allocate stable storage from the retained final-overlay vertex/index arena.
/// @details Invalid alignment falls back to pointer alignment. The arena may grow
///   only while empty because moving it after recording a command would invalidate
///   pointers. A cumulative overflow records its desired high-water mark for the
///   subsequent reset to grow the next frame's capacity.
/// @param c Canvas owning the retained overlay arena.
/// @param bytes Positive number of uninitialized bytes requested.
/// @param alignment Requested power-of-two byte alignment; invalid or sub-pointer
///   values are promoted to pointer alignment.
/// @return Aligned storage valid until overlay-arena reset or canvas destruction,
///   or `NULL` for invalid input, overflow, allocation failure, or required growth
///   while existing allocations are live.
void *canvas3d_alloc_final_overlay_arena(rt_canvas3d *c, size_t bytes, size_t alignment) {
    size_t mask;
    size_t aligned_offset;
    size_t end_offset;
    size_t requested_capacity;
    uint8_t *grown;

    if (!c || bytes == 0u)
        return NULL;
    if (!canvas3d_valid_power_of_two_alignment(alignment))
        alignment = sizeof(void *);
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    mask = alignment - 1u;
    if (c->final_overlay_arena_used > SIZE_MAX - mask)
        return NULL;
    aligned_offset = (c->final_overlay_arena_used + mask) & ~mask;
    if (aligned_offset > SIZE_MAX - bytes)
        return NULL;
    end_offset = aligned_offset + bytes;
    if (end_offset > c->final_overlay_arena_peak)
        c->final_overlay_arena_peak = end_offset;
    if (end_offset > c->final_overlay_arena_capacity) {
        if (c->final_overlay_arena_used != 0u)
            return NULL;
        requested_capacity = end_offset;
        if (requested_capacity < CANVAS3D_FINAL_OVERLAY_ARENA_DEFAULT_BYTES)
            requested_capacity = CANVAS3D_FINAL_OVERLAY_ARENA_DEFAULT_BYTES;
        requested_capacity = canvas3d_next_power_of_two_size(requested_capacity);
        grown = (uint8_t *)malloc(requested_capacity);
        if (!grown)
            return NULL;
        free(c->final_overlay_arena);
        c->final_overlay_arena = grown;
        c->final_overlay_arena_capacity = requested_capacity;
    }
    c->final_overlay_arena_used = end_offset;
    return c->final_overlay_arena + aligned_offset;
}

/// @brief Reset retained final-overlay arena state after overlay replay.
/// @details Growth happens HERE, from the recorded high-water mark: the
///   allocator itself can only grow while the arena is empty, so a frame whose
///   CUMULATIVE overlay demand exceeded capacity dropped the overflow. Without
///   this regrow, a HUD needing more than the current arena (many small
///   allocations, none individually over capacity) would drop geometry every
///   frame forever. Shrinking back below the retain cap only happens when the
///   last frame's peak no longer justifies the oversized arena.
/// @param c Canvas whose outstanding overlay allocations have already been replayed.
void canvas3d_reset_final_overlay_arena(rt_canvas3d *c) {
    size_t peak;
    if (!c)
        return;
    peak = c->final_overlay_arena_peak;
    c->final_overlay_arena_used = 0u;
    c->final_overlay_arena_peak = 0u;
    if (peak > c->final_overlay_arena_capacity) {
        size_t requested = canvas3d_next_power_of_two_size(peak);
        uint8_t *grown = (uint8_t *)malloc(requested);
        if (grown) {
            free(c->final_overlay_arena);
            c->final_overlay_arena = grown;
            c->final_overlay_arena_capacity = requested;
        }
    } else if (c->final_overlay_arena_capacity > CANVAS3D_FINAL_OVERLAY_ARENA_RETAIN_BYTES &&
               peak <= CANVAS3D_FINAL_OVERLAY_ARENA_RETAIN_BYTES) {
        free(c->final_overlay_arena);
        c->final_overlay_arena = NULL;
        c->final_overlay_arena_capacity = 0u;
    }
}

/// @brief Clear the per-frame transient-buffer tracking set (all slots empty).
/// @param c Canvas whose allocated duplicate-filter table is cleared; missing
///   tables and `NULL` canvases are ignored.
static void canvas3d_temp_buffer_set_clear(rt_canvas3d *c) {
    if (!c || !c->temp_buffer_set || c->temp_buffer_set_capacity <= 0)
        return;
    memset(c->temp_buffer_set, 0, (size_t)c->temp_buffer_set_capacity * sizeof(void *));
}

/// @brief Ensure the transient-buffer set is sized for @p count_hint tracked buffers.
/// @details The set is an open-addressed duplicate filter for `temp_buffers`. It is rebuilt from
///          the list after growth so callers can keep using swap-remove on the list.
/// @param c Canvas that owns the per-frame temp buffers.
/// @param count_hint Expected tracked buffer count.
/// @return Non-zero when the set exists and contains the current list contents.
static int canvas3d_ensure_temp_buffer_set(rt_canvas3d *c, int32_t count_hint) {
    int32_t needed;
    void **grown;
    if (!c)
        return 0;
    if (count_hint > INT32_MAX / 2)
        return 0;
    needed = canvas3d_next_power_of_two_i32(count_hint > 0 ? count_hint * 2 : 32);
    if (needed < 32)
        needed = 32;
    if (c->temp_buffer_set_capacity >= needed)
        return 1;
    if ((size_t)needed > SIZE_MAX / sizeof(*c->temp_buffer_set))
        return 0;
    grown = (void **)realloc(c->temp_buffer_set, (size_t)needed * sizeof(*grown));
    if (!grown)
        return 0;
    c->temp_buffer_set = grown;
    c->temp_buffer_set_capacity = needed;
    canvas3d_temp_buffer_set_clear(c);
    for (int32_t i = 0; i < c->temp_buf_count; ++i) {
        void *existing = c->temp_buffers[i];
        int32_t mask;
        int32_t slot;
        if (!existing)
            continue;
        mask = c->temp_buffer_set_capacity - 1;
        slot = (int32_t)(canvas3d_hash_u64((uintptr_t)existing) & (uint32_t)mask);
        for (int32_t probe = 0; probe < c->temp_buffer_set_capacity; ++probe) {
            if (!c->temp_buffer_set[slot]) {
                c->temp_buffer_set[slot] = existing;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    return 1;
}

/// @brief Return whether @p buffer is currently tracked as a per-frame transient buffer.
/// @details Uses the hash set when available and falls back to a linear scan if scratch allocation
///          fails, preserving the old no-duplicate behavior under memory pressure.
/// @param c Canvas whose frame-owned buffer list is searched.
/// @param buffer Non-null allocation identity to locate.
/// @return Nonzero when the exact pointer is tracked; zero when absent or input is invalid.
static int canvas3d_temp_buffer_set_contains(rt_canvas3d *c, void *buffer) {
    int32_t mask;
    int32_t slot;
    if (!c || !buffer || c->temp_buf_count <= 0)
        return 0;
    if (!c->temp_buffer_set || c->temp_buffer_set_capacity < c->temp_buf_count * 2) {
        if (!canvas3d_ensure_temp_buffer_set(c, c->temp_buf_count + 1)) {
            for (int32_t i = 0; i < c->temp_buf_count; ++i) {
                if (c->temp_buffers[i] == buffer)
                    return 1;
            }
            return 0;
        }
    }
    mask = c->temp_buffer_set_capacity - 1;
    slot = (int32_t)(canvas3d_hash_u64((uintptr_t)buffer) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->temp_buffer_set_capacity; ++probe) {
        void *entry = c->temp_buffer_set[slot];
        if (!entry)
            return 0;
        if (entry == buffer)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Insert @p buffer into the transient-buffer duplicate set.
/// @param c Canvas owning the duplicate-filter set.
/// @param buffer Non-null allocation identity to insert without taking additional ownership.
/// @return Non-zero when the buffer is present in the set after the call.
static int canvas3d_temp_buffer_set_insert(rt_canvas3d *c, void *buffer) {
    int32_t mask;
    int32_t slot;
    if (!c || !buffer)
        return 0;
    if (!canvas3d_ensure_temp_buffer_set(c, c->temp_buf_count + 1))
        return 0;
    mask = c->temp_buffer_set_capacity - 1;
    slot = (int32_t)(canvas3d_hash_u64((uintptr_t)buffer) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->temp_buffer_set_capacity; ++probe) {
        if (!c->temp_buffer_set[slot]) {
            c->temp_buffer_set[slot] = buffer;
            return 1;
        }
        if (c->temp_buffer_set[slot] == buffer)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Rebuild the transient-buffer hash set from the tracked-buffer list.
/// @param c Canvas whose existing set is cleared and repopulated; a missing set is ignored.
static void canvas3d_rebuild_temp_buffer_set(rt_canvas3d *c) {
    if (!c || !c->temp_buffer_set)
        return;
    canvas3d_temp_buffer_set_clear(c);
    for (int32_t i = 0; i < c->temp_buf_count; ++i)
        canvas3d_temp_buffer_set_insert(c, c->temp_buffers[i]);
}

/// @brief Track a malloc'd temp buffer so it is freed at end-of-frame.
/// @details A pointer already tracked is accepted without adding duplicate ownership.
///   On success the caller transfers responsibility for freeing @p buffer to @p c.
/// @param c Canvas that will own the allocation through frame cleanup.
/// @param buffer Non-null allocation compatible with `free`.
/// @return Nonzero when the buffer is tracked; zero for invalid input, capacity
///   overflow, or tracking-array allocation failure. Ownership remains with the
///   caller on failure.
int canvas3d_track_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!c || !buffer)
        return 0;
    if (canvas3d_temp_buffer_set_contains(c, buffer))
        return 1;
    if (c->temp_buf_count >= c->temp_buf_capacity) {
        if (c->temp_buf_capacity < 0 || c->temp_buf_capacity > INT32_MAX / 2)
            return 0;
        int32_t new_cap = c->temp_buf_capacity == 0 ? 8 : c->temp_buf_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
            return 0;
        void **nb = (void **)realloc(c->temp_buffers, (size_t)new_cap * sizeof(void *));
        if (!nb)
            return 0;
        c->temp_buffers = nb;
        c->temp_buf_capacity = new_cap;
    }
    if (!canvas3d_temp_buffer_set_insert(c, buffer)) {
        for (int32_t i = 0; i < c->temp_buf_count; ++i) {
            if (c->temp_buffers[i] == buffer)
                return 1;
        }
    }
    c->temp_buffers[c->temp_buf_count++] = buffer;
    return 1;
}

/// @brief Remove a tracked temp buffer without freeing it.
/// @param c Canvas whose tracking list and duplicate set are updated.
/// @param buffer Exact allocation pointer to remove.
/// @return Nonzero when found and removed, transferring ownership back to the
///   caller; zero when absent or input is invalid.
int canvas3d_untrack_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!c || !buffer)
        return 0;
    for (int32_t i = 0; i < c->temp_buf_count; ++i) {
        if (c->temp_buffers[i] == buffer) {
            int32_t last = c->temp_buf_count - 1;
            c->temp_buffers[i] = c->temp_buffers[last];
            c->temp_buffers[last] = NULL;
            c->temp_buf_count = last;
            canvas3d_rebuild_temp_buffer_set(c);
            return 1;
        }
    }
    return 0;
}

/// @brief Untrack and free a temp buffer when a later allocation path fails.
/// @param c Canvas expected to own @p buffer.
/// @param buffer Allocation to free only if it is currently tracked; `NULL` is ignored.
void canvas3d_release_tracked_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!buffer)
        return;
    if (canvas3d_untrack_temp_buffer(c, buffer))
        free(buffer);
}

/// @brief Release a tracked mesh-geometry snapshot and refund its frame byte budget.
/// @details Mesh snapshots are ordinary frame-temp buffers, but snapshot byte accounting is
///   maintained separately so large dynamic meshes cannot consume unbounded memory. Callers use
///   this helper when a later rebase, tangent-generation, or validation step fails after the
///   buffers were successfully tracked. The byte refund is saturating: stale or duplicate rollback
///   calls cannot underflow the frame counter.
/// @param c Canvas that owns the per-frame snapshot budget.
/// @param vertices Tracked vertex snapshot buffer, or NULL.
/// @param vertex_bytes Number of bytes charged for @p vertices.
/// @param indices Tracked index snapshot buffer, or NULL.
/// @param index_bytes Number of bytes charged for @p indices.
void canvas3d_release_tracked_mesh_snapshot(
    rt_canvas3d *c, void *vertices, size_t vertex_bytes, void *indices, size_t index_bytes) {
    size_t total_bytes;
    if (!c)
        return;
    if (vertex_bytes > SIZE_MAX - index_bytes)
        total_bytes = SIZE_MAX;
    else
        total_bytes = vertex_bytes + index_bytes;
    if (total_bytes >= c->mesh_snapshot_bytes)
        c->mesh_snapshot_bytes = 0u;
    else
        c->mesh_snapshot_bytes -= total_bytes;
    canvas3d_release_tracked_temp_buffer(c, vertices);
    canvas3d_release_tracked_temp_buffer(c, indices);
}

/// @brief Track a malloc'd buffer used by deferred final-overlay commands.
///
/// Final overlays are recorded before frame finalization and replayed after
/// post-FX. Their geometry must survive normal End() cleanup, so they use a
/// separate temp-buffer list cleared after Flip() or ClearOverlay().
/// @param c Canvas that assumes frame-delayed ownership on success.
/// @param buffer Non-null allocation compatible with `free`.
/// @return Nonzero when appended to the final-overlay list; zero for invalid
///   input, capacity overflow, or allocation failure. This list does not filter
///   duplicate pointers, so callers must transfer each allocation only once.
int canvas3d_track_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!c || !buffer)
        return 0;
    if (c->final_overlay_temp_buf_count >= c->final_overlay_temp_buf_capacity) {
        if (c->final_overlay_temp_buf_capacity < 0 ||
            c->final_overlay_temp_buf_capacity > INT32_MAX / 2)
            return 0;
        int32_t new_cap =
            c->final_overlay_temp_buf_capacity == 0 ? 8 : c->final_overlay_temp_buf_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
            return 0;
        void **nb =
            (void **)realloc(c->final_overlay_temp_buffers, (size_t)new_cap * sizeof(void *));
        if (!nb)
            return 0;
        c->final_overlay_temp_buffers = nb;
        c->final_overlay_temp_buf_capacity = new_cap;
    }
    c->final_overlay_temp_buffers[c->final_overlay_temp_buf_count++] = buffer;
    return 1;
}

/// @brief Remove a buffer from the final-overlay temp-buffer tracking list (does not free it).
/// @param c Canvas whose delayed-cleanup list is updated.
/// @param buffer Exact allocation pointer whose first occurrence is removed.
/// @return 1 if it was found and removed, 0 otherwise.
int canvas3d_untrack_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!c || !buffer)
        return 0;
    for (int32_t i = 0; i < c->final_overlay_temp_buf_count; ++i) {
        if (c->final_overlay_temp_buffers[i] == buffer) {
            int32_t last = c->final_overlay_temp_buf_count - 1;
            c->final_overlay_temp_buffers[i] = c->final_overlay_temp_buffers[last];
            c->final_overlay_temp_buffers[last] = NULL;
            c->final_overlay_temp_buf_count = last;
            return 1;
        }
    }
    return 0;
}

/// @brief Untrack and free a final-overlay temp buffer in one step.
/// @param c Canvas expected to own @p buffer in its final-overlay list.
/// @param buffer Allocation to free only when successfully untracked; `NULL` is ignored.
void canvas3d_release_tracked_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer) {
    if (!buffer)
        return;
    if (canvas3d_untrack_final_overlay_temp_buffer(c, buffer))
        free(buffer);
}

/// @brief Clear the per-frame transient-object tracking set (all slots empty).
/// @param c Canvas whose allocated object duplicate-filter table is cleared;
///   unavailable tables and `NULL` canvases are ignored.
void canvas3d_temp_object_set_clear(rt_canvas3d *c) {
    if (!c || !c->temp_object_set || c->temp_object_set_capacity <= 0)
        return;
    memset(c->temp_object_set, 0, (size_t)c->temp_object_set_capacity * sizeof(void *));
}

/// @brief Ensure the transient-object set has a power-of-two capacity sized for @p count_hint
/// entries.
/// @details Growth rebuilds every current list entry into the new open-addressed table.
/// @param c Canvas owning the retained-object list and duplicate set.
/// @param count_hint Expected number of tracked objects.
/// @return Nonzero when the set can represent the hinted count; zero for invalid
///   input, arithmetic overflow, or allocation failure.
int canvas3d_ensure_temp_object_set(rt_canvas3d *c, int32_t count_hint) {
    if (!c)
        return 0;
    if (count_hint > INT32_MAX / 2)
        return 0;
    int32_t needed = canvas3d_next_power_of_two_i32(count_hint > 0 ? count_hint * 2 : 32);
    if (needed < 32)
        needed = 32;
    if (c->temp_object_set_capacity >= needed)
        return 1;
    if ((size_t)needed > SIZE_MAX / sizeof(*c->temp_object_set))
        return 0;
    void **grown = (void **)realloc(c->temp_object_set, (size_t)needed * sizeof(*grown));
    if (!grown)
        return 0;
    c->temp_object_set = grown;
    c->temp_object_set_capacity = needed;
    canvas3d_temp_object_set_clear(c);
    for (int32_t i = 0; i < c->temp_obj_count; ++i) {
        void *existing = c->temp_objects[i];
        if (!existing)
            continue;
        int32_t mask = c->temp_object_set_capacity - 1;
        int32_t slot = (int32_t)(canvas3d_hash_u64((uintptr_t)existing) & (uint32_t)mask);
        for (int32_t probe = 0; probe < c->temp_object_set_capacity; ++probe) {
            if (!c->temp_object_set[slot]) {
                c->temp_object_set[slot] = existing;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    return 1;
}

/// @brief Whether @p obj is currently tracked as a per-frame transient object (linear-probe
/// lookup).
/// @details Rebuilds a missing or undersized hash set when possible and falls
///   back to a linear scan when growth fails or the count cannot be doubled safely.
/// @param c Canvas whose retained-object collection is searched.
/// @param obj Non-null object identity to locate.
/// @return Nonzero when the exact pointer is tracked; zero when absent or invalid.
int canvas3d_temp_object_set_contains(rt_canvas3d *c, void *obj) {
    if (!c || !obj || c->temp_obj_count <= 0)
        return 0;
    if (c->temp_obj_count > INT32_MAX / 2) {
        for (int32_t i = 0; i < c->temp_obj_count; ++i) {
            if (c->temp_objects[i] == obj)
                return 1;
        }
        return 0;
    }
    if (!c->temp_object_set || c->temp_object_set_capacity < c->temp_obj_count * 2) {
        if (!canvas3d_ensure_temp_object_set(c, c->temp_obj_count + 1)) {
            for (int32_t i = 0; i < c->temp_obj_count; ++i) {
                if (c->temp_objects[i] == obj)
                    return 1;
            }
            return 0;
        }
    }
    int32_t mask = c->temp_object_set_capacity - 1;
    int32_t slot = (int32_t)(canvas3d_hash_u64((uintptr_t)obj) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->temp_object_set_capacity; ++probe) {
        void *entry = c->temp_object_set[slot];
        if (!entry)
            return 0;
        if (entry == obj)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Track @p obj as a per-frame transient object (linear-probe insert; grows as needed).
/// @param c Canvas owning the duplicate set.
/// @param obj Non-null object identity to insert; no retain is performed here.
/// @return Nonzero when the pointer is present after insertion; zero for invalid
///   input, count overflow, allocation failure, or a saturated table.
int canvas3d_temp_object_set_insert(rt_canvas3d *c, void *obj) {
    if (!c || !obj)
        return 0;
    if (c->temp_obj_count >= INT32_MAX)
        return 0;
    if (!canvas3d_ensure_temp_object_set(c, c->temp_obj_count + 1))
        return 0;
    int32_t mask = c->temp_object_set_capacity - 1;
    int32_t slot = (int32_t)(canvas3d_hash_u64((uintptr_t)obj) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->temp_object_set_capacity; ++probe) {
        if (!c->temp_object_set[slot]) {
            c->temp_object_set[slot] = obj;
            return 1;
        }
        if (c->temp_object_set[slot] == obj)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Rebuild the transient-object hash set from the tracked-object list (after
/// growth/removal).
/// @param c Canvas whose existing object set is cleared and repopulated.
void canvas3d_rebuild_temp_object_set(rt_canvas3d *c) {
    if (!c || !c->temp_object_set)
        return;
    canvas3d_temp_object_set_clear(c);
    for (int32_t i = 0; i < c->temp_obj_count; ++i)
        canvas3d_temp_object_set_insert(c, c->temp_objects[i]);
}

/// @brief Track a GC-managed object for end-of-frame release.
///
/// Retains `obj` immediately so it survives at least until the
/// frame ends, then releases at end-of-frame via `clear_temp_objects`.
/// @param c Canvas that assumes one conditional runtime reference on success.
/// @param obj Non-null GC-managed or retain-compatible object.
/// @return Nonzero when already tracked or newly retained and appended; zero for
///   invalid input, count/capacity overflow, set failure, or list allocation failure.
int canvas3d_track_temp_object(rt_canvas3d *c, void *obj) {
    if (!c || !obj)
        return 0;
    if (canvas3d_temp_object_set_contains(c, obj))
        return 1;
    if (c->temp_obj_count >= c->temp_obj_capacity) {
        if (c->temp_obj_capacity < 0 || c->temp_obj_capacity > INT32_MAX / 2)
            return 0;
        int32_t new_cap = c->temp_obj_capacity == 0 ? 8 : c->temp_obj_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
            return 0;
        void **nb = (void **)realloc(c->temp_objects, (size_t)new_cap * sizeof(void *));
        if (!nb)
            return 0;
        c->temp_objects = nb;
        c->temp_obj_capacity = new_cap;
    }
    if (!canvas3d_temp_object_set_insert(c, obj))
        return 0;
    rt_obj_retain_maybe(obj);
    c->temp_objects[c->temp_obj_count++] = obj;
    return 1;
}

/// @brief Untrack a per-frame transient object and release its reference.
/// @param c Canvas whose retained-object list and duplicate set are updated.
/// @param obj Exact tracked object pointer. Absent or invalid inputs are ignored.
void canvas3d_release_tracked_temp_object(rt_canvas3d *c, void *obj) {
    if (!c || !obj)
        return;
    for (int32_t i = 0; i < c->temp_obj_count; ++i) {
        if (c->temp_objects[i] == obj) {
            int32_t last = c->temp_obj_count - 1;
            c->temp_objects[i] = c->temp_objects[last];
            c->temp_objects[last] = NULL;
            c->temp_obj_count = last;
            canvas3d_rebuild_temp_object_set(c);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            return;
        }
    }
}

/// @brief Free every tracked transient buffer (called at end of frame).
/// @details Publishes saturating snapshot-byte diagnostics, frees ordinary temp
///   allocations, releases retained mesh revisions, resets snapshot/hash state,
///   and invalidates frame-arena allocations. Final-overlay allocations are not
///   part of this cleanup phase.
/// @param c Canvas whose current frame native resources are released; `NULL` is ignored.
void canvas3d_clear_temp_buffers(rt_canvas3d *c) {
    if (!c)
        return;
    if (c->mesh_snapshot_bytes > (size_t)INT64_MAX)
        c->last_mesh_snapshot_bytes = INT64_MAX;
    else
        c->last_mesh_snapshot_bytes = (int64_t)c->mesh_snapshot_bytes;
    for (int32_t i = 0; i < c->temp_buf_count; i++)
        free(c->temp_buffers[i]);
    c->temp_buf_count = 0;
    canvas3d_temp_buffer_set_clear(c);
    c->float_snapshot_count = 0;
    canvas3d_release_retained_mesh_revisions(c);
    c->mesh_snapshot_count = 0;
    canvas3d_mesh_snapshot_hash_clear(c);
    c->mesh_snapshot_bytes = 0u;
    canvas3d_frame_arena_reset(c);
}

/// @brief Release every tracked transient GC object (called at end of frame).
/// @param c Canvas whose retained objects are conditionally freed and whose
///   list/set are reset; `NULL` is ignored.
void canvas3d_clear_temp_objects(rt_canvas3d *c) {
    if (!c)
        return;
    for (int32_t i = 0; i < c->temp_obj_count; i++) {
        if (c->temp_objects[i] && rt_obj_release_check0(c->temp_objects[i]))
            rt_obj_free(c->temp_objects[i]);
        c->temp_objects[i] = NULL;
    }
    c->temp_obj_count = 0;
    canvas3d_temp_object_set_clear(c);
}

#endif /* ZANNA_ENABLE_GRAPHICS */
