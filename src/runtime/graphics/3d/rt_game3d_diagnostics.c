//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_diagnostics.c
// Purpose: Process-wide Game3D degradation diagnostics and summary formatting.
//
// Key invariants:
//   - Recording paths perform only bounded integer increments.
//   - Summary lines are emitted in a stable order and omitted for zero counters.
//
// Ownership/Lifetime:
//   - Counter storage is process-global for the lifetime of the process.
//   - Summary allocates and returns a runtime string owned by the caller.
//
// Links: src/runtime/graphics/3d/rt_game3d_diagnostics.h,
//   src/il/runtime/runtime.def, docs/zannalib/graphics/game3d.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements process-wide Game3D degradation and throughput diagnostics.
/// @details Producers record bounded event counts through the internal hooks in
///          rt_game3d_diagnostics.h. Public getters expose non-negative snapshots,
///          while Summary serializes only degradation counters in a stable
///          key-value order. Audio voice eviction storage remains owned by the
///          audio diagnostics module and is bridged here so Game3D presents one
///          consolidated diagnostic surface.

#include "rt_game3d_diagnostics.h"

#include "rt_audio_diagnostics.h"
#include "rt_platform.h"

#include <stdio.h>

/// @brief Process-lifetime storage for Game3D-owned diagnostic counters.
/// @details Counters saturate at INT64_MAX through diag_increment. Every field
///          is accessed atomically because producers include worker, streaming,
///          render, and audio-adjacent threads.
typedef struct {
    volatile int64_t broadphase_fallback_count;
    volatile int64_t ccd_clamped_frames;
    volatile int64_t ccd_clamped_bodies;
    volatile int64_t anim_events_dropped;
    volatile int64_t nav_grid_fallbacks;
    volatile int64_t stale_entity_calls;
    volatile int64_t stale_async_loads_dropped;
    volatile int64_t stream_staging_errors;
    volatile int64_t stream_stale_stages_dropped;
    /* EPA hit its polytope caps and reported a 0-depth contact: persistent counts
     * here usually mean an over-detailed convex hull collider (decimate it). */
    volatile int64_t epa_fallbacks;
    /* Shadow slots satisfied from their previous-frame depth (signature match). */
    volatile int64_t shadow_slots_reused;
    /* Opaque draws folded into auto-instanced batches. */
    volatile int64_t auto_instanced_draws;
} rt_game3d_diagnostics_state;

static rt_game3d_diagnostics_state g_game3d_diagnostics;

/// @brief Normalize a diagnostic counter for public observation.
/// @param value Raw counter value.
/// @return @p value when positive, otherwise zero.
static int64_t diag_nonnegative(int64_t value) {
    return value > 0 ? value : 0;
}

/// @brief Atomically observe and normalize one diagnostic counter.
/// @param counter Counter storage to read; NULL produces zero.
/// @return A non-negative snapshot of the counter.
static int64_t diag_load(const volatile int64_t *counter) {
    return counter ? diag_nonnegative(rt_atomic_load_i64(counter, __ATOMIC_ACQUIRE)) : 0;
}

/// @brief Add a positive amount to a counter without signed overflow.
/// @param[in,out] counter Counter to update; NULL is ignored.
/// @param amount Positive increment. Zero and negative amounts are ignored.
/// @post A successful update saturates at INT64_MAX.
static void diag_increment(volatile int64_t *counter, int64_t amount) {
    int64_t observed;
    if (!counter || amount <= 0)
        return;
    observed = rt_atomic_load_i64(counter, __ATOMIC_RELAXED);
    for (;;) {
        int64_t base = observed > 0 ? observed : 0;
        int64_t desired = base > INT64_MAX - amount ? INT64_MAX : base + amount;
        if (rt_atomic_compare_exchange_i64(
                counter, &observed, desired, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

/// @brief Append one positive `name=value` diagnostic line to a bounded buffer.
/// @details The function preserves NUL termination and appends only complete
///          lines. Zero-valued counters are intentionally omitted from summaries.
/// @param[in,out] buffer Destination character buffer.
/// @param capacity Total byte capacity of @p buffer, including the terminator.
/// @param[in,out] offset Current used length; advanced by bytes copied.
/// @param name Stable diagnostic property name.
/// @param value Counter value to append; non-positive values are ignored.
static void diag_append_line(
    char *buffer, size_t capacity, size_t *offset, const char *name, int64_t value) {
    char line[96];
    int written;
    if (!buffer || capacity == 0 || !offset || !name || value <= 0 || *offset >= capacity)
        return;
    written = snprintf(line, sizeof(line), "%s=%lld\n", name, (long long)value);
    if (written <= 0 || (size_t)written >= sizeof(line))
        return;
    if ((size_t)written >= capacity - *offset)
        return;
    for (int i = 0; i < written; ++i)
        buffer[*offset + (size_t)i] = line[i];
    *offset += (size_t)written;
    buffer[*offset] = '\0';
}

/// @brief Return the number of broadphase operations that used a fallback path.
/// @return The non-negative process-wide fallback count.
int64_t rt_game3d_diagnostics_get_broadphase_fallback_count(void) {
    return diag_load(&g_game3d_diagnostics.broadphase_fallback_count);
}

/// @brief Return how many frames clamped continuous-collision work.
/// @return The non-negative process-wide clamped-frame count.
int64_t rt_game3d_diagnostics_get_ccd_clamped_frames(void) {
    return diag_load(&g_game3d_diagnostics.ccd_clamped_frames);
}

/// @brief Return the accumulated number of bodies affected by CCD clamping.
/// @return The non-negative process-wide affected-body count.
int64_t rt_game3d_diagnostics_get_ccd_clamped_bodies(void) {
    return diag_load(&g_game3d_diagnostics.ccd_clamped_bodies);
}

/// @brief Return the number of animation events discarded by bounded queues.
/// @return The non-negative process-wide dropped-event count.
int64_t rt_game3d_diagnostics_get_anim_events_dropped(void) {
    return diag_load(&g_game3d_diagnostics.anim_events_dropped);
}

/// @brief Return the number of spatial audio voices evicted under pressure.
/// @return The audio module's non-negative process-wide eviction count.
int64_t rt_game3d_diagnostics_get_audio_voices_evicted(void) {
    return rt_audio_diagnostics_get_spatial_voice_evictions();
}

/// @brief Return the number of navigation queries that fell back from the grid.
/// @return The non-negative process-wide navigation fallback count.
int64_t rt_game3d_diagnostics_get_nav_grid_fallbacks(void) {
    return diag_load(&g_game3d_diagnostics.nav_grid_fallbacks);
}

/// @brief Return the number of API calls rejected for stale entity handles.
/// @return The non-negative process-wide stale-call count.
int64_t rt_game3d_diagnostics_get_stale_entity_calls(void) {
    return diag_load(&g_game3d_diagnostics.stale_entity_calls);
}

/// @brief Return the number of obsolete asynchronous asset results discarded.
/// @return The non-negative process-wide stale-load count.
int64_t rt_game3d_diagnostics_get_stale_async_loads_dropped(void) {
    return diag_load(&g_game3d_diagnostics.stale_async_loads_dropped);
}

/// @brief Return the number of streaming staging operations that failed.
/// @return The non-negative process-wide staging-error count.
int64_t rt_game3d_diagnostics_get_stream_staging_errors(void) {
    return diag_load(&g_game3d_diagnostics.stream_staging_errors);
}

/// @brief Return the number of obsolete prepared stream stages discarded.
/// @return The non-negative process-wide stale-stage count.
int64_t rt_game3d_diagnostics_get_stream_stale_stages_dropped(void) {
    return diag_load(&g_game3d_diagnostics.stream_stale_stages_dropped);
}

/// @brief Return the number of EPA contacts that exhausted polytope limits.
/// @return The non-negative process-wide EPA fallback count.
int64_t rt_game3d_diagnostics_get_epa_fallbacks(void) {
    return diag_load(&g_game3d_diagnostics.epa_fallbacks);
}

/// @brief Return the number of shadow slots reused from compatible prior frames.
/// @return The non-negative process-wide reuse count.
int64_t rt_game3d_diagnostics_get_shadow_slots_reused(void) {
    return diag_load(&g_game3d_diagnostics.shadow_slots_reused);
}

/// @brief Return the number of opaque draws folded into automatic instance batches.
/// @return The non-negative process-wide folded-draw count.
int64_t rt_game3d_diagnostics_get_auto_instanced_draws(void) {
    return diag_load(&g_game3d_diagnostics.auto_instanced_draws);
}

/// @brief Clear all Game3D and bridged spatial-audio diagnostic counters.
/// @post Every public diagnostic getter returns zero until another event is recorded.
void rt_game3d_diagnostics_reset(void) {
    rt_atomic_store_i64(&g_game3d_diagnostics.broadphase_fallback_count, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.ccd_clamped_frames, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.ccd_clamped_bodies, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.anim_events_dropped, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.nav_grid_fallbacks, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.stale_entity_calls, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.stale_async_loads_dropped, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.stream_staging_errors, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.stream_stale_stages_dropped, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.epa_fallbacks, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.shadow_slots_reused, 0, __ATOMIC_RELEASE);
    rt_atomic_store_i64(&g_game3d_diagnostics.auto_instanced_draws, 0, __ATOMIC_RELEASE);
    rt_audio_diagnostics_reset_spatial_voice_evictions();
}

/// @brief Format the current degradation counters as stable newline-delimited text.
/// @details Each positive degradation counter contributes one `Name=value` line
///          in API-defined order. Health/throughput counters such as shadow-slot
///          reuse and automatic instancing remain available through getters but
///          are excluded so a healthy run yields an empty summary.
/// @return A newly created runtime string containing the degradation digest, or
///         the canonical empty string when every degradation counter is zero.
rt_string rt_game3d_diagnostics_summary(void) {
    char buffer[1024];
    size_t offset = 0;
    buffer[0] = '\0';
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "BroadphaseFallbackCount",
                     rt_game3d_diagnostics_get_broadphase_fallback_count());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "CcdClampedFrames",
                     rt_game3d_diagnostics_get_ccd_clamped_frames());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "CcdClampedBodies",
                     rt_game3d_diagnostics_get_ccd_clamped_bodies());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "AnimEventsDropped",
                     rt_game3d_diagnostics_get_anim_events_dropped());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "AudioVoicesEvicted",
                     rt_audio_diagnostics_get_spatial_voice_evictions());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "NavGridFallbacks",
                     rt_game3d_diagnostics_get_nav_grid_fallbacks());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "StaleEntityCalls",
                     rt_game3d_diagnostics_get_stale_entity_calls());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "StaleAsyncLoadsDropped",
                     rt_game3d_diagnostics_get_stale_async_loads_dropped());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "StreamStagingErrors",
                     rt_game3d_diagnostics_get_stream_staging_errors());
    diag_append_line(buffer,
                     sizeof(buffer),
                     &offset,
                     "StreamStaleStagesDropped",
                     rt_game3d_diagnostics_get_stream_stale_stages_dropped());
    diag_append_line(
        buffer, sizeof(buffer), &offset, "EpaFallbacks", rt_game3d_diagnostics_get_epa_fallbacks());
    /* ShadowSlotsReused and AutoInstancedDraws are HEALTH/throughput counters,
     * not degradation: they are exposed only through their properties. Summary()
     * stays a pure degradation digest so smoke probes can keep asserting it is
     * empty on a clean run. */
    if (offset == 0)
        return rt_str_empty();
    return rt_string_from_bytes(buffer, offset);
}

/// @brief Record one broadphase fallback.
void rt_game3d_diag_record_broadphase_fallback(void) {
    diag_increment(&g_game3d_diagnostics.broadphase_fallback_count, 1);
}

/// @brief Record one frame that clamped continuous-collision processing.
/// @param affected_bodies Number of bodies affected in the frame; non-positive
///                        values do not change the body total.
void rt_game3d_diag_record_ccd_clamp(int64_t affected_bodies) {
    diag_increment(&g_game3d_diagnostics.ccd_clamped_frames, 1);
    diag_increment(&g_game3d_diagnostics.ccd_clamped_bodies, affected_bodies);
}

/// @brief Record animation events discarded by a bounded event queue.
/// @param count Number of dropped events; non-positive values are ignored.
void rt_game3d_diag_record_anim_events_dropped(int64_t count) {
    diag_increment(&g_game3d_diagnostics.anim_events_dropped, count);
}

/// @brief Record one EPA polytope-limit fallback.
void rt_game3d_diag_record_epa_fallback(void) {
    diag_increment(&g_game3d_diagnostics.epa_fallbacks, 1);
}

/// @brief Record one shadow slot satisfied from compatible prior-frame depth.
void rt_game3d_diag_record_shadow_slot_reused(void) {
    diag_increment(&g_game3d_diagnostics.shadow_slots_reused, 1);
}

/// @brief Record opaque draws folded into automatic instance batches.
/// @param count Number of folded draws; non-positive values are ignored.
void rt_game3d_diag_record_auto_instanced_draws(int64_t count) {
    diag_increment(&g_game3d_diagnostics.auto_instanced_draws, count);
}

/// @brief Record one spatial-audio voice eviction in the audio diagnostics store.
void rt_game3d_diag_record_audio_voice_evicted(void) {
    rt_audio_diag_record_spatial_voice_evicted();
}

/// @brief Record one navigation-grid fallback.
void rt_game3d_diag_record_nav_grid_fallback(void) {
    diag_increment(&g_game3d_diagnostics.nav_grid_fallbacks, 1);
}

/// @brief Record one API operation attempted through a stale entity handle.
void rt_game3d_diag_record_stale_entity_call(void) {
    diag_increment(&g_game3d_diagnostics.stale_entity_calls, 1);
}

/// @brief Record one obsolete asynchronous load result that was discarded.
void rt_game3d_diag_record_stale_async_load_dropped(void) {
    diag_increment(&g_game3d_diagnostics.stale_async_loads_dropped, 1);
}

/// @brief Record one streaming staging failure.
void rt_game3d_diag_record_stream_staging_error(void) {
    diag_increment(&g_game3d_diagnostics.stream_staging_errors, 1);
}

/// @brief Record one obsolete prepared stream stage that was discarded.
void rt_game3d_diag_record_stream_stale_stage_dropped(void) {
    diag_increment(&g_game3d_diagnostics.stream_stale_stages_dropped, 1);
}
