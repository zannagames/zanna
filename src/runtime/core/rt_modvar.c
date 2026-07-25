//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_modvar.c
// Purpose: Provides runtime-managed storage for module-level BASIC variables.
//          Each VM instance holds an independent indexed table keyed by variable
//          name and kind tag, enabling multiple VMs to maintain isolated global
//          variable namespaces without shared state.
//
// Key invariants:
//   - Variables are keyed by (name, kind) pairs; the same name with different
//     kind tags (I64, F64, I1, PTR, STR) is stored as a separate entry.
//   - The backing entry table and hash index both grow geometrically.
//   - Storage for each variable is zero-initialized at creation time.
//   - Table entries are never removed; once created, a variable persists for
//     the lifetime of the RtContext.
//   - Repeated lookups for the same (name, kind) pair return the same address,
//     and mismatched storage sizes trap instead of silently reusing the wrong
//     slot.
//   - All operations require an active RtContext; passing NULL traps.
//
// Ownership/Lifetime:
//   - Storage/name blocks use rt_alloc; entry and index tables use the C
//     allocator. All are owned and freed by the RtContext.
//   - Variable names are compared by value (strcmp); no ownership of the
//     name pointer is taken beyond the duration of the lookup call.
//
// Links: src/runtime/core/rt_modvar.h (public API),
//        src/runtime/core/rt_context.h (RtContext definition),
//        src/runtime/core/rt_memory.c (rt_alloc)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements per-context, name-and-kind keyed module-variable storage.

#include "rt_modvar.h"
#include "rt_context.h"
#include "rt_context_internal.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Internal aliases for the stable module-variable ABI kind tags.
typedef enum {
    MV_I64 = RT_MODVAR_KIND_I64,
    MV_F64 = RT_MODVAR_KIND_F64,
    MV_I1 = RT_MODVAR_KIND_I1,
    MV_PTR = RT_MODVAR_KIND_PTR,
    MV_STR = RT_MODVAR_KIND_STR,
    MV_BLOCK = RT_MODVAR_KIND_BLOCK
} mv_kind_t;

/// @brief FNV-1a 64-bit hash of a module-variable @p key, salted by its kind tag.
/// @details Salting with `kind` (XORed into the FNV offset basis) lets `(name, kind)`
///          tuples coexist in the index without collision — different module-variable
///          kinds occupying the same name hash to different bucket slots. Returns 1 if
///          the natural hash happened to land on 0 so the caller can use 0 as a
///          "free slot" sentinel in the index.
/// @param key Non-NULL NUL-terminated variable name.
/// @param kind Storage-kind tag included in the hash identity.
/// @return Nonzero salted FNV-1a hash.
static uint64_t mv_hash_key(const char *key, mv_kind_t kind) {
    uint64_t hash = 1469598103934665603ULL ^ (uint64_t)kind;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

/// @brief Grow the module-variable index when it would otherwise exceed 70% load.
/// @details Open-addressing hash tables degrade rapidly past ~0.7 load. This helper
///          checks the projected load and doubles the capacity (rehashing every existing
///          entry) when needed. This predicate performs no allocation itself.
/// @param existing_count Number of currently indexed entries.
/// @param capacity Current power-of-two slot count, or zero before initialization.
/// @return 1 when inserting one more entry requires capacity growth, otherwise 0.
static int mv_index_needs_grow(size_t existing_count, size_t capacity) {
    if (capacity == 0)
        return 1;
    if (existing_count == SIZE_MAX)
        return 1;

    size_t projected = existing_count + 1;
    size_t threshold = (capacity / 10) * 7 + (((capacity % 10) * 7 + 9) / 10);
    if (threshold == 0)
        threshold = 1;
    return projected >= threshold;
}

/// @brief Ensure the context hash index can accept one additional entry.
/// @details Chooses an initial 32 slots or doubles a power-of-two table until
///   projected load is below 70 percent, then rehashes all entries. Overflow
///   and allocation failure raise a trap while leaving the old index installed.
/// @param ctx Active context whose module-variable state is exclusively held.
/// @param existing_count Number of entries that will remain during rehash.
/// @return 1 when sufficient capacity is available, otherwise 0 after a
///   returning error trap.
static int mv_ensure_index_capacity(RtContext *ctx, size_t existing_count) {
    if (!mv_index_needs_grow(existing_count, ctx->modvar_index_capacity))
        return 1;

    size_t new_cap = ctx->modvar_index_capacity ? ctx->modvar_index_capacity * 2 : 32;
    while (mv_index_needs_grow(existing_count, new_cap)) {
        if (new_cap > SIZE_MAX / 2) {
            rt_trap("rt_modvar: index capacity overflow");
            return 0;
        }
        new_cap *= 2;
    }

    size_t *new_slots = (size_t *)calloc(new_cap, sizeof(size_t));
    if (!new_slots) {
        rt_trap("rt_modvar: index alloc failed");
        return 0;
    }

    size_t mask = new_cap - 1;
    for (size_t i = 0; i < ctx->modvar_count; ++i) {
        RtModvarEntry *entry = &ctx->modvar_entries[i];
        size_t pos = (size_t)(entry->hash & mask);
        while (new_slots[pos] != 0)
            pos = (pos + 1) & mask;
        new_slots[pos] = i + 1;
    }

    free(ctx->modvar_index_slots);
    ctx->modvar_index_slots = new_slots;
    ctx->modvar_index_capacity = new_cap;
    return 1;
}

/// @brief Linear-probing lookup for an existing module-variable entry.
/// @details Walks the open-addressing index from `hash & mask`, comparing `(hash, kind,
///          name)` against each occupied slot. Slot value 0 marks an empty terminator
///          (probe stops); non-zero is `entry_index + 1` (the +1 keeps 0 reserved as
///          "empty"). On a match, validates the stored size against @p size — a size
///          mismatch traps because it indicates a programming bug where two callers
///          declared the same name with incompatible storage shapes.
/// @param ctx Active context whose module-variable state is locked.
/// @param key Non-NULL NUL-terminated variable name.
/// @param hash Precomputed nonzero hash for @p key and @p kind.
/// @param kind Required storage-kind tag.
/// @param size Required byte size used to validate an existing declaration.
/// @return Matching borrowed table entry, or NULL when absent.
static RtModvarEntry *mv_lookup(
    RtContext *ctx, const char *key, uint64_t hash, mv_kind_t kind, size_t size) {
    if (!ctx->modvar_index_slots || ctx->modvar_index_capacity == 0)
        return NULL;

    size_t mask = ctx->modvar_index_capacity - 1;
    size_t pos = (size_t)(hash & mask);
    while (ctx->modvar_index_slots[pos] != 0) {
        RtModvarEntry *entry = &ctx->modvar_entries[ctx->modvar_index_slots[pos] - 1];
        if (entry->hash == hash && entry->kind == (int)kind && strcmp(entry->name, key) == 0) {
            if (entry->size != size)
                rt_trap("rt_modvar: storage size mismatch");
            return entry;
        }
        pos = (pos + 1) & mask;
    }
    return NULL;
}

/// @brief Insert a fresh entry into the open-addressing index.
/// @details Caller has already appended the entry to `ctx->modvar_entries`; this helper
///          finds its bucket slot by linear probing from `entry->hash & mask` and stores
///          `entry_index + 1` (the +1 keeps 0 reserved as the empty marker). Caller
///          must hold capacity room — `mv_ensure_index_capacity` should have run first.
/// @param ctx Active context with an allocated index and appended entry.
/// @param entry_index Zero-based entry-table index to publish.
static void mv_insert_index(RtContext *ctx, size_t entry_index) {
    RtModvarEntry *entry = &ctx->modvar_entries[entry_index];
    size_t mask = ctx->modvar_index_capacity - 1;
    size_t pos = (size_t)(entry->hash & mask);
    while (ctx->modvar_index_slots[pos] != 0)
        pos = (pos + 1) & mask;
    ctx->modvar_index_slots[pos] = entry_index + 1;
}

/// @brief Allocate zero-initialized storage for a module variable.
///
/// @details Uses the runtime allocator, trapping on failure, and initializes
///          the entire region to zero. The pointer is suitable for use as the
///          backing storage of a module-level variable of @p size bytes.
/// @param size Size in bytes of the requested storage.
/// @return Caller-owned zeroed storage, or NULL after a returning overflow or
///   allocation trap.
static void *mv_alloc(size_t size) {
    if (size > (size_t)INT64_MAX) {
        rt_trap("rt_modvar: storage size overflow");
        return NULL;
    }
    void *p = rt_alloc((int64_t)size);
    if (!p) {
        rt_trap("rt_modvar: alloc failed");
        return NULL;
    }
    memset(p, 0, size); // Defensive: ensure all fields zeroed regardless of rt_alloc behavior
    return p;
}

/// @brief Find or create a module variable entry in the current VM context.
///
/// @details Performs an indexed lookup over the per-VM modvar table using the
///          exact @p key and @p kind. When not found, grows the entry table and
///          hash index, then appends a new entry with a freshly allocated,
///          zeroed storage block sized for the requested type.
/// @param ctx  Active runtime context (thread-local binding).
/// @param key  Canonical variable name.
/// @param kind Kind tag used to distinguish same-named variables of different types.
/// @param size Size in bytes for the associated storage.
/// @return Borrowed context-owned entry, or NULL after a returning allocation,
///   sizing, or index-growth trap.
static RtModvarEntry *mv_find_or_create(RtContext *ctx,
                                        const char *key,
                                        mv_kind_t kind,
                                        size_t size) {
    assert(ctx && "mv_find_or_create called without active RtContext");
    uint64_t hash = mv_hash_key(key, kind);
    RtModvarEntry *existing = mv_lookup(ctx, key, hash, kind, size);
    if (existing)
        return existing;

    if (ctx->modvar_count == ctx->modvar_capacity) {
        size_t oldCap = ctx->modvar_capacity;
        if (oldCap > (SIZE_MAX / 2)) {
            rt_trap("rt_modvar: table capacity overflow");
            return NULL;
        }
        size_t newCap = oldCap ? oldCap * 2 : 16;
        if (newCap > (SIZE_MAX / sizeof(RtModvarEntry))) {
            rt_trap("rt_modvar: table size overflow");
            return NULL;
        }
        RtModvarEntry *np =
            (RtModvarEntry *)realloc(ctx->modvar_entries, newCap * sizeof(RtModvarEntry));
        if (!np) {
            rt_trap("rt_modvar: table alloc failed");
            return NULL;
        }
        if (newCap > oldCap) {
            memset(np + oldCap, 0, (newCap - oldCap) * sizeof(RtModvarEntry));
        }
        ctx->modvar_entries = np;
        ctx->modvar_capacity = newCap;
    }

    size_t nlen = strlen(key);
    if (nlen > (size_t)INT64_MAX - 1) {
        rt_trap("rt_modvar: name size overflow");
        return NULL;
    }

    void *addr = mv_alloc(size);
    if (!addr)
        return NULL;

    char *name_copy = (char *)rt_alloc((int64_t)(nlen + 1));
    if (!name_copy) {
        free(addr);
        rt_trap("rt_modvar: name alloc failed");
        return NULL;
    }
    memcpy(name_copy, key, nlen + 1);

    if (!mv_ensure_index_capacity(ctx, ctx->modvar_count)) {
        free(name_copy);
        free(addr);
        return NULL;
    }

    RtModvarEntry *e = &ctx->modvar_entries[ctx->modvar_count];
    e->name = name_copy;
    e->kind = kind;
    e->size = size;
    e->hash = hash;
    e->addr = addr;
    ctx->modvar_count++;
    mv_insert_index(ctx, ctx->modvar_count - 1);
    return e;
}

/// @brief Resolve the address of a module variable by (name, kind).
///
/// @details Converts the runtime string to a C string (trapping on NULL),
///          looks up or creates the corresponding entry in the active runtime
///          context, and returns the stable address of the storage. Names are
///          keyed as NUL-terminated text; embedded bytes after the first NUL do
///          not participate in identity. Empty names are accepted.
/// @param name Borrowed valid runtime string naming the variable.
/// @param kind Module variable kind tag (I64/F64/I1/PTR/STR).
/// @param size Size of the storage to allocate for new entries.
/// @return Stable context-owned storage address, or NULL after a returning
///   validation, context-acquisition, or allocation trap.
static void *mv_addr(rt_string name, mv_kind_t kind, size_t size) {
    if (!name) {
        rt_trap("rt_modvar: null name");
        return NULL;
    }
    if (!rt_string_is_handle(name)) {
        rt_trap("rt_modvar: invalid name");
        return NULL;
    }

    const char *c = rt_string_cstr(name);
    if (!c) {
        rt_trap("rt_modvar: null name");
        return NULL;
    }
    if (size == 0) {
        rt_trap("rt_modvar: zero-sized storage");
        return NULL;
    }

    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_MODVAR, NULL);
    if (!ctx)
        return NULL;
    RtModvarEntry *e = mv_find_or_create(ctx, c, kind, size);
    void *address = e ? e->addr : NULL;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_MODVAR);
    return address;
}

/// @brief Address of a 64-bit integer module variable.
/// @param name Borrowed runtime string key.
/// @return Stable context-owned address of zero-initialized 8-byte storage.
void *rt_modvar_addr_i64(rt_string name) {
    return mv_addr(name, MV_I64, 8);
}

/// @brief Address of a 64-bit floating module variable.
/// @param name Borrowed runtime string key.
/// @return Stable context-owned address of zero-initialized 8-byte storage.
void *rt_modvar_addr_f64(rt_string name) {
    return mv_addr(name, MV_F64, 8);
}

/// @brief Address of a boolean (i1) module variable.
/// @param name Borrowed runtime string key.
/// @return Stable context-owned address of zero-initialized one-byte storage.
void *rt_modvar_addr_i1(rt_string name) {
    return mv_addr(name, MV_I1, 1);
}

/// @brief Address of a pointer module variable.
/// @param name Borrowed runtime string key.
/// @return Stable context-owned address of zero-initialized 8-byte storage.
void *rt_modvar_addr_ptr(rt_string name) {
    return mv_addr(name, MV_PTR, 8);
}

/// @brief Address of a string module variable (stores rt_string handle).
/// @param name Borrowed runtime string key.
/// @return Stable context-owned address of a NULL-initialized `rt_string` slot.
void *rt_modvar_addr_str(rt_string name) {
    return mv_addr(name, MV_STR, sizeof(void *));
}

/// @brief Address of a module variable block with arbitrary size.
/// @details Used for arrays and records that need more than 8 bytes.
/// @param name Borrowed runtime string key.
/// @param size Positive storage size in bytes; negative and zero sizes trap.
/// @return Stable context-owned zero-initialized block, or NULL after a
///   returning validation or allocation trap.
void *rt_modvar_addr_block(rt_string name, int64_t size) {
    if (size < 0) {
        rt_trap("rt_modvar: negative block size");
        return NULL;
    }
    return mv_addr(name, MV_BLOCK, (size_t)size);
}
