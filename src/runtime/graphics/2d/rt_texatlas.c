//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_texatlas.c
// Purpose: 2D texture atlas implementation. Maps string names to rectangular
//          sub-regions of a backing Pixels buffer. Supports manual region
//          definition and grid-based auto-slicing for sprite sheets.
//
// Key invariants:
//   - Backing Pixels is retained on creation and released on finalize.
//   - Region lookup uses a fixed-size open-addressing table for O(1) average
//     lookup while keeping storage bounded.
//   - Region names longer than 31 bytes are rejected instead of truncated.
//   - At most 512 regions are stored. Grid loading uses complete cells in
//     row-major order, ignores partial right/bottom edges, and stops at the cap.
//   - Manual rectangles must be positive and wholly inside the backing Pixels.
//     Reusing a case-sensitive name replaces its rectangle in place.
//   - When graphics support is disabled, every entry point is a benign stub.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects. Each
//     atlas retains its backing Pixels until finalization.
//
// Links: src/runtime/graphics/2d/rt_texatlas.h (public API),
//        src/runtime/graphics/2d/rt_spritebatch.c (batch atlas drawing)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements fixed-capacity named texture atlases and batch draw adapters.
 *
 * @details The graphics implementation retains a Pixels backing image, stores
 *          bounded atlas rectangles, and resolves case-sensitive names through
 *          an embedded open-addressing table. It also supplies grid slicing,
 *          region queries, SpriteBatch delegation, and benign no-graphics
 *          stubs for the same runtime ABI.
 */

#include "rt_texatlas.h"

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_heap.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_spritebatch.h"
#include "rt_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

//=============================================================================
// Internal Types
//=============================================================================

/// @brief Maximum number of named regions stored by one atlas.
#define TEXATLAS_MAX_REGIONS 512
/// @brief Embedded region-name capacity including the NUL terminator.
#define TEXATLAS_NAME_LEN 32
/// @brief Number of slots in the open-addressed lookup table.
#define TEXATLAS_HASH_SIZE 1024
/// Private initialized-payload cookie for TextureAtlas objects.
#define RT_TEXATLAS_STATE_MAGIC UINT64_C(0x5A54455841544C32)

/// @brief Fixed-size named atlas rectangle.
typedef struct {
    char name[TEXATLAS_NAME_LEN]; ///< Owned NUL-terminated region name.
    uint32_t name_hash;           ///< Cached FNV-1a hash of the exact name bytes.
    uint8_t name_len;             ///< Exact name length, excluding the terminator.
    /// @brief Atlas-space left, top, width, and height coordinates.
    int64_t x, y, w, h;
} texatlas_region;

/// @brief Private runtime-managed atlas and open-addressing lookup state.
typedef struct {
    uint64_t state_magic;                          ///< Private initialized-payload cookie.
    void *pixels;                                  ///< Retained backing Pixels object.
    texatlas_region regions[TEXATLAS_MAX_REGIONS]; ///< Embedded region records.
    int32_t region_count;                     ///< Number of initialized entries in @ref regions.
    int32_t region_slots[TEXATLAS_HASH_SIZE]; ///< Region index plus one; zero is empty.
    int64_t pixel_width;                      ///< Immutable backing-image width.
    int64_t pixel_height;                     ///< Immutable backing-image height.
} texatlas_impl;

//=============================================================================
// Helpers
//=============================================================================

static uint32_t hash_name(const char *name, size_t name_len);

/// @brief Cast a generic atlas handle to the concrete `texatlas_impl` pointer.
/// @param atlas Candidate opaque TextureAtlas handle.
/// @return Validated implementation pointer, or `NULL` for a null, undersized,
///         or wrong-class object.
static texatlas_impl *get_impl(void *atlas) {
    if (!atlas || !rt_obj_is_instance(atlas, RT_TEXATLAS_CLASS_ID, sizeof(texatlas_impl)))
        return NULL;
    texatlas_impl *impl = (texatlas_impl *)atlas;
    rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(impl->pixels);
    if (impl->state_magic != RT_TEXATLAS_STATE_MAGIC || !pixels || impl->region_count < 0 ||
        impl->region_count > TEXATLAS_MAX_REGIONS || pixels->width != impl->pixel_width ||
        pixels->height != impl->pixel_height)
        return NULL;
    return impl;
}

/// @brief Validate one published atlas region before lookup or drawing.
static int8_t texatlas_region_is_valid(const texatlas_impl *impl, int32_t index) {
    if (!impl || index < 0 || index >= impl->region_count)
        return 0;
    const texatlas_region *region = &impl->regions[index];
    size_t name_len = region->name_len;
    if (name_len == 0 || name_len >= TEXATLAS_NAME_LEN || region->name[name_len] != '\0' ||
        memchr(region->name, '\0', name_len) != NULL ||
        hash_name(region->name, name_len) != region->name_hash || region->x < 0 || region->y < 0 ||
        region->w <= 0 || region->h <= 0 || region->x >= impl->pixel_width ||
        region->y >= impl->pixel_height || region->w > impl->pixel_width - region->x ||
        region->h > impl->pixel_height - region->y)
        return 0;
    return 1;
}

/// @brief FNV-1a 32-bit hash of a region name string.
/// @details FNV-1a (Fowler-Noll-Vo) is chosen for its good avalanche properties on
///   short strings and its simplicity. The offset basis (2166136261) and prime
///   (16777619) are the standard 32-bit FNV constants. Used to index the open-addressing
///   hash table `region_slots` with linear probing.
/// @param name Nonnull NUL-terminated byte string.
/// @return Deterministic 32-bit FNV-1a hash.
static uint32_t hash_name(const char *name, size_t name_len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < name_len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

/// @brief Validate a bounded nonempty NUL-free runtime atlas name.
static int8_t texatlas_runtime_name(void *name,
                                    const char **bytes_out,
                                    size_t *name_len_out,
                                    uint32_t *hash_out) {
    if (!name || !bytes_out || !name_len_out || !hash_out)
        return 0;
    int64_t raw_len = rt_str_len((rt_string)name);
    if (raw_len <= 0 || raw_len >= TEXATLAS_NAME_LEN)
        return 0;
    const char *bytes = rt_string_cstr((rt_string)name);
    size_t name_len = (size_t)raw_len;
    if (!bytes || memchr(bytes, '\0', name_len) != NULL)
        return 0;
    *bytes_out = bytes;
    *name_len_out = name_len;
    *hash_out = hash_name(bytes, name_len);
    return 1;
}

/// @brief Look up a region by name in the open-addressing hash table.
/// @details Linear-probes up to TEXATLAS_HASH_SIZE slots starting from `hash_name(name)
///   & (TEXATLAS_HASH_SIZE - 1)`. A slot value of 0 means empty (sentinel); a non-zero
///   entry is `region_index + 1` to distinguish "index 0" from "empty". Full-name
///   strcmp confirms the match to handle collisions. If the maximum probe count is
///   reached without finding a match or an empty slot, returns -1.
/// @param impl Atlas whose slot table should be searched.
/// @param name Nonempty case-sensitive region name.
/// @return Zero-based region index, or -1 if not found.
static int find_region(texatlas_impl *impl, const char *name, size_t name_len, uint32_t name_hash) {
    if (!impl || !name || name_len == 0 || name_len >= TEXATLAS_NAME_LEN)
        return -1;

    for (int probe = 0; probe < TEXATLAS_HASH_SIZE; ++probe) {
        int32_t entry =
            impl->region_slots[(name_hash + (uint32_t)probe) & (TEXATLAS_HASH_SIZE - 1)];
        if (entry == 0)
            return -1;
        int idx = entry - 1;
        if (!texatlas_region_is_valid(impl, idx))
            return -1;
        if (impl->regions[idx].name_hash == name_hash && impl->regions[idx].name_len == name_len &&
            memcmp(impl->regions[idx].name, name, name_len) == 0)
            return idx;
    }
    return -1;
}

/// @brief Insert a region (by its zero-based index) into the open-addressing hash table.
/// @details Probes from `hash_name(name) & (TEXATLAS_HASH_SIZE - 1)` using linear
///   probing, storing `idx + 1` (to distinguish "index 0" from "empty slot = 0").
///   If the same index is already in the slot (idempotent re-insert) it is accepted.
///   Traps if no empty slot is found, which only occurs when the table is completely
///   full — the number of regions is capped at TEXATLAS_MAX_REGIONS ≤ TEXATLAS_HASH_SIZE/2
///   so a well-formed atlas never exhausts the table.
/// @param impl Valid atlas implementation containing the named region.
/// @param idx Valid zero-based index to bind.
static int8_t bind_region_slot(texatlas_impl *impl, int idx) {
    if (!impl || idx < 0 || idx >= TEXATLAS_MAX_REGIONS)
        return 0;
    uint32_t hash = impl->regions[idx].name_hash;
    for (int probe = 0; probe < TEXATLAS_HASH_SIZE; ++probe) {
        int32_t *slot = &impl->region_slots[(hash + (uint32_t)probe) & (TEXATLAS_HASH_SIZE - 1)];
        if (*slot < 0 || *slot > impl->region_count)
            return 0;
        if (*slot == 0 || *slot == idx + 1) {
            *slot = idx + 1;
            return 1;
        }
    }

    rt_trap("TextureAtlas: lookup table exhausted");
    return 0;
}

//=============================================================================
// Lifecycle
//=============================================================================

/// @brief GC finalizer for a TextureAtlas — releases the retained Pixels reference.
/// @details Region metadata (name strings, coordinate arrays) are embedded in the
///   fixed-size `regions` array inside the struct and require no separate free. Only
///   the `pixels` object needs an explicit release because it was retained separately
///   in `rt_texatlas_new` to extend the pixel data's lifetime to match the atlas.
/// @param obj Candidate TextureAtlas supplied by the runtime finalizer.
static void texatlas_finalize(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_TEXATLAS_CLASS_ID, sizeof(texatlas_impl)))
        return;
    texatlas_impl *impl = (texatlas_impl *)obj;
    if (impl->state_magic != RT_TEXATLAS_STATE_MAGIC)
        return;
    impl->state_magic = 0;
    if (impl->pixels && rt_heap_is_payload(impl->pixels)) {
        rt_heap_release(impl->pixels);
    }
    impl->pixels = NULL;
}

/// @brief Construct an empty texture atlas backed by `pixels`. The Pixels object is retained;
/// regions are added later via `rt_texatlas_add` or by using `rt_texatlas_load_grid`. Traps if
/// `pixels` is NULL. Returns NULL on allocation failure.
/// @param pixels Valid Pixels object retained as the atlas image.
/// @return A caller-owned empty TextureAtlas, or `NULL` after an invalid-object
///         trap or allocation failure.
void *rt_texatlas_new(void *pixels) {
    if (!pixels) {
        rt_trap("TextureAtlas.New: null pixels");
        return NULL;
    }
    rt_pixels_impl *pixel_impl = rt_pixels_checked_impl_or_null(pixels);
    if (!pixel_impl) {
        rt_trap("TextureAtlas.New: invalid pixels");
        return NULL;
    }

    rt_heap_retain(pixels);
    texatlas_impl *impl =
        (texatlas_impl *)rt_obj_new_i64(RT_TEXATLAS_CLASS_ID, (int64_t)sizeof(texatlas_impl));
    if (!impl) {
        rt_heap_release(pixels);
        return NULL;
    }

    memset(impl, 0, sizeof(texatlas_impl));
    impl->state_magic = RT_TEXATLAS_STATE_MAGIC;
    impl->pixels = pixels;
    impl->pixel_width = pixel_impl->width;
    impl->pixel_height = pixel_impl->height;
    rt_obj_set_finalizer(impl, texatlas_finalize);
    return impl;
}

/// @brief Build an atlas from `pixels` by auto-slicing it into a uniform grid of `frame_w ×
/// frame_h` cells (left-to-right, top-to-bottom, named "0", "1", ... up to 512 regions). Traps
/// if `pixels` is NULL or either dimension is non-positive. Useful for sprite sheets.
/// @details Only complete cells are included; partial right and bottom edges
///          are ignored. Generation stops silently at the 512-region limit.
/// @param pixels Valid Pixels object retained by the returned atlas.
/// @param frame_w Positive grid-cell width.
/// @param frame_h Positive grid-cell height.
/// @return A caller-owned TextureAtlas, or `NULL` after an argument trap or
///         allocation failure.
void *rt_texatlas_load_grid(void *pixels, int64_t frame_w, int64_t frame_h) {
    if (!rt_pixels_checked_impl_or_null(pixels) || frame_w <= 0 || frame_h <= 0) {
        rt_trap("TextureAtlas.LoadGrid: invalid arguments");
        return NULL;
    }

    void *atlas = rt_texatlas_new(pixels);
    if (!atlas)
        return NULL;

    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return NULL;
    int64_t img_w = impl->pixel_width;
    int64_t img_h = impl->pixel_height;
    int64_t cols = img_w / frame_w;
    int64_t rows = img_h / frame_h;

    int idx = 0;
    for (int64_t row = 0; row < rows && idx < TEXATLAS_MAX_REGIONS; row++) {
        for (int64_t col = 0; col < cols && idx < TEXATLAS_MAX_REGIONS; col++) {
            texatlas_region *r = &impl->regions[idx];
            int written = snprintf(r->name, TEXATLAS_NAME_LEN, "%d", idx);
            if (written <= 0 || written >= TEXATLAS_NAME_LEN) {
                rt_heap_release(atlas);
                return NULL;
            }
            r->name_len = (uint8_t)written;
            r->name_hash = hash_name(r->name, r->name_len);
            r->x = col * frame_w;
            r->y = row * frame_h;
            r->w = frame_w;
            r->h = frame_h;
            if (!bind_region_slot(impl, idx)) {
                rt_heap_release(atlas);
                return NULL;
            }
            impl->region_count = idx + 1;
            idx++;
        }
    }
    return atlas;
}

//=============================================================================
// Region Management
//=============================================================================

/// @brief Add or replace a case-sensitive named atlas rectangle.
/// @details The name is copied into fixed embedded storage. Null handles are
///          ignored; empty/overlong names, invalid geometry, and exceeding 512
///          regions trap. Replacement preserves the existing table slot/index.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime string of at most 31 bytes.
/// @param x Nonnegative atlas-space left edge.
/// @param y Nonnegative atlas-space top edge.
/// @param w Positive region width wholly inside the backing Pixels.
/// @param h Positive region height wholly inside the backing Pixels.
void rt_texatlas_add(void *atlas, void *name, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (!atlas || !name)
        return;

    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash)) {
        rt_trap("TextureAtlas.Add: name must be 1..31 bytes without embedded NUL");
        return;
    }
    if (w <= 0 || h <= 0) {
        rt_trap("TextureAtlas.Add: width/height must be positive");
        return;
    }
    int64_t atlas_w = impl->pixel_width;
    int64_t atlas_h = impl->pixel_height;
    if (x < 0 || y < 0 || x > atlas_w || y > atlas_h || w > atlas_w - x || h > atlas_h - y) {
        rt_trap("TextureAtlas.Add: region outside backing pixels");
        return;
    }

    // Overwrite if name already exists
    int existing = find_region(impl, cname, name_len, name_hash);
    if (existing < 0 && impl->region_count >= TEXATLAS_MAX_REGIONS) {
        rt_trap("TextureAtlas.Add: region limit exceeded (512)");
        return;
    }
    texatlas_region *r;
    if (existing >= 0) {
        r = &impl->regions[existing];
    } else {
        r = &impl->regions[impl->region_count];
    }

    memcpy(r->name, cname, name_len);
    r->name[name_len] = '\0';
    r->name_len = (uint8_t)name_len;
    r->name_hash = name_hash;
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    if (existing < 0) {
        int new_index = impl->region_count;
        if (!bind_region_slot(impl, new_index)) {
            memset(r, 0, sizeof(*r));
            return;
        }
        impl->region_count++;
    }
}

/// @brief Test whether a case-sensitive region name exists.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime string to look up.
/// @return `1` when found, otherwise `0`.
int8_t rt_texatlas_has(void *atlas, void *name) {
    if (!atlas || !name)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return 0;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return 0;
    return find_region(impl, cname, name_len, name_hash) >= 0 ? 1 : 0;
}

/// @brief Get a named region's source X coordinate.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @return Region X, or `0` for invalid input or an absent name.
int64_t rt_texatlas_get_x(void *atlas, void *name) {
    if (!atlas || !name)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return 0;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return 0;
    int idx = find_region(impl, cname, name_len, name_hash);
    return idx >= 0 ? impl->regions[idx].x : 0;
}

/// @brief Get a named region's source Y coordinate.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @return Region Y, or `0` for invalid input or an absent name.
int64_t rt_texatlas_get_y(void *atlas, void *name) {
    if (!atlas || !name)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return 0;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return 0;
    int idx = find_region(impl, cname, name_len, name_hash);
    return idx >= 0 ? impl->regions[idx].y : 0;
}

/// @brief Get a named region's source width.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @return Region width, or `0` for invalid input or an absent name.
int64_t rt_texatlas_get_w(void *atlas, void *name) {
    if (!atlas || !name)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return 0;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return 0;
    int idx = find_region(impl, cname, name_len, name_hash);
    return idx >= 0 ? impl->regions[idx].w : 0;
}

/// @brief Get a named region's source height.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @return Region height, or `0` for invalid input or an absent name.
int64_t rt_texatlas_get_h(void *atlas, void *name) {
    if (!atlas || !name)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return 0;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return 0;
    int idx = find_region(impl, cname, name_len, name_hash);
    return idx >= 0 ? impl->regions[idx].h : 0;
}

/// @brief Return the backing Pixels object (still owned by the atlas — do not release).
/// @param atlas Candidate TextureAtlas handle.
/// @return Borrowed Pixels pointer, or `NULL` for an invalid atlas.
void *rt_texatlas_get_pixels(void *atlas) {
    if (!atlas)
        return NULL;
    texatlas_impl *impl = get_impl(atlas);
    return impl ? impl->pixels : NULL;
}

/// @brief Get the number of registered regions.
/// @param atlas Candidate TextureAtlas handle.
/// @return Region count, or `0` for an invalid atlas.
int64_t rt_texatlas_region_count(void *atlas) {
    if (!atlas)
        return 0;
    texatlas_impl *impl = get_impl(atlas);
    return impl ? impl->region_count : 0;
}

//=============================================================================
// SpriteBatch Atlas Drawing Extensions
//=============================================================================

// These are thin wrappers that resolve the named region and delegate to
// the existing rt_spritebatch_draw_region function.

/// @brief Look up region `name` in `atlas` and queue a sprite-batch draw at (x, y) at native
/// size. Silent no-op if the named region doesn't exist (no trap — typical sprite-sheet usage).
/// @param batch Active SpriteBatch that will retain the backing Pixels.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
void rt_spritebatch_draw_atlas(void *batch, void *atlas, void *name, int64_t x, int64_t y) {
    if (!batch || !atlas || !name)
        return;

    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return;
    int idx = find_region(impl, cname, name_len, name_hash);
    if (idx < 0)
        return; // Silent no-op for missing region

    texatlas_region *r = &impl->regions[idx];
    rt_spritebatch_draw_region(batch, impl->pixels, x, y, r->x, r->y, r->w, r->h);
}

/// @brief Look up a named region and queue it with uniform percentage scale.
/// @details Uses zero rotation and depth. Null inputs or missing names are silent
///          no-ops.
/// @param batch Active SpriteBatch that will retain the backing Pixels.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
/// @param scale Horizontal and vertical percentage scale.
void rt_spritebatch_draw_atlas_scaled(
    void *batch, void *atlas, void *name, int64_t x, int64_t y, int64_t scale) {
    if (!batch || !atlas || !name)
        return;

    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return;
    int idx = find_region(impl, cname, name_len, name_hash);
    if (idx < 0)
        return;

    texatlas_region *r = &impl->regions[idx];
    rt_spritebatch_draw_region_ex(
        batch, impl->pixels, x, y, r->x, r->y, r->w, r->h, scale, scale, 0, 0);
}

/// @brief Full sprite-batch atlas draw: scale + rotation (degrees) + depth-sort layer. Silent
/// no-op for missing regions or null inputs.
/// @param batch Active SpriteBatch that will retain the backing Pixels.
/// @param atlas Candidate TextureAtlas handle.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X coordinate before rotation recentering.
/// @param y Destination top-left Y coordinate before rotation recentering.
/// @param scale Horizontal and vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
/// @param depth Explicit SpriteBatch sort key.
void rt_spritebatch_draw_atlas_ex(void *batch,
                                  void *atlas,
                                  void *name,
                                  int64_t x,
                                  int64_t y,
                                  int64_t scale,
                                  int64_t rotation,
                                  int64_t depth) {
    if (!batch || !atlas || !name)
        return;

    texatlas_impl *impl = get_impl(atlas);
    if (!impl)
        return;
    const char *cname = NULL;
    size_t name_len = 0;
    uint32_t name_hash = 0;
    if (!texatlas_runtime_name(name, &cname, &name_len, &name_hash))
        return;
    int idx = find_region(impl, cname, name_len, name_hash);
    if (idx < 0)
        return;

    texatlas_region *r = &impl->regions[idx];
    rt_spritebatch_draw_region_ex(
        batch, impl->pixels, x, y, r->x, r->y, r->w, r->h, scale, scale, rotation, depth);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

// Stubs when graphics is disabled — every public entry-point returns a benign zero/no-op so
// callers compile and link, but no pixels are produced. See the `ZANNA_ENABLE_GRAPHICS` block
// above for behavioral docs.

/// @brief Stub — graphics disabled at build time. Returns 0.
/// @param pixels Ignored backing Pixels argument.
/// @return Always `NULL`.
void *rt_texatlas_new(void *pixels) {
    (void)pixels;
    return 0;
}

/// @brief Stub — graphics disabled at build time. Returns 0.
/// @param p Ignored Pixels argument.
/// @param w Ignored cell width.
/// @param h Ignored cell height.
/// @return Always `NULL`.
void *rt_texatlas_load_grid(void *p, int64_t w, int64_t h) {
    (void)p;
    (void)w;
    (void)h;
    return 0;
}

/// @brief Stub that ignores a region-add request when graphics is disabled.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
/// @param w Ignored width.
/// @param h Ignored height.
void rt_texatlas_add(void *a, void *n, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)a;
    (void)n;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

/// @brief Stub region-existence query.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @return Always `0`.
int8_t rt_texatlas_has(void *a, void *n) {
    (void)a;
    (void)n;
    return 0;
}

/// @brief Stub region-X query.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @return Always `0`.
int64_t rt_texatlas_get_x(void *a, void *n) {
    (void)a;
    (void)n;
    return 0;
}

/// @brief Stub region-Y query.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @return Always `0`.
int64_t rt_texatlas_get_y(void *a, void *n) {
    (void)a;
    (void)n;
    return 0;
}

/// @brief Stub region-width query.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @return Always `0`.
int64_t rt_texatlas_get_w(void *a, void *n) {
    (void)a;
    (void)n;
    return 0;
}

/// @brief Stub region-height query.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @return Always `0`.
int64_t rt_texatlas_get_h(void *a, void *n) {
    (void)a;
    (void)n;
    return 0;
}

/// @brief Stub — graphics disabled at build time. Returns 0.
/// @param a Ignored atlas.
/// @return Always `NULL`.
void *rt_texatlas_get_pixels(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub region-count query.
/// @param a Ignored atlas.
/// @return Always `0`.
int64_t rt_texatlas_region_count(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub atlas draw when graphics is disabled.
/// @param b Ignored batch.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
void rt_spritebatch_draw_atlas(void *b, void *a, void *n, int64_t x, int64_t y) {
    (void)b;
    (void)a;
    (void)n;
    (void)x;
    (void)y;
}

/// @brief Stub scaled atlas draw when graphics is disabled.
/// @param b Ignored batch.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
/// @param s Ignored scale.
void rt_spritebatch_draw_atlas_scaled(void *b, void *a, void *n, int64_t x, int64_t y, int64_t s) {
    (void)b;
    (void)a;
    (void)n;
    (void)x;
    (void)y;
    (void)s;
}

/// @brief Stub transformed atlas draw when graphics is disabled.
/// @param b Ignored batch.
/// @param a Ignored atlas.
/// @param n Ignored name.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
/// @param s Ignored scale.
/// @param r Ignored rotation.
/// @param d Ignored depth.
void rt_spritebatch_draw_atlas_ex(
    void *b, void *a, void *n, int64_t x, int64_t y, int64_t s, int64_t r, int64_t d) {
    (void)b;
    (void)a;
    (void)n;
    (void)x;
    (void)y;
    (void)s;
    (void)r;
    (void)d;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
