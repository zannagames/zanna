//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_cache.c
// Purpose: Glyph cache implementation — hash-table with LRU eviction for
//          rasterised glyph bitmaps, keyed by (size, codepoint) pairs.
// Key invariants:
//   - Cache keys encode both float size (as raw bits) and codepoint to avoid
//     floating-point comparison across distinct size values.
//   - The LRU eviction discards the 25% least-recently-used entries when
//     VG_CACHE_MAX_MEMORY is exceeded, retaining hot glyphs under load.
//   - g_cache_tick is a monotonic uint64_t to avoid wrap-around on long-running
//     applications; every cache hit increments it.
// Ownership/Lifetime:
//   - Each cache entry owns its glyph.bitmap; freed on eviction or destroy.
//   - The cache struct owns its bucket array; freed in vg_cache_destroy.
// Links: lib/gui/src/font/vg_ttf_internal.h,
//        lib/gui/src/font/vg_raster.c
//
//===----------------------------------------------------------------------===//
#include "vg_ttf_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements the rasterized-glyph hash cache and its LRU eviction policy.
/// @details Cache entries are keyed by a quantized pixel size and Unicode code point. Each entry
/// owns a copy of its bitmap, while cache-wide counters enforce bounded memory use and trigger
/// least-recently-used eviction under pressure.

/// @brief Compute the storage required by one glyph bitmap.
/// @details Empty or non-positive glyph extents are valid and require zero bytes. Positive
/// dimensions are multiplied only after checking for a `size_t` overflow.
/// @param glyph Glyph metadata whose one-byte-per-pixel bitmap dimensions are inspected.
/// @param out_size Receives the required byte count, including zero for an empty bitmap.
/// @return Nonzero when the size is representable, or zero when the multiplication would overflow.
static int glyph_bitmap_size(const vg_glyph_t *glyph, size_t *out_size) {
    if (!glyph || !out_size || glyph->width <= 0 || glyph->height <= 0) {
        if (out_size)
            *out_size = 0;
        return 1;
    }
    size_t width = (size_t)glyph->width;
    size_t height = (size_t)glyph->height;
    if (width > SIZE_MAX / height)
        return 0;
    *out_size = width * height;
    return 1;
}

//=============================================================================
// Hash Function
//=============================================================================

/// @brief Quantize a font size to a stable fixed-point cache component.
/// @details Sub-pixel float noise can otherwise create duplicate glyph cache
///          entries for sizes that render indistinguishably.  A 1/64 pixel grid
///          preserves practical precision while improving cache hit rates.
/// @param size Font size in pixels.
/// @return Unsigned fixed-point representation, saturated on extreme input.
static uint32_t quantize_cache_size(float size) {
    if (!isfinite(size) || size <= 0.0f)
        return 0;
    double scaled = (double)size * 64.0 + 0.5;
    if (scaled >= (double)UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)scaled;
}

/// @brief Pack a quantized size and code point into one cache key.
/// @param size Font size in pixels; invalid and non-positive values map to a zero size component.
/// @param codepoint Unicode scalar value stored in the low 32 bits.
/// @return Stable 64-bit key for the `(size, codepoint)` pair.
static uint64_t make_cache_key(float size, uint32_t codepoint) {
    return ((uint64_t)quantize_cache_size(size) << 32) | codepoint;
}

/// @brief Hash a cache key to a bucket index using avalanche-style mixing.
/// @param key Packed glyph cache key.
/// @param bucket_count Number of buckets available in the destination table.
/// @return Index in `[0, bucket_count)`, or zero when @p bucket_count is zero.
static size_t hash_key(uint64_t key, size_t bucket_count) {
    if (bucket_count == 0)
        return 0;

    // FNV-1a style mixing
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return (size_t)(key % bucket_count);
}

/// @brief Normalize cache access ticks after a uint64_t wrap.
/// @details Wrap is practically unreachable, but keeping entries ordered after
///          it costs little and avoids reintroducing global shared state.
/// @param cache Glyph cache whose entries should be marked as old.
static void cache_reset_access_ticks(vg_glyph_cache_t *cache) {
    if (!cache || !cache->buckets)
        return;
    for (size_t i = 0; i < cache->bucket_count; i++) {
        for (vg_cache_entry_t *entry = cache->buckets[i]; entry; entry = entry->next)
            entry->access_tick = 0;
    }
    cache->access_tick = 0;
}

/// @brief Return the next per-cache LRU access tick.
/// @param cache Glyph cache being accessed.
/// @return New non-zero access tick for one cache entry.
static uint64_t cache_next_access_tick(vg_glyph_cache_t *cache) {
    if (!cache)
        return 0;
    cache->access_tick++;
    if (cache->access_tick == 0) {
        cache_reset_access_ticks(cache);
        cache->access_tick = 1;
    }
    return cache->access_tick;
}

//=============================================================================
// Cache Creation/Destruction
//=============================================================================

/// @brief Allocate an empty glyph cache.
/// @details Initializes a hash table with `VG_CACHE_INITIAL_SIZE` buckets;
///          entries are added lazily on first cache miss for a given
///          `(size, codepoint)` pair.
/// @return Newly-allocated cache, or NULL on allocation failure.
vg_glyph_cache_t *vg_cache_create(void) {
    vg_glyph_cache_t *cache = calloc(1, sizeof(vg_glyph_cache_t));
    if (!cache)
        return NULL;

    cache->bucket_count = VG_CACHE_INITIAL_SIZE;
    cache->buckets = calloc(cache->bucket_count, sizeof(vg_cache_entry_t *));
    if (!cache->buckets) {
        free(cache);
        return NULL;
    }

    return cache;
}

/// @brief Free a single cache entry and the bitmap it owns.
/// @param entry Entry to release; callers must pass a valid cache entry.
static void free_entry(vg_cache_entry_t *entry) {
    if (entry->glyph.bitmap) {
        free(entry->glyph.bitmap);
    }
    free(entry);
}

/// @brief Free a glyph cache and every entry it owns.
/// @details Walks every bucket chain, releases each entry's bitmap, then
///          frees the bucket array and the cache itself. Safe on NULL.
/// @param cache Cache produced by @ref vg_cache_create (may be NULL).
void vg_cache_destroy(vg_glyph_cache_t *cache) {
    if (!cache)
        return;

    // Free all entries
    for (size_t i = 0; i < cache->bucket_count; i++) {
        vg_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            vg_cache_entry_t *next = entry->next;
            free_entry(entry);
            entry = next;
        }
    }

    free(cache->buckets);
    free(cache);
}

//=============================================================================
// Cache Clear
//=============================================================================

/// @brief Drop every entry without releasing the cache itself.
/// @details Walks every bucket and frees each entry's bitmap. Resets the
///          entry / memory counters to zero so the cache is ready to grow
///          again on the next put. Bucket array is preserved at its
///          current capacity. Safe on NULL.
/// @param cache Cache to clear (may be NULL).
void vg_cache_clear(vg_glyph_cache_t *cache) {
    if (!cache)
        return;

    for (size_t i = 0; i < cache->bucket_count; i++) {
        vg_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            vg_cache_entry_t *next = entry->next;
            free_entry(entry);
            entry = next;
        }
        cache->buckets[i] = NULL;
    }

    cache->entry_count = 0;
    cache->memory_used = 0;
}

//=============================================================================
// Cache Resize
//=============================================================================

/// @brief Double the bucket array and rehash every entry.
/// @details Growth is capped at `VG_CACHE_MAX_SIZE`. A failed allocation leaves the original
/// bucket array and all entry chains unchanged.
/// @param cache Cache whose hash table should grow.
/// @return `true` when the table is replaced, or `false` for invalid input, a size limit, integer
/// overflow, or allocation failure.
static bool cache_resize(vg_glyph_cache_t *cache) {
    if (!cache || cache->bucket_count == 0)
        return false;
    if (cache->bucket_count > SIZE_MAX / 2 && cache->bucket_count < VG_CACHE_MAX_SIZE)
        return false;

    size_t new_count = cache->bucket_count * 2;
    if (new_count > VG_CACHE_MAX_SIZE) {
        new_count = VG_CACHE_MAX_SIZE;
        if (new_count == cache->bucket_count) {
            return false; // Already at max size
        }
    }

    vg_cache_entry_t **new_buckets = calloc(new_count, sizeof(vg_cache_entry_t *));
    if (!new_buckets)
        return false;

    // Rehash all entries
    for (size_t i = 0; i < cache->bucket_count; i++) {
        vg_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            vg_cache_entry_t *next = entry->next;
            size_t new_idx = hash_key(entry->key, new_count);
            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;
            entry = next;
        }
    }

    free(cache->buckets);
    cache->buckets = new_buckets;
    cache->bucket_count = new_count;

    return true;
}

//=============================================================================
// Cache Eviction — LRU: sort all entries by access_tick and free the 25% with
// the smallest ticks (least recently used). New entries have access_tick = 0,
// so they are the first candidates if they are never hit.
//=============================================================================

/// @brief Compare cache-entry pointers by ascending access tick.
/// @param a Address of the first `vg_cache_entry_t *`.
/// @param b Address of the second `vg_cache_entry_t *`.
/// @return A negative value when @p a is older, a positive value when @p b is older, or zero for
/// equal access ticks.
static int compare_ticks(const void *a, const void *b) {
    const vg_cache_entry_t *const *entry_a = (const vg_cache_entry_t *const *)a;
    const vg_cache_entry_t *const *entry_b = (const vg_cache_entry_t *const *)b;
    uint64_t ta = (*entry_a)->access_tick;
    uint64_t tb = (*entry_b)->access_tick;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

/// @brief Evict the oldest quarter of a cache to reclaim bitmap memory.
/// @details The preferred path sorts a temporary entry-pointer array by access tick. If that
/// allocation fails, the fallback repeatedly scans the table for the oldest entry. Both paths
/// unlink victims, update accounting, and release their owned bitmaps.
/// @param cache Cache to trim; NULL and structurally empty caches are ignored.
static void cache_evict_some(vg_glyph_cache_t *cache) {
    if (!cache || cache->bucket_count == 0 || !cache->buckets)
        return;

    size_t count = cache->entry_count;
    if (count == 0)
        return;

    size_t to_evict = count / 4;
    if (to_evict < 1)
        to_evict = 1;

    vg_cache_entry_t **entries = (vg_cache_entry_t **)malloc(count * sizeof(*entries));
    if (entries) {
        size_t used = 0;
        for (size_t i = 0; i < cache->bucket_count; i++) {
            for (vg_cache_entry_t *entry = cache->buckets[i]; entry; entry = entry->next)
                entries[used++] = entry;
        }
        qsort(entries, used, sizeof(*entries), compare_ticks);
        if (to_evict > used)
            to_evict = used;
        for (size_t k = 0; k < to_evict; k++) {
            vg_cache_entry_t *victim = entries[k];
            size_t bi = hash_key(victim->key, cache->bucket_count);
            vg_cache_entry_t **prev = &cache->buckets[bi];
            while (*prev && *prev != victim)
                prev = &(*prev)->next;
            if (*prev != victim)
                continue;
            *prev = victim->next;

            size_t victim_memory = 0;
            if (glyph_bitmap_size(&victim->glyph, &victim_memory) &&
                victim_memory <= cache->memory_used)
                cache->memory_used -= victim_memory;
            else
                cache->memory_used = 0;
            cache->entry_count--;
            free_entry(victim);
        }
        free(entries);
        return;
    }

    for (size_t k = 0; k < to_evict; k++) {
        vg_cache_entry_t *victim = NULL;
        for (size_t i = 0; i < cache->bucket_count; i++) {
            for (vg_cache_entry_t *entry = cache->buckets[i]; entry; entry = entry->next) {
                if (!victim || compare_ticks(&entry, &victim) < 0)
                    victim = entry;
            }
        }
        if (!victim)
            break;
        // Remove from its bucket chain
        size_t bi = hash_key(victim->key, cache->bucket_count);
        vg_cache_entry_t **prev = &cache->buckets[bi];
        while (*prev && *prev != victim)
            prev = &(*prev)->next;
        if (*prev == victim)
            *prev = victim->next;

        size_t victim_memory = 0;
        if (glyph_bitmap_size(&victim->glyph, &victim_memory) &&
            victim_memory <= cache->memory_used)
            cache->memory_used -= victim_memory;
        else
            cache->memory_used = 0;
        cache->entry_count--;
        free_entry(victim);
    }
}

//=============================================================================
// Cache Get
//=============================================================================

/// @brief Look up a rasterized glyph and mark it as recently used.
/// @details The requested size is quantized exactly as it is during insertion. A successful lookup
/// advances the cache-local access clock and stores the new tick on the matching entry.
/// @param cache Cache to search.
/// @param size Font size in pixels.
/// @param codepoint Unicode code point identifying the glyph.
/// @return Borrowed glyph data owned by @p cache, or NULL when the cache is invalid or the key is
/// absent. The pointer remains valid until that entry is evicted or the cache is cleared/destroyed.
const vg_glyph_t *vg_cache_get(vg_glyph_cache_t *cache, float size, uint32_t codepoint) {
    if (!cache)
        return NULL;
    if (cache->bucket_count == 0 || !cache->buckets)
        return NULL;

    uint64_t key = make_cache_key(size, codepoint);
    size_t idx = hash_key(key, cache->bucket_count);

    vg_cache_entry_t *entry = cache->buckets[idx];
    while (entry) {
        if (entry->key == key) {
            // Update LRU timestamp on every hit
            entry->access_tick = cache_next_access_tick(cache);
            return &entry->glyph;
        }
        entry = entry->next;
    }

    return NULL;
}

//=============================================================================
// Cache Put
//=============================================================================

/// @brief Insert a rasterised glyph into the cache.
/// @details No-op when the entry already exists. Enforces the per-glyph
///          and per-cache memory caps: oversize bitmaps are rejected;
///          when adding @p glyph would push total memory over
///          `VG_CACHE_MAX_MEMORY`, the LRU evictor runs first. Resizes
///          the bucket array when the load factor exceeds 0.75. The
///          bitmap is copied into a freshly-allocated buffer so the
///          caller's @p glyph buffer can be freed safely after the call.
/// @param cache     Destination cache.
/// @param size      Font size in points.
/// @param codepoint Unicode code point.
/// @param glyph     Rasterised glyph to cache (caller retains ownership of @p glyph itself).
void vg_cache_put(vg_glyph_cache_t *cache,
                  float size,
                  uint32_t codepoint,
                  const vg_glyph_t *glyph) {
    if (!cache || !glyph)
        return;
    if (cache->bucket_count == 0 || !cache->buckets)
        return;

    uint64_t key = make_cache_key(size, codepoint);

    // Check if already exists
    size_t idx = hash_key(key, cache->bucket_count);
    vg_cache_entry_t *entry = cache->buckets[idx];
    while (entry) {
        if (entry->key == key) {
            return; // Already cached
        }
        entry = entry->next;
    }

    // Check memory limit and evict if necessary
    size_t glyph_memory = 0;
    if (!glyph_bitmap_size(glyph, &glyph_memory))
        return;
    if (glyph_memory > VG_CACHE_MAX_MEMORY)
        return;
    /* A full-budget glyph can only fit by emptying every existing entry. Evict
     * one LRU batch for pressure relief, then skip caching it if hot entries
     * remain rather than replacing the whole cache with one bitmap. */
    if (glyph_memory == VG_CACHE_MAX_MEMORY && cache->memory_used > 0) {
        cache_evict_some(cache);
        if (cache->memory_used > 0)
            return;
    }
    while (cache->memory_used > VG_CACHE_MAX_MEMORY - glyph_memory && cache->entry_count > 0) {
        size_t before_entries = cache->entry_count;
        size_t before_memory = cache->memory_used;
        cache_evict_some(cache);
        if (cache->entry_count == before_entries && cache->memory_used == before_memory)
            return;
    }

    // Check load factor and resize if necessary
    size_t resize_threshold = cache->bucket_count - cache->bucket_count / 4;
    if (cache->entry_count >= resize_threshold) {
        cache_resize(cache);
        idx = hash_key(key, cache->bucket_count); // Recalculate after resize
    }

    // Create new entry
    entry = calloc(1, sizeof(vg_cache_entry_t));
    if (!entry)
        return;

    entry->key = key;
    entry->glyph = *glyph;

    // Copy bitmap if present
    if (glyph->bitmap && glyph->width > 0 && glyph->height > 0) {
        size_t bitmap_size = 0;
        if (!glyph_bitmap_size(glyph, &bitmap_size)) {
            free(entry);
            return;
        }
        entry->glyph.bitmap = malloc(bitmap_size);
        if (!entry->glyph.bitmap) {
            // Bitmap allocation failed — discard entry rather than caching a NULL bitmap
            free(entry);
            return;
        }
        memcpy(entry->glyph.bitmap, glyph->bitmap, bitmap_size);
        cache->memory_used += bitmap_size;
    }

    // Insert at head of bucket
    entry->access_tick = cache_next_access_tick(cache);
    entry->next = cache->buckets[idx];
    cache->buckets[idx] = entry;
    cache->entry_count++;
}
