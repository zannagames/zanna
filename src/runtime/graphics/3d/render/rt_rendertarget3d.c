//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_rendertarget3d.c
// Purpose: Zanna.Graphics3D.RenderTarget3D — offscreen rendering target
//   with color + depth buffers. Enables render-to-texture for effects
//   like shadow maps, reflections, and post-processing.
//
// Key invariants:
//   - CPU readback buffer is always RGBA uint8_t (same format as Pixels)
//   - HDR RTT readback also keeps an optional linear RGBA32F CPU mirror
//   - Depth buffer is float (same as Z-buffer)
//   - CPU-side color/depth storage is allocated lazily on first CPU use
//     or when the software backend binds the target.
//   - AsPixels() returns a NEW Pixels object (fresh copy each call)
//   - GC finalizer frees both buffers
//
// Ownership/Lifetime:
//   - RenderTarget3D is GC-managed; finalizer frees the backend rendertarget
//     and its lazily-allocated color/depth buffers.
//   - Canvas3D retains a reference when the target is bound and releases it
//     on unbind / canvas destruction.
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h, plans/3d/08-render-to-texture.md,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements budgeted off-screen RenderTarget3D storage, readback, and canvas binding.
/// @details Render-target shells reserve their maximum CPU/GPU footprint up front, allocate
///   buffers lazily through the active backend, expose copied or cached Pixels mirrors, and retain
///   safely while bound to a Canvas3D.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "vgfx3d_backend.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define RT_RENDERTARGET3D_DEFAULT_BUDGET_BYTES (1024ull * 1024ull * 1024ull)

static size_t g_rendertarget3d_reserved_bytes = 0;
static uint64_t g_rendertarget3d_next_identity = 1;

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_pixels_new(int64_t width, int64_t height);

//=============================================================================
// Render target allocation
//=============================================================================

/// @brief Multiply two size_t values with overflow detection, writing the product to *@p out.
/// @param a First factor.
/// @param b Second factor.
/// @param[out] out Destination for the product; cleared before validation when non-null.
/// @return 1 with *@p out set on success, or 0 when @p out is `NULL` or the product would exceed
///   `SIZE_MAX`.
static int rt_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out)
        *out = 0u;
    if (!out)
        return 0;
    if (a != 0u && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Estimate the maximum owned memory of a @p w×@p h render target for the given color
///   format, using overflow-checked products.
/// @details The reservation covers native color/depth storage plus every lazy CPU mirror that the
///   target may allocate. LDR targets reserve 16 bytes/texel (GPU color/depth and CPU
///   color/depth); HDR targets reserve 36 bytes/texel (HDR16F GPU color, GPU depth, RGBA32F HDR
///   mirror, RGBA8 tonemapped mirror, and CPU depth). This deliberately budgets the maximum rather
///   than only the shell's initial allocation so lazy readback cannot bypass the process ceiling.
/// @param w Target width in pixels.
/// @param h Target height in pixels.
/// @param color_format LDR or HDR backend color format.
/// @param[out] out_bytes Destination for the maximum owned footprint estimate.
/// @return 1 with @p out_bytes set on success; 0 for non-positive dimensions or on overflow.
static int rt_rendertarget_estimate_bytes(int32_t w,
                                          int32_t h,
                                          vgfx3d_rendertarget_color_format_t color_format,
                                          size_t *out_bytes) {
    size_t pixels;
    size_t total;
    size_t bytes_per_pixel = color_format == VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F ? 36u : 16u;
    if (out_bytes)
        *out_bytes = 0u;
    if (!out_bytes || w <= 0 || h <= 0)
        return 0;
    if (!rt_checked_mul_size((size_t)w, (size_t)h, &pixels) ||
        !rt_checked_mul_size(pixels, bytes_per_pixel, &total))
        return 0;
    *out_bytes = total;
    return 1;
}

/// @brief Reserve @p bytes against the process-wide RenderTarget3D footprint budget.
/// @details Render-target CPU mirrors and GPU textures are allocated lazily by the active backend,
///   but the dimensions define a predictable maximum footprint. Reserving that estimate up front
///   prevents scripts from creating many huge targets and deferring failure to backend OOM.
/// @param bytes Maximum target footprint to add to the atomic process-wide reservation.
/// @return 1 when the reservation was accepted; 0 when it would exceed the default budget.
static int rt_rendertarget_reserve_budget(uint64_t bytes) {
    const size_t budget_bytes = (size_t)RT_RENDERTARGET3D_DEFAULT_BUDGET_BYTES;
    size_t reserve_bytes;
    size_t old_value;
    if (bytes == 0u)
        return 1;
    if (bytes > RT_RENDERTARGET3D_DEFAULT_BUDGET_BYTES)
        return 0;
    reserve_bytes = (size_t)bytes;
    for (;;) {
        old_value = rt_atomic_load_size(&g_rendertarget3d_reserved_bytes, __ATOMIC_RELAXED);
        if (old_value > budget_bytes - reserve_bytes)
            return 0;
        if (rt_atomic_compare_exchange_size(&g_rendertarget3d_reserved_bytes,
                                            &old_value,
                                            old_value + reserve_bytes,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
            return 1;
    }
}

/// @brief Release a previous RenderTarget3D footprint reservation.
/// @details Saturates to zero on accounting mismatch so finalization remains idempotent even if a
///   partially constructed target is torn down during error recovery.
/// @param bytes Previously reserved footprint to subtract from the process-wide total.
static void rt_rendertarget_release_budget(uint64_t bytes) {
    size_t release_bytes;
    size_t old_value;
    if (bytes == 0u)
        return;
    release_bytes = bytes > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)bytes;
    for (;;) {
        old_value = rt_atomic_load_size(&g_rendertarget3d_reserved_bytes, __ATOMIC_RELAXED);
        size_t new_value = old_value > release_bytes ? old_value - release_bytes : 0u;
        if (rt_atomic_compare_exchange_size(&g_rendertarget3d_reserved_bytes,
                                            &old_value,
                                            new_value,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
            return;
    }
}

/// @brief Allocate a monotonic non-zero cache identity for a render-target shell.
/// @details Backends use this instead of raw C pointers when caching native textures, avoiding
///   stale cache hits if the allocator reuses a recently freed target address.
/// @return A process-unique non-zero identity, subject only to full 64-bit counter wraparound.
static uint64_t rt_rendertarget_next_cache_identity(void) {
    uint64_t identity =
        rt_atomic_fetch_add_u64(&g_rendertarget3d_next_identity, UINT64_C(1), __ATOMIC_RELAXED);
    if (identity == 0u)
        identity =
            rt_atomic_fetch_add_u64(&g_rendertarget3d_next_identity, UINT64_C(1), __ATOMIC_RELAXED);
    return identity ? identity : 1u;
}

/// @brief Allocate the backend-side `vgfx3d_rendertarget_t` shell at `w × h` (RGBA8 stride =
/// w * 4), with `color_buf` / `depth_buf` left NULL. CPU-side buffers are allocated lazily
/// on first read or when the software backend binds the target — see
/// `vgfx3d_rendertarget_ensure_color` / `_ensure_depth` in `rt_canvas3d_internal.h`. Returns
/// NULL on calloc failure; the caller traps with a user-visible message.
/// @param w Target width in pixels.
/// @param h Target height in pixels.
/// @param color_format Backend color storage format.
/// @return An owned zero-initialized backend shell with a budget reservation, or `NULL` on invalid
///   layout, exhausted budget, or allocation failure.
static vgfx3d_rendertarget_t *rt_alloc(int32_t w,
                                       int32_t h,
                                       vgfx3d_rendertarget_color_format_t color_format) {
    size_t estimated_bytes = 0u;
    vgfx3d_rendertarget_t *rt = (vgfx3d_rendertarget_t *)calloc(1, sizeof(vgfx3d_rendertarget_t));
    if (!rt)
        return NULL;
    if (w > INT32_MAX / 4 ||
        !rt_rendertarget_estimate_bytes(w, h, color_format, &estimated_bytes)) {
        free(rt);
        return NULL;
    }
    if (!rt_rendertarget_reserve_budget((uint64_t)estimated_bytes)) {
        free(rt);
        return NULL;
    }

    rt->width = w;
    rt->height = h;
    rt->stride = w * 4;
    rt->color_format = (int32_t)color_format;
    rt->estimated_bytes = (uint64_t)estimated_bytes;
    rt->cache_identity = rt_rendertarget_next_cache_identity();

    return rt;
}

/// @brief Tear down a `vgfx3d_rendertarget_t`. Frees both CPU-side buffers (NULL-safe — they
/// may never have been allocated under the lazy-ensure model) and then the shell itself.
/// @param rt Owned backend shell to release; ignored when `NULL`.
static void rt_free(vgfx3d_rendertarget_t *rt) {
    if (!rt)
        return;
    /* Native caches may borrow this shell as a readback/lifetime key. Notify their owner before
     * any field or buffer becomes invalid so cache pruning cannot later dereference freed memory.
     */
    vgfx3d_rendertarget_release_backend(rt);
    vgfx3d_rendertarget_clear_sync(rt);
    free(rt->color_buf);
    free(rt->hdr_color_buf);
    free(rt->depth_buf);
    rt_rendertarget_release_budget(rt->estimated_bytes);
    free(rt);
}

//=============================================================================
// Runtime type
//=============================================================================

/// @brief GC finalizer for `RenderTarget3D`. Frees the underlying backend rendertarget if
/// still live. The Canvas3D side releases its own retained reference separately, so the
/// finalizer only owns `target` itself, not the canvas pointer.
/// @param obj RenderTarget3D runtime object being finalized; ignored when `NULL`.
static void rt_rendertarget3d_finalize(void *obj) {
    rt_rendertarget3d *rtd = (rt_rendertarget3d *)obj;
    if (!rtd)
        return;
    if (rtd->target) {
        rt_free(rtd->target);
        rtd->target = NULL;
    }
    if (rtd->material_pixels) {
        if (rt_obj_release_check0(rtd->material_pixels))
            rt_obj_free(rtd->material_pixels);
        rtd->material_pixels = NULL;
    }
}

/// @brief Validate @p obj as a RenderTarget3D handle and return its typed pointer (NULL on
/// mismatch).
/// @param obj Candidate runtime object.
/// @return The typed RenderTarget3D pointer, or `NULL` for a null or wrong-class object.
static rt_rendertarget3d *rendertarget3d_checked(void *obj) {
    return (rt_rendertarget3d *)rt_g3d_checked_or_null(obj, RT_G3D_RENDERTARGET3D_CLASS_ID);
}

/// @brief Create an offscreen render target for render-to-texture effects.
/// @details Allocates a backend shell and reserves the target's maximum footprint; actual buffers
///          remain lazy until backend binding or CPU readback. Once bound to a Canvas3D, rendering
///          is redirected from the window and results can be copied into Pixels.
/// @param width Target width in pixels (1–VGFX3D_RENDERTARGET_DIM_MAX).
/// @param height Target height in pixels (1–VGFX3D_RENDERTARGET_DIM_MAX).
/// @param color_format Requested LDR or HDR backend color storage.
/// @param trap_name Diagnostic used when dimensions are outside the supported range.
/// @return Opaque render target handle, or NULL on failure.
static void *rt_rendertarget3d_new_with_format(int64_t width,
                                               int64_t height,
                                               vgfx3d_rendertarget_color_format_t color_format,
                                               const char *trap_name) {
    if (width <= 0 || height <= 0 || width > VGFX3D_RENDERTARGET_DIM_MAX ||
        height > VGFX3D_RENDERTARGET_DIM_MAX) {
        rt_trap(trap_name);
        return NULL;
    }

    rt_rendertarget3d *rtd = (rt_rendertarget3d *)rt_obj_new_i64(
        RT_G3D_RENDERTARGET3D_CLASS_ID, (int64_t)sizeof(rt_rendertarget3d));
    if (!rtd) {
        rt_trap("RenderTarget3D: memory allocation failed");
        return NULL;
    }

    rtd->vptr = NULL;
    rtd->width = width;
    rtd->height = height;
    rtd->material_pixels = NULL;
    rtd->material_pixels_revision = 0;
    rtd->target = rt_alloc((int32_t)width, (int32_t)height, color_format);

    if (!rtd->target) {
        if (rt_obj_release_check0(rtd))
            rt_obj_free(rtd);
        rt_trap("RenderTarget3D: buffer allocation failed");
        return NULL;
    }

    rt_obj_set_finalizer(rtd, rt_rendertarget3d_finalize);
    return rtd;
}

/// @brief Allocate an LDR (8-bit per channel) off-screen render target.
/// @details Thin wrapper around the shared constructor that selects the
///   `UNORM8` color format. Dimensions outside the supported render-target range
///   trap with a descriptive message rather than silently clamping.
/// @param width Target width in pixels.
/// @param height Target height in pixels.
/// @return Retained pointer to the new `rt_rendertarget3d`, or traps on failure.
void *rt_rendertarget3d_new(int64_t width, int64_t height) {
    return rt_rendertarget3d_new_with_format(width,
                                             height,
                                             VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8,
                                             "RenderTarget3D.New: dimensions must be 1-16384");
}

/// @brief Allocate an HDR (16-bit float per channel) off-screen render target.
/// @details Same API as `rt_rendertarget3d_new`, but picks the `HDR16F` color
///   format so tone-mapping and bloom effects can work with values > 1.0
///   without early clamping. Callers that don't need HDR should use the plain
///   `new` variant — HDR targets consume 2x the VRAM per pixel.
/// @param width Target width in pixels.
/// @param height Target height in pixels.
/// @return Retained pointer to the new HDR `rt_rendertarget3d`, or `NULL` after trapping on
///   invalid dimensions or allocation failure.
void *rt_rendertarget3d_new_hdr(int64_t width, int64_t height) {
    return rt_rendertarget3d_new_with_format(width,
                                             height,
                                             VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F,
                                             "RenderTarget3D.NewHdr: dimensions must be 1-16384");
}

/// @brief Get the width of the render target in pixels.
/// @param obj Candidate RenderTarget3D handle.
/// @return The validated width, or zero when object and backend metadata disagree.
int64_t rt_rendertarget3d_get_width(void *obj) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd || !rtd->target || rtd->width <= 0 || rtd->width > VGFX3D_RENDERTARGET_DIM_MAX ||
        rtd->target->width <= 0 || rtd->target->width > VGFX3D_RENDERTARGET_DIM_MAX ||
        rtd->target->width != rtd->width)
        return 0;
    return rtd->target->width;
}

/// @brief Get the height of the render target in pixels.
/// @param obj Candidate RenderTarget3D handle.
/// @return The validated height, or zero when object and backend metadata disagree.
int64_t rt_rendertarget3d_get_height(void *obj) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd || !rtd->target || rtd->height <= 0 || rtd->height > VGFX3D_RENDERTARGET_DIM_MAX ||
        rtd->target->height <= 0 || rtd->target->height > VGFX3D_RENDERTARGET_DIM_MAX ||
        rtd->target->height != rtd->height)
        return 0;
    return rtd->target->height;
}

/// @brief Return whether the target stores HDR color on the GPU path.
/// @param obj Candidate RenderTarget3D handle.
/// @return 1 for a valid HDR target, or 0 for LDR or invalid input.
int32_t rt_rendertarget3d_get_is_hdr(void *obj) {
    const rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    return (rtd && vgfx3d_rendertarget_is_hdr(rtd->target)) ? 1 : 0;
}

/// @brief Copy the render target contents into a new Pixels object for CPU access.
/// @details Converts the CPU readback buffer to Pixels' packed uint32_t format
///          (0xRRGGBBAA). HDR render targets are tonemapped into that buffer by
///          the active GPU backend before this copy occurs. This is a full copy
///          — the Pixels can be used as a texture, saved to disk, or processed
///          independently.
/// @param obj Render target handle.
/// @return New Pixels handle with the framebuffer contents, or NULL on failure.
void *rt_rendertarget3d_as_pixels(void *obj) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd)
        return NULL;
    if (!rtd->target)
        return NULL;
    size_t color_bytes = 0u;
    if (!vgfx3d_rendertarget_valid_color_layout(rtd->target, &color_bytes) || color_bytes == 0u)
        return NULL;
    if (!vgfx3d_rendertarget_ensure_color(rtd->target))
        return NULL;
    if (!vgfx3d_rendertarget_sync_color_if_needed(rtd->target))
        return NULL;

    int32_t w = rtd->target->width;
    int32_t h = rtd->target->height;
    int32_t stride = rtd->target->stride;
    if (w <= 0 || h <= 0 || (int64_t)stride < (int64_t)w * 4 || !rtd->target->color_buf)
        return NULL;
    if ((int64_t)w != rtd->width || (int64_t)h != rtd->height)
        return NULL;

    void *pixels = rt_pixels_new((int64_t)w, (int64_t)h);
    if (!pixels)
        return NULL;

    /* Copy render target RGBA bytes → Pixels 0xRRGGBBAA uint32_t */
    typedef struct {
        int64_t w;
        int64_t h;
        uint32_t *data;
    } px_view;

    px_view *pv = (px_view *)pixels;
    if (!pv->data || pv->w != (int64_t)w || pv->h != (int64_t)h) {
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *src = &rtd->target->color_buf[(size_t)y * (size_t)stride];
        uint32_t *dst = &pv->data[(size_t)y * (size_t)pv->w];
        for (int32_t x = 0; x < w; x++, src += 4) {
            dst[x] = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
                     (uint32_t)src[3];
        }
    }

    return pixels;
}

/// @brief Sync the target's CPU color mirror and convert it into @p pv's uint32 buffer.
/// @param rtd RenderTarget3D whose backend color storage is read.
/// @param pv Existing same-size Pixels implementation receiving packed RGBA values.
/// @return 1 on success, 0 when the target has no readable color or sizes disagree.
static int rendertarget3d_read_into_pixels(rt_rendertarget3d *rtd, rt_pixels_impl *pv) {
    if (!rtd || !rtd->target || !pv || !pv->data)
        return 0;
    size_t color_bytes = 0u;
    if (!vgfx3d_rendertarget_valid_color_layout(rtd->target, &color_bytes) || color_bytes == 0u)
        return 0;
    if (!vgfx3d_rendertarget_ensure_color(rtd->target))
        return 0;
    if (!vgfx3d_rendertarget_sync_color_if_needed(rtd->target))
        return 0;

    int32_t w = rtd->target->width;
    int32_t h = rtd->target->height;
    int32_t stride = rtd->target->stride;
    if (w <= 0 || h <= 0 || (int64_t)stride < (int64_t)w * 4 || !rtd->target->color_buf)
        return 0;
    if ((int64_t)w != rtd->width || (int64_t)h != rtd->height)
        return 0;
    if (pv->width != (int64_t)w || pv->height != (int64_t)h)
        return 0;

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *src = &rtd->target->color_buf[(size_t)y * (size_t)stride];
        uint32_t *dst = &pv->data[(size_t)y * (size_t)pv->width];
        for (int32_t x = 0; x < w; x++, src += 4) {
            dst[x] = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
                     (uint32_t)src[3];
        }
    }
    pixels_touch(pv);
    return 1;
}

/// @brief Copy the render target contents into an existing Pixels buffer.
/// @details Allocation-free readback for per-frame monitor/scope loops: reuses
///   the caller's Pixels instead of allocating a fresh copy like `as_pixels`.
///   Dimensions must match the target exactly. Bumps the Pixels generation so
///   GPU texture caches pick up the new contents.
/// @param obj Render target handle.
/// @param pixels Destination Pixels whose width/height equal the target's.
/// @brief Non-trapping RGBA8 readback straight into a caller buffer.
/// @details Row-copies the synced backend color mirror without any packing
///          pass, so per-frame consumers (widget buffers, shared-memory
///          frames) skip the uint32 round trip entirely. Any mismatch or
///          unreadable target reports 0 and writes nothing.
/// @param obj RenderTarget3D handle.
/// @param dst Destination buffer of width*height*4 bytes.
/// @param width Expected frame width in pixels.
/// @param height Expected frame height in pixels.
/// @return 1 when the full frame was written, otherwise 0.
int rt_rendertarget3d_try_read_rgba(void *obj, uint8_t *dst, int64_t width, int64_t height) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd || !rtd->target || !dst || width <= 0 || height <= 0)
        return 0;
    size_t color_bytes = 0u;
    if (!vgfx3d_rendertarget_valid_color_layout(rtd->target, &color_bytes) || color_bytes == 0u)
        return 0;
    if (!vgfx3d_rendertarget_ensure_color(rtd->target))
        return 0;
    if (!vgfx3d_rendertarget_sync_color_if_needed(rtd->target))
        return 0;
    int32_t w = rtd->target->width;
    int32_t h = rtd->target->height;
    int32_t stride = rtd->target->stride;
    if (w <= 0 || h <= 0 || (int64_t)stride < (int64_t)w * 4 || !rtd->target->color_buf)
        return 0;
    if ((int64_t)w != width || (int64_t)h != height)
        return 0;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *src = &rtd->target->color_buf[(size_t)y * (size_t)stride];
        memcpy(&dst[(size_t)y * (size_t)w * 4u], src, (size_t)w * 4u);
    }
    return 1;
}

void rt_rendertarget3d_copy_to(void *obj, void *pixels) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd || !rtd->target) {
        rt_trap("RenderTarget3D.CopyTo: invalid render target");
        return;
    }
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    if (!pv || !pv->data) {
        rt_trap("RenderTarget3D.CopyTo: destination must be a valid Pixels");
        return;
    }
    if (pv->width != rtd->width || pv->height != rtd->height) {
        rt_trap("RenderTarget3D.CopyTo: size mismatch");
        return;
    }
    (void)rendertarget3d_read_into_pixels(rtd, pv);
}

/// @brief Resolve a RenderTarget3D to its material-binding Pixels mirror.
/// @details Lazily creates one same-size Pixels per target and refreshes it only
///   when the target's `content_revision` advanced (a frame into it ended). While
///   the target is being rendered to, its revision has not advanced yet, so a
///   material bound to it keeps sampling the previous completed frame — this is
///   what makes self-referential monitor setups safe without a hazard trap.
/// @param obj Candidate RenderTarget3D handle.
/// @return A borrowed target-owned Pixels mirror, or `NULL` when validation, allocation, or
///   readback fails.
void *rt_rendertarget3d_material_pixels(void *obj) {
    rt_rendertarget3d *rtd = rendertarget3d_checked(obj);
    if (!rtd || !rtd->target)
        return NULL;
    if (!rtd->material_pixels) {
        rtd->material_pixels = rt_pixels_new(rtd->width, rtd->height);
        if (!rtd->material_pixels)
            return NULL;
        rtd->material_pixels_revision = rtd->target->content_revision - 1u;
    }
    if (rtd->material_pixels_revision != rtd->target->content_revision) {
        rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(rtd->material_pixels);
        if (pv && rendertarget3d_read_into_pixels(rtd, pv))
            rtd->material_pixels_revision = rtd->target->content_revision;
    }
    return rtd->material_pixels;
}

//=============================================================================
// Canvas3D render target binding
//=============================================================================

/// @brief Bind an offscreen render target. All subsequent Begin/DrawMesh/End
/// calls render to the target instead of the window. The active backend
/// (Metal, software, etc.) handles RTT natively — no backend switching needed.
/// @param canvas Canvas3D whose output destination is changed.
/// @param target RenderTarget3D retained by the canvas, or `NULL` to reset window rendering.
void rt_canvas3d_set_render_target(void *canvas, void *target) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return;
    if (c->in_frame) {
        rt_trap("Canvas3D.SetRenderTarget: cannot change render targets during a frame");
        return;
    }
    if (!target) {
        rt_canvas3d_reset_render_target(canvas);
        return;
    }
    rt_rendertarget3d *rtd =
        (rt_rendertarget3d *)rt_g3d_checked_or_null(target, RT_G3D_RENDERTARGET3D_CLASS_ID);
    if (!rtd || !rtd->target)
        return;
    if (c->backend == &vgfx3d_software_backend && !vgfx3d_rendertarget_ensure_color(rtd->target)) {
        rt_trap("Canvas3D.SetRenderTarget: buffer allocation failed");
        return;
    }
    if (!vgfx3d_rendertarget_ensure_depth(rtd->target)) {
        rt_trap("Canvas3D.SetRenderTarget: buffer allocation failed");
        return;
    }
    if (c->render_target_owner == rtd)
        return;
    rt_obj_retain_maybe(rtd);
    if (c->render_target_owner && rt_obj_release_check0(c->render_target_owner))
        rt_obj_free(c->render_target_owner);
    c->render_target_owner = rtd;
    c->render_target = rtd->target;

    if (c->backend && c->backend->set_render_target)
        c->backend->set_render_target(c->backend_ctx, rtd->target);
}

/// @brief `Canvas3D.RenderTarget` — borrowed retained render target (ADR 0233).
/// @param canvas Canvas3D receiver or supported stack-wrapper handle.
/// @return Borrowed RenderTarget3D handle, or NULL when rendering to the window.
void *rt_canvas3d_get_render_target(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    return c ? c->render_target_owner : NULL;
}

/// @brief Unbind the render target. Subsequent rendering goes to the window.
/// @param canvas Window-backed Canvas3D whose retained target is released.
void rt_canvas3d_reset_render_target(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (!c)
        return;
    if (c->offscreen) {
        rt_trap("Canvas3D.ResetRenderTarget: offscreen canvas requires a render target");
        return;
    }
    if (c->in_frame) {
        rt_trap("Canvas3D.ResetRenderTarget: cannot change render targets during a frame");
        return;
    }

    if (c->backend && c->backend->set_render_target)
        c->backend->set_render_target(c->backend_ctx, NULL);

    if (c->render_target_owner && rt_obj_release_check0(c->render_target_owner))
        rt_obj_free(c->render_target_owner);
    c->render_target_owner = NULL;
    c->render_target = NULL;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
