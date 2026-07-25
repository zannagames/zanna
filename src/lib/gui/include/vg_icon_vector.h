//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_icon_vector.h
// Purpose: Deterministic scalable vector icon library — named icons rendered
//          from compact fixed-point path tables with anti-aliased integer
//          scanline fills and an LRU coverage-mask cache.
// Key invariants:
//   - All rasterization math is fixed point (Q16); output is bit-identical
//     across platforms for identical inputs (vg_draw determinism contract).
//   - Coverage masks are tint-independent; color applies at blit time.
//   - Icon names are stable kebab-case identifiers (ADR 0137).
// Ownership/Lifetime:
//   - Icon tables are static const data; the cache owns its masks and evicts
//     least-recently-used entries under pressure.
// Links: lib/gui/src/core/vg_icon_vector.c, docs/adr/0137-premium-rendering-surface.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vgfx.h"
#include <stdint.h>

/// @file
/// @brief Declares lookup, rendering, and cache control for deterministic vector icons.
/// @details Stable kebab-case names resolve to compact fixed-point path definitions.
/// Rasterized tint-independent coverage masks are cached by icon and size, then composited
/// into a graphics window with the requested role color.

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Invalid icon id returned by failed lookups.
#define VG_ICON_VECTOR_INVALID (-1)

/// @brief Resolve a stable icon name (e.g. "file-zia") to an icon id.
/// @param name NUL-terminated kebab-case icon name; NULL/unknown return
///             VG_ICON_VECTOR_INVALID.
/// @return Icon id usable with vg_icon_vector_draw, or VG_ICON_VECTOR_INVALID.
int32_t vg_icon_vector_find(const char *name);

/// @brief Return the stable name for an icon id, or NULL when out of range.
/// @param icon_id Registered icon id to query.
/// @return Process-lifetime kebab-case name, or NULL for an invalid id.
const char *vg_icon_vector_name(int32_t icon_id);

/// @brief Return the number of registered vector icons.
/// @return Non-negative number of valid icon ids.
int32_t vg_icon_vector_count(void);

/// @brief Draw a vector icon with anti-aliased coverage into a window target.
/// @details Renders (and caches) the icon's coverage masks at @p size_px and
///          composites them at (x, y) via vgfx_pset_alpha. Single-role icons
///          tint with @p tint_rgb; multi-role icons (the brand mark) resolve
///          their fixed role palette internally and ignore the tint for
///          those subpaths. Out-of-range ids and non-positive sizes are
///          no-ops.
/// @param win Destination window target (mock backend included).
/// @param icon_id Icon id from vg_icon_vector_find.
/// @param x Left edge in target pixels.
/// @param y Top edge in target pixels.
/// @param size_px Icon box edge length in pixels (icons are square).
/// @param tint_rgb Packed 0x00RRGGBB tint for role-0 subpaths.
void vg_icon_vector_draw(vgfx_window_t win,
                         int32_t icon_id,
                         int32_t x,
                         int32_t y,
                         int32_t size_px,
                         uint32_t tint_rgb);

/// @brief Drop every cached coverage mask (e.g. between pixel-hash tests).
/// @details Static icon definitions remain available; the next draw lazily rasterizes its
/// requested icon and size again.
void vg_icon_vector_cache_clear(void);

#ifdef __cplusplus
}
#endif
