//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_intern.c
// Purpose: Implements the process-global canonicalization table that maps
//          equal runtime byte strings to a shared rt_string pointer.
//
// Key invariants:
//   - The table uses open-addressing with linear probing and FNV-1a (64-bit)
//     hashing; capacity is always a power of two for fast modular arithmetic.
//   - The table grows (doubles capacity, rehashes) when load > 5/8 (62.5%);
//     allocation failure leaves the existing slots and references unchanged.
//   - Interned strings are retained (rt_string_ref) and become effectively
//     immortal until rt_string_intern_drain() releases all entries.
//   - rt_string_interned_eq(a, b) reduces to a == b (pointer equality) for
//     any two strings that have been interned.
//   - Intern calls are serialized by a Windows critical section or POSIX mutex.
//     Drain is a shutdown/reset operation and must not race with intern calls.
//
// Ownership/Lifetime:
//   - The slot array is heap-allocated (calloc/free) and resized on growth;
//     the old array is freed after rehashing.
//   - Each interned rt_string has its refcount incremented by one; the table
//     holds that reference until drain.
//   - Successful table hits/inserts return an additional owned reference. If
//     initial table allocation fails, the input pointer is returned as-is with
//     no new retain.
//
// Links: src/runtime/core/rt_string_intern.h (public API),
//        src/runtime/core/rt_string.h (rt_string ref-counting),
//        src/runtime/core/rt_string_ops.c (string operations)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Thread-serialized global canonical runtime string table.

#include "rt_string_intern.h"

#include "rt_internal.h" // struct rt_string_impl (data, literal_len fields)
#include "rt_string.h"   // rt_string_ref, rt_string_unref, rt_str_len
#include "rt_trap.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "../text/rt_hash_util.h"

/// @brief Install a non-local recovery target for traps on the current thread.
/// @param buf Borrowed jump buffer that remains live until recovery is cleared.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Remove the current thread's installed trap recovery target.
void rt_trap_clear_recovery(void);

/// @brief Borrow the most recent trap diagnostic for the current thread.
/// @return Borrowed diagnostic string, or an empty/null pointer when absent.
const char *rt_trap_get_error(void);

/// @brief Hash a byte sequence using FNV-1a.
/// @param data Borrowed byte span.
/// @param len Number of bytes to hash.
/// @return Deterministic 64-bit FNV-1a hash.
static uint64_t hash_bytes(const char *data, size_t len) {
    return rt_fnv1a(data, len);
}

/// @brief Snapshot the current trap error message into @p buffer (or
///        @p fallback if none) so it survives lock cleanup before re-raise.
/// @param buffer Writable destination for a NUL-terminated diagnostic.
/// @param buffer_size Total capacity of @p buffer.
/// @param fallback Borrowed message used when no non-empty trap error exists.
static void intern_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

// ============================================================================
// Hash table internals
// ============================================================================

/// @brief One open-addressed entry in the intern table.
/// @details A null @ref InternSlot::str marks an unused slot; entries are never
///          individually deleted, so tombstone state is unnecessary.
typedef struct {
    uint64_t hash; ///< Cached hash to avoid recomputing on probe.
    rt_string str; ///< Retained canonical string; NULL = empty slot.
} InternSlot;

#define INTERN_INIT_CAP 256u ///< Initial capacity (must be a power of two).

static InternSlot *g_slots_ = NULL;
static size_t g_cap_ = 0;
static size_t g_count_ = 0;

#ifdef _WIN32
static INIT_ONCE g_lock_once_ = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock_;

/// @brief One-time initializer (InitOnceExecuteOnce callback) that creates the
///        Windows critical section guarding the intern table.
/// @param InitOnce Windows one-time initialization token.
/// @param Param Unused caller parameter.
/// @param Ctx Unused context output location.
/// @return `TRUE` after initialization, or `FALSE` when native allocation fails.
static BOOL CALLBACK intern_lock_init_callback(PINIT_ONCE InitOnce, PVOID Param, PVOID *Ctx) {
    (void)InitOnce;
    (void)Param;
    (void)Ctx;
    return InitializeCriticalSectionEx(&g_lock_, 0, 0);
}

/// @brief Acquire the global intern-table lock (lazily initializing it on the
///        Windows path; the POSIX path uses a static-initialized mutex).
/// @details Returns with exclusive ownership of the table state.
static void intern_lock(void) {
    if (!InitOnceExecuteOnce(&g_lock_once_, intern_lock_init_callback, NULL, NULL))
        rt_abort("String intern: lock initialization failed");
    EnterCriticalSection(&g_lock_);
}

/// @brief Release the global intern-table lock.
/// @details The calling thread must currently own @c g_lock_.
static void intern_unlock(void) {
    LeaveCriticalSection(&g_lock_);
}
#else
static pthread_mutex_t g_lock_ = PTHREAD_MUTEX_INITIALIZER;

/// @brief Acquire the global intern-table lock (POSIX mutex variant).
/// @details Returns after `pthread_mutex_lock` grants exclusive table access;
///          the platform return code is intentionally not surfaced.
static void intern_lock(void) {
    pthread_mutex_lock(&g_lock_);
}

/// @brief Release the global intern-table lock (POSIX mutex variant).
/// @details The calling thread must currently own @c g_lock_; the platform
///          return code is intentionally not surfaced.
static void intern_unlock(void) {
    pthread_mutex_unlock(&g_lock_);
}
#endif

/// @brief Grow and rehash the table when load factor exceeds 5/8.
/// @details Initializes an absent table at @ref INTERN_INIT_CAP or doubles an
///          existing power-of-two capacity once its threshold is reached.
///          Every live entry is reinserted by cached hash without changing its
///          retained string reference. Allocation failure or an unrepresentable
///          doubled capacity leaves all globals unchanged.
/// @pre The caller holds the intern-table lock.
static void intern_ensure_capacity(void) {
    // Grow at 5/8 load factor, using division-first arithmetic to avoid
    // overflow when the table is near the address-space limit.
    if (g_cap_ > 0) {
        if (g_count_ < (g_cap_ / 8) * 5)
            return;
        if (g_cap_ > SIZE_MAX / 2)
            return;
    }

    size_t new_cap = g_cap_ ? g_cap_ * 2 : INTERN_INIT_CAP;
    InternSlot *new_slots = (InternSlot *)calloc(new_cap, sizeof(InternSlot));
    if (!new_slots)
        return; // out of memory — leave table at high load

    // Rehash all live entries into the new table.
    for (size_t i = 0; i < g_cap_; i++) {
        if (!g_slots_[i].str)
            continue;

        size_t slot = (size_t)(g_slots_[i].hash & (new_cap - 1));
        while (new_slots[slot].str)
            slot = (slot + 1) & (new_cap - 1);

        new_slots[slot] = g_slots_[i];
    }

    free(g_slots_);
    g_slots_ = new_slots;
    g_cap_ = new_cap;
}

// ============================================================================
// Public API
// ============================================================================

/// @brief Intern @p s, returning the canonical `rt_string` for its byte content.
/// @details Hashes the complete stored byte span, then probes by cached hash,
///          byte length, and `memcmp`. A hit retains the existing canonical
///          handle. A miss retains @p s once for the table and again for the
///          returned caller claim. Retain traps are caught long enough to clear
///          recovery state, release the lock, undo a partial table retain, and
///          re-raise the saved diagnostic.
///
///          If initial slot allocation fails, this function returns @p s
///          unchanged and unretained; it is not canonicalized. A null input
///          returns NULL without locking.
/// @param s Borrowed live string to canonicalize; may be NULL.
/// @return Owned canonical reference on table success, the unchanged input
///         claim if initial allocation fails, or `NULL` after a returning
///         retain trap.
rt_string rt_string_intern(rt_string s) {
    if (!s)
        return NULL;

    const char *data = s->data;
    size_t len = (size_t)rt_str_len(s);
    uint64_t h = hash_bytes(data, len);

    intern_lock();
    intern_ensure_capacity();

    if (!g_slots_ || g_cap_ == 0) {
        intern_unlock();
        return s; // Table allocation failed — return input as-is (not interned)
    }

    size_t slot = (size_t)(h & (g_cap_ - 1));
    for (;;) {
        InternSlot *e = &g_slots_[slot];

        if (!e->str) {
            // Empty slot: insert s as the canonical copy.
            rt_string volatile table_ref = NULL;
            rt_string result = NULL;
            jmp_buf recovery;
            rt_trap_set_recovery(&recovery);
            if (setjmp(recovery) != 0) {
                char saved_error[256];
                intern_save_trap_error(
                    saved_error, sizeof(saved_error), "rt_string_intern: retain failed");
                rt_trap_clear_recovery();
                intern_unlock();
                if (table_ref)
                    rt_string_unref((rt_string)table_ref);
                rt_trap(saved_error);
                return NULL;
            }
            table_ref = rt_string_ref(s); // table holds one reference
            result = rt_string_ref(s);    // caller's reference
            rt_trap_clear_recovery();

            e->hash = h;
            e->str = (rt_string)table_ref;
            g_count_++;
            intern_unlock();
            return result;
        }

        if (e->hash == h) {
            size_t entry_len = (size_t)rt_str_len(e->str);
            if (entry_len == len && memcmp(e->str->data, data, len) == 0) {
                // Hit: return a retained reference to the canonical string.
                rt_string result = NULL;
                jmp_buf recovery;
                rt_trap_set_recovery(&recovery);
                if (setjmp(recovery) != 0) {
                    char saved_error[256];
                    intern_save_trap_error(
                        saved_error, sizeof(saved_error), "rt_string_intern: retain failed");
                    rt_trap_clear_recovery();
                    intern_unlock();
                    rt_trap(saved_error);
                    return NULL;
                }
                result = rt_string_ref(e->str);
                rt_trap_clear_recovery();
                intern_unlock();
                return result;
            }
        }

        slot = (slot + 1) & (g_cap_ - 1);
    }
}

/// @brief Release every interned string and free the intern-table storage.
/// @details Under the table lock, drops each canonical table reference, frees
///          slots, and zeros capacity/count state. On Windows it then destroys
///          the critical section and resets one-time initialization so a later
///          intern call can recreate it; the POSIX mutex remains initialized.
/// @warning This is a shutdown/reset operation. No other thread may be
///          interning or attempting to enter the table during drain.
void rt_string_intern_drain(void) {
    intern_lock();

    for (size_t i = 0; i < g_cap_; i++) {
        if (g_slots_[i].str) {
            rt_string_unref(g_slots_[i].str);
            g_slots_[i].str = NULL;
        }
    }

    free(g_slots_);
    g_slots_ = NULL;
    g_cap_ = 0;
    g_count_ = 0;

    intern_unlock();

#ifdef _WIN32
    DeleteCriticalSection(&g_lock_);
    g_lock_once_ = (INIT_ONCE)INIT_ONCE_STATIC_INIT;
#endif
}
