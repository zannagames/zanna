//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_surfaces.c
// Purpose: Game3D.Surfaces — a process-global name <-> id registry for
//   physics surface tags (plan 20). Footsteps, impact VFX, and decal tables
//   key on the stable session-scoped ids; data files persist names.
// Key invariants:
//   - Register is idempotent; ids are stable from 1 for the process lifetime.
//   - Thread-safe via the platform once/mutex init pattern (CONC rules).
// Ownership/Lifetime:
//   - Names are copied into process-global storage; never freed (bounded 255).
// Links: misc/plans/thirdpersonupgrade/20-physics-materials.md, ADR 0091.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the thread-safe process-wide Game3D surface-name registry.
/// @details The registry assigns stable one-based session identifiers to
/// canonicalized, case-sensitive surface names. Windows and POSIX adapters use
/// their native once/mutex primitives while exposing identical locking semantics.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_game3d.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GAME3D_SURFACES_MAX 255
#define GAME3D_SURFACE_NAME_MAX 64

static char g_surface_names[GAME3D_SURFACES_MAX][GAME3D_SURFACE_NAME_MAX];
static int32_t g_surface_count;

#if RT_PLATFORM_WINDOWS
#include <windows.h>
static CRITICAL_SECTION g_surfaces_lock;
static INIT_ONCE g_surfaces_once = INIT_ONCE_STATIC_INIT;

/// @brief Initialize the Windows critical section exactly once.
/// @param once Windows one-time initialization token.
/// @param param Optional initialization parameter, unused.
/// @param ctx Optional initialization context destination, unused.
/// @return `TRUE` to mark one-time initialization successful.
static BOOL CALLBACK surfaces_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_surfaces_lock);
    return TRUE;
}

/// @brief Ensure the Windows registry mutex exists and acquire it.
static void surfaces_lock(void) {
    InitOnceExecuteOnce(&g_surfaces_once, surfaces_init_once, NULL, NULL);
    EnterCriticalSection(&g_surfaces_lock);
}

/// @brief Release the Windows registry mutex.
static void surfaces_unlock(void) {
    LeaveCriticalSection(&g_surfaces_lock);
}
#else
#include <pthread.h>
static pthread_mutex_t g_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

/// @brief Acquire the POSIX registry mutex.
static void surfaces_lock(void) {
    pthread_mutex_lock(&g_surfaces_lock);
}

/// @brief Release the POSIX registry mutex.
static void surfaces_unlock(void) {
    pthread_mutex_unlock(&g_surfaces_lock);
}
#endif

/// @brief Copy a public surface name into the registry's canonical fixed-width form.
/// @details Surface names are documented as truncating to 63 bytes. Canonicalizing before both
///   insertion and lookup keeps Register idempotent for long names and makes IdOf agree with
///   NameOf. The registry stores bytes rather than interpreting UTF-8, matching rt_string_cstr.
/// @param name Null-terminated source name, or `NULL` for the empty canonical form.
/// @param[out] canonical Fixed-width destination receiving at most 63 bytes plus a terminator.
static void surfaces_canonicalize_name(const char *name, char canonical[GAME3D_SURFACE_NAME_MAX]) {
    size_t length = name ? strlen(name) : 0u;
    if (length >= GAME3D_SURFACE_NAME_MAX)
        length = GAME3D_SURFACE_NAME_MAX - 1u;
    if (length > 0u)
        memcpy(canonical, name, length);
    canonical[length] = '\0';
}

/// @brief Register (or look up) a surface name; ids are stable from 1.
/// @details Idempotent; names are case-sensitive and truncated at 63 bytes.
///   Traps past the 255-surface budget (a data error, not a runtime state).
/// @param name Borrowed non-empty runtime string to canonicalize and register.
/// @return Stable one-based surface identifier, or zero after invalid input or budget exhaustion.
int64_t rt_game3d_surfaces_register(rt_string name) {
    const char *cname = name ? rt_string_cstr(name) : NULL;
    char canonical[GAME3D_SURFACE_NAME_MAX];
    if (!cname || !*cname) {
        rt_trap("Game3D.Surfaces.Register: name must not be empty");
        return 0;
    }
    surfaces_canonicalize_name(cname, canonical);
    surfaces_lock();
    for (int32_t i = 0; i < g_surface_count; ++i) {
        if (strcmp(g_surface_names[i], canonical) == 0) {
            surfaces_unlock();
            return i + 1;
        }
    }
    if (g_surface_count >= GAME3D_SURFACES_MAX) {
        surfaces_unlock();
        rt_trap("Game3D.Surfaces.Register: surface budget (255) exceeded");
        return 0;
    }
    int32_t index = g_surface_count++;
    memcpy(g_surface_names[index], canonical, sizeof(canonical));
    surfaces_unlock();
    return index + 1;
}

/// @brief Name for a surface id, or "" when unknown.
/// @param id One-based surface identifier.
/// @return Runtime string for the canonical name, or the shared empty string when unknown.
rt_string rt_game3d_surfaces_name_of(int64_t id) {
    const char *result = NULL;
    surfaces_lock();
    if (id >= 1 && id <= g_surface_count)
        result = g_surface_names[id - 1];
    surfaces_unlock();
    return result ? rt_const_cstr(result) : rt_str_empty();
}

/// @brief Id for a surface name, or 0 when unregistered.
/// @param name Borrowed runtime string canonicalized before lookup.
/// @return Stable one-based identifier, or zero when empty or unregistered.
int64_t rt_game3d_surfaces_id_of(rt_string name) {
    const char *cname = name ? rt_string_cstr(name) : NULL;
    char canonical[GAME3D_SURFACE_NAME_MAX];
    int64_t id = 0;
    if (!cname || !*cname)
        return 0;
    surfaces_canonicalize_name(cname, canonical);
    surfaces_lock();
    for (int32_t i = 0; i < g_surface_count; ++i) {
        if (strcmp(g_surface_names[i], canonical) == 0) {
            id = i + 1;
            break;
        }
    }
    surfaces_unlock();
    return id;
}

/// @brief Number of registered surfaces.
/// @return Current process-wide surface count in the range zero through 255.
int64_t rt_game3d_surfaces_count(void) {
    surfaces_lock();
    int64_t count = g_surface_count;
    surfaces_unlock();
    return count;
}

#else
typedef int rt_game3d_surfaces_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
