//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_motion.c
// Purpose: Canvas3D motion-history — per-object previous-frame model matrices
//   used to derive motion vectors. A growable entry array plus an open-addressing
//   hash index keyed by a stable per-draw key. Split out of rt_canvas3d.c; the
//   backing arrays live on rt_canvas3d (see rt_canvas3d_internal.h).
// Key invariants:
//   - The hash mirrors the entry array (slot stores entry index + 1; 0 = empty).
//   - An entry's current→previous roll happens at most once per frame_serial.
// Ownership/Lifetime:
//   - Canvas3D owns the motion entry array and open-addressing hash table.
//   - Matrix inputs are borrowed and copied before any persistent publication.
// Links: rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D previous-transform history and stable draw identities.
/// @details A compact entry array stores temporal matrices while a rebuilt
///   open-addressing index provides constant-time lookup. Keys combine caller
///   identities with allocation generations so address reuse cannot inherit
///   stale motion, and retention tolerates temporarily culled objects.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "rt_platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS3D_MOTION_HASH_MAX_CAPACITY (INT32_C(1) << 30)

/// @brief Return whether a motion hash capacity is a supported power of two.
/// @param capacity Candidate slot count.
/// @return Nonzero for powers of two in `[1, 2^30]`.
static int canvas3d_motion_valid_hash_capacity(int32_t capacity) {
    return capacity > 0 && capacity <= CANVAS3D_MOTION_HASH_MAX_CAPACITY &&
           ((uint32_t)capacity & ((uint32_t)capacity - 1u)) == 0u;
}

/// @brief Repair motion entry-array count/capacity metadata before iteration.
/// @param c Canvas owning the motion entry array.
static void canvas3d_repair_motion_history_metadata(rt_canvas3d *c) {
    if (!c)
        return;
    if (!c->motion_history || c->motion_history_capacity < 0) {
        c->motion_history_count = 0;
        c->motion_history_capacity = 0;
        return;
    }
    if (c->motion_history_count < 0)
        c->motion_history_count = 0;
    if (c->motion_history_count > c->motion_history_capacity)
        c->motion_history_count = c->motion_history_capacity;
}

/// @brief Drop an absent or malformed motion hash so callers rebuild it safely.
/// @param c Canvas owning the hash table.
static void canvas3d_repair_motion_hash_metadata(rt_canvas3d *c) {
    if (!c)
        return;
    if (!c->motion_history_hash) {
        c->motion_history_hash_capacity = 0;
        c->motion_history_hash_count = 0;
        return;
    }
    if (!canvas3d_motion_valid_hash_capacity(c->motion_history_hash_capacity)) {
        free(c->motion_history_hash);
        c->motion_history_hash = NULL;
        c->motion_history_hash_capacity = 0;
        c->motion_history_hash_count = 0;
    } else if (c->motion_history_hash_count < 0 ||
               c->motion_history_hash_count > c->motion_history_hash_capacity) {
        c->motion_history_hash_count = 0;
    }
}

/// @brief Validate that all lanes of a model matrix are finite.
/// @param model Borrowed sixteen-lane float matrix.
/// @return Nonzero when every component is finite.
static int canvas3d_motion_model_is_finite(const float *model) {
    if (!model)
        return 0;
    for (int32_t i = 0; i < 16; ++i) {
        if (!isfinite(model[i]))
            return 0;
    }
    return 1;
}

/// @brief Ensure the motion-history entry array can hold @p needed entries (grows geometrically).
/// @param c Canvas3D owning the entry allocation.
/// @param needed Positive minimum number of entries.
/// @return Non-zero when capacity is available; zero on invalid input,
///   representational overflow, or allocation failure.
static int ensure_motion_history_capacity(rt_canvas3d *c, int32_t needed) {
    if (!c || needed <= 0)
        return 0;
    canvas3d_repair_motion_history_metadata(c);
    if (c->motion_history && c->motion_history_capacity >= needed)
        return 1;

    int32_t new_cap = c->motion_history_capacity > 0 ? c->motion_history_capacity : 32;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            new_cap = needed;
        else
            new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(canvas_motion_history_t))
        return 0;

    canvas_motion_history_t *new_hist = (canvas_motion_history_t *)realloc(
        c->motion_history, (size_t)new_cap * sizeof(canvas_motion_history_t));
    if (!new_hist)
        return 0;
    c->motion_history = new_hist;
    c->motion_history_capacity = new_cap;
    return 1;
}

/// @brief Clear the motion-history hash table (all slots back to the empty sentinel 0).
/// @param c Canvas3D owning the hash allocation; missing storage is a no-op.
static void canvas3d_motion_hash_reset(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_repair_motion_hash_metadata(c);
    if (!c->motion_history_hash)
        return;
    memset(c->motion_history_hash,
           0,
           (size_t)c->motion_history_hash_capacity * sizeof(*c->motion_history_hash));
    c->motion_history_hash_count = 0;
}

/// @brief Drop all previous-model history entries without freeing their backing storage.
/// @details Used after floating-origin rebases and other whole-world coordinate shifts. The next
///   frame starts each motion key fresh, avoiding temporal artifacts from comparing transforms
///   captured before and after an origin discontinuity.
/// @param c Canvas3D whose logical entry count and hash index are reset.
void canvas3d_clear_motion_history(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_repair_motion_history_metadata(c);
    c->motion_history_count = 0;
    canvas3d_motion_hash_reset(c);
}

/// @brief Ensure the motion-history hash has a power-of-two capacity sized for @p count_hint
/// entries.
/// @details Targets ~2x load headroom (min 32) and resets the table on growth.
/// @param c Canvas3D owning the open-addressing table.
/// @param count_hint Non-negative anticipated number of live history entries.
/// @return Non-zero when a sufficiently large table exists; zero on invalid
///   input, overflow, or allocation failure.
static int canvas3d_ensure_motion_hash_capacity(rt_canvas3d *c, int32_t count_hint) {
    if (!c)
        return 0;
    canvas3d_repair_motion_hash_metadata(c);
    if (count_hint < 0 || count_hint > CANVAS3D_MOTION_HASH_MAX_CAPACITY / 2)
        return 0;
    int32_t needed = canvas3d_next_power_of_two_i32(count_hint > 0 ? count_hint * 2 : 32);
    if (needed < 32)
        needed = 32;
    if (c->motion_history_hash && c->motion_history_hash_capacity >= needed)
        return 1;
    if ((size_t)needed > SIZE_MAX / sizeof(*c->motion_history_hash))
        return 0;
    int32_t *grown = (int32_t *)realloc(c->motion_history_hash, (size_t)needed * sizeof(*grown));
    if (!grown)
        return 0;
    c->motion_history_hash = grown;
    c->motion_history_hash_capacity = needed;
    canvas3d_motion_hash_reset(c);
    return 1;
}

/// @brief Insert history entry @p index into the hash by linear probing (slot stores index+1).
/// @param c Canvas3D owning synchronized history and hash arrays.
/// @param index Zero-based existing history entry to index.
/// @return 1 on success, 0 if the table is full or inputs are invalid.
static int canvas3d_motion_hash_insert_existing(rt_canvas3d *c, int32_t index) {
    if (!c || !c->motion_history_hash ||
        !canvas3d_motion_valid_hash_capacity(c->motion_history_hash_capacity) || index < 0 ||
        index >= c->motion_history_count || index >= INT32_MAX)
        return 0;
    canvas_motion_history_t *hist = (canvas_motion_history_t *)c->motion_history;
    if (!hist)
        return 0;
    int32_t mask = c->motion_history_hash_capacity - 1;
    int32_t slot = (int32_t)(canvas3d_hash_u64(hist[index].key) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->motion_history_hash_capacity; ++probe) {
        if (c->motion_history_hash[slot] == 0) {
            c->motion_history_hash[slot] = index + 1;
            if (c->motion_history_hash_count < INT32_MAX)
                c->motion_history_hash_count++;
            return 1;
        }
        if (c->motion_history_hash[slot] == index + 1)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Rebuild the motion-history hash from scratch over the current history array.
/// @details Sizes the table to the entry count, clears it, and re-inserts every entry.
/// @param c Canvas3D whose hash is reconstructed from its authoritative entry array.
/// @return Non-zero when all live entries are indexed; zero on invalid input
///   or capacity/insertion failure.
static int canvas3d_rebuild_motion_hash(rt_canvas3d *c) {
    if (!c)
        return 0;
    canvas3d_repair_motion_history_metadata(c);
    canvas3d_repair_motion_hash_metadata(c);
    if (c->motion_history_count <= 0) {
        canvas3d_motion_hash_reset(c);
        return 1;
    }
    if (c->motion_history_count >= CANVAS3D_MOTION_HASH_MAX_CAPACITY / 2 ||
        !canvas3d_ensure_motion_hash_capacity(c, c->motion_history_count + 1))
        return 0;
    canvas3d_motion_hash_reset(c);
    for (int32_t i = 0; i < c->motion_history_count; ++i) {
        if (!canvas3d_motion_hash_insert_existing(c, i))
            return 0;
    }
    return 1;
}

/// @brief Look up the motion-history index for @p key, rebuilding the hash if it is
/// stale/undersized.
/// @param c Canvas3D owning the history and hash arrays.
/// @param key Non-zero stable motion identity to locate.
/// @return The history index, or -1 if the key is absent.
static int32_t canvas3d_motion_hash_find_index(rt_canvas3d *c, uintptr_t key) {
    if (!c || key == 0)
        return -1;
    canvas3d_repair_motion_history_metadata(c);
    canvas3d_repair_motion_hash_metadata(c);
    if (c->motion_history_count <= 0 || !c->motion_history ||
        c->motion_history_count >= CANVAS3D_MOTION_HASH_MAX_CAPACITY / 2)
        return -1;
    if (!c->motion_history_hash || c->motion_history_hash_count != c->motion_history_count ||
        c->motion_history_hash_capacity <
            canvas3d_next_power_of_two_i32(c->motion_history_count * 2)) {
        if (!canvas3d_rebuild_motion_hash(c))
            return -1;
    }
    canvas_motion_history_t *hist = (canvas_motion_history_t *)c->motion_history;
    int32_t mask = c->motion_history_hash_capacity - 1;
    int32_t slot = (int32_t)(canvas3d_hash_u64(key) & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->motion_history_hash_capacity; ++probe) {
        int32_t encoded = c->motion_history_hash[slot];
        if (encoded == 0)
            return -1;
        int32_t index = encoded - 1;
        if (index < 0 || index >= c->motion_history_count) {
            c->motion_history_hash_count = 0;
            if (!canvas3d_rebuild_motion_hash(c))
                return -1;
            return canvas3d_motion_hash_find_index(c, key);
        }
        if (index >= 0 && index < c->motion_history_count && hist[index].key == key)
            return index;
        slot = (slot + 1) & mask;
    }
    return -1;
}

/// @brief Drop motion-history entries that have not been touched for the retention window.
///
/// In-place compaction. Retaining a few missed frames keeps motion vectors stable when objects are
/// temporarily skipped by frustum or occlusion culling, while bounded eviction still prevents the
/// table from growing without bound after objects stop drawing or are destroyed.
/// @param c Canvas3D whose history is compacted in place and reindexed.
void canvas3d_prune_motion_history(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_repair_motion_history_metadata(c);
    if (c->motion_history_count <= 0)
        return;

    canvas_motion_history_t *hist = (canvas_motion_history_t *)c->motion_history;
    int32_t dst = 0;
    int64_t retention =
        c->motion_history_retention_frames > 0 ? c->motion_history_retention_frames : 1;
    for (int32_t i = 0; i < c->motion_history_count; i++) {
        uint64_t age;
        if (hist[i].key == 0 || !canvas3d_motion_model_is_finite(hist[i].current_model))
            continue;
        if (hist[i].last_frame_seen > c->frame_serial) {
            /* A future stamp cannot be genuine after the owning frame serial
             * advances monotonically. Keep the current matrix as a fresh entry
             * instead of retaining it forever or overflowing signed subtraction. */
            hist[i].last_frame_seen = c->frame_serial;
            hist[i].has_prev = 0;
        }
        age = (uint64_t)c->frame_serial - (uint64_t)hist[i].last_frame_seen;
        if (age > (uint64_t)retention)
            continue;
        hist[i].has_current = hist[i].has_current ? 1 : 0;
        hist[i].has_prev =
            hist[i].has_prev && canvas3d_motion_model_is_finite(hist[i].prev_model) ? 1 : 0;
        if (dst != i)
            hist[dst] = hist[i];
        dst++;
    }
    c->motion_history_count = dst;
    canvas3d_rebuild_motion_hash(c);
}

/// @brief Look up (and update) the previous-frame model matrix for a mesh.
///
/// Three cases:
///   1. Existing entry, first lookup this frame → roll current→previous,
///      update current, return previous.
///   2. Existing entry, repeat lookup this frame → just return the
///      previous (don't roll twice).
///   3. New entry → register, return "no previous yet".
/// Returns through `out_has_prev` whether the previous frame was
/// available — first-frame draws fall back to current=previous.
/// @param c Canvas3D owning the temporal history.
/// @param motion_key Non-zero stable identity for the logical draw instance.
/// @param current_model Borrowed current row-major 16-float model matrix.
/// @param out_prev_model Non-`NULL` output receiving the prior matrix when available.
/// @param out_has_prev Non-`NULL` output set to one only for genuine history.
void canvas3d_resolve_previous_model(rt_canvas3d *c,
                                     uintptr_t motion_key,
                                     const float *current_model,
                                     float *out_prev_model,
                                     int8_t *out_has_prev) {
    float current_copy[16];
    int32_t old_hash_capacity;
    int hash_was_present;
    if (current_model)
        memcpy(current_copy, current_model, sizeof(current_copy));
    if (out_has_prev)
        *out_has_prev = 0;
    if (out_prev_model)
        memset(out_prev_model, 0, sizeof(float) * 16);
    if (!c || motion_key == 0 || !current_model || !out_prev_model || !out_has_prev ||
        !canvas3d_motion_model_is_finite(current_copy))
        return;

    canvas3d_repair_motion_history_metadata(c);
    canvas_motion_history_t *hist = (canvas_motion_history_t *)c->motion_history;
    int32_t found_index = canvas3d_motion_hash_find_index(c, motion_key);
    if (found_index >= 0) {
        canvas_motion_history_t *entry = &hist[found_index];
        entry->has_current =
            entry->has_current && canvas3d_motion_model_is_finite(entry->current_model) ? 1 : 0;
        entry->has_prev =
            entry->has_prev && canvas3d_motion_model_is_finite(entry->prev_model) ? 1 : 0;
        if (entry->last_frame_seen > c->frame_serial) {
            entry->has_current = 0;
            entry->has_prev = 0;
        }
        if (entry->last_frame_seen != c->frame_serial || !entry->has_current) {
            if (entry->has_current) {
                memcpy(entry->prev_model, entry->current_model, sizeof(entry->prev_model));
                entry->has_prev = 1;
            }
            memcpy(entry->current_model, current_copy, sizeof(entry->current_model));
            entry->has_current = 1;
            entry->last_frame_seen = c->frame_serial;
        }

        if (entry->has_prev) {
            memcpy(out_prev_model, entry->prev_model, sizeof(entry->prev_model));
            *out_has_prev = 1;
        }
        return;
    }

    if (c->motion_history_count >= INT32_MAX ||
        !ensure_motion_history_capacity(c, c->motion_history_count + 1))
        return;
    old_hash_capacity = c->motion_history_hash_capacity;
    hash_was_present = c->motion_history_hash != NULL;
    if (!canvas3d_ensure_motion_hash_capacity(c, c->motion_history_count + 1))
        return;
    if (c->motion_history_count > 0 &&
        (!hash_was_present || old_hash_capacity != c->motion_history_hash_capacity) &&
        !canvas3d_rebuild_motion_hash(c))
        return;

    hist = (canvas_motion_history_t *)c->motion_history;
    int32_t new_index = c->motion_history_count;
    canvas_motion_history_t *entry = &hist[new_index];
    memset(entry, 0, sizeof(*entry));
    entry->key = motion_key;
    memcpy(entry->current_model, current_copy, sizeof(entry->current_model));
    entry->has_current = 1;
    entry->last_frame_seen = c->frame_serial;
    c->motion_history_count = new_index + 1;
    if (!canvas3d_motion_hash_insert_existing(c, new_index)) {
        c->motion_history_count = new_index;
        memset(entry, 0, sizeof(*entry));
        (void)canvas3d_rebuild_motion_hash(c);
    }
}

/// @brief Mix one pointer/value into a running motion-history hash key (boost-style
///   hash_combine with the golden-ratio constant) so per-object motion vectors stay stable.
/// @param key Current accumulated identity.
/// @param value Next pointer-sized component to incorporate.
/// @return Mixed pointer-sized identity.
static uintptr_t canvas3d_mix_motion_key(uintptr_t key, uintptr_t value) {
    key ^= value + (uintptr_t)0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
    return key;
}

/// @brief Mix a pointer-sized key into a 32-bit hash for the motion-history table.
/// @param value Pointer-sized identity to hash.
/// @return Deterministic 32-bit avalanche hash.
uint32_t canvas3d_hash_u64(uintptr_t value) {
    uint64_t x = (uint64_t)value;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return (uint32_t)x;
}

/// @brief Round @p value up to the next power of two (used to size the open-addressing hash table).
/// @param value Requested signed capacity.
/// @return One for values at most one; otherwise the smallest representable
///   positive signed power of two at least @p value, or zero when none exists.
int32_t canvas3d_next_power_of_two_i32(int32_t value) {
    int32_t cap = 1;
    if (value <= 1)
        return 1;
    if (value > CANVAS3D_MOTION_HASH_MAX_CAPACITY)
        return 0;
    while (cap < value) {
        cap <<= 1;
    }
    return cap;
}

/// @brief Monotonic allocation generation for pointer-keyed history salting.
/// @details Relaxed atomic ordering provides uniqueness without imposing publication ordering.
/// @return A newly allocated non-zero 64-bit identity, wrapping past zero to one.
uint64_t rt_g3d_next_identity_serial(void) {
    static uint64_t serial = 1;
    uint64_t identity;
    do {
        identity = rt_atomic_fetch_add_u64(&serial, UINT64_C(1), __ATOMIC_RELAXED);
    } while (identity == 0);
    return identity;
}

/// @brief Allocation-generation salt for a mesh or material handle (0 for other types).
/// @details Motion (and occlusion) history outlives object frees by a few frames; a new
///   object recycled onto the same address would otherwise inherit the dead object's
///   previous transform and flash a bogus motion vector on its first frame.
/// @param obj Borrowed candidate Mesh3D or Material3D runtime handle.
/// @return The object's non-zero allocation generation, or zero for another
///   type or an invalid handle.
static uintptr_t canvas3d_object_identity_salt(const void *obj) {
    void *handle = (void *)(uintptr_t)obj;
    const rt_mesh3d *mesh =
        (const rt_mesh3d *)rt_g3d_checked_or_null(handle, RT_G3D_MESH3D_CLASS_ID);
    if (mesh)
        return (uintptr_t)mesh->identity_serial;
    const rt_material3d *mat =
        (const rt_material3d *)rt_g3d_checked_or_null(handle, RT_G3D_MATERIAL3D_CLASS_ID);
    if (mat)
        return (uintptr_t)mat->identity_serial;
    return 0;
}

/// @brief Derive a stable object draw key for transform-handle draw calls.
/// @param mesh_obj Borrowed Mesh3D identity.
/// @param material_obj Borrowed Material3D identity.
/// @param transform_obj Borrowed Mat4 identity.
/// @return Non-zero mixed key incorporating addresses and allocation generations.
uintptr_t canvas3d_mesh_transform_motion_key(const void *mesh_obj,
                                             const void *material_obj,
                                             const void *transform_obj) {
    uintptr_t key = (uintptr_t)transform_obj;
    key = canvas3d_mix_motion_key(key, (uintptr_t)mesh_obj);
    key = canvas3d_mix_motion_key(key, canvas3d_object_identity_salt(mesh_obj));
    key = canvas3d_mix_motion_key(key, (uintptr_t)material_obj);
    key = canvas3d_mix_motion_key(key, canvas3d_object_identity_salt(material_obj));
    return key ? key : (uintptr_t)1u;
}

/// @brief Derive a stable per-instance key for the motion-blur history table.
/// @details Includes the caller's batch buffer identity so two batches with
///          the same mesh/material/count do not alias one another's previous
///          transforms. Keeping the same matrix buffer across frames preserves
///          continuous history; a reallocated buffer safely starts fresh.
/// @param mesh_obj Borrowed shared Mesh3D identity.
/// @param material_obj Borrowed shared Material3D identity.
/// @param batch_obj Borrowed stable identity of the original matrix array.
/// @param instance_count Original number of instances in the batch.
/// @param index Original zero-based instance index.
/// @return Non-zero mixed per-instance key, or zero for an invalid count/index pair.
uintptr_t canvas3d_instance_motion_key(const void *mesh_obj,
                                       const void *material_obj,
                                       const void *batch_obj,
                                       int32_t instance_count,
                                       int32_t index) {
    if (instance_count <= 0 || index < 0 || index >= instance_count)
        return (uintptr_t)0;
    uintptr_t key = (uintptr_t)mesh_obj;
    uintptr_t instance_key = (uintptr_t)((uint32_t)index + 1u);
    key = canvas3d_mix_motion_key(key, canvas3d_object_identity_salt(mesh_obj));
    key = canvas3d_mix_motion_key(key, (uintptr_t)material_obj);
    key = canvas3d_mix_motion_key(key, canvas3d_object_identity_salt(material_obj));
    key = canvas3d_mix_motion_key(key, (uintptr_t)batch_obj);
    key = canvas3d_mix_motion_key(key, (uintptr_t)((uint32_t)instance_count + 1u));
    key ^= instance_key * (uintptr_t)0xc2b2ae35u;
    return key ? key : instance_key;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
