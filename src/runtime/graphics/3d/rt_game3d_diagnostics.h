//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_diagnostics.h
// Purpose: Process-wide degradation diagnostics for Game3D and Graphics3D
//   subsystems that may fall back to slower or lower-fidelity behavior.
//
// Key invariants:
//   - Counters are plain signed 64-bit totals and never allocate on record.
//   - Summary output is stable, newline-delimited, and empty when all counters
//     are zero.
//
// Ownership/Lifetime:
//   - Counter storage is process-global and owned by the runtime.
//   - Summary returns a caller-owned runtime string.
//
// Links: src/runtime/graphics/3d/rt_game3d_diagnostics.c,
//   src/il/runtime/runtime.def, docs/zannalib/graphics/game3d.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the process-wide Game3D diagnostic counters and recording hooks.
/// @details Runtime subsystems use the `rt_game3d_diag_record_*` hooks to report
///          bounded degradation or throughput events without allocating. The
///          public diagnostic surface reads or resets those totals and can format
///          the degradation subset as stable text. Counter storage is implemented
///          by rt_game3d_diagnostics.c, except spatial voice evictions, which are
///          delegated to the audio diagnostics module.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Return the number of broadphase operations that used a fallback path.
/// @return The non-negative process-wide fallback count.
int64_t rt_game3d_diagnostics_get_broadphase_fallback_count(void);

/// @brief Return how many frames clamped continuous-collision work.
/// @return The non-negative process-wide clamped-frame count.
int64_t rt_game3d_diagnostics_get_ccd_clamped_frames(void);

/// @brief Return the accumulated number of bodies affected by CCD clamping.
/// @return The non-negative process-wide affected-body count.
int64_t rt_game3d_diagnostics_get_ccd_clamped_bodies(void);

/// @brief Return the number of animation events discarded by bounded queues.
/// @return The non-negative process-wide dropped-event count.
int64_t rt_game3d_diagnostics_get_anim_events_dropped(void);

/// @brief Return the number of spatial audio voices evicted under pressure.
/// @return The audio module's non-negative process-wide eviction count.
int64_t rt_game3d_diagnostics_get_audio_voices_evicted(void);

/// @brief Return the number of navigation queries that fell back from the grid.
/// @return The non-negative process-wide navigation fallback count.
int64_t rt_game3d_diagnostics_get_nav_grid_fallbacks(void);

/// @brief Return the number of API calls rejected for stale entity handles.
/// @return The non-negative process-wide stale-call count.
int64_t rt_game3d_diagnostics_get_stale_entity_calls(void);

/// @brief Return the number of obsolete asynchronous asset results discarded.
/// @return The non-negative process-wide stale-load count.
int64_t rt_game3d_diagnostics_get_stale_async_loads_dropped(void);

/// @brief Process-wide count of streaming cell/tile staging failures (missing/corrupt payloads).
/// @return The non-negative process-wide staging-error count.
int64_t rt_game3d_diagnostics_get_stream_staging_errors(void);

/// @brief Process-wide count of worker-staged streaming payloads dropped as stale/undesired.
/// @return The non-negative process-wide stale-stage count.
int64_t rt_game3d_diagnostics_get_stream_stale_stages_dropped(void);

/// @brief Process-wide count of EPA polytope-cap fallbacks (0-depth contacts emitted).
/// @return The non-negative process-wide EPA fallback count.
int64_t rt_game3d_diagnostics_get_epa_fallbacks(void);

/// @brief Process-wide count of shadow slots reused from their previous-frame depth.
/// @return The non-negative process-wide reuse count.
int64_t rt_game3d_diagnostics_get_shadow_slots_reused(void);

/// @brief Process-wide count of opaque draws folded into auto-instanced batches.
/// @return The non-negative process-wide folded-draw count.
int64_t rt_game3d_diagnostics_get_auto_instanced_draws(void);

/// @brief Clear all Game3D and bridged spatial-audio diagnostic counters.
/// @post Every diagnostic getter returns zero until another event is recorded.
void rt_game3d_diagnostics_reset(void);

/// @brief Format all positive degradation counters as stable `Name=value` lines.
/// @details Health/throughput counters are intentionally omitted so a healthy
///          runtime produces an empty summary.
/// @return A caller-owned runtime string containing the digest, or the
///         canonical empty string when no degradation has been recorded.
rt_string rt_game3d_diagnostics_summary(void);

/// @brief Record one broadphase fallback.
void rt_game3d_diag_record_broadphase_fallback(void);

/// @brief Record one frame that clamped continuous-collision processing.
/// @param affected_bodies Number of bodies affected; non-positive values do
///                        not change the accumulated body count.
void rt_game3d_diag_record_ccd_clamp(int64_t affected_bodies);

/// @brief Record animation events discarded by a bounded event queue.
/// @param count Number of dropped events; non-positive values are ignored.
void rt_game3d_diag_record_anim_events_dropped(int64_t count);

/// @brief Record one spatial-audio voice eviction in the audio diagnostics store.
void rt_game3d_diag_record_audio_voice_evicted(void);

/// @brief Record one navigation-grid fallback.
void rt_game3d_diag_record_nav_grid_fallback(void);

/// @brief Record one API operation attempted through a stale entity handle.
void rt_game3d_diag_record_stale_entity_call(void);

/// @brief Record one obsolete asynchronous load result that was discarded.
void rt_game3d_diag_record_stale_async_load_dropped(void);

/// @brief Record a streaming staging failure (worker could not read/parse a payload).
void rt_game3d_diag_record_stream_staging_error(void);

/// @brief Record a worker-staged streaming payload dropped without being committed.
void rt_game3d_diag_record_stream_stale_stage_dropped(void);

/// @brief Record an EPA polytope-cap fallback (overlap reported with 0 depth).
void rt_game3d_diag_record_epa_fallback(void);

/// @brief Record a shadow slot satisfied from its previous-frame depth contents.
void rt_game3d_diag_record_shadow_slot_reused(void);

/// @brief Record @p count opaque draws folded into one auto-instanced batch.
/// @param count Number of folded draws; non-positive values are ignored.
void rt_game3d_diag_record_auto_instanced_draws(int64_t count);

#ifdef __cplusplus
}
#endif
