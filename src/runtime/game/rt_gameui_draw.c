//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_gameui_draw.c
/// @file
/// @brief Resolves polymorphic GameUI drawing operations for canvas handles.
//
// Purpose: Resolve a canvas handle into the Game.UI draw-operation table:
//   direct 2D-canvas bindings, plus the Canvas3D binding registered by the
//   graphics/3d layer at Canvas3D creation (ADR 0065).
// Key invariants:
//   - The 2D binding points straight at the rt_canvas_* primitives, so the
//     adapter refactor is behavior-identical for 2D callers.
//   - Unknown handles resolve to 0 and widgets draw nothing (matches the old
//     rt_canvas_is_handle guard behavior).
// Ownership/Lifetime:
//   - Registered hooks are process-global function pointers set once by
//     Canvas3D; no allocation or teardown.
// Links: rt_gameui_draw.h, misc/plans/3d_overhaul/08-ui-toolkit-3d.md
//
//===----------------------------------------------------------------------===//
#include "rt_gameui_draw.h"

#include "rt_graphics.h"

#include <stddef.h>

extern void rt_canvas_text_font(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t color);
extern void rt_canvas_text_font_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t scale, int64_t color);

/// @brief Process-global predicate for the optional Canvas3D adapter.
static int8_t (*g_canvas3d_probe)(void *canvas) = NULL;
/// @brief Process-global operation-table filler paired with g_canvas3d_probe.
static void (*g_canvas3d_fill)(void *canvas, rt_gameui_draw_ops_t *ops) = NULL;

/// @brief Register the Canvas3D probe + ops-fill hooks (called by Canvas3D at init).
/// @param probe Predicate returning nonzero for handles accepted by @p fill.
/// @param fill Callback that populates every operation for an accepted handle.
/// @details Stores the pointers verbatim. Later calls replace both hooks, and
///          null pointers can disable Canvas3D resolution. Registration
///          allocates nothing and performs no synchronization.
void rt_gameui_register_canvas3d_ops(int8_t (*probe)(void *canvas),
                                     void (*fill)(void *canvas, rt_gameui_draw_ops_t *ops)) {
    g_canvas3d_probe = probe;
    g_canvas3d_fill = fill;
}

/// @brief Fill @p ops for @p canvas (2D Canvas or a registered Canvas3D).
/// @param canvas Candidate canvas handle.
/// @param ops Required caller-owned table populated on success.
/// @return `1` for a recognized 2D Canvas or accepted Canvas3D; otherwise `0`.
/// @details Null arguments and unknown handles return zero without modifying a
///          complete table. The built-in 2D path binds directly to rt_canvas_*
///          functions. The optional 3D path runs the registered probe and, on a
///          nonzero result, delegates population to its paired fill callback.
int rt_gameui_resolve_draw_ops(void *canvas, rt_gameui_draw_ops_t *ops) {
    if (!canvas || !ops)
        return 0;
    if (rt_canvas_is_handle(canvas)) {
        ops->canvas = canvas;
        ops->box = rt_canvas_box;
        ops->box_alpha = rt_canvas_box_alpha;
        ops->frame = rt_canvas_frame;
        ops->line = rt_canvas_line;
        ops->round_box = rt_canvas_round_box;
        ops->round_frame = rt_canvas_round_frame;
        ops->text = rt_canvas_text;
        ops->text_scaled = rt_canvas_text_scaled;
        ops->text_font = rt_canvas_text_font;
        ops->text_font_scaled = rt_canvas_text_font_scaled;
        ops->blit_region = rt_canvas_blit_region;
        ops->width = rt_canvas_width;
        ops->height = rt_canvas_height;
        return 1;
    }
    if (g_canvas3d_probe && g_canvas3d_fill && g_canvas3d_probe(canvas)) {
        g_canvas3d_fill(canvas, ops);
        return 1;
    }
    return 0;
}
