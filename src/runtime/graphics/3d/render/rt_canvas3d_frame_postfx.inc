//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_frame_postfx.inc
// Purpose: Latch Canvas3D post-processing state and derive per-frame motion-data needs.
// Key invariants:
//   - Included only by rt_canvas3d.c; this is not a standalone translation unit.
//   - Post-processing state is snapshotted once at frame begin.
//   - Incapable backends leave GPU post-FX disabled. Render-target frames never
//     PRESENT through the GPU post-FX route but still PUBLISH the frame's chain
//     snapshot so the backend can resolve the bound target through it (ADR 0301).
// Ownership/Lifetime:
//   - The Canvas3D frame owns the copied effect descriptors through presentation.
//   - Backend callbacks borrow the latched chain and must not retain its address.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h, vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements per-frame Canvas3D GPU post-processing state latching.
/// @details The helpers copy mutable post-processing configuration into frame
///   storage before drawing begins, publish that stable snapshot to capable
///   backends, and detect effects that require previous-transform data.

/// @brief Push the current frame's post-FX enable flag + snapshot to the backend.
///
/// Called by `latch_gpu_postfx_state` after capturing a snapshot.
/// Backends that don't implement these hooks silently no-op.
/// @param c Borrowed Canvas3D whose already-latched state is published. A
///   `NULL` canvas or missing backend is ignored.
static void canvas3d_apply_gpu_postfx_state(rt_canvas3d *c) {
    const vgfx3d_postfx_chain_t *chain = NULL;

    if (!c || !c->backend)
        return;
    if (c->frame_postfx_chain_latched)
        chain = &c->frame_postfx_chain;
    /* ADR 0301: the present-route toggle belongs to WINDOW frames only. A
     * render-target frame used to call it with 0 and the next window frame
     * with 1 — on Metal each flip commits pending work and reallocates every
     * main target (scene/overlay/post-FX/bloom/TAA), and the NULL snapshot that
     * followed cleared TAA history, so an interleaved RT frame cost two
     * target rebuilds per window frame. The chain snapshot is published on
     * every frame so a bound target can be resolved through it. */
    if (!c->render_target && c->backend->set_gpu_postfx_enabled)
        c->backend->set_gpu_postfx_enabled(c->backend_ctx, c->frame_gpu_postfx_enabled);
    if (c->backend->set_gpu_postfx_snapshot)
        c->backend->set_gpu_postfx_snapshot(c->backend_ctx, chain);
}

/// @brief Capture the current post-FX state into a per-frame snapshot.
///
/// Snapshotting once per frame ensures the post-FX parameters used
/// by the present-time composite match what was active at frame start
/// (avoids tearing if the user toggles a post-FX setting mid-frame).
/// Skips snapshot capture for non-postfx backends. Render-target frames capture
/// and publish the chain (ADR 0301) but never enable the window present route.
/// @param c Borrowed Canvas3D whose frame snapshot is reset and populated. A
///   `NULL` canvas is ignored.
static void canvas3d_latch_gpu_postfx_state(rt_canvas3d *c) {
    if (!c)
        return;
    vgfx3d_postfx_chain_reset(&c->frame_postfx_chain);
    c->frame_gpu_postfx_enabled = 0;
    c->frame_postfx_chain_latched = 0;
    c->frame_postfx_state_latched = 1;
    if (canvas3d_backend_accepts_gpu_postfx_chain(c) &&
        vgfx3d_postfx_get_chain(c->postfx, &c->frame_postfx_chain)) {
        c->frame_postfx_chain_latched = 1;
        if (canvas3d_backend_uses_gpu_postfx(c))
            c->frame_gpu_postfx_enabled = 1;
    }
    canvas3d_apply_gpu_postfx_state(c);
}

/// @brief True when this latched post-FX frame requires previous transforms.
/// @details Motion blur and temporal anti-aliasing consume motion data only
///   when their corresponding latched effect descriptor is enabled.
/// @param c Borrowed Canvas3D containing the immutable per-frame effect chain.
/// @return Non-zero when an enabled motion-blur or TAA effect needs motion
///   vectors; zero for disabled post-FX, other effects, or a `NULL` canvas.
static int canvas3d_frame_needs_motion_vectors(const rt_canvas3d *c) {
    if (!c || !c->frame_gpu_postfx_enabled || !c->frame_postfx_chain.enabled)
        return 0;
    for (int32_t i = 0; i < c->frame_postfx_chain.effect_count; ++i) {
        const vgfx3d_postfx_effect_desc_t *effect = &c->frame_postfx_chain.effects[i];
        if (effect->type == VGFX3D_POSTFX_EFFECT_MOTION_BLUR &&
            effect->snapshot.motion_blur_enabled)
            return 1;
        if (effect->type == VGFX3D_POSTFX_EFFECT_TAA && effect->snapshot.taa_enabled)
            return 1;
    }
    return 0;
}
