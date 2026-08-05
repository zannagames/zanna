//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_connections.inc
// Purpose: Budgeted FBX connection-table storage and indexed endpoint adjacency traversal.
//
// Key invariants:
//   - Property labels retain their exact non-NUL byte sequence.
//   - Child/parent endpoint maps preserve source connection order.
//   - Every retained allocation is charged before allocation and released on rollback.
//
// Ownership/Lifetime:
//   - Included by rt_fbx_loader_parse.inc after object-id hashing is defined.
//   - fbx_conn_table_t exclusively owns entries, labels, and endpoint maps until freed.
//
// Links: rt_fbx_loader.c, rt_fbx_loader_parse.inc, ADR 0173
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_connections.inc
 * @brief Implements retained FBX edges and indexed endpoint adjacency.
 *
 * Parsed object-object and object-property connections are stored in source
 * order with exact property labels. Separate power-of-two open-addressed maps
 * locate the head and tail of child-ID and parent-ID chains, allowing later
 * extraction passes to traverse only relevant edges. Entry growth, labels,
 * and both indexes are charged to the current FBX load budget.
 */
/// @brief One retained FBX connection and its two adjacency links.
typedef struct {
    /// Numeric child object identifier.
    int64_t child_id;
    /// Numeric parent object identifier.
    int64_t parent_id;
    /// Owned optional object-property label.
    char *prop;
    /// Exact property-label byte count.
    uint32_t prop_len;
    /// Next source-ordered entry with the same child ID.
    int32_t next_by_child;
    /// Next source-ordered entry with the same parent ID.
    int32_t next_by_parent;
} fbx_conn_t;

/// @brief One open-addressed endpoint-to-connection-chain slot.
typedef struct {
    /// Numeric child or parent endpoint key.
    int64_t id;
    /// First source-ordered connection index.
    int32_t head;
    /// Last source-ordered connection index.
    int32_t tail;
    /// Nonzero when this hash slot contains a key.
    int8_t occupied;
} fbx_conn_endpoint_slot_t;

/// @brief Load-local connection array plus child/parent adjacency indexes.
typedef struct {
    /// Owned connection array in source order.
    fbx_conn_t *entries;
    /// Logical connection count.
    int32_t count;
    /// Allocated entry capacity.
    int32_t capacity;
    /// Owned child-ID endpoint hash table.
    fbx_conn_endpoint_slot_t *child_slots;
    /// Power-of-two child hash capacity.
    size_t child_slot_capacity;
    /// Owned parent-ID endpoint hash table.
    fbx_conn_endpoint_slot_t *parent_slots;
    /// Power-of-two parent hash capacity.
    size_t parent_slot_capacity;
    /// Borrowed load context used for charging and probe telemetry.
    fbx_load_context_t *context;
} fbx_conn_table_t;

/// @brief Release every allocation owned by a parsed connection table.
/// @param ct Table to clear; NULL is ignored.
static void fbx_free_connections(fbx_conn_table_t *ct) {
    if (!ct)
        return;
    for (int32_t i = 0; i < ct->count; i++)
        free(ct->entries[i].prop);
    free(ct->entries);
    free(ct->child_slots);
    free(ct->parent_slots);
    memset(ct, 0, sizeof(*ct));
}

/// @brief Ensure one more connection entry can be appended.
/// @details Growth is geometric and only the newly reserved capacity delta is charged.
/// @param ct Destination connection table.
/// @return Nonzero on success; zero on overflow, budget exhaustion, or allocation failure.
static int fbx_connections_reserve_entry(fbx_conn_table_t *ct) {
    int32_t new_capacity;
    size_t old_bytes;
    size_t new_bytes;
    fbx_conn_t *grown;
    if (!ct)
        return 0;
    if (ct->count < ct->capacity)
        return 1;
    if (ct->count < 0 || ct->capacity < 0 || ct->capacity > INT32_MAX / 2)
        return 0;
    new_capacity = ct->capacity == 0 ? 64 : ct->capacity * 2;
    if (!rt_untrusted_count_ok(
            (int64_t)new_capacity, sizeof(fbx_conn_t), FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(fbx_conn_t) ||
        !fbx_budget_charge(
            ct->context, (uint64_t)(new_capacity - ct->capacity), sizeof(fbx_conn_t))) {
        return 0;
    }
    old_bytes = (size_t)ct->capacity * sizeof(fbx_conn_t);
    new_bytes = (size_t)new_capacity * sizeof(fbx_conn_t);
    grown = (fbx_conn_t *)realloc(ct->entries, new_bytes);
    if (!grown)
        return 0;
    memset((uint8_t *)grown + old_bytes, 0, new_bytes - old_bytes);
    ct->entries = grown;
    ct->capacity = new_capacity;
    return 1;
}

/// @brief Find or create one endpoint hash slot while preserving probe telemetry.
/// @param slots Power-of-two endpoint slot array.
/// @param capacity Number of slots in @p slots.
/// @param id Numeric endpoint id.
/// @param context Load context receiving one probe count per examined slot.
/// @param create Nonzero to claim the first empty slot; zero for lookup-only behavior.
/// @return Matching/created slot, or NULL when absent or the table is full.
static fbx_conn_endpoint_slot_t *fbx_connection_endpoint_slot(fbx_conn_endpoint_slot_t *slots,
                                                              size_t capacity,
                                                              int64_t id,
                                                              fbx_load_context_t *context,
                                                              int create) {
    size_t slot;
    if (!slots || capacity == 0 || (capacity & (capacity - 1u)) != 0)
        return NULL;
    slot = (size_t)fbx_hash_object_id(id) & (capacity - 1u);
    for (size_t probe = 0; probe < capacity; probe++) {
        fbx_conn_endpoint_slot_t *entry = &slots[slot];
        fbx_record_lookup_probes(context, 1u);
        if (!entry->occupied) {
            if (!create)
                return NULL;
            entry->occupied = 1;
            entry->id = id;
            entry->head = -1;
            entry->tail = -1;
            return entry;
        }
        if (entry->id == id)
            return entry;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return NULL;
}

/// @brief Build child-id and parent-id adjacency chains over an immutable connection array.
/// @details Both endpoint maps use power-of-two open addressing at no more than 50 percent load.
/// Entries for the same endpoint are linked in source order, retaining deterministic FBX ordering.
/// @param ct Parsed connection table whose entries are complete.
/// @return Nonzero on success, zero on overflow, budget exhaustion, or allocation failure.
static int fbx_build_connection_indexes(fbx_conn_table_t *ct) {
    size_t capacity = 8u;
    if (!ct)
        return 0;
    if (ct->child_slots || ct->parent_slots)
        return ct->child_slots && ct->parent_slots;
    while (capacity < (size_t)ct->count * 2u) {
        if (capacity > SIZE_MAX / 2u)
            return 0;
        capacity *= 2u;
    }
    if (!fbx_budget_charge(ct->context, capacity, sizeof(fbx_conn_endpoint_slot_t)) ||
        !fbx_budget_charge(ct->context, capacity, sizeof(fbx_conn_endpoint_slot_t))) {
        return 0;
    }
    ct->child_slots =
        (fbx_conn_endpoint_slot_t *)calloc(capacity, sizeof(fbx_conn_endpoint_slot_t));
    ct->parent_slots =
        (fbx_conn_endpoint_slot_t *)calloc(capacity, sizeof(fbx_conn_endpoint_slot_t));
    if (!ct->child_slots || !ct->parent_slots)
        return 0;
    ct->child_slot_capacity = capacity;
    ct->parent_slot_capacity = capacity;
    for (int32_t i = 0; i < ct->count; i++) {
        fbx_conn_endpoint_slot_t *child_slot = fbx_connection_endpoint_slot(
            ct->child_slots, capacity, ct->entries[i].child_id, ct->context, 1);
        fbx_conn_endpoint_slot_t *parent_slot = fbx_connection_endpoint_slot(
            ct->parent_slots, capacity, ct->entries[i].parent_id, ct->context, 1);
        if (!child_slot || !parent_slot)
            return 0;
        ct->entries[i].next_by_child = -1;
        ct->entries[i].next_by_parent = -1;
        if (child_slot->tail >= 0)
            ct->entries[child_slot->tail].next_by_child = i;
        else
            child_slot->head = i;
        child_slot->tail = i;
        if (parent_slot->tail >= 0)
            ct->entries[parent_slot->tail].next_by_parent = i;
        else
            parent_slot->head = i;
        parent_slot->tail = i;
    }
    return 1;
}

/// @brief Parse exact connection labels and build child/parent endpoint adjacency indexes.
/// @details Accepts object-object and object-property edges, dynamically retains the complete
/// property label, and rejects embedded NULs instead of silently truncating it.
/// @param root Typed binary or ASCII FBX root.
/// @param ct Zero-initialized output table.
/// @param context Active load context charged for entries, labels, and indexes.
/// @return Nonzero on success; zero on malformed labels, overflow, budget exhaustion, or failure.
static int fbx_parse_connections(fbx_node_t *root,
                                 fbx_conn_table_t *ct,
                                 fbx_load_context_t *context) {
    fbx_node_t *conns_node = fbx_find_child(root, "Connections");
    if (!ct)
        return 0;
    ct->context = context;
    if (!conns_node)
        return fbx_build_connection_indexes(ct);
    for (int32_t i = 0; i < conns_node->child_count; i++) {
        fbx_node_t *c = &conns_node->children[i];
        fbx_conn_t *entry;
        if (!c->name || strcmp(c->name, "C") != 0 || c->prop_count < 3)
            continue;
        if (!fbx_connections_reserve_entry(ct))
            return 0;
        entry = &ct->entries[ct->count];
        entry->child_id = fbx_prop_i64(c, 1);
        entry->parent_id = fbx_prop_i64(c, 2);
        entry->next_by_child = -1;
        entry->next_by_parent = -1;
        if (c->prop_count >= 4 && c->props[3].type == 'S') {
            const char *data = c->props[3].v.string.str;
            uint32_t length = c->props[3].v.string.len;
            if (!data || memchr(data, '\0', length) != NULL ||
                !fbx_budget_charge(context, (uint64_t)length + 1u, 1u)) {
                return 0;
            }
            entry->prop = (char *)malloc((size_t)length + 1u);
            if (!entry->prop)
                return 0;
            memcpy(entry->prop, data, length);
            entry->prop[length] = '\0';
            entry->prop_len = length;
        }
        ct->count++;
    }
    return fbx_build_connection_indexes(ct);
}

/// @brief Return the first source-ordered connection for a child endpoint.
/// @param ct Indexed connection table.
/// @param child_id Numeric child endpoint.
/// @return Entry index, or -1 when absent.
static int32_t fbx_connections_first_for_child(const fbx_conn_table_t *ct, int64_t child_id) {
    fbx_conn_endpoint_slot_t *slot;
    if (!ct)
        return -1;
    slot = fbx_connection_endpoint_slot(
        ct->child_slots, ct->child_slot_capacity, child_id, ct->context, 0);
    return slot ? slot->head : -1;
}

/// @brief Advance along the source-ordered adjacency chain for one child endpoint.
/// @param ct Indexed connection table.
/// @param connection_index Current entry index.
/// @return Next entry index for the same child, or -1.
static int32_t fbx_connections_next_for_child(const fbx_conn_table_t *ct,
                                              int32_t connection_index) {
    if (!ct || connection_index < 0 || connection_index >= ct->count)
        return -1;
    return ct->entries[connection_index].next_by_child;
}

/// @brief Return the first source-ordered connection for a parent endpoint.
/// @param ct Indexed connection table.
/// @param parent_id Numeric parent endpoint.
/// @return Entry index, or -1 when absent.
static int32_t fbx_connections_first_for_parent(const fbx_conn_table_t *ct, int64_t parent_id) {
    fbx_conn_endpoint_slot_t *slot;
    if (!ct)
        return -1;
    slot = fbx_connection_endpoint_slot(
        ct->parent_slots, ct->parent_slot_capacity, parent_id, ct->context, 0);
    return slot ? slot->head : -1;
}

/// @brief Advance along the source-ordered adjacency chain for one parent endpoint.
/// @param ct Indexed connection table.
/// @param connection_index Current entry index.
/// @return Next entry index for the same parent, or -1.
static int32_t fbx_connections_next_for_parent(const fbx_conn_table_t *ct,
                                               int32_t connection_index) {
    if (!ct || connection_index < 0 || connection_index >= ct->count)
        return -1;
    return ct->entries[connection_index].next_by_parent;
}

/// @brief Resolve the scene-hierarchy parent Model of a child object.
/// @details A child's connection chain is not only its hierarchy link: exporters attach the
///          same object to display layers, MotionBuilder groups, Deformer links, and — via OP
///          property links — constraints (a Biped's Character rig links every core bone as
///          `SpineLink`/`FootLink`/... BEFORE its real parent). Taking the first chain entry
///          therefore resolved constraint or group IDs as "parents", silently flattening the
///          imported skeleton, node-animation, pose, and scene hierarchies for such files.
///          Hierarchy parents are exactly the object-to-object (`OO`, no property label) links
///          whose parent is the scene root or another `Model` node, so this walk skips
///          everything else.
/// @param objects FBX Objects node used to classify candidate parents.
/// @param ct Indexed connection table.
/// @param child_id Numeric child endpoint.
/// @return Parent Model ID, or zero for a scene-root child or missing endpoint.
static int64_t fbx_find_model_parent(fbx_node_t *objects,
                                     const fbx_conn_table_t *ct,
                                     int64_t child_id) {
    for (int32_t index = fbx_connections_first_for_child(ct, child_id); index >= 0;
         index = fbx_connections_next_for_child(ct, index)) {
        const fbx_conn_t *entry = &ct->entries[index];
        fbx_node_t *parent;
        if (entry->prop)
            continue; /* OP property link, not a hierarchy attachment */
        if (entry->parent_id == 0)
            return 0; /* explicit scene-root attachment */
        parent = fbx_find_object_by_id(objects, entry->parent_id);
        if (parent && parent->name && strcmp(parent->name, "Model") == 0)
            return entry->parent_id;
    }
    return 0;
}
