//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_diagnostics.c
// Purpose: Audio-domain degradation diagnostics counter storage.
//
// Key invariants:
//   - Recording paths are bounded integer increments and allocate no memory.
//   - Counters saturate at INT64_MAX instead of overflowing.
//
// Ownership/Lifetime:
//   - Counter storage is process-global and owned by the runtime.
//
// Links: src/runtime/audio/rt_audio_diagnostics.h,
//        src/runtime/audio/rt_sound3d.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Stores process-wide counters for degraded audio-runtime behavior.
/// @details The current diagnostic records spatial-voice evictions using
///          allocation-free saturating arithmetic. Callers may sample or reset
///          the counter through the public diagnostics ABI.

#include "rt_audio_diagnostics.h"
#include "rt_platform.h"

#include <stdint.h>

/// @brief Cumulative number of spatial voices evicted by capacity pressure.
/// @details Saturates at `INT64_MAX`; storage is owned for the process lifetime.
static volatile int64_t g_spatial_voice_evictions = 0;

/// @brief Read the spatial-voice eviction counter.
/// @return Current non-negative count. A negative stored value, if introduced
///         by memory corruption or unsupported direct mutation, is reported as
///         zero.
int64_t rt_audio_diagnostics_get_spatial_voice_evictions(void) {
    int64_t value = rt_atomic_load_i64(&g_spatial_voice_evictions, __ATOMIC_ACQUIRE);
    return value > 0 ? value : 0;
}

/// @brief Reset the cumulative spatial-voice eviction counter.
/// @details Subsequent reads return zero until another eviction is recorded.
void rt_audio_diagnostics_reset_spatial_voice_evictions(void) {
    rt_atomic_store_i64(&g_spatial_voice_evictions, 0, __ATOMIC_RELEASE);
}

/// @brief Record one spatial-voice eviction.
/// @details Increments the process-wide counter unless it has already reached
///          `INT64_MAX`, in which case the saturated value is preserved.
void rt_audio_diag_record_spatial_voice_evicted(void) {
    int64_t observed = rt_atomic_load_i64(&g_spatial_voice_evictions, __ATOMIC_RELAXED);
    for (;;) {
        int64_t desired;
        if (observed >= INT64_MAX)
            return;
        desired = observed > 0 ? observed + 1 : 1;
        if (rt_atomic_compare_exchange_i64(
                &g_spatial_voice_evictions, &observed, desired, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}
