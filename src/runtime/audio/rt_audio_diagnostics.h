//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_diagnostics.h
// Purpose: Audio-domain degradation diagnostics counters.
//
// Key invariants:
//   - Recording paths are bounded integer increments and allocate no memory.
//   - Counters saturate at INT64_MAX instead of overflowing.
//
// Ownership/Lifetime:
//   - Counter storage is process-global and owned by the runtime.
//
// Links: src/runtime/audio/rt_audio_diagnostics.c,
//        src/runtime/audio/rt_sound3d.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares allocation-free counters for audio degradation diagnostics.
/// @details The runtime owns process-global saturating counter storage. Public
///          consumers can sample and reset the spatial-voice eviction count,
///          while internal capacity-management paths record eviction events.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Number of spatial voices evicted since process start or last reset.
/// @return Non-negative saturating eviction count.
int64_t rt_audio_diagnostics_get_spatial_voice_evictions(void);

/// @brief Reset the spatial-voice eviction counter to zero.
/// @details Does not otherwise alter active voices or spatial-audio state.
void rt_audio_diagnostics_reset_spatial_voice_evictions(void);

/// @brief Record one spatial-voice eviction (saturates at INT64_MAX).
/// @details Intended for internal bounded-capacity paths; performs no allocation.
void rt_audio_diag_record_spatial_voice_evicted(void);

#ifdef __cplusplus
}
#endif
