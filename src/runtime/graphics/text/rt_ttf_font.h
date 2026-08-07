//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/text/rt_ttf_font.h
// Purpose: GC-managed TrueType font handles for the runtime (the E1 font
//          bridge): loading faces from disk or the embedded JetBrains Mono,
//          measuring UTF-8 text, and exposing the vg_font face to renderers
//          such as Canvas3D's DrawText2DTtf.
// Key invariants:
//   - Handles are rt_obj payloads stamped RT_TTF_FONT_CLASS_ID; the GC
//     finalizer destroys the underlying vg_font face.
//   - cache_identity is unique per live font and never reused within a
//     process, so raster caches may key on it.
// Ownership/Lifetime:
//   - The handle owns its vg_font face exclusively; borrowers (canvas text
//     caches) must not outlive the handle they were keyed from — the
//     identity key makes stale entries unreachable rather than dangling.
// Links: src/lib/gui/include/vg_font.h (face API),
//        src/runtime/graphics/3d/render/rt_canvas3d_overlay.c (renderer).
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

/// Runtime class identifier assigned to TtfFont object payloads.
#define RT_TTF_FONT_CLASS_ID INT64_C(-0x60020A)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque loaded font face (vg_font_t) — forward declared to keep the
///        runtime header free of GUI-library includes.
struct vg_font;

/// @brief Load a TrueType font from a file path.
/// @param path Borrowed runtime string naming a .ttf file (UTF-8).
/// @return A new GC-managed TtfFont handle, or NULL after a returning trap
///         when the file cannot be read or parsed.
void *rt_ttf_font_load(rt_string path);

/// @brief Load the embedded fallback face (JetBrains Mono Regular).
/// @return A new GC-managed TtfFont handle, or NULL after an allocation trap.
void *rt_ttf_font_load_default(void);

/// @brief Measure the pixel width of UTF-8 text at a pixel size.
/// @param obj Borrowed TtfFont handle.
/// @param text Borrowed runtime string to measure.
/// @param size_px Font size in pixels (clamped to a sane range).
/// @return Total advance width in pixels, or 0 for invalid input.
double rt_ttf_font_measure_width(void *obj, rt_string text, double size_px);

/// @brief Recommended line height at a pixel size.
/// @param obj Borrowed TtfFont handle.
/// @param size_px Font size in pixels (clamped to a sane range).
/// @return Line height in pixels, or 0 for invalid input.
double rt_ttf_font_line_height(void *obj, double size_px);

/// @brief Baseline ascent at a pixel size.
/// @param obj Borrowed TtfFont handle.
/// @param size_px Font size in pixels (clamped to a sane range).
/// @return Ascent in pixels (positive), or 0 for invalid input.
double rt_ttf_font_ascent(void *obj, double size_px);

/// @brief The face's family name ("JetBrains Mono", ...).
/// @param obj Borrowed TtfFont handle.
/// @return Owned runtime string (possibly empty), or NULL after a trap.
rt_string rt_ttf_font_family(void *obj);

/// @brief Borrow the underlying vg_font face for rendering.
/// @param obj Candidate TtfFont handle.
/// @return The live face, or NULL when @p obj is not a live TtfFont.
struct vg_font *rt_ttf_font_face(void *obj);

/// @brief Process-unique identity for raster-cache keys.
/// @param obj Candidate TtfFont handle.
/// @return Nonzero identity, or 0 when @p obj is not a live TtfFont.
int64_t rt_ttf_font_identity(void *obj);

/// @brief Clamp a caller-supplied pixel size into the supported range.
/// @param size_px Requested size.
/// @return The size clamped to [4, 256]; non-finite values become 16.
double rt_ttf_font_clamp_size(double size_px);

#ifdef __cplusplus
}
#endif
