//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_type_registry.c
// Purpose: Implements the runtime type registry that stores class and interface
//          metadata for Zanna's OOP features. Supports class registration with
//          vtables, interface registration with slot counts, interface binding
//          (itable association), and type-ID-based inheritance checks.
//
// Key invariants:
//   - Classes are registered before their derived classes; base lookups succeed
//     only when the base has already been registered.
//   - Virtual method slots 0-2 are reserved for Object.ToString/Equals/GetHashCode.
//   - Type IDs are unique non-negative integers assigned at registration time.
//   - Interface binding associates an itable (function pointer array) with a
//     class-interface pair; dispatch uses this for interface method calls.
//   - The registry is per-VM-context; each context has its own isolated copy.
//   - A reader-writer lock protects concurrent access until explicit sealing;
//     sealing rejects subsequent writes while reads continue using the lock.
//
// Ownership/Lifetime:
//   - Registry arrays and metadata allocated by direct registration live in
//     the active runtime context and are released by cleanup.
//   - Aggregate descriptors and C-string names are borrowed; runtime-string
//     bridge names are copied and owned by the registry.
//   - Vtable and itable arrays are caller-owned static data; the registry
//     stores pointers without copying.
//
// Links: src/runtime/oop/rt_oop.h (public API),
//        src/runtime/oop/rt_oop_dispatch.c (vtable lookup using registry data),
//        src/runtime/oop/rt_object.h (object type-tag layout)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_type_registry.c
 * @brief Implements the per-context class and interface metadata registry.
 * @details The registry records compiler-emitted class descriptors, vtables,
 *          interface slot metadata, and class-to-interface itable bindings;
 *          answers type and inheritance queries; and enforces synchronized
 *          registration followed by immutable sealed operation and cleanup.
 */

#include "rt_context.h"
#include "rt_context_internal.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_oop.h"

#include "rt_atomic_compat.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

// ============================================================================
// Reader-Writer Lock Helpers
// ============================================================================

/// @brief Initialize the per-registry reader-writer lock transactionally.
/// @param st Empty type-registry state owned by an initializing context.
/// @return Non-zero when the lock is ready; zero when native allocation or
///         initialization fails. Failure leaves `rw_lock` null.
static int tr_rwlock_init(RtTypeRegistryState *st) {
    st->sealed = 0;
    st->rw_lock = NULL;
#ifdef _WIN32
    SRWLOCK *lock = (SRWLOCK *)malloc(sizeof(SRWLOCK));
    if (!lock)
        return 0;
    InitializeSRWLock(lock);
    st->rw_lock = lock;
#else
    pthread_rwlock_t *lock = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (!lock)
        return 0;
    if (pthread_rwlock_init(lock, NULL) != 0) {
        free(lock);
        return 0;
    }
    st->rw_lock = lock;
#endif
    return 1;
}

/// @brief Destroy and free the per-registry reader-writer lock.
/// @param st Registry state whose native lock storage should be released.
///        The state must be non-NULL; a null `rw_lock` is an idempotent no-op.
static void tr_rwlock_destroy(RtTypeRegistryState *st) {
    if (!st->rw_lock)
        return;
#if !defined(_WIN32)
    pthread_rwlock_destroy((pthread_rwlock_t *)st->rw_lock);
#endif
    free(st->rw_lock);
    st->rw_lock = NULL;
}

/// @brief Acquire the registry's shared read lock when one is installed.
/// @param st Non-NULL registry state containing the optional native lock.
/// @return @c 1 after acquiring a lock, or @c 0 when `rw_lock` is null.
static int tr_rdlock(RtTypeRegistryState *st) {
    if (!st->rw_lock)
        return 0;
#ifdef _WIN32
    AcquireSRWLockShared((SRWLOCK *)st->rw_lock);
#else
    pthread_rwlock_rdlock((pthread_rwlock_t *)st->rw_lock);
#endif
    return 1;
}

/// @brief Release a shared read lock acquired by @ref tr_rdlock.
/// @param st Non-NULL registry state containing the optional native lock.
/// @param locked Nonzero only when the matching read-lock call acquired a lock;
///        zero makes this function a no-op.
static void tr_rdunlock(RtTypeRegistryState *st, int locked) {
    if (!locked)
        return;
    if (!st->rw_lock)
        return;
#ifdef _WIN32
    ReleaseSRWLockShared((SRWLOCK *)st->rw_lock);
#else
    pthread_rwlock_unlock((pthread_rwlock_t *)st->rw_lock);
#endif
}

/// @brief Acquire the registry's exclusive write lock when installed.
/// @param st Non-NULL registry state; a null `rw_lock` makes this a no-op.
static void tr_wrlock(RtTypeRegistryState *st) {
    if (!st->rw_lock)
        return;
#ifdef _WIN32
    AcquireSRWLockExclusive((SRWLOCK *)st->rw_lock);
#else
    pthread_rwlock_wrlock((pthread_rwlock_t *)st->rw_lock);
#endif
}

/// @brief Release the registry's exclusive write lock when installed.
/// @param st Non-NULL registry state; a null `rw_lock` makes this a no-op.
static void tr_wrunlock(RtTypeRegistryState *st) {
    if (!st->rw_lock)
        return;
#ifdef _WIN32
    ReleaseSRWLockExclusive((SRWLOCK *)st->rw_lock);
#else
    pthread_rwlock_unlock((pthread_rwlock_t *)st->rw_lock);
#endif
}

/// @brief Acquire an explicit VM context's type-registry write lock.
/// @param ctx Context whose registry should be locked; @c NULL is ignored.
void rt_type_registry_state_write_lock(RtContext *ctx) {
    if (ctx)
        tr_wrlock(&ctx->type_registry);
}

/// @brief Release an explicit VM context's type-registry write lock.
/// @param ctx Context whose registry should be unlocked; @c NULL is ignored.
void rt_type_registry_state_write_unlock(RtContext *ctx) {
    if (ctx)
        tr_wrunlock(&ctx->type_registry);
}

/// @brief Check whether the registry is sealed and unlock + trap if so (called under write lock).
/// @param st Registry state whose write lock the caller currently holds; may be
///        @c NULL when no state is available.
/// @return @c 1 when sealed (after releasing the write lock and raising a
///         trap), otherwise @c 0 with the lock still held.
static int tr_reject_if_sealed_locked(RtTypeRegistryState *st) {
    if (st && __atomic_load_n(&st->sealed, __ATOMIC_ACQUIRE)) {
        tr_wrunlock(st);
        rt_trap("rt_type_registry: cannot register after registry is sealed");
        return 1;
    }
    return 0;
}

/// @brief Free a registry-owned C string copied by runtime-string registration.
/// @param text Owned allocation returned by `strdup`; @c NULL is accepted by `free`.
static void tr_free_owned_cstr(const char *text) {
    free((void *)(uintptr_t)text);
}

// ============================================================================
// Internal Data Structures
// ============================================================================

/// @brief Entry in the class registry tracking one registered class.
///
/// Each entry associates a type ID with its class metadata (rt_class_info).
/// The base_type_id enables inheritance chain traversal for is-a checks.
///
/// @note owned_ci indicates whether ci was allocated by the registry and
///       must be freed during cleanup (vs. static metadata from codegen).
typedef struct {
    int type_id;             ///< Unique class identifier.
    const rt_class_info *ci; ///< Class metadata (vtable, name, base).
    int base_type_id;        ///< Base class ID, or -1 for root classes.
    int owned_ci;            ///< Non-zero if ci should be freed on cleanup.
    int owned_qname;         ///< Non-zero if ci->qname should be freed on cleanup.
} class_entry;

/// @brief Entry in the interface registry tracking one registered interface.
///
/// Stores the interface's unique ID and its registration metadata including
/// the qualified name and slot count.
typedef struct {
    int iface_id;     ///< Unique interface identifier.
    rt_iface_reg reg; ///< Interface registration info (name, slot count).
    int owned_qname;  ///< Non-zero if reg.qname should be freed on cleanup.
} iface_entry;

/// @brief Entry in the bindings table associating a class with an interface.
///
/// When a class implements an interface, a binding is created that links
/// the class type_id and iface_id to the interface method table (itable).
/// The itable is an array of function pointers for the interface's methods.
///
/// **Example:**
/// ```
/// Class Dog implements IComparable
///   → binding_entry { type_id=Dog, iface_id=IComparable, itable=... }
/// ```
typedef struct {
    int type_id;   ///< Class implementing the interface.
    int iface_id;  ///< Interface being implemented.
    void **itable; ///< Array of function pointers for interface methods.
} binding_entry;

// ============================================================================
// State Access Helpers
// ============================================================================

/// @brief Get the type registry state for the current context.
///
/// Returns the type registry from either the thread's bound VM context or
/// the legacy fallback context. This enables per-VM type isolation.
///
/// @return Pointer to the current context's type registry state.
static inline RtTypeRegistryState *rt_tr_state(void) {
    RtContext *ctx = rt_get_current_context();
    if (!ctx)
        ctx = rt_legacy_context();
    return &ctx->type_registry;
}

/// @brief Access the class-entry array and optional counters for the active context.
/// @param plen Optional output receiving the address of the registry's class count.
/// @param pcap Optional output receiving the address of the registry's class capacity.
/// @return Borrowed class-entry array, or @c NULL when empty or no state is available.
static inline class_entry *get_classes(size_t **plen, size_t **pcap) {
    RtTypeRegistryState *st = rt_tr_state();
    if (!st)
        return NULL;
    if (plen)
        *plen = &st->classes_len;
    if (pcap)
        *pcap = &st->classes_cap;
    return (class_entry *)st->classes;
}

/// @brief Store a potentially reallocated class-entry array in the active context.
/// @param p New registry-owned array pointer; may be @c NULL.
static inline void set_classes(class_entry *p) {
    RtTypeRegistryState *st = rt_tr_state();
    if (st)
        st->classes = p;
}

/// @brief Access the interface-entry array and optional counters for the active context.
/// @param plen Optional output receiving the address of the interface count.
/// @param pcap Optional output receiving the address of the interface capacity.
/// @return Borrowed interface-entry array, or @c NULL when empty or unavailable.
static inline iface_entry *get_ifaces(size_t **plen, size_t **pcap) {
    RtTypeRegistryState *st = rt_tr_state();
    if (!st)
        return NULL;
    if (plen)
        *plen = &st->ifaces_len;
    if (pcap)
        *pcap = &st->ifaces_cap;
    return (iface_entry *)st->ifaces;
}

/// @brief Store a potentially reallocated interface-entry array in the active context.
/// @param p New registry-owned array pointer; may be @c NULL.
static inline void set_ifaces(iface_entry *p) {
    RtTypeRegistryState *st = rt_tr_state();
    if (st)
        st->ifaces = p;
}

/// @brief Access the binding-entry array and optional counters for the active context.
/// @param plen Optional output receiving the address of the binding count.
/// @param pcap Optional output receiving the address of the binding capacity.
/// @return Borrowed binding-entry array, or @c NULL when empty or unavailable.
static inline binding_entry *get_bindings(size_t **plen, size_t **pcap) {
    RtTypeRegistryState *st = rt_tr_state();
    if (!st)
        return NULL;
    if (plen)
        *plen = &st->bindings_len;
    if (pcap)
        *pcap = &st->bindings_cap;
    return (binding_entry *)st->bindings;
}

/// @brief Store a potentially reallocated binding-entry array in the active context.
/// @param p New registry-owned array pointer; may be @c NULL.
static inline void set_bindings(binding_entry *p) {
    RtTypeRegistryState *st = rt_tr_state();
    if (st)
        st->bindings = p;
}

/// @brief Grow a dynamic registry array by 2× when capacity is exhausted.
/// @details Initial capacity is 16; subsequent growth doubles with overflow guards.
///          Existing storage remains unchanged when allocation or arithmetic fails.
/// @param buf Address of the caller's array pointer, updated on success.
/// @param cap Address of the current capacity, updated to 16 or twice its old value.
/// @param elem_size Size in bytes of one array element.
/// @return @c NULL on success, or borrowed static diagnostic text on failure.
static const char *ensure_cap(void **buf, size_t *cap, size_t elem_size) {
    if (*cap == 0) {
        size_t new_cap = 16;
        if (elem_size != 0 && new_cap > (SIZE_MAX / elem_size))
            return "rt_type_registry: size overflow";
        void *tmp = malloc(new_cap * elem_size);
        if (!tmp)
            return "rt_type_registry: alloc failed";
        *buf = tmp;
        *cap = new_cap;
        return NULL;
    }

    // Exponential growth with overflow guards
    if (*cap > (SIZE_MAX / 2))
        return "rt_type_registry: capacity overflow";
    size_t new_cap = (*cap) * 2;
    if (elem_size != 0 && new_cap > (SIZE_MAX / elem_size))
        return "rt_type_registry: size overflow";
    void *new_buf = realloc(*buf, new_cap * elem_size);
    if (!new_buf)
        return "rt_type_registry: realloc failed";
    *buf = new_buf;
    *cap = new_cap;
    return NULL;
}

/// @brief Linear-search the active class array by type ID.
/// @param type_id Exact class identifier to find.
/// @return Borrowed matching entry, or @c NULL when unregistered.
static const class_entry *find_class_by_type(int type_id) {
    size_t *plen = NULL;
    class_entry *arr = get_classes(&plen, NULL);
    size_t len = arr && plen ? *plen : 0;
    for (size_t i = 0; i < len; ++i)
        if (arr[i].type_id == type_id)
            return &arr[i];
    return NULL;
}

/// @brief Linear-search the active class array by canonical vtable identity.
/// @param vptr Exact borrowed vtable pointer to match.
/// @return Borrowed matching class entry, or @c NULL when unregistered.
static const class_entry *find_class_by_vptr(void **vptr) {
    // Heuristic: vtable pointer equals ci->vtable
    size_t *plen = NULL;
    class_entry *arr = get_classes(&plen, NULL);
    size_t len = arr && plen ? *plen : 0;
    for (size_t i = 0; i < len; ++i)
        if (arr[i].ci && arr[i].ci->vtable == vptr)
            return &arr[i];
    return NULL;
}

/// @brief Linear-search the active interface array by interface ID.
/// @param iface_id Exact interface identifier to find.
/// @return Borrowed matching entry, or @c NULL when unregistered.
static const iface_entry *find_iface(int iface_id) {
    size_t *plen = NULL;
    iface_entry *arr = get_ifaces(&plen, NULL);
    size_t len = arr && plen ? *plen : 0;
    for (size_t i = 0; i < len; ++i)
        if (arr[i].iface_id == iface_id)
            return &arr[i];
    return NULL;
}

/// @brief Find the itable for an exact class/interface binding pair.
/// @param type_id Exact registered class identifier.
/// @param iface_id Exact registered interface identifier.
/// @return Borrowed itable pointer, or @c NULL when the pair is unbound.
static void **find_binding(int type_id, int iface_id) {
    size_t *plen = NULL;
    binding_entry *arr = get_bindings(&plen, NULL);
    size_t len = arr && plen ? *plen : 0;
    for (size_t i = 0; i < len; ++i)
        if (arr[i].type_id == type_id && arr[i].iface_id == iface_id)
            return arr[i].itable;
    return NULL;
}

/// @brief NULL-safe `strcmp`-equality test for two C strings.
/// @details Treats `(NULL, NULL)` and pointer-equal cases as equal up front so registry
///          lookups that hand in optional names don't crash on a NULL probe.
/// @param a First borrowed C string; may be @c NULL.
/// @param b Second borrowed C string; may be @c NULL.
/// @return @c 1 for identical pointers or equal non-null text, otherwise @c 0.
static int tr_cstr_eq(const char *a, const char *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

/// @brief Return an object's vtable only after validating a real runtime heap object.
/// @details Cast/type/interface helpers are public C ABI entry points and may receive raw,
///          stale, or stack pointers. The generated runtime only passes heap objects, so reject
///          anything else before reading the first payload word.
/// @param obj Candidate runtime object payload; may be @c NULL.
/// @return Borrowed vtable pointer for a live, sufficiently large heap object,
///         or @c NULL when validation fails or its vptr is null.
static void **tr_object_vptr_or_null(void *obj) {
    if (!obj)
        return NULL;
    rt_heap_info_t info;
    if (!rt_heap_get_info(obj, &info))
        return NULL;
    if ((rt_heap_kind_t)info.kind != RT_HEAP_OBJECT || info.cap < sizeof(rt_object))
        return NULL;
    rt_object *o = (rt_object *)obj;
    return o->vptr;
}

/// @brief Selectively free the parts of a class-info record that the registry owns.
/// @details The registry sometimes accepts caller-owned `rt_class_info` records and
///          sometimes allocates its own. The two ownership flags let cleanup skip the
///          parts that belong to someone else: only @p owned_qname-flagged qnames are
///          freed, and only @p owned_ci-flagged outer records are freed.
/// @param ci Class descriptor whose owned portions should be released; may be @c NULL.
/// @param owned_ci Nonzero when the descriptor allocation belongs to the registry.
/// @param owned_qname Nonzero when `ci->qname` belongs to the registry.
static void tr_free_owned_class_info(const rt_class_info *ci, int owned_ci, int owned_qname) {
    if (owned_qname && ci && ci->qname)
        tr_free_owned_cstr(ci->qname);
    if (owned_ci && ci)
        free((void *)(uintptr_t)ci);
}

/// @brief Test whether an existing registry entry already represents the same class.
/// @details Compares `(type_id, vtable, vtable_len, base_type_id, qname)`. Used by
///          re-registration paths to recognise idempotent calls (same class registered
///          twice from different translation units) versus genuine collisions.
/// @param entry Existing borrowed registry entry.
/// @param ci Candidate borrowed descriptor.
/// @return @c 1 when all registry-significant fields match, otherwise @c 0.
static int tr_class_entry_matches(const class_entry *entry, const rt_class_info *ci) {
    if (!entry || !entry->ci || !ci)
        return 0;
    int base_type_id = ci->base ? ci->base->type_id : -1;
    return entry->type_id == ci->type_id && entry->ci->vtable == ci->vtable &&
           entry->ci->vtable_len == ci->vtable_len && entry->base_type_id == base_type_id &&
           tr_cstr_eq(entry->ci->qname, ci->qname);
}

/// @brief Append a class descriptor to the registry's class array (caller holds write lock).
/// @param ci Descriptor to register; @c NULL is an idempotent no-op.
/// @param owned_ci    Non-zero if the registry should free @p ci on cleanup.
/// @param owned_qname Non-zero if the registry should free ci->qname on cleanup.
/// @return @c NULL for success/idempotent repetition, or borrowed static
///         diagnostic text for invalid metadata, collision, or allocation failure.
static const char *rt_register_class_entry(const rt_class_info *ci, int owned_ci, int owned_qname) {
    if (!ci)
        return NULL;
    if (ci->type_id < 0) {
        tr_free_owned_class_info(ci, owned_ci, owned_qname);
        return "rt_type_registry: class type id must be non-negative";
    }
    const class_entry *existing_type = find_class_by_type(ci->type_id);
    if (existing_type) {
        int same = tr_class_entry_matches(existing_type, ci);
        tr_free_owned_class_info(ci, owned_ci, owned_qname);
        return same ? NULL : "rt_type_registry: duplicate class type id";
    }
    if (ci->vtable) {
        const class_entry *existing_vptr = find_class_by_vptr(ci->vtable);
        if (existing_vptr) {
            tr_free_owned_class_info(ci, owned_ci, owned_qname);
            return "rt_type_registry: duplicate class vtable";
        }
    }
    size_t *plen = NULL, *pcap = NULL;
    class_entry *arr = get_classes(&plen, &pcap);
    if (!plen || !pcap)
        return NULL;
    if (*plen == *pcap) {
        const char *err = ensure_cap((void **)&arr, pcap, sizeof(class_entry));
        set_classes(arr);
        if (err) {
            tr_free_owned_class_info(ci, owned_ci, owned_qname);
            return err;
        }
    }
    int base_type_id = -1;
    if (ci->base)
        base_type_id = ci->base->type_id;
    arr[(*plen)++] = (class_entry){ci->type_id, ci, base_type_id, owned_ci, owned_qname};
    return NULL;
}

/// @brief Register a class metadata descriptor with the active VM registry.
///
/// @details Appends @p ci to the per-VM class table, growing the table as
///          needed. The descriptor's @c base pointer is not modified here; use
///          @ref rt_register_class_with_base to wire base classes by id.
///          Identical repetition is idempotent; invalid IDs, conflicting type
///          IDs/vtables, allocation failure, and post-seal writes trap.
/// @param ci Borrowed class descriptor whose referenced name, base metadata,
///        and vtable must outlive the registry; @c NULL is ignored.
void rt_register_class(const rt_class_info *ci) {
    RtTypeRegistryState *st = rt_tr_state();
    if (st && __atomic_load_n(&st->sealed, __ATOMIC_ACQUIRE)) {
        rt_trap("rt_type_registry: cannot register after registry is sealed");
        return;
    }
    if (st)
        tr_wrlock(st);
    if (tr_reject_if_sealed_locked(st))
        return;
    const char *err = rt_register_class_entry(ci, 0, 0);
    if (st)
        tr_wrunlock(st);
    if (err)
        rt_trap(err);
}

/// @brief Register an interface descriptor with the active VM registry.
/// @details Copies the descriptor fields into the registry, treating an
///          identical repeated ID as idempotent. Invalid metadata, conflicting
///          duplicate IDs, allocation failure, and post-seal writes trap.
/// @param iface Borrowed interface registration record; @c NULL is ignored.
/// @param owned_qname Nonzero when the descriptor's name allocation is being
///        transferred and must be freed on rejection or registry cleanup.
static void rt_register_interface_entry(const rt_iface_reg *iface, int owned_qname) {
    if (!iface)
        return;
    if (iface->iface_id < 0 || iface->slot_count < 0) {
        if (owned_qname && iface->qname)
            tr_free_owned_cstr(iface->qname);
        rt_trap("rt_type_registry: invalid interface metadata");
        return;
    }
    RtTypeRegistryState *st = rt_tr_state();
    if (st && __atomic_load_n(&st->sealed, __ATOMIC_ACQUIRE)) {
        if (owned_qname && iface->qname)
            tr_free_owned_cstr(iface->qname);
        rt_trap("rt_type_registry: cannot register after registry is sealed");
        return;
    }
    if (st)
        tr_wrlock(st);
    if (tr_reject_if_sealed_locked(st)) {
        if (owned_qname && iface->qname)
            tr_free_owned_cstr(iface->qname);
        return;
    }
    size_t *plen = NULL, *pcap = NULL;
    iface_entry *arr = get_ifaces(&plen, &pcap);
    if (!arr && (!plen || !pcap)) {
        if (st)
            tr_wrunlock(st);
        if (owned_qname && iface->qname)
            tr_free_owned_cstr(iface->qname);
        return;
    }
    const iface_entry *existing = find_iface(iface->iface_id);
    if (existing) {
        int same = existing->reg.slot_count == iface->slot_count &&
                   tr_cstr_eq(existing->reg.qname, iface->qname);
        if (st)
            tr_wrunlock(st);
        if (owned_qname && iface->qname)
            tr_free_owned_cstr(iface->qname);
        if (!same)
            rt_trap("rt_type_registry: duplicate interface id");
        return;
    }
    if (*plen == *pcap) {
        const char *err = ensure_cap((void **)&arr, pcap, sizeof(iface_entry));
        set_ifaces(arr);
        if (err) {
            if (st)
                tr_wrunlock(st);
            if (owned_qname && iface->qname)
                tr_free_owned_cstr(iface->qname);
            rt_trap(err);
            return;
        }
    }
    arr[(*plen)++] = (iface_entry){iface->iface_id, *iface, owned_qname};
    if (st)
        tr_wrunlock(st);
}

/// @brief Register a borrowed interface descriptor in the active VM registry.
/// @details The descriptor is copied by value but its qualified-name pointer
///          remains caller-owned and must outlive the registry. Registration
///          must precede any @ref rt_bind_interface call for this ID.
/// @param iface Borrowed interface metadata; @c NULL is ignored.
void rt_register_interface(const rt_iface_reg *iface) {
    rt_register_interface_entry(iface, 0);
}

/// @brief Bind an interface method table to a class type id.
///
/// @details Records the association so virtual dispatch via iface calls can
///          locate the correct itable for instances of @p type_id. The class
///          and interface must already exist. An identical repeated binding is
///          idempotent; conflicts, allocation failure, and post-seal writes trap.
/// @param type_id     Concrete class type id.
/// @param iface_id    Interface id to bind.
/// @param itable_slots Borrowed function-pointer array with the registered
///        interface's slot count; @c NULL is ignored and storage must outlive
///        the registry.
void rt_bind_interface(int type_id, int iface_id, void **itable_slots) {
    if (!itable_slots)
        return;
    RtTypeRegistryState *st = rt_tr_state();
    if (st && __atomic_load_n(&st->sealed, __ATOMIC_ACQUIRE)) {
        rt_trap("rt_type_registry: cannot register after registry is sealed");
        return;
    }
    if (st)
        tr_wrlock(st);
    if (tr_reject_if_sealed_locked(st))
        return;
    if (!find_class_by_type(type_id)) {
        if (st)
            tr_wrunlock(st);
        rt_trap("rt_type_registry: cannot bind unknown class");
        return;
    }
    if (!find_iface(iface_id)) {
        if (st)
            tr_wrunlock(st);
        rt_trap("rt_type_registry: cannot bind unknown interface");
        return;
    }
    void **existing = find_binding(type_id, iface_id);
    if (existing) {
        if (st)
            tr_wrunlock(st);
        if (existing != itable_slots)
            rt_trap("rt_type_registry: duplicate interface binding");
        return;
    }
    size_t *plen = NULL, *pcap = NULL;
    binding_entry *arr = get_bindings(&plen, &pcap);
    if (!arr && (!plen || !pcap)) {
        if (st)
            tr_wrunlock(st);
        return;
    }
    if (*plen == *pcap) {
        const char *err = ensure_cap((void **)&arr, pcap, sizeof(binding_entry));
        set_bindings(arr);
        if (err) {
            if (st)
                tr_wrunlock(st);
            rt_trap(err);
            return;
        }
    }
    arr[(*plen)++] = (binding_entry){type_id, iface_id, itable_slots};
    if (st)
        tr_wrunlock(st);
}

/// @brief Return the registered runtime type ID for an object instance.
/// @details Validates that @p obj is a live, sufficiently large heap object
///          before reading its vptr, then resolves that vtable under the
///          registry read lock.
/// @param obj Candidate object payload; may be @c NULL.
/// @return Registered type ID, zero for null, or -1 for invalid objects, null
///         vptrs, and unregistered vtables.
int rt_typeid_of(void *obj) {
    if (!obj)
        return 0;
    void **vptr = tr_object_vptr_or_null(obj);
    if (!vptr)
        return -1;
    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);
    const class_entry *ce = find_class_by_vptr(vptr);
    int result = ce ? ce->type_id : -1;
    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Check class inheritance (is-a) by type id.
/// @details Finds @p type_id in the active registry and walks its stored base-ID
///          chain while holding the read lock.
/// @param type_id Registered candidate class identifier.
/// @param test_type_id Registered target class identifier.
/// @return @c 1 when the candidate equals or derives from the target; @c 0 for
///         negative/unknown IDs or an unrelated chain.
int8_t rt_type_is_a(int type_id, int test_type_id) {
    if (type_id < 0 || test_type_id < 0)
        return 0;
    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);
    const class_entry *ce = find_class_by_type(type_id);
    int8_t result = ce && type_id == test_type_id ? 1 : 0;
    while (ce && ce->base_type_id >= 0) {
        if (ce->base_type_id == test_type_id) {
            result = 1;
            break;
        }
        ce = find_class_by_type(ce->base_type_id);
    }
    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Check whether a class implements an interface by id.
/// @details Searches the exact class/interface binding first and then walks
///          registered base classes for an inherited implementation.
/// @param type_id Registered candidate class identifier.
/// @param iface_id Interface identifier to search for.
/// @return @c 1 when the class or an ancestor has a binding; @c 0 for negative
///         IDs, unknown metadata, or no implementation.
int8_t rt_type_implements(int type_id, int iface_id) {
    if (type_id < 0 || iface_id < 0)
        return 0;
    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);

    int8_t result = 0;

    // Check the exact type first.
    if (find_binding(type_id, iface_id) != NULL) {
        result = 1;
    } else {
        // Walk the base class chain to see if any ancestor implements the interface.
        const class_entry *ce = find_class_by_type(type_id);
        while (ce && ce->base_type_id >= 0) {
            if (find_binding(ce->base_type_id, iface_id) != NULL) {
                result = 1;
                break;
            }
            ce = find_class_by_type(ce->base_type_id);
        }
    }

    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Safe-cast an object to an interface by id.
/// @details Validates the heap object and its registered vtable, then searches
///          the dynamic class and base chain for an interface binding. The
///          operation does not adjust reference counts.
/// @param obj Candidate runtime object payload; may be @c NULL.
/// @param iface_id Interface identifier required by the cast.
/// @return Borrowed original @p obj when compatible, otherwise @c NULL.
void *rt_cast_as_iface(void *obj, int iface_id) {
    if (!obj || iface_id < 0)
        return NULL;
    void **vptr = tr_object_vptr_or_null(obj);
    if (!vptr)
        return NULL;

    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);

    void *result = NULL;
    const class_entry *ce = find_class_by_vptr(vptr);
    if (ce) {
        int tid = ce->type_id;
        if (find_binding(tid, iface_id) != NULL) {
            result = obj;
        } else {
            const class_entry *base = find_class_by_type(tid);
            while (base && base->base_type_id >= 0) {
                if (find_binding(base->base_type_id, iface_id) != NULL) {
                    result = obj;
                    break;
                }
                base = find_class_by_type(base->base_type_id);
            }
        }
    }

    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Safe-cast an object to a target class by id.
/// @details Validates the heap object and its registered vtable, then compares
///          the dynamic type and each stored base ID with @p target_type_id.
///          The operation does not adjust reference counts.
/// @param obj Candidate runtime object payload; may be @c NULL.
/// @param target_type_id Target class identifier; negative IDs always fail.
/// @return Borrowed original @p obj when compatible, otherwise @c NULL.
void *rt_cast_as(void *obj, int target_type_id) {
    if (!obj || target_type_id < 0)
        return NULL;
    void **vptr = tr_object_vptr_or_null(obj);
    if (!vptr)
        return NULL;

    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);

    void *result = NULL;
    const class_entry *ce = find_class_by_vptr(vptr);
    if (ce) {
        int tid = ce->type_id;
        if (tid == target_type_id) {
            result = obj;
        } else {
            const class_entry *cur = find_class_by_type(tid);
            while (cur && cur->base_type_id >= 0) {
                if (cur->base_type_id == target_type_id) {
                    result = obj;
                    break;
                }
                cur = find_class_by_type(cur->base_type_id);
            }
        }
    }

    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Lookup the active interface method table for an object instance.
/// @details Validates the object's registered dynamic class and searches its
///          exact binding followed by each base class. No ownership is created.
/// @param obj Candidate object payload; may be @c NULL.
/// @param iface_id Interface ID to search; negative IDs fail.
/// @return Borrowed itable when the class or an ancestor is bound, otherwise @c NULL.
void **rt_itable_lookup(void *obj, int iface_id) {
    if (!obj || iface_id < 0)
        return NULL;
    void **vptr = tr_object_vptr_or_null(obj);
    if (!vptr)
        return NULL;

    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);

    void **result = NULL;
    const class_entry *ce = find_class_by_vptr(vptr);
    if (ce) {
        int tid = ce->type_id;
        result = find_binding(tid, iface_id);
        if (!result) {
            const class_entry *cur = find_class_by_type(tid);
            while (cur && cur->base_type_id >= 0) {
                result = find_binding(cur->base_type_id, iface_id);
                if (result)
                    break;
                cur = find_class_by_type(cur->base_type_id);
            }
        }
    }

    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Register an interface from direct C ABI fields.
/// @details Builds a temporary descriptor and delegates to borrowed-name
///          registration; the registry does not copy @p qname.
/// @param iface_id Stable nonnegative interface identifier.
/// @param qname Borrowed qualified name that must outlive the registry; may be @c NULL.
/// @param slot_count Nonnegative number of interface method slots.
void rt_register_interface_direct(int iface_id, const char *qname, int slot_count) {
    (void)qname;
    (void)slot_count; // stored in reg; currently unused by runtime
    rt_iface_reg r = {iface_id, qname, slot_count};
    rt_register_interface_entry(&r, 0);
}

/// @brief Register an interface while copying a runtime-string qualified name.
/// @details Validates both 64-bit numeric inputs and the optional runtime
///          string handle, copies its C-string text, and transfers that copy to
///          registry ownership. Invalid input or allocation failure traps.
/// @param iface_id Interface ID representable as a nonnegative C `int`.
/// @param qname Borrowed runtime string name; may be @c NULL.
/// @param slot_count Slot count representable as a nonnegative C `int`.
void rt_register_interface_direct_rs(int64_t iface_id, rt_string qname, int64_t slot_count) {
    if (iface_id < 0 || iface_id > INT_MAX || slot_count < 0 || slot_count > INT_MAX) {
        rt_trap("rt_type_registry: interface metadata out of range");
        return;
    }
    if (qname && !rt_string_is_handle((const void *)qname)) {
        rt_trap("rt_type_registry: invalid interface name string");
        return;
    }
    const char *name = qname ? strdup(rt_string_cstr(qname)) : NULL;
    if (qname && !name) {
        rt_trap("rt_type_registry: interface name alloc failed");
        return;
    }
    rt_iface_reg r = {(int)iface_id, name, (int)slot_count};
    rt_register_interface_entry(&r, 1);
}

/// @brief Resolve a class descriptor from a vtable pointer.
/// @param vptr Borrowed canonical vtable pointer; may be @c NULL.
/// @return Borrowed registered class metadata, or @c NULL when unregistered.
const rt_class_info *rt_get_class_info_from_vptr(void **vptr) {
    if (!vptr)
        return NULL;
    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);
    const class_entry *ce = find_class_by_vptr(vptr);
    const rt_class_info *result = ce ? ce->ci : NULL;
    if (st)
        tr_rdunlock(st, locked);
    return result;
}

/// @brief Register a class descriptor built from parts, with base by id.
/// @details Validates metadata, resolves a nonnegative base ID before allocating
///          an owned class descriptor, and registers it under the write lock.
///          A null vtable is ignored. Rejection frees an owned qualified name.
/// @param type_id      Assigned class id.
/// @param vtable       Vtable pointer array.
/// @param qname        Qualified class name (borrowed).
/// @param vslot_count  Number of entries in the vtable.
/// @param base_type_id Base class id or -1 when none.
/// @param owned_qname Nonzero when @p qname is a registry-owned allocation
///        that must be freed on rejection and cleanup.
static void rt_register_class_with_base_impl(int type_id,
                                             void **vtable,
                                             const char *qname,
                                             int vslot_count,
                                             int base_type_id,
                                             int owned_qname) {
    if (!vtable) {
        if (owned_qname && qname)
            tr_free_owned_cstr(qname);
        return;
    }
    if (type_id < 0 || vslot_count < 0 || base_type_id < -1) {
        if (owned_qname && qname)
            tr_free_owned_cstr(qname);
        rt_trap("rt_type_registry: invalid class metadata");
        return;
    }
    RtTypeRegistryState *st = rt_tr_state();
    if (st && __atomic_load_n(&st->sealed, __ATOMIC_ACQUIRE)) {
        if (owned_qname && qname)
            tr_free_owned_cstr(qname);
        rt_trap("rt_type_registry: cannot register after registry is sealed");
        return;
    }

    if (st)
        tr_wrlock(st);
    if (tr_reject_if_sealed_locked(st)) {
        if (owned_qname && qname)
            tr_free_owned_cstr(qname);
        return;
    }

    const class_entry *base_entry = NULL;
    if (base_type_id >= 0) {
        base_entry = find_class_by_type(base_type_id);
        if (!base_entry || !base_entry->ci) {
            if (st)
                tr_wrunlock(st);
            if (owned_qname && qname)
                tr_free_owned_cstr(qname);
            rt_trap("rt_type_registry: base class is not registered");
            return;
        }
    }

    rt_class_info *ci = (rt_class_info *)malloc(sizeof(rt_class_info));
    if (!ci) {
        if (st)
            tr_wrunlock(st);
        if (owned_qname && qname)
            tr_free_owned_cstr(qname);
        rt_trap("rt_type_registry: class meta alloc failed");
        return;
    }
    ci->type_id = type_id;
    ci->qname = qname;
    ci->vtable = vtable;
    ci->vtable_len = (uint32_t)(vslot_count < 0 ? 0 : vslot_count);

    // Wire base class pointer by looking up base_type_id in the registry.
    // The base class must be registered before derived classes for this to work.
    ci->base = NULL;
    if (base_entry)
        ci->base = base_entry->ci;

    const char *err = rt_register_class_entry(ci, 1, owned_qname);

    if (st)
        tr_wrunlock(st);
    if (err)
        rt_trap(err);
}

/// @brief Register a class with explicit base-class wiring. `base_type_id == -1` for root
/// classes. The qname string is borrowed (caller-owned static string); use `_rs` variants for
/// rt_string-based registration that copies internally.
/// @param type_id Stable nonnegative class identifier.
/// @param vtable Borrowed canonical vtable array; @c NULL is ignored.
/// @param qname Borrowed qualified name that must outlive the registry; may be @c NULL.
/// @param vslot_count Nonnegative number of entries in @p vtable.
/// @param base_type_id Previously registered base class ID, or -1 for a root.
void rt_register_class_with_base(
    int type_id, void **vtable, const char *qname, int vslot_count, int base_type_id) {
    rt_register_class_with_base_impl(type_id, vtable, qname, vslot_count, base_type_id, 0);
}

/// @brief Register a root class from direct C ABI fields.
/// @param type_id Stable nonnegative class identifier.
/// @param vtable Borrowed canonical vtable array; @c NULL is ignored.
/// @param qname Borrowed qualified name that must outlive the registry; may be @c NULL.
/// @param vslot_count Nonnegative number of entries in @p vtable.
void rt_register_class_direct(int type_id, void **vtable, const char *qname, int vslot_count) {
    // Delegate to the base-aware version with no base class.
    rt_register_class_with_base_impl(type_id, vtable, qname, vslot_count, -1, 0);
}

/// @brief Fetch the vtable pointer array for a registered class id.
/// @param type_id Class identifier to resolve.
/// @return Borrowed canonical vtable pointer, or @c NULL for negative or
///         unregistered IDs.
void **rt_get_class_vtable(int type_id) {
    if (type_id < 0)
        return NULL;
    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);
    const class_entry *ce = find_class_by_type(type_id);
    void **result = (ce && ce->ci) ? ce->ci->vtable : NULL;
    if (st)
        tr_rdunlock(st, locked);
    return result;
}

// Runtime bridge wrapper: accept runtime string for qname
/// @brief Register a root class while copying a runtime-string qualified name.
/// @details Validates the optional string handle and 64-bit slot count, copies
///          name text, and transfers the copy to registry ownership.
/// @param type_id Stable class identifier validated by the shared implementation.
/// @param vtable Borrowed canonical vtable array; @c NULL is ignored.
/// @param qname Borrowed runtime string name; may be @c NULL.
/// @param vslot_count Slot count representable as a nonnegative C `int`.
void rt_register_class_direct_rs(int type_id, void **vtable, rt_string qname, int64_t vslot_count) {
    // strdup the name to avoid dangling pointer if the rt_string is freed
    if (vslot_count < 0 || vslot_count > INT_MAX) {
        rt_trap("rt_type_registry: class metadata out of range");
        return;
    }
    if (qname && !rt_string_is_handle((const void *)qname)) {
        rt_trap("rt_type_registry: invalid class name string");
        return;
    }
    const char *name = qname ? strdup(rt_string_cstr(qname)) : NULL;
    if (qname && !name) {
        rt_trap("rt_type_registry: class name alloc failed");
        return;
    }
    rt_register_class_with_base_impl(type_id, vtable, name, (int)vslot_count, -1, 1);
}

// Runtime bridge wrapper: accept runtime string for qname with base class
/// @brief Register a derived class while copying a runtime-string qualified name.
/// @details Validates the optional string and 64-bit counts, copies name text,
///          and resolves the base through the shared registration implementation.
/// @param type_id Stable class identifier validated by the shared implementation.
/// @param vtable Borrowed canonical vtable array; @c NULL is ignored.
/// @param qname Borrowed runtime string name; may be @c NULL.
/// @param vslot_count Slot count representable as a nonnegative C `int`.
/// @param base_type_id Base ID in `[-1, INT_MAX]`, where -1 denotes a root.
void rt_register_class_with_base_rs(
    int type_id, void **vtable, rt_string qname, int64_t vslot_count, int64_t base_type_id) {
    // strdup the name to avoid dangling pointer if the rt_string is freed
    if (vslot_count < 0 || vslot_count > INT_MAX || base_type_id < -1 || base_type_id > INT_MAX) {
        rt_trap("rt_type_registry: class metadata out of range");
        return;
    }
    if (qname && !rt_string_is_handle((const void *)qname)) {
        rt_trap("rt_type_registry: invalid class name string");
        return;
    }
    const char *name = qname ? strdup(rt_string_cstr(qname)) : NULL;
    if (qname && !name) {
        rt_trap("rt_type_registry: class name alloc failed");
        return;
    }
    rt_register_class_with_base_impl(type_id, vtable, name, (int)vslot_count, (int)base_type_id, 1);
}

/// @brief Register an interface implementation for a class (IL-friendly wrapper).
/// @details Validates that both 64-bit IDs fit nonnegative C `int` values, then
///          delegates all existence, collision, seal, and allocation handling
///          to @ref rt_bind_interface.
/// @param type_id Class type ID.
/// @param iface_id Interface ID.
/// @param itable Borrowed interface method table; @c NULL is ignored.
void rt_register_interface_impl(int64_t type_id, int64_t iface_id, void **itable) {
    if (type_id < 0 || type_id > INT_MAX || iface_id < 0 || iface_id > INT_MAX) {
        rt_trap("rt_type_registry: interface binding metadata out of range");
        return;
    }
    rt_bind_interface((int)type_id, (int)iface_id, itable);
}

/// @brief Lookup interface implementation table by type id and interface id.
/// @details Validates the 64-bit IDs, searches an exact binding, and then walks
///          the registered base chain for an inherited implementation.
/// @param type_id Class type id.
/// @param iface_id Interface id.
/// @return Borrowed interface method table, or @c NULL for out-of-range IDs or
///         when neither the class nor an ancestor has a binding.
void **rt_get_interface_impl(int64_t type_id, int64_t iface_id) {
    if (type_id < 0 || type_id > INT_MAX || iface_id < 0 || iface_id > INT_MAX)
        return NULL;
    int tid = (int)type_id;
    int iid = (int)iface_id;

    RtTypeRegistryState *st = rt_tr_state();
    int locked = 0;
    if (st)
        locked = tr_rdlock(st);

    void **result = find_binding(tid, iid);
    if (!result) {
        const class_entry *ce = find_class_by_type(tid);
        while (ce && ce->base_type_id >= 0) {
            result = find_binding(ce->base_type_id, iid);
            if (result)
                break;
            ce = find_class_by_type(ce->base_type_id);
        }
    }

    if (st)
        tr_rdunlock(st, locked);
    return result;
}

// ============================================================================
// Registry Lifecycle
// ============================================================================

/// @brief Initialize the type registry's reader-writer lock for a context.
///
/// Called during context creation to set up the rwlock that protects
/// concurrent access to the registry arrays. Must be called before any
/// registration or lookup operations.
///
/// @param ctx Context whose type registry should be initialized.
/// @return Non-zero when the lock is ready. NULL input and native
///         allocation/initialization failure return zero without trapping.
int rt_type_registry_init(RtContext *ctx) {
    if (!ctx)
        return 0;
    return tr_rwlock_init(&ctx->type_registry);
}

/// @brief Seal the type registry against further registration.
///
/// After all type registration is complete (typically at the end of module
/// initialization), call this to mark the registry as immutable. Subsequent
/// registration attempts trap. Read operations remain protected by the
/// installed reader lock; the atomic sealed flag coordinates writers and
/// publishes the immutable transition.
///
/// @thread_safety Safe to call from any thread; sealing is performed while the
///                registry write lock is held so no writer can race the
///                immutable transition.
void rt_type_registry_seal(void) {
    RtTypeRegistryState *st = rt_tr_state();
    if (!st)
        return;
    tr_wrlock(st);
    __atomic_store_n(&st->sealed, 1, __ATOMIC_RELEASE);
    tr_wrunlock(st);
}

/// @brief Clean up type registry resources for a context.
///
/// Frees all memory associated with the type registry including:
/// - Class entries and their owned rt_class_info structures
/// - Interface entries
/// - Interface binding entries
/// - The reader-writer lock
///
/// After cleanup, the registry is empty and ready for reinitialization
/// if needed.
///
/// @param ctx Context whose type registry should be cleaned up.
///
/// @note Safe to call with NULL or on an already-cleaned context.
/// @note Owned class info structures are freed; static ones are left alone.
void rt_type_registry_cleanup(RtContext *ctx) {
    if (!ctx)
        return;

    class_entry *classes = (class_entry *)ctx->type_registry.classes;
    size_t len = ctx->type_registry.classes_len;
    if (classes) {
        for (size_t i = 0; i < len; ++i) {
            if (classes[i].owned_qname && classes[i].ci && classes[i].ci->qname)
                tr_free_owned_cstr(classes[i].ci->qname);
            if (classes[i].owned_ci && classes[i].ci)
                free((void *)(uintptr_t)classes[i].ci);
            classes[i].ci = NULL;
            classes[i].owned_ci = 0;
            classes[i].owned_qname = 0;
            classes[i].type_id = 0;
            classes[i].base_type_id = -1;
        }
    }

    free(ctx->type_registry.classes);
    iface_entry *ifaces = (iface_entry *)ctx->type_registry.ifaces;
    size_t ifaces_len = ctx->type_registry.ifaces_len;
    if (ifaces) {
        for (size_t i = 0; i < ifaces_len; ++i) {
            if (ifaces[i].owned_qname && ifaces[i].reg.qname)
                tr_free_owned_cstr(ifaces[i].reg.qname);
            ifaces[i].reg.qname = NULL;
            ifaces[i].owned_qname = 0;
        }
    }
    free(ctx->type_registry.ifaces);
    free(ctx->type_registry.bindings);

    ctx->type_registry.classes = NULL;
    ctx->type_registry.classes_len = 0;
    ctx->type_registry.classes_cap = 0;
    ctx->type_registry.ifaces = NULL;
    ctx->type_registry.ifaces_len = 0;
    ctx->type_registry.ifaces_cap = 0;
    ctx->type_registry.bindings = NULL;
    ctx->type_registry.bindings_len = 0;
    ctx->type_registry.bindings_cap = 0;
    tr_rwlock_destroy(&ctx->type_registry);
    ctx->type_registry.rw_lock = NULL;
    ctx->type_registry.sealed = 0;
}
