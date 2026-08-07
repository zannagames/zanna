//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/text/rt_ttf_font.c
// Purpose: Implements the GC-managed TtfFont runtime class over the shared
//          vg_font TrueType stack (loading, metrics, measurement) — the E1
//          font bridge between Zia code and real typography.
// Key invariants:
//   - Every live handle owns exactly one vg_font face; the GC finalizer is
//     the single release site.
//   - Identities are handed out by a monotonically increasing counter
//     starting at 1, so 0 always means "not a font".
// Ownership/Lifetime:
//   - rt_ttf_font_face borrows; callers must not destroy or outlive the
//     handle. Canvas raster caches key on the identity, never the pointer.
// Links: src/runtime/graphics/text/rt_ttf_font.h,
//        src/lib/gui/src/font/vg_font.c.
//
//===----------------------------------------------------------------------===//

#include "rt_ttf_font.h"

#include "fonts/embedded_font.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "vg_font.h"

#include <math.h>
#include <string.h>

/// @brief Runtime payload for one loaded TrueType face.
typedef struct rt_ttf_font_impl {
    vg_font_t *face;        ///< Owned vg_font face; destroyed by the finalizer.
    int64_t cache_identity; ///< Process-unique nonzero raster-cache key.
} rt_ttf_font_impl;

/// @brief Next identity handed to a newly loaded font (0 reserved).
static int64_t g_ttf_font_next_identity = 1;

/// @brief Validate a candidate handle as a live TtfFont payload.
/// @param obj Candidate handle; may be NULL or unrelated.
/// @param trap_msg Trap message for invalid handles, or NULL to return NULL
///        silently.
/// @return The payload, or NULL when @p obj is not a live TtfFont.
static rt_ttf_font_impl *ttf_font_checked(void *obj, const char *trap_msg) {
    if (obj && rt_obj_is_instance(obj, RT_TTF_FONT_CLASS_ID, sizeof(rt_ttf_font_impl)))
        return (rt_ttf_font_impl *)obj;
    if (trap_msg)
        rt_trap(trap_msg);
    return NULL;
}

/// @brief GC finalizer: destroy the owned vg_font face.
/// @param obj Finalized TtfFont payload; NULL is ignored.
static void ttf_font_finalize(void *obj) {
    rt_ttf_font_impl *font = (rt_ttf_font_impl *)obj;
    if (!font)
        return;
    vg_font_destroy(font->face);
    font->face = NULL;
}

/// @brief Wrap an owned vg_font face in a new GC-managed handle.
/// @param face Owned face transferred to the handle; destroyed on failure.
/// @return A new TtfFont handle, or NULL after an allocation trap.
static void *ttf_font_wrap(vg_font_t *face) {
    rt_ttf_font_impl *font;
    if (!face)
        return NULL;
    font = (rt_ttf_font_impl *)rt_obj_new_i64(RT_TTF_FONT_CLASS_ID,
                                              (int64_t)sizeof(rt_ttf_font_impl));
    if (!font) {
        vg_font_destroy(face);
        rt_trap("TtfFont: allocation failed");
        return NULL;
    }
    font->face = face;
    font->cache_identity = g_ttf_font_next_identity++;
    rt_obj_set_finalizer(font, ttf_font_finalize);
    return font;
}

void *rt_ttf_font_load(rt_string path) {
    const char *cpath;
    vg_font_t *face;
    if (!path) {
        rt_trap("TtfFont.Load: null path");
        return NULL;
    }
    cpath = rt_string_cstr(path);
    if (!cpath || cpath[0] == '\0') {
        rt_trap("TtfFont.Load: empty path");
        return NULL;
    }
    face = vg_font_load_file(cpath);
    if (!face) {
        rt_trap("TtfFont.Load: cannot read or parse font file");
        return NULL;
    }
    return ttf_font_wrap(face);
}

void *rt_ttf_font_load_default(void) {
    vg_font_t *face = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    if (!face) {
        rt_trap("TtfFont.LoadDefault: embedded face failed to parse");
        return NULL;
    }
    return ttf_font_wrap(face);
}

double rt_ttf_font_clamp_size(double size_px) {
    if (!isfinite(size_px))
        return 16.0;
    if (size_px < 4.0)
        return 4.0;
    if (size_px > 256.0)
        return 256.0;
    return size_px;
}

double rt_ttf_font_measure_width(void *obj, rt_string text, double size_px) {
    vg_text_metrics_t metrics;
    const char *str;
    rt_ttf_font_impl *font = ttf_font_checked(obj, "TtfFont.MeasureWidth: invalid font");
    if (!font || !text)
        return 0.0;
    str = rt_string_cstr(text);
    if (!str || str[0] == '\0')
        return 0.0;
    memset(&metrics, 0, sizeof(metrics));
    vg_font_measure_text(font->face, (float)rt_ttf_font_clamp_size(size_px), str, &metrics);
    return (double)metrics.width;
}

double rt_ttf_font_line_height(void *obj, double size_px) {
    vg_font_metrics_t metrics;
    rt_ttf_font_impl *font = ttf_font_checked(obj, "TtfFont.LineHeight: invalid font");
    if (!font)
        return 0.0;
    memset(&metrics, 0, sizeof(metrics));
    vg_font_get_metrics(font->face, (float)rt_ttf_font_clamp_size(size_px), &metrics);
    return (double)metrics.line_height;
}

double rt_ttf_font_ascent(void *obj, double size_px) {
    vg_font_metrics_t metrics;
    rt_ttf_font_impl *font = ttf_font_checked(obj, "TtfFont.Ascent: invalid font");
    if (!font)
        return 0.0;
    memset(&metrics, 0, sizeof(metrics));
    vg_font_get_metrics(font->face, (float)rt_ttf_font_clamp_size(size_px), &metrics);
    return (double)metrics.ascent;
}

rt_string rt_ttf_font_family(void *obj) {
    const char *family;
    rt_ttf_font_impl *font = ttf_font_checked(obj, "TtfFont.Family: invalid font");
    if (!font)
        return NULL;
    family = vg_font_get_family(font->face);
    return rt_const_cstr(family ? family : "");
}

struct vg_font *rt_ttf_font_face(void *obj) {
    rt_ttf_font_impl *font = ttf_font_checked(obj, NULL);
    return font ? font->face : NULL;
}

int64_t rt_ttf_font_identity(void *obj) {
    rt_ttf_font_impl *font = ttf_font_checked(obj, NULL);
    return font ? font->cache_identity : 0;
}
