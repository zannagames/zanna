//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_texatlas3d.c
// Purpose: Texture atlas with shelf-based bin packing.
//
// Key invariants:
//   - Shelf Next-Fit: textures placed left-to-right, new row when full.
//   - 1-pixel padding border around each sub-texture (replicated source edges).
//   - UV rect returned in normalized [0,1] atlas coordinates.
//   - Maximum 256 packed regions per atlas; capacity is fixed at construction.
//
// Ownership/Lifetime:
//   - TextureAtlas3D is GC-managed; finalizer frees the backing pixel buffer
//     and the cached Pixels mirror.
//
// Links: rt_texatlas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements fixed-capacity shelf-packed TextureAtlas3D objects.
/// @details Each atlas owns a packed RGBA buffer, replicates source edges into one-pixel borders,
///   repairs private metadata before use, and lazily rebuilds a borrowed Pixels mirror after
///   packing changes.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_texatlas3d.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_pixels_new(int64_t w, int64_t h);

typedef struct {
    int32_t x, y, w, h; /* position and size in atlas */
} atlas_region_t;

#define ATLAS_MAX_REGIONS 256

typedef struct {
    void *vptr;
    uint32_t *data;
    int32_t width, height;
    atlas_region_t regions[ATLAS_MAX_REGIONS];
    int32_t region_count;
    int32_t shelf_x, shelf_y, shelf_h; /* current shelf position */
    void *cached_pixels;               /* Pixels object, rebuilt when dirty */
    int8_t dirty;
} rt_texatlas3d;

/// @brief Drop one GC reference held in `*slot` and clear the slot. NULL-safe.
/// @param[in,out] slot Address of the retained reference to release and clear.
static void texatlas3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief GC finalizer — free the atlas backing pixel buffer and cached Pixels copy.
/// @details The atlas owns its `data` buffer directly and owns `cached_pixels`
///   as a separate Pixels object rebuilt from the backing buffer.
/// @param obj TextureAtlas3D runtime object being finalized; ignored when `NULL`.
static void texatlas3d_finalizer(void *obj) {
    rt_texatlas3d *a = (rt_texatlas3d *)obj;
    if (!a)
        return;
    free(a->data);
    a->data = NULL;
    texatlas3d_release_ref(&a->cached_pixels);
}

/// @brief Compute `width * height` as a checked size_t pixel count.
/// @param width Atlas width in pixels.
/// @param height Atlas height in pixels.
/// @param[out] out_count Destination for the validated pixel count.
/// @return 1 when dimensions and byte size are positive and representable, or 0 otherwise.
static int texatlas3d_pixel_count(int32_t width, int32_t height, size_t *out_count) {
    size_t w;
    size_t h;
    size_t count;
    if (!out_count || width <= 0 || height <= 0)
        return 0;
    w = (size_t)width;
    h = (size_t)height;
    if (w > SIZE_MAX / h)
        return 0;
    count = w * h;
    if (count > SIZE_MAX / sizeof(uint32_t))
        return 0;
    *out_count = count;
    return 1;
}

/// @brief Number of region slots safe to read after clamping corrupt private counts.
/// @param a Atlas whose region metadata is inspected.
/// @return The non-negative region count capped at `ATLAS_MAX_REGIONS`, or zero for invalid input.
static int32_t texatlas3d_safe_region_count(const rt_texatlas3d *a) {
    if (!a || a->region_count <= 0)
        return 0;
    return a->region_count < ATLAS_MAX_REGIONS ? a->region_count : ATLAS_MAX_REGIONS;
}

/// @brief Validate and compact atlas metadata before reads or packing mutations.
/// @param a Atlas whose backing storage, regions, shelf cursor, and cache state are normalized.
static void texatlas3d_repair(rt_texatlas3d *a) {
    if (!a)
        return;
    size_t pixel_count = 0u;
    if (!a->data || a->width < 16 || a->height < 16 || a->width > 8192 || a->height > 8192 ||
        !texatlas3d_pixel_count(a->width, a->height, &pixel_count)) {
        a->region_count = 0;
        a->shelf_x = 0;
        a->shelf_y = 0;
        a->shelf_h = 0;
        a->dirty = 1;
        texatlas3d_release_ref(&a->cached_pixels);
        return;
    }
    (void)pixel_count;
    int32_t count = texatlas3d_safe_region_count(a);
    int32_t write = 0;
    for (int32_t read = 0; read < count; ++read) {
        atlas_region_t r = a->regions[read];
        if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x > a->width || r.y > a->height ||
            r.w > a->width - r.x || r.h > a->height - r.y)
            continue;
        a->regions[write++] = r;
    }
    for (int32_t i = write; i < count; ++i)
        memset(&a->regions[i], 0, sizeof(a->regions[i]));
    a->region_count = write;
    if (a->shelf_x < 0 || a->shelf_x > a->width)
        a->shelf_x = 0;
    if (a->shelf_y < 0 || a->shelf_y > a->height)
        a->shelf_y = 0;
    if (a->shelf_h < 0 || a->shelf_h > a->height - a->shelf_y)
        a->shelf_h = 0;
    a->dirty = a->dirty ? 1 : 0;
}

/// @brief Initialize any supplied UV outputs to the full normalized atlas rectangle.
/// @param[out] u0 Optional destination for the left coordinate.
/// @param[out] v0 Optional destination for the top coordinate.
/// @param[out] u1 Optional destination for the right coordinate.
/// @param[out] v1 Optional destination for the bottom coordinate.
static void texatlas3d_full_uv(double *u0, double *v0, double *u1, double *v1) {
    if (u0)
        *u0 = 0.0;
    if (v0)
        *v0 = 0.0;
    if (u1)
        *u1 = 1.0;
    if (v1)
        *v1 = 1.0;
}

/// @brief Construct a 3D texture atlas with `width × height` blank pixels (zero-initialized).
/// Tracks shelf packing state so subsequent `_add` calls fit textures left-to-right with 1-pixel
/// padding. Traps if dimensions are outside [16, 8192] or on allocation failure.
/// @param width Atlas width in pixels.
/// @param height Atlas height in pixels.
/// @return A new GC-managed TextureAtlas3D, or `NULL` after trapping on invalid size or allocation
///   failure.
void *rt_texatlas3d_new(int64_t width, int64_t height) {
    size_t pixel_count;
    if (width < 16 || height < 16 || width > 8192 || height > 8192) {
        rt_trap("TextureAtlas3D.New: dimensions must be 16-8192");
        return NULL;
    }
    if (!texatlas3d_pixel_count((int32_t)width, (int32_t)height, &pixel_count)) {
        rt_trap("TextureAtlas3D.New: dimensions overflow");
        return NULL;
    }
    rt_texatlas3d *a = (rt_texatlas3d *)rt_obj_new_i64(RT_G3D_TEXTUREATLAS3D_CLASS_ID,
                                                       (int64_t)sizeof(rt_texatlas3d));
    if (!a) {
        rt_trap("TextureAtlas3D.New: allocation failed");
        return NULL;
    }
    a->vptr = NULL;
    a->width = (int32_t)width;
    a->height = (int32_t)height;
    a->data = (uint32_t *)calloc(pixel_count, sizeof(uint32_t));
    if (!a->data) {
        if (rt_obj_release_check0(a))
            rt_obj_free(a);
        rt_trap("TextureAtlas3D.New: allocation failed");
        return NULL;
    }
    a->region_count = 0;
    a->shelf_x = 0;
    a->shelf_y = 0;
    a->shelf_h = 0;
    a->cached_pixels = NULL;
    a->dirty = 1;
    rt_obj_set_finalizer(a, texatlas3d_finalizer);
    return a;
}

/// @brief Pack a Pixels sub-image into the next free shelf slot. Allocates 1-pixel padding
/// around the image, advances the shelf cursor, and returns the integer region ID for later
/// `_get_uv_rect` lookup. Returns -1 if the atlas is full (256 regions) or if the texture
/// won't fit in the remaining vertical space. Marks the atlas dirty for the next texture rebuild.
/// @param obj TextureAtlas3D receiving the packed image.
/// @param pixels Non-empty Pixels source copied into the atlas; the atlas does not retain it.
/// @return The stable non-negative region identifier on success, or -1 for invalid input, full
///   capacity, or insufficient space.
int64_t rt_texatlas3d_add(void *obj, void *pixels) {
    if (!obj || !pixels)
        return -1;
    rt_texatlas3d *a = (rt_texatlas3d *)rt_g3d_checked_or_null(obj, RT_G3D_TEXTUREATLAS3D_CLASS_ID);
    if (!a)
        return -1;
    texatlas3d_repair(a);
    if (a->region_count >= ATLAS_MAX_REGIONS)
        return -1;

    rt_pixels_impl *pv = rt_pixels_checked_impl(pixels, "TextureAtlas3D.Add: expected Pixels");
    if (!pv || !pv->data || pv->width <= 0 || pv->height <= 0 || pv->width > INT32_MAX - 2 ||
        pv->height > INT32_MAX - 2)
        return -1;

    int32_t pw = (int32_t)pv->width;
    int32_t ph = (int32_t)pv->height;
    int32_t tw = pw + 2; /* +2 for 1px border padding */
    int32_t th = ph + 2;
    if (tw > a->width || th > a->height)
        return -1;

    /* Try to fit on current shelf */
    if (tw > a->width - a->shelf_x) {
        /* Start new shelf */
        a->shelf_y += a->shelf_h;
        a->shelf_x = 0;
        a->shelf_h = 0;
    }
    if (th > a->height - a->shelf_y)
        return -1; /* atlas full */

    /* Place texture at (shelf_x + 1, shelf_y + 1) — skip 1px border */
    int32_t dx = a->shelf_x + 1;
    int32_t dy = a->shelf_y + 1;

    for (int32_t y = 0; y < ph; y++) {
        for (int32_t x = 0; x < pw; x++) {
            int32_t ax = dx + x, ay = dy + y;
            if (ax < a->width && ay < a->height)
                a->data[(size_t)ay * (size_t)a->width + (size_t)ax] =
                    pv->data[(int64_t)y * pv->width + x];
        }
    }

    for (int32_t x = 0; x < pw; x++) {
        uint32_t top = pv->data[x];
        uint32_t bottom = pv->data[(int64_t)(ph - 1) * pv->width + x];
        a->data[(size_t)(dy - 1) * (size_t)a->width + (size_t)(dx + x)] = top;
        a->data[(size_t)(dy + ph) * (size_t)a->width + (size_t)(dx + x)] = bottom;
    }
    for (int32_t y = 0; y < ph; y++) {
        uint32_t left = pv->data[(int64_t)y * pv->width];
        uint32_t right = pv->data[(int64_t)y * pv->width + (pw - 1)];
        a->data[(size_t)(dy + y) * (size_t)a->width + (size_t)(dx - 1)] = left;
        a->data[(size_t)(dy + y) * (size_t)a->width + (size_t)(dx + pw)] = right;
    }
    a->data[(size_t)(dy - 1) * (size_t)a->width + (size_t)(dx - 1)] = pv->data[0];
    a->data[(size_t)(dy - 1) * (size_t)a->width + (size_t)(dx + pw)] = pv->data[pw - 1];
    a->data[(size_t)(dy + ph) * (size_t)a->width + (size_t)(dx - 1)] =
        pv->data[(int64_t)(ph - 1) * pv->width];
    a->data[(size_t)(dy + ph) * (size_t)a->width + (size_t)(dx + pw)] =
        pv->data[(int64_t)(ph - 1) * pv->width + (pw - 1)];

    atlas_region_t *r = &a->regions[a->region_count];
    r->x = dx;
    r->y = dy;
    r->w = pw;
    r->h = ph;

    a->shelf_x += tw;
    if (th > a->shelf_h)
        a->shelf_h = th;
    a->dirty = 1;

    return a->region_count++;
}

/// @brief Return the atlas as a Pixels object, lazily rebuilding it from the internal data
/// buffer when dirty. The returned Pixels is owned by the atlas — do not free or release. Each
/// call after a `_add` rebuilds; results in stable storage when no changes have been made.
/// @param obj Candidate TextureAtlas3D instance.
/// @return A borrowed atlas-owned Pixels mirror, or `NULL` on invalid input or rebuild failure.
void *rt_texatlas3d_get_texture(void *obj) {
    if (!obj)
        return NULL;
    rt_texatlas3d *a = (rt_texatlas3d *)rt_g3d_checked_or_null(obj, RT_G3D_TEXTUREATLAS3D_CLASS_ID);
    if (!a)
        return NULL;
    texatlas3d_repair(a);
    if (!a->data)
        return NULL;

    if (a->dirty || !a->cached_pixels) {
        size_t pixel_count;
        /* Create a Pixels object from atlas data */
        void *new_pixels = rt_pixels_new(a->width, a->height);
        if (!new_pixels)
            return NULL;
        rt_pixels_impl *pv =
            rt_pixels_checked_impl(new_pixels, "TextureAtlas3D.GetTexture: expected Pixels");
        if (!pv || !pv->data) {
            if (rt_obj_release_check0(new_pixels))
                rt_obj_free(new_pixels);
            return NULL;
        }
        if (!texatlas3d_pixel_count(a->width, a->height, &pixel_count)) {
            if (rt_obj_release_check0(new_pixels))
                rt_obj_free(new_pixels);
            return NULL;
        }
        memcpy(pv->data, a->data, pixel_count * sizeof(uint32_t));
        texatlas3d_release_ref(&a->cached_pixels);
        a->cached_pixels = new_pixels;
        a->dirty = 0;
    }
    return a->cached_pixels;
}

/// @brief Output the normalized UV rectangle [0,1] for region `id` into the four out-pointers.
/// Use these UVs as mesh texture coordinates to sample the packed sub-image. Out-of-range IDs
/// return the full atlas rect (0..1) so missing textures degrade visibly rather than silently.
/// @param obj Candidate TextureAtlas3D instance.
/// @param id Region identifier returned by `rt_texatlas3d_add`.
/// @param[out] u0 Required destination for the left coordinate.
/// @param[out] v0 Required destination for the top coordinate.
/// @param[out] u1 Required destination for the right coordinate.
/// @param[out] v1 Required destination for the bottom coordinate.
void rt_texatlas3d_get_uv_rect(
    void *obj, int64_t id, double *u0, double *v0, double *u1, double *v1) {
    if (!u0 || !v0 || !u1 || !v1)
        return;
    texatlas3d_full_uv(u0, v0, u1, v1);
    if (!obj)
        return;
    rt_texatlas3d *a = (rt_texatlas3d *)rt_g3d_checked_or_null(obj, RT_G3D_TEXTUREATLAS3D_CLASS_ID);
    if (!a)
        return;
    texatlas3d_repair(a);
    int32_t region_count = texatlas3d_safe_region_count(a);
    if (id < 0 || id >= region_count)
        return;
    atlas_region_t *r = &a->regions[id];
    *u0 = (double)r->x / a->width;
    *v0 = (double)r->y / a->height;
    *u1 = (double)(r->x + r->w) / a->width;
    *v1 = (double)(r->y + r->h) / a->height;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
