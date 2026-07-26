//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_indices.inc
// Purpose: Borrowed body-to-entity and name-to-entity lookup indices for World3D.
// Key invariants:
//   - Capacities are powers of two and open-addressed tables retain no objects.
//   - The entity registry is authoritative; corrupt or stale indices are rebuilt.
// Ownership/Lifetime:
//   - World3D owns the table allocations; slots borrow body and entity pointers.
// Links: rt_game3d_internal.h, rt_game3d.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements recoverable open-addressing indices for World3D entities.
/// @details The body index maps Physics3DBody identity to Entity3D and repairs
///          deletion clusters in place. The name index maps non-empty strings
///          to the earliest registered entity and is lazily rebuilt after
///          registry/name mutations. Both tables use power-of-two capacities,
///          bounded linear probing, and borrowed pointers only.

/// @brief Murmur3 64-bit finalizer (fmix64) mixing pointer `body` into a hash; never returns 0 so
///   the value can double as a non-empty-slot marker in the open-addressing body→entity index.
/// @param body Opaque body identity to mix; NULL still maps to the non-zero sentinel.
/// @return A non-zero pointer-sized hash.
static uintptr_t game3d_hash_body_ptr(const void *body) {
    uintptr_t x = (uintptr_t)body;
    x ^= x >> 33;
    x *= (uintptr_t)0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= (uintptr_t)0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x ? x : (uintptr_t)1u;
}

/// @brief Return true when @p value is a positive power-of-two table capacity.
/// @param value Candidate table capacity.
/// @return Non-zero exactly for positive powers of two.
static int game3d_index_capacity_is_power_of_two(int32_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

/// @brief Drop body-index storage and reset its counters after invariant corruption.
/// @details The index stores borrowed body/entity pointers only; the authoritative ownership lives
///          in the world registry, so clearing the table does not release runtime objects.
/// @param world World3D whose body index is discarded; NULL is ignored.
static void game3d_world_reset_body_index(rt_game3d_world *world) {
    if (!world)
        return;
    free(world->body_index_entries);
    world->body_index_entries = NULL;
    world->body_index_count = 0;
    world->body_index_capacity = 0;
}

/// @brief Return true when the body-index arrays and counters are safe to probe with a mask.
/// @param world World3D whose index invariants are inspected.
/// @return Non-zero for a consistent empty or allocated index, otherwise zero.
static int game3d_world_body_index_usable(const rt_game3d_world *world) {
    if (!world)
        return 0;
    if (world->body_index_count < 0 || world->body_index_capacity < 0 ||
        world->body_index_count > world->body_index_capacity)
        return 0;
    if (world->body_index_capacity == 0)
        return world->body_index_entries == NULL && world->body_index_count == 0;
    return world->body_index_entries &&
           game3d_index_capacity_is_power_of_two(world->body_index_capacity);
}

/// @brief Linear-probe the body→entity table for `body`, returning its slot — the existing entry
///   or the first empty one — and setting *`found_out` when an existing mapping was matched.
/// @details Assumes a power-of-two `capacity` (mask = capacity - 1).
/// @param entries Open-addressed entry array.
/// @param capacity Positive power-of-two slot count.
/// @param body Non-NULL body identity used as the lookup key.
/// @param[out] found_out Optional flag set when the key already occupies the returned slot.
/// @return The slot index, or -1 when the table is full, empty, or the inputs are invalid.
static int game3d_world_body_index_find_slot(const rt_game3d_body_index_entry *entries,
                                             int32_t capacity,
                                             void *body,
                                             int *found_out) {
    uintptr_t mask = (uintptr_t)(capacity - 1);
    uintptr_t slot;
    if (found_out)
        *found_out = 0;
    if (!entries || capacity <= 0 || !body || !game3d_index_capacity_is_power_of_two(capacity))
        return -1;
    slot = game3d_hash_body_ptr(body) & mask;
    for (int32_t probe = 0; probe < capacity; ++probe) {
        const rt_game3d_body_index_entry *entry = &entries[slot];
        if (!entry->body)
            return (int)slot;
        if (entry->body == body) {
            if (found_out)
                *found_out = 1;
            return (int)slot;
        }
        slot = (slot + 1u) & mask;
    }
    return -1;
}

/// @brief Insert or overwrite the `body`→`entity` mapping in the existing table, assuming spare
///   capacity (callers grow first via game3d_world_grow_body_index). Bumps count only for a new
///   key.
/// @param world World3D containing a usable allocated body index.
/// @param body Non-NULL body key.
/// @param entity Borrowed entity value associated with @p body.
/// @return 1 on success, 0 when the table has no free slot.
static int game3d_world_body_index_insert_no_grow(rt_game3d_world *world,
                                                  void *body,
                                                  rt_game3d_entity *entity) {
    int found = 0;
    int slot = game3d_world_body_index_find_slot(
        world->body_index_entries, world->body_index_capacity, body, &found);
    if (slot < 0)
        return 0;
    if (!found)
        world->body_index_count++;
    world->body_index_entries[slot].body = body;
    world->body_index_entries[slot].entity = entity;
    return 1;
}

/// @brief Ensure the body→entity hash table can hold `need` entries at <= 50% load.
/// @details Corrupt metadata is reset before sizing. Growth allocates a new
///          zeroed table, reinserts every old mapping, and restores the entire
///          old table if reinsertion unexpectedly fails.
/// @param world World3D whose table may grow.
/// @param need Required mapping count.
/// @return Non-zero when existing or new capacity is sufficient, otherwise zero.
static int game3d_world_grow_body_index(rt_game3d_world *world, int32_t need) {
    int32_t new_cap = 16;
    rt_game3d_body_index_entry *new_entries;
    rt_game3d_body_index_entry *old_entries;
    int32_t old_capacity;
    int32_t old_count;
    if (!world)
        return 0;
    if (!game3d_world_body_index_usable(world))
        game3d_world_reset_body_index(world);
    if (need < 0 || need > INT32_MAX / 2)
        return 0;
    if (world->body_index_capacity > 0 &&
        world->body_index_count * 2 < world->body_index_capacity &&
        need * 2 < world->body_index_capacity)
        return 1;
    while (new_cap < need * 2) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }
    if (new_cap <= world->body_index_capacity)
        return 1;
    new_entries = (rt_game3d_body_index_entry *)calloc((size_t)new_cap, sizeof(*new_entries));
    if (!new_entries)
        return 0;
    old_entries = world->body_index_entries;
    old_capacity = world->body_index_capacity;
    old_count = world->body_index_count;
    world->body_index_entries = new_entries;
    world->body_index_capacity = new_cap;
    world->body_index_count = 0;
    for (int32_t i = 0; i < old_capacity; ++i) {
        if (old_entries[i].body && !game3d_world_body_index_insert_no_grow(
                                       world, old_entries[i].body, old_entries[i].entity)) {
            free(new_entries);
            world->body_index_entries = old_entries;
            world->body_index_capacity = old_capacity;
            world->body_index_count = old_count;
            return 0;
        }
    }
    free(old_entries);
    return 1;
}

/// @brief Rebuild the body→entity index from the world's authoritative entity registry.
/// @details Used after capacity/counter corruption is detected, or when callers need to recover
/// from
///          a stale open-addressing table. The index remains borrowed metadata; no entity refs
///          move.
/// @param world World3D whose authoritative live, spawned entity registry is scanned.
/// @return Non-zero when the index was rebuilt completely, otherwise zero with
///         the index reset empty.
static int game3d_world_rebuild_body_index(rt_game3d_world *world) {
    int32_t entity_count;
    if (!world)
        return 0;
    game3d_world_reset_body_index(world);
    entity_count = game3d_world_safe_entity_count(world);
    for (int32_t i = 0; i < entity_count; ++i) {
        rt_game3d_entity *entity = world->entities[i];
        void *body = game3d_entity_body_ref(entity);
        if (!entity || !entity->alive || !entity->spawned || entity->world != world || !body)
            continue;
        if (!game3d_world_grow_body_index(world, world->body_index_count + 1) ||
            !game3d_world_body_index_insert_no_grow(world, body, entity)) {
            game3d_world_reset_body_index(world);
            return 0;
        }
    }
    return 1;
}

/// @brief Ensure the body index is safe to probe, rebuilding it if invariant checks fail.
/// @param world World3D whose body index is required.
/// @return Non-zero when the existing or rebuilt index is usable, otherwise zero.
static int game3d_world_body_index_ensure_usable(rt_game3d_world *world) {
    if (!world)
        return 0;
    if (game3d_world_body_index_usable(world))
        return 1;
    return game3d_world_rebuild_body_index(world);
}

/// @brief Remove the entry for physics `body` from the open-addressed body→entity index.
/// @details After clearing the matched slot, every following entry in the probe
///          cluster is removed and reinserted so lookups still stop safely at
///          the first empty slot.
/// @param world World3D whose borrowed mapping is removed.
/// @param body Non-NULL Physics3DBody identity; missing keys are ignored.
void game3d_world_body_index_remove(rt_game3d_world *world, void *body) {
    int found = 0;
    int slot;
    if (!world || !body || world->body_index_capacity <= 0)
        return;
    if (!game3d_world_body_index_ensure_usable(world))
        return;
    slot = game3d_world_body_index_find_slot(
        world->body_index_entries, world->body_index_capacity, body, &found);
    if (slot < 0 || !found)
        return;
    world->body_index_entries[slot].body = NULL;
    world->body_index_entries[slot].entity = NULL;
    world->body_index_count--;
    for (;;) {
        rt_game3d_body_index_entry moved;
        slot = (slot + 1) & (world->body_index_capacity - 1);
        if (!world->body_index_entries[slot].body)
            break;
        moved = world->body_index_entries[slot];
        world->body_index_entries[slot].body = NULL;
        world->body_index_entries[slot].entity = NULL;
        world->body_index_count--;
        (void)game3d_world_body_index_insert_no_grow(world, moved.body, moved.entity);
    }
}

/// @brief Index `entity` under its physics body for O(1) body→entity lookup.
/// @param world World3D receiving the borrowed mapping.
/// @param entity Live Entity3D whose current body is indexed.
/// @return Non-zero after insertion or when there is nothing to index;
///         zero when index recovery or allocation fails.
int game3d_world_body_index_add(rt_game3d_world *world, rt_game3d_entity *entity) {
    void *body = game3d_entity_body_ref(entity);
    if (!world || !entity || !entity->alive || !body)
        return 1;
    if (!game3d_world_body_index_ensure_usable(world))
        return 0;
    if (!game3d_world_grow_body_index(world, world->body_index_count + 1))
        return 0;
    return game3d_world_body_index_insert_no_grow(world, body, entity);
}

/// @brief FNV-1a 64-bit hash of a non-NULL NUL-terminated name.
/// @details Non-NULL inputs never return zero so the hash doubles as the
///          occupied-slot marker; NULL returns zero to represent invalid input.
/// @param s Name bytes to hash, or NULL.
/// @return The non-zero name hash, or zero for NULL.
static uint64_t game3d_hash_name(const char *s) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!s)
        return 0;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

/// @brief Ensure the name hash table has a power-of-two capacity above 2×`need`
///   (kept sparse to bound probe lengths); reallocates and resets to empty on growth.
/// @details Caller must repopulate via game3d_world_rebuild_name_index after a grow,
///   since growth discards existing slots (count is reset to 0). Returns 0 on OOM.
/// @param world World3D whose parallel hash/entity arrays may grow.
/// @param need Required entry capacity before applying the sparse-load policy.
/// @return Non-zero when capacity is sufficient, otherwise zero.
static int game3d_world_grow_name_index(rt_game3d_world *world, int32_t need) {
    int32_t new_cap = 16;
    if (!world)
        return 0;
    if (world->name_index_capacity < 0 || world->name_index_count < 0 ||
        world->name_index_count > world->name_index_capacity ||
        (world->name_index_capacity > 0 &&
         (!world->name_index_hashes || !world->name_index_entities ||
          !game3d_index_capacity_is_power_of_two(world->name_index_capacity)))) {
        free(world->name_index_hashes);
        free(world->name_index_entities);
        world->name_index_hashes = NULL;
        world->name_index_entities = NULL;
        world->name_index_count = 0;
        world->name_index_capacity = 0;
        world->name_index_valid = 0;
    }
    if (need < 0 || need > INT32_MAX / 2)
        return 0;
    while (new_cap < need * 2) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }
    if (world->name_index_capacity >= new_cap)
        return 1;
    uint64_t *new_hashes = (uint64_t *)calloc((size_t)new_cap, sizeof(*new_hashes));
    rt_game3d_entity **new_entities =
        (rt_game3d_entity **)calloc((size_t)new_cap, sizeof(*new_entities));
    if (!new_hashes || !new_entities) {
        free(new_hashes);
        free(new_entities);
        return 0;
    }
    free(world->name_index_hashes);
    free(world->name_index_entities);
    world->name_index_hashes = new_hashes;
    world->name_index_entities = new_entities;
    world->name_index_capacity = new_cap;
    world->name_index_count = 0;
    return 1;
}

/// @brief Clear all name-index slots without changing capacity.
/// @details Invalid parallel-array state is freed and normalized to zero
///          capacity before counters are cleared.
/// @param world World3D whose borrowed name slots are cleared; NULL is ignored.
static void game3d_world_clear_name_index(rt_game3d_world *world) {
    if (!world)
        return;
    if (world->name_index_capacity < 0 ||
        (world->name_index_capacity > 0 &&
         (!world->name_index_hashes || !world->name_index_entities ||
          !game3d_index_capacity_is_power_of_two(world->name_index_capacity)))) {
        free(world->name_index_hashes);
        free(world->name_index_entities);
        world->name_index_hashes = NULL;
        world->name_index_entities = NULL;
        world->name_index_capacity = 0;
    }
    if (world->name_index_capacity > 0 && world->name_index_hashes && world->name_index_entities) {
        memset(world->name_index_hashes,
               0,
               (size_t)world->name_index_capacity * sizeof(*world->name_index_hashes));
        memset(world->name_index_entities,
               0,
               (size_t)world->name_index_capacity * sizeof(*world->name_index_entities));
    }
    world->name_index_count = 0;
}

/// @brief Insert `entity` into the name hash table by linear probing on its name hash.
/// @details Skips entities with no/empty name; the first entity wins a duplicate name
///   (a later same-name entity is ignored so find returns the earliest registrant).
/// @param world World3D containing a usable allocated name index.
/// @param entity Live named Entity3D to borrow into the table.
static void game3d_world_name_index_insert(rt_game3d_world *world, rt_game3d_entity *entity) {
    const char *name;
    uint64_t hash;
    int32_t mask;
    if (!world || !entity || !entity->alive || !entity->name || world->name_index_capacity <= 0 ||
        !world->name_index_hashes || !world->name_index_entities ||
        !game3d_index_capacity_is_power_of_two(world->name_index_capacity))
        return;
    name = rt_string_cstr(entity->name);
    if (!name || !*name)
        return;
    hash = game3d_hash_name(name);
    mask = world->name_index_capacity - 1;
    for (int32_t probe = 0; probe < world->name_index_capacity; ++probe) {
        int32_t slot = (int32_t)((hash + (uint64_t)probe) & (uint64_t)mask);
        rt_game3d_entity *existing = world->name_index_entities[slot];
        if (!existing) {
            world->name_index_hashes[slot] = hash;
            world->name_index_entities[slot] = entity;
            world->name_index_count++;
            return;
        }
        if (world->name_index_hashes[slot] == hash && existing->name) {
            const char *existing_name = rt_string_cstr(existing->name);
            if (existing_name && strcmp(existing_name, name) == 0)
                return;
        }
    }
}

/// @brief Rebuild the entire name→entity hash table from the current entity registry.
/// @details Called after any spawn/despawn/rename mutates the entity set. Clears all
///   slots then re-inserts every entity; grows the table first when entity_count rises.
/// @param world World3D whose authoritative entity registry is scanned.
/// @post `name_index_valid` is set only after a complete rebuild; allocation
///       failure leaves the index empty and invalid after trapping.
static void game3d_world_rebuild_name_index(rt_game3d_world *world) {
    if (!world)
        return;
    int32_t entity_count = game3d_world_safe_entity_count(world);
    if (entity_count <= 0) {
        game3d_world_clear_name_index(world);
        world->name_index_valid = 1;
        return;
    }
    if (!game3d_world_grow_name_index(world, entity_count)) {
        game3d_world_clear_name_index(world);
        world->name_index_valid = 0;
        rt_trap("Game3D.World3D: name index allocation failed");
        return;
    }
    game3d_world_clear_name_index(world);
    for (int32_t i = 0; i < entity_count; ++i)
        game3d_world_name_index_insert(world, world->entities[i]);
    world->name_index_valid = 1;
}

/// @brief Incrementally add the newest entity to the name index, rebuilding only when growth
///   replaced the open-addressing table.
/// @param world World3D receiving the borrowed name mapping.
/// @param entity Newly registered live Entity3D.
/// @return Non-zero after insertion/rebuild or when there is nothing to index;
///         zero when allocation or rebuild fails.
int game3d_world_name_index_add_entity(rt_game3d_world *world, rt_game3d_entity *entity) {
    int32_t old_capacity;
    if (!world || !entity || !entity->alive)
        return 1;
    if (!world->name_index_valid ||
        (world->name_index_capacity > 0 &&
         (!world->name_index_hashes || !world->name_index_entities ||
          !game3d_index_capacity_is_power_of_two(world->name_index_capacity)))) {
        game3d_world_rebuild_name_index(world);
        return world->name_index_valid ? 1 : 0;
    }
    old_capacity = world->name_index_capacity;
    int32_t entity_count = game3d_world_safe_entity_count(world);
    if (!game3d_world_grow_name_index(world, entity_count)) {
        game3d_world_clear_name_index(world);
        world->name_index_valid = 0;
        rt_trap("Game3D.World3D: name index allocation failed");
        return 0;
    }
    if (world->name_index_capacity != old_capacity) {
        game3d_world_clear_name_index(world);
        for (int32_t i = 0; i < entity_count; ++i)
            game3d_world_name_index_insert(world, world->entities[i]);
    } else {
        game3d_world_name_index_insert(world, entity);
    }
    world->name_index_valid = 1;
    return 1;
}

/// @brief Look up an entity by name via the hash table; returns NULL if not present.
/// @details Linear-probes from the name's hash slot, comparing full strings on a hash
///   match; stops at the first empty slot (open-addressing absence proof).
/// @param world World3D whose valid name index is searched. Corrupt arrays
///              trigger one authoritative rebuild attempt.
/// @param name Non-empty NUL-terminated entity name.
/// @return The earliest registered matching Entity3D, or NULL.
rt_game3d_entity *game3d_world_name_index_find(rt_game3d_world *world, const char *name) {
    uint64_t hash;
    int32_t mask;
    if (!world || !world->name_index_valid || !name || !*name || world->name_index_capacity <= 0)
        return NULL;
    if (!world->name_index_hashes || !world->name_index_entities ||
        !game3d_index_capacity_is_power_of_two(world->name_index_capacity)) {
        world->name_index_valid = 0;
        game3d_world_rebuild_name_index(world);
        if (!world->name_index_valid || world->name_index_capacity <= 0)
            return NULL;
    }
    hash = game3d_hash_name(name);
    mask = world->name_index_capacity - 1;
    for (int32_t probe = 0; probe < world->name_index_capacity; ++probe) {
        int32_t slot = (int32_t)((hash + (uint64_t)probe) & (uint64_t)mask);
        rt_game3d_entity *entity = world->name_index_entities[slot];
        if (!entity)
            return NULL;
        if (world->name_index_hashes[slot] == hash && entity->name) {
            const char *entity_name = rt_string_cstr(entity->name);
            if (entity_name && strcmp(entity_name, name) == 0)
                return entity;
        }
    }
    return NULL;
}
