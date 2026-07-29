//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_spritesheet.c
// Purpose: Implements a named-region SpriteSheet over one retained Pixels
//   atlas. Regions may be defined individually or generated from an exactly
//   divisible uniform grid, enumerated in insertion order, queried by name,
//   removed, and extracted as independent Pixels copies.
//
// Key invariants:
//   - A grid sheet requires atlas dimensions to be exact multiples of cell
//     dimensions. Numeric names are assigned in row-major order:
//       col = N % cols,  row = N / cols
//       src_x = col * frame_w,  src_y = row * frame_h
//   - Names and rectangles occupy parallel dynamic arrays; lookup is a
//     case-sensitive linear scan. Reusing a name replaces its rectangle without
//     changing insertion order.
//   - Empty names and rectangles outside the atlas are silently rejected.
//   - The atlas Pixels buffer is retained by the sheet and released on destroy.
//     Extracted frame Pixels objects are independent copies; they do not hold
//     a reference back to the atlas.
//   - A missing/unknown region name returns NULL rather than trapping, so callers
//     can detect a misspelled frame ID without crashing.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - The finalizer releases the retained atlas and frees copied C names and
//     both parallel metadata arrays.
//
// Links: src/runtime/graphics/2d/rt_spritesheet.h (public API),
//        src/runtime/graphics/2d/rt_sprite.h (consumer of atlas frames),
//        docs/zannalib/game.md (SpriteSheet section)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements named rectangular regions over a retained Pixels atlas.
 *
 * @details SpriteSheet supports explicit region insertion and replacement,
 *          exact uniform-grid generation, case-sensitive lookup, enumeration,
 *          removal, and extraction into independent Pixels objects. Parallel
 *          owned name and rectangle arrays preserve insertion order.
 */

#include "rt_spritesheet.h"

#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Stored atlas-space rectangle for one named SpriteSheet region.
typedef struct {
    /// @brief Atlas-space left, top, width, and height coordinates.
    int64_t x, y, w, h;
    size_t name_len;    ///< Exact byte length of the parallel copied name.
    uint64_t name_hash; ///< Cached FNV-1a hash for inexpensive lookup rejection.
} ss_region;

/// @brief Private runtime-managed SpriteSheet state.
/// @details `regions[i]` and `names[i]` describe the same entry for every
///          index below @c count. The atlas reference and every name are owned.
typedef struct {
    void *vptr;           ///< Reserved runtime virtual-table slot.
    void *atlas;          ///< Retained Pixels atlas.
    ss_region *regions;   ///< Owned region array parallel to @ref names.
    char **names;         ///< Owned copied names parallel to @ref regions.
    int64_t count;        ///< Number of initialized region/name entries.
    int64_t capacity;     ///< Number of allocated parallel-array slots.
    int64_t atlas_width;  ///< Immutable retained-atlas width.
    int64_t atlas_height; ///< Immutable retained-atlas height.
} rt_spritesheet_impl;

/// @brief Initial number of named-region slots allocated for a sheet.
#define SS_INITIAL_CAP 16

/// @brief Validate-and-return a SpriteSheet pointer; NULL for NULL or wrong class.
/// @details Soft check (no trap) — used by every public SpriteSheet entry
///          and the GC finalizer so wrong-class handles silently no-op.
/// @param obj Candidate opaque SpriteSheet handle.
/// @return Validated implementation pointer, or `NULL` for null, undersized, or
///         wrong-class objects.
static rt_spritesheet_impl *spritesheet_checked_or_null(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_SPRITESHEET_CLASS_ID, sizeof(rt_spritesheet_impl)))
        return NULL;
    return (rt_spritesheet_impl *)obj;
}

/// @brief Test whether @p pixels is a non-NULL Pixels handle (correct class id).
/// @details Used during atlas swap-in / region extraction to reject foreign
///          handles before they reach rt_pixels_get / rt_pixels_blit.
/// @param pixels Candidate opaque Pixels handle.
/// @return Nonzero when @p pixels has a valid Pixels class and payload size.
static rt_pixels_impl *spritesheet_valid_pixels(void *pixels) {
    return rt_pixels_checked_impl_or_null(pixels);
}

/// @brief Release one runtime-managed object reference.
static void spritesheet_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Compute a stable byte-wise FNV-1a hash for a region name.
static uint64_t spritesheet_name_hash(const char *bytes, size_t len) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/// @brief Validate a nonempty NUL-free runtime region name.
/// @details Runtime strings carry an explicit byte count and may contain NUL,
///          so C-string scans alone would alias distinct names. Length is also
///          checked against the host allocation range before conversion.
static int8_t spritesheet_runtime_name(rt_string name,
                                       const char **bytes_out,
                                       size_t *len_out,
                                       uint64_t *hash_out) {
    if (!name || !bytes_out || !len_out || !hash_out)
        return 0;
    int64_t raw_len = rt_str_len(name);
    if (raw_len <= 0 || (uint64_t)raw_len > (uint64_t)SIZE_MAX - 1u)
        return 0;
    const char *bytes = rt_string_cstr(name);
    size_t len = (size_t)raw_len;
    if (!bytes || memchr(bytes, '\0', len) != NULL)
        return 0;
    *bytes_out = bytes;
    *len_out = len;
    *hash_out = spritesheet_name_hash(bytes, len);
    return 1;
}

/// @brief GC finalizer for a SpriteSheet — frees the name strings, region/name arrays,
///        and releases the retained atlas Pixels reference.
/// @details Name strings are heap-allocated via `strdup` and must be freed one by one.
///   The `regions` and `names` pointer arrays are plain `malloc` blocks freed with
///   `free()`. The atlas Pixels is GC-managed so it gets a proper `release_check0`/
///   `free` pair rather than a raw `free`.
/// @param obj Candidate SpriteSheet supplied by the runtime finalizer.
static void ss_finalizer(void *obj) {
    rt_spritesheet_impl *ss = spritesheet_checked_or_null(obj);
    if (ss) {
        int64_t i;
        for (i = 0; i < ss->count; i++) {
            free(ss->names[i]);
        }
        free(ss->regions);
        free(ss->names);
        ss->regions = NULL;
        ss->names = NULL;
        ss->count = 0;
        if (ss->atlas) {
            if (rt_obj_release_check0(ss->atlas))
                rt_obj_free(ss->atlas);
            ss->atlas = NULL;
        }
    }
}

/// @brief Linear scan for a region by name; returns its index or -1 if not found.
/// @details The sheet is not expected to have thousands of regions so a linear scan is
///   acceptable. If performance becomes a concern a hash map could replace this, but
///   the allocation complexity of the current sequential two-array layout favors the
///   simple approach.
/// @param ss Valid SpriteSheet implementation.
/// @param name NUL-terminated case-sensitive name to find.
/// @return Zero-based region index, or `-1` when absent.
static int64_t find_region(rt_spritesheet_impl *ss,
                           const char *name,
                           size_t name_len,
                           uint64_t name_hash) {
    if (!ss || !name)
        return -1;
    for (int64_t i = 0; i < ss->count; i++) {
        if (ss->regions[i].name_hash == name_hash && ss->regions[i].name_len == name_len &&
            memcmp(ss->names[i], name, name_len) == 0)
            return i;
    }
    return -1;
}

/// @brief Validate that a proposed region rectangle lies entirely within the atlas bounds.
/// @details Checks x/y/w/h are positive, that the origin is inside the atlas, and that
///   the region does not exceed the right/bottom edge. The arithmetic `w > atlas_w - x`
///   is safe because x < atlas_w guarantees atlas_w - x > 0. Callers can use this to
///   reject out-of-bounds regions before they cause buffer overreads during `GetRegion`.
/// @param ss SpriteSheet providing the retained atlas dimensions.
/// @param x Proposed nonnegative left edge.
/// @param y Proposed nonnegative top edge.
/// @param w Proposed positive width.
/// @param h Proposed positive height.
/// @return 1 if the region is within bounds, 0 otherwise.
static int8_t spritesheet_region_valid(
    rt_spritesheet_impl *ss, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (!ss || !ss->atlas || x < 0 || y < 0 || w <= 0 || h <= 0)
        return 0;
    int64_t atlas_w = ss->atlas_width;
    int64_t atlas_h = ss->atlas_height;
    if (atlas_w <= 0 || atlas_h <= 0 || x >= atlas_w || y >= atlas_h)
        return 0;
    if (w > atlas_w - x || h > atlas_h - y)
        return 0;
    return 1;
}

/// @brief Double the regions/names arrays when the sheet is full.
/// @details Deliberately uses malloc+memcpy rather than realloc so both arrays can be
///   freed independently on partial failure — if the second `malloc` fails the first
///   temp buffer is freed before trapping, leaving the existing arrays intact. The
///   two-array parallel structure (regions and names) cannot be grown by a single
///   realloc call, so this approach avoids the asymmetric state that would result from
///   one realloc succeeding and the other failing.
/// @param ss SpriteSheet whose parallel arrays may need to grow.
/// @return 1 if capacity is sufficient (or was successfully grown), 0 on overflow/OOM.
static int8_t spritesheet_reserve(rt_spritesheet_impl *ss, int64_t required) {
    if (!ss || !ss->regions || !ss->names || required < 0 || ss->count < 0 || ss->capacity <= 0 ||
        ss->count > ss->capacity)
        return 0;
    if (required <= ss->capacity)
        return 1;

    int64_t new_cap = ss->capacity;
    while (new_cap < required) {
        if (new_cap > INT64_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }
    if ((uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(ss_region) ||
        (uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(char *)) {
        rt_trap("SpriteSheet: capacity overflow");
        return 0;
    }

    ss_region *tmp_regions = (ss_region *)malloc((size_t)new_cap * sizeof(ss_region));
    if (!tmp_regions) {
        rt_trap("SpriteSheet: memory allocation failed");
        return 0;
    }
    char **tmp_names = (char **)malloc((size_t)new_cap * sizeof(char *));
    if (!tmp_names) {
        free(tmp_regions);
        rt_trap("SpriteSheet: memory allocation failed");
        return 0;
    }
    memcpy(tmp_regions, ss->regions, (size_t)ss->count * sizeof(ss_region));
    memcpy(tmp_names, ss->names, (size_t)ss->count * sizeof(char *));
    free(ss->regions);
    free(ss->names);
    ss->regions = tmp_regions;
    ss->names = tmp_names;
    ss->capacity = new_cap;
    return 1;
}

/// @brief Ensure space for one more metadata entry.
static int8_t ensure_cap(rt_spritesheet_impl *ss) {
    if (!ss || ss->count == INT64_MAX) {
        rt_trap("SpriteSheet: capacity overflow");
        return 0;
    }
    return spritesheet_reserve(ss, ss->count + 1);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct a SpriteSheet that wraps an existing Pixels atlas. The atlas is retained;
/// regions are added later via `_set_region` or `_from_grid`. Returns NULL if `atlas_pixels`
/// is NULL or on allocation failure (which also traps).
/// @param atlas_pixels Valid Pixels object retained as the complete atlas.
/// @return A caller-owned empty SpriteSheet, or `NULL` for an invalid Pixels
///         handle or allocation failure.
void *rt_spritesheet_new(void *atlas_pixels) {
    rt_spritesheet_impl *ss;
    rt_pixels_impl *atlas = spritesheet_valid_pixels(atlas_pixels);
    if (!atlas)
        return NULL;

    ss = (rt_spritesheet_impl *)rt_obj_new_i64(RT_SPRITESHEET_CLASS_ID,
                                               (int64_t)sizeof(rt_spritesheet_impl));
    if (!ss) {
        rt_trap("SpriteSheet: memory allocation failed");
        return NULL;
    }
    ss->vptr = NULL;
    ss->atlas = atlas_pixels;
    rt_obj_retain_maybe(atlas_pixels);
    ss->count = 0;
    ss->capacity = SS_INITIAL_CAP;
    ss->atlas_width = atlas->width;
    ss->atlas_height = atlas->height;
    ss->regions = (ss_region *)calloc((size_t)SS_INITIAL_CAP, sizeof(ss_region));
    ss->names = (char **)calloc((size_t)SS_INITIAL_CAP, sizeof(char *));
    if (!ss->regions || !ss->names) {
        free(ss->regions);
        free(ss->names);
        ss->regions = NULL;
        ss->names = NULL;
        if (ss->atlas) {
            if (rt_obj_release_check0(ss->atlas))
                rt_obj_free(ss->atlas);
            ss->atlas = NULL;
        }
        if (rt_obj_release_check0(ss))
            rt_obj_free(ss);
        rt_trap("SpriteSheet: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(ss, ss_finalizer);
    return ss;
}

/// @brief Build a SpriteSheet from an atlas auto-sliced into a uniform `frame_w × frame_h` grid.
/// Frames are named "0", "1", ... in row-major order (left-to-right, top-to-bottom). Useful for
/// engine-friendly sprite sheets where each frame is the same size.
/// @details Both atlas dimensions must be exact multiples of their cell
///          dimensions; partial edge cells are not created.
/// @param atlas_pixels Valid Pixels object retained by the returned sheet.
/// @param frame_w Positive cell width in pixels.
/// @param frame_h Positive cell height in pixels.
/// @return A caller-owned SpriteSheet, or `NULL` for invalid input, a
///         non-divisible grid, arithmetic overflow, or initial sheet allocation
///         failure. A later region-allocation failure can leave a partial sheet.
void *rt_spritesheet_from_grid(void *atlas_pixels, int64_t frame_w, int64_t frame_h) {
    void *sheet;
    int64_t atlas_w, atlas_h, cols, rows, idx, iy, ix;
    rt_pixels_impl *atlas = spritesheet_valid_pixels(atlas_pixels);
    if (!atlas || frame_w <= 0 || frame_h <= 0)
        return NULL;

    atlas_w = atlas->width;
    atlas_h = atlas->height;
    if (atlas_w <= 0 || atlas_h <= 0 || atlas_w % frame_w != 0 || atlas_h % frame_h != 0)
        return NULL;

    cols = atlas_w / frame_w;
    rows = atlas_h / frame_h;
    if (cols <= 0 || rows <= 0 || rows > INT64_MAX / cols)
        return NULL;

    sheet = rt_spritesheet_new(atlas_pixels);
    if (!sheet)
        return NULL;
    int64_t region_total = rows * cols;
    if (!spritesheet_reserve((rt_spritesheet_impl *)sheet, region_total)) {
        spritesheet_release_object(sheet);
        return NULL;
    }

    idx = 0;
    for (iy = 0; iy < rows; iy++) {
        for (ix = 0; ix < cols; ix++) {
            char name_buf[32];
            int written = snprintf(name_buf, sizeof(name_buf), "%lld", (long long)idx);
            if (written <= 0 || (size_t)written >= sizeof(name_buf)) {
                spritesheet_release_object(sheet);
                return NULL;
            }
            rt_string name = rt_string_from_bytes(name_buf, (size_t)written);
            if (!name) {
                spritesheet_release_object(sheet);
                return NULL;
            }
            rt_spritesheet_set_region(sheet, name, ix * frame_w, iy * frame_h, frame_w, frame_h);
            rt_string_unref(name);
            if (rt_spritesheet_region_count(sheet) != idx + 1) {
                spritesheet_release_object(sheet);
                return NULL;
            }
            idx++;
        }
    }
    return sheet;
}

/// @brief Define or update a named region (sub-rectangle) within the atlas. If a region with
/// `name` already exists, its bounds are overwritten in place; otherwise a new entry is appended,
/// growing the internal arrays as needed. The name string is copied internally.
/// @details Null/invalid sheets or strings, empty names, and nonpositive or
///          out-of-bounds rectangles are silently ignored.
/// @param obj Candidate SpriteSheet handle.
/// @param name Borrowed runtime string containing the case-sensitive name.
/// @param x Nonnegative atlas-space left edge.
/// @param y Nonnegative atlas-space top edge.
/// @param w Positive region width.
/// @param h Positive region height.
void rt_spritesheet_set_region(
    void *obj, rt_string name, int64_t x, int64_t y, int64_t w, int64_t h) {
    rt_spritesheet_impl *ss;
    const char *cstr;
    size_t name_len = 0;
    uint64_t name_hash = 0;
    int64_t idx;
    if (!obj || !name)
        return;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return;
    if (!spritesheet_runtime_name(name, &cstr, &name_len, &name_hash))
        return;
    if (!spritesheet_region_valid(ss, x, y, w, h))
        return;

    /* Update existing region if name matches */
    idx = find_region(ss, cstr, name_len, name_hash);
    if (idx >= 0) {
        ss->regions[idx].x = x;
        ss->regions[idx].y = y;
        ss->regions[idx].w = w;
        ss->regions[idx].h = h;
        return;
    }

    /* Add new region */
    if (!ensure_cap(ss))
        return;
    {
        ss->names[ss->count] = (char *)malloc(name_len + 1);
        if (!ss->names[ss->count]) {
            rt_trap("SpriteSheet: memory allocation failed");
            return;
        }
        memcpy(ss->names[ss->count], cstr, name_len);
        ss->names[ss->count][name_len] = '\0';
        ss->regions[ss->count].x = x;
        ss->regions[ss->count].y = y;
        ss->regions[ss->count].w = w;
        ss->regions[ss->count].h = h;
        ss->regions[ss->count].name_len = name_len;
        ss->regions[ss->count].name_hash = name_hash;
        ss->count++;
    }
}

/// @brief Extract the named region as a freshly allocated Pixels copy. Returns NULL if the
/// region doesn't exist or on allocation failure. The returned Pixels is independent of the
/// atlas — modifying it has no effect on the source sheet.
/// @param obj Candidate SpriteSheet handle.
/// @param name Borrowed runtime string containing the case-sensitive name.
/// @return A caller-owned Pixels copy, or `NULL` for invalid input, a missing
///         region, or allocation failure.
void *rt_spritesheet_get_region(void *obj, rt_string name) {
    rt_spritesheet_impl *ss;
    const char *cstr;
    size_t name_len = 0;
    uint64_t name_hash = 0;
    int64_t idx;
    ss_region *r;
    void *dst;
    if (!obj || !name)
        return NULL;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return NULL;
    if (!spritesheet_runtime_name(name, &cstr, &name_len, &name_hash))
        return NULL;

    idx = find_region(ss, cstr, name_len, name_hash);
    if (idx < 0)
        return NULL;

    r = &ss->regions[idx];
    dst = rt_pixels_new(r->w, r->h);
    if (!dst)
        return NULL;

    rt_pixels_copy(dst, 0, 0, ss->atlas, r->x, r->y, r->w, r->h);
    return dst;
}

/// @brief Test whether a case-sensitive region name exists.
/// @param obj Candidate SpriteSheet handle.
/// @param name Borrowed runtime string to look up.
/// @return `1` when present, otherwise `0`.
int8_t rt_spritesheet_has_region(void *obj, rt_string name) {
    rt_spritesheet_impl *ss;
    const char *cstr;
    size_t name_len = 0;
    uint64_t name_hash = 0;
    if (!obj || !name)
        return 0;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    if (!spritesheet_runtime_name(name, &cstr, &name_len, &name_hash))
        return 0;
    return find_region(ss, cstr, name_len, name_hash) >= 0 ? 1 : 0;
}

/// @brief Get the number of defined named regions.
/// @param obj Candidate SpriteSheet handle.
/// @return Region count, or `0` for an invalid sheet.
int64_t rt_spritesheet_region_count(void *obj) {
    rt_spritesheet_impl *ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    return ss->count;
}

/// @brief Get the retained atlas width.
/// @param obj Candidate SpriteSheet handle.
/// @return Atlas width in pixels, or `0` for an invalid sheet.
int64_t rt_spritesheet_width(void *obj) {
    rt_spritesheet_impl *ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    return ss->atlas_width;
}

/// @brief Get the retained atlas height.
/// @param obj Candidate SpriteSheet handle.
/// @return Atlas height in pixels, or `0` for an invalid sheet.
int64_t rt_spritesheet_height(void *obj) {
    rt_spritesheet_impl *ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    return ss->atlas_height;
}

/// @brief Return a Seq of all defined region names (as rt_strings) in insertion order. Empty
/// Seq for a NULL handle. Useful for enumerating the sheet's frames in editor/debug tools.
/// @details Each internal C name is converted to a runtime string and pushed
///          into a newly owned sequence. The sheet remains unchanged.
/// @param obj Candidate SpriteSheet handle.
/// @return A caller-owned Seq of runtime strings; invalid handles produce a
///         newly allocated empty Seq.
void *rt_spritesheet_region_names(void *obj) {
    rt_spritesheet_impl *ss;
    void *seq;
    int64_t i;
    if (!obj)
        return rt_seq_new_owned();
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return rt_seq_new_owned();
    seq = rt_seq_new_owned();
    if (!seq)
        return NULL;
    for (i = 0; i < ss->count; i++) {
        rt_string s = rt_string_from_bytes(ss->names[i], ss->regions[i].name_len);
        if (!s) {
            spritesheet_release_object(seq);
            return NULL;
        }
        rt_seq_push(seq, (void *)s);
        rt_str_release_maybe(s);
        if (rt_seq_len(seq) != i + 1) {
            spritesheet_release_object(seq);
            return NULL;
        }
    }
    return seq;
}

/// @brief Remove a case-sensitive named region.
/// @details Frees the copied name and shifts later entries left, preserving
///          insertion order among the survivors.
/// @param obj Candidate SpriteSheet handle.
/// @param name Borrowed runtime string to remove.
/// @return `1` when an entry is removed, otherwise `0`.
int8_t rt_spritesheet_remove_region(void *obj, rt_string name) {
    rt_spritesheet_impl *ss;
    const char *cstr;
    size_t name_len = 0;
    uint64_t name_hash = 0;
    int64_t idx, j;
    if (!obj || !name)
        return 0;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    if (!spritesheet_runtime_name(name, &cstr, &name_len, &name_hash))
        return 0;

    idx = find_region(ss, cstr, name_len, name_hash);
    if (idx < 0)
        return 0;

    free(ss->names[idx]);
    for (j = idx; j < ss->count - 1; j++) {
        ss->names[j] = ss->names[j + 1];
        ss->regions[j] = ss->regions[j + 1];
    }
    ss->count--;
    ss->names[ss->count] = NULL;
    memset(&ss->regions[ss->count], 0, sizeof(ss->regions[ss->count]));
    return 1;
}
