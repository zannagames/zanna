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
    int64_t x, y, w, h;
} ss_region;

/// @brief Private runtime-managed SpriteSheet state.
/// @details `regions[i]` and `names[i]` describe the same entry for every
///          index below @c count. The atlas reference and every name are owned.
typedef struct {
    void *vptr;
    void *atlas;
    ss_region *regions;
    char **names;
    int64_t count;
    int64_t capacity;
} rt_spritesheet_impl;

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
static int8_t spritesheet_is_valid_pixels(void *pixels) {
    return pixels && rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl));
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
static int64_t find_region(rt_spritesheet_impl *ss, const char *name) {
    int64_t i;
    for (i = 0; i < ss->count; i++) {
        if (strcmp(ss->names[i], name) == 0)
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
    int64_t atlas_w = rt_pixels_width(ss->atlas);
    int64_t atlas_h = rt_pixels_height(ss->atlas);
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
static int8_t ensure_cap(rt_spritesheet_impl *ss) {
    if (ss->count < ss->capacity)
        return 1;
    if (ss->capacity > INT64_MAX / 2) {
        rt_trap("SpriteSheet: capacity overflow");
        return 0;
    }
    int64_t new_cap = ss->capacity * 2;

    // Use malloc+memcpy so both arrays can be freed independently on partial failure.
    // realloc cannot be rolled back after success, so sequential reallocs risk leaving
    // the struct with mismatched array sizes if the second call fails.
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
    if (!spritesheet_is_valid_pixels(atlas_pixels))
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
    if (!spritesheet_is_valid_pixels(atlas_pixels) || frame_w <= 0 || frame_h <= 0)
        return NULL;

    atlas_w = rt_pixels_width(atlas_pixels);
    atlas_h = rt_pixels_height(atlas_pixels);
    if (atlas_w <= 0 || atlas_h <= 0 || atlas_w % frame_w != 0 || atlas_h % frame_h != 0)
        return NULL;

    cols = atlas_w / frame_w;
    rows = atlas_h / frame_h;
    if (cols <= 0 || rows <= 0 || rows > INT64_MAX / cols)
        return NULL;

    sheet = rt_spritesheet_new(atlas_pixels);
    if (!sheet)
        return NULL;

    idx = 0;
    for (iy = 0; iy < rows; iy++) {
        for (ix = 0; ix < cols; ix++) {
            char name_buf[32];
            snprintf(name_buf, sizeof(name_buf), "%lld", (long long)idx);
            rt_string name = rt_const_cstr(name_buf);
            rt_spritesheet_set_region(sheet, name, ix * frame_w, iy * frame_h, frame_w, frame_h);
            rt_string_unref(name);
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
    int64_t idx;
    if (!obj || !name)
        return;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return;
    cstr = rt_string_cstr(name);
    if (!cstr)
        return;
    if (!*cstr)
        return;
    if (!spritesheet_region_valid(ss, x, y, w, h))
        return;

    /* Update existing region if name matches */
    idx = find_region(ss, cstr);
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
        size_t name_len = strlen(cstr);
        ss->names[ss->count] = (char *)malloc(name_len + 1);
        if (!ss->names[ss->count])
            return;
        memcpy(ss->names[ss->count], cstr, name_len + 1);
        ss->regions[ss->count].x = x;
        ss->regions[ss->count].y = y;
        ss->regions[ss->count].w = w;
        ss->regions[ss->count].h = h;
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
    int64_t idx;
    ss_region *r;
    void *dst;
    if (!obj || !name)
        return NULL;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return NULL;
    cstr = rt_string_cstr(name);
    if (!cstr)
        return NULL;

    idx = find_region(ss, cstr);
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
    if (!obj || !name)
        return 0;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    return find_region(ss, cstr) >= 0 ? 1 : 0;
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
    return rt_pixels_width(ss->atlas);
}

/// @brief Get the retained atlas height.
/// @param obj Candidate SpriteSheet handle.
/// @return Atlas height in pixels, or `0` for an invalid sheet.
int64_t rt_spritesheet_height(void *obj) {
    rt_spritesheet_impl *ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    return rt_pixels_height(ss->atlas);
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
    for (i = 0; i < ss->count; i++) {
        rt_string s = rt_const_cstr(ss->names[i]);
        rt_seq_push(seq, (void *)s);
        rt_str_release_maybe(s);
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
    int64_t idx, j;
    if (!obj || !name)
        return 0;
    ss = spritesheet_checked_or_null(obj);
    if (!ss)
        return 0;
    cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;

    idx = find_region(ss, cstr);
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
