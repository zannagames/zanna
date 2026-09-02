//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_subhandle.c
// Purpose: Runtime sub-object handle layer for GUI widgets — wraps non-widget
//          children (tree nodes, tabs, list items, menus/items, context menus,
//          status-bar and tool-bar items) in identity-stable boxed handles.
//          Split out of rt_gui_widgets.c.
//
// Key invariants:
//   - Every live sub-handle is linked into the process-global registry list and
//     carries a magic tag so stale/forged handles are rejected on lookup.
//   - Wrapping is idempotent: an existing handle for a (kind, ptr) pair is
//     reused so identity stays stable across calls.
//   - Invalidation clears the underlying pointer when its owner widget dies,
//     leaving the boxed handle safe to query (returns "invalid").
//
// Ownership/Lifetime:
//   - Sub-handles are GC objects; the registry holds weak links unlinked on
//     finalize. Underlying widget pointers are owned by the widget tree.
//
// Links: src/runtime/graphics/gui/rt_gui_widgets.c (widget lifecycle),
//        src/runtime/graphics/gui/rt_gui_internal.h (shared GUI types + API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements identity-stable managed handles for GUI subobjects.
 *
 * @details Non-widget toolkit objects are wrapped in authenticated weak
 *          runtime records indexed by `(kind, target)` and owner widget.
 *          Wrapping is idempotent, invalidation preserves safe tombstones, and
 *          intrusive-list fallbacks retain correctness when index allocation
 *          fails.
 */

#include "rt_gui_internal.h"
#include "rt_platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Runtime Subobject Handles
//=============================================================================

/// @brief Runtime discriminator for every supported GUI subobject category.
typedef enum {
    RT_GUI_HANDLE_TREE_NODE = 1,      ///< TreeView node.
    RT_GUI_HANDLE_TAB = 2,            ///< TabBar tab.
    RT_GUI_HANDLE_LISTBOX_ITEM = 3,   ///< ListBox item.
    RT_GUI_HANDLE_MENU = 4,           ///< Menu or submenu.
    RT_GUI_HANDLE_MENU_ITEM = 5,      ///< Menu command item.
    RT_GUI_HANDLE_CONTEXTMENU = 6,    ///< Standalone ContextMenu.
    RT_GUI_HANDLE_STATUSBAR_ITEM = 7, ///< StatusBar item.
    RT_GUI_HANDLE_TOOLBAR_ITEM = 8,   ///< Toolbar item.
} rt_gui_subhandle_kind_t;

/// @brief Magic value authenticating live managed GUI subhandle records.
#define RT_GUI_SUBHANDLE_MAGIC UINT64_C(0x52544755484E444C)
/// @brief Largest power-of-two index that can hold the bounded wrapper population at 70% load.
#define RT_GUI_SUBHANDLE_MAX_INDEX_CAPACITY ((size_t)1u << 21u)

/// @brief Managed weak wrapper linked into global, target, and owner indexes.
typedef struct rt_gui_subhandle {
    uint64_t magic;                      ///< Must equal @ref RT_GUI_SUBHANDLE_MAGIC while live.
    uint64_t generation;                 ///< Monotonic wrapper generation for identity diagnostics.
    uint32_t kind;                       ///< One @ref rt_gui_subhandle_kind_t value.
    uint32_t retired;                    ///< Nonzero after the lower target is invalidated.
    void *ptr;                           ///< Borrowed lower target, or NULL after invalidation.
    vg_widget_t *owner_widget;           ///< Borrowed owning widget, or NULL.
    uint64_t owner_widget_id;            ///< Stable owner identity captured while live.
    struct rt_gui_subhandle *next;       ///< Next wrapper in the global intrusive list.
    struct rt_gui_subhandle *prev;       ///< Previous wrapper in the global intrusive list.
    struct rt_gui_subhandle *owner_next; ///< Next wrapper in the owner bucket.
    struct rt_gui_subhandle *owner_prev; ///< Previous wrapper in the owner bucket.
    bool target_indexed;                 ///< Whether the target hash index contains this wrapper.
    bool owner_indexed;                  ///< Whether an owner bucket contains this wrapper.
} rt_gui_subhandle_t;

/// @brief Head of the authoritative global wrapper list.
static rt_gui_subhandle_t *s_gui_subhandles = NULL;
/// @brief Generation assigned to the next newly allocated wrapper.
static uint64_t s_gui_subhandle_generation = 1;
/// @brief Number of currently allocated wrappers.
static size_t s_gui_subhandle_count = 0;

/// @brief One open-addressed `(kind, target)` lookup slot.
typedef struct rt_gui_subhandle_slot {
    void *ptr;                  ///< Borrowed indexed target address.
    rt_gui_subhandle_t *handle; ///< Weak wrapper stored for the key.
    uint32_t kind;              ///< Indexed subhandle-kind discriminator.
    uint8_t state;              ///< Empty, live, or tombstone slot state.
} rt_gui_subhandle_slot_t;

/// @brief Slot-state values for the open-addressed target index.
enum {
    RT_GUI_SUBHANDLE_SLOT_EMPTY = 0,     ///< Probe terminator with no prior entry.
    RT_GUI_SUBHANDLE_SLOT_LIVE = 1,      ///< Occupied key and wrapper.
    RT_GUI_SUBHANDLE_SLOT_TOMBSTONE = 2, ///< Deleted slot retained for probing.
};

/// @brief Owned target-index slot allocation.
static rt_gui_subhandle_slot_t *s_target_slots = NULL;
/// @brief Power-of-two target-index slot count.
static size_t s_target_capacity = 0;
/// @brief Number of live target keys.
static size_t s_target_live = 0;
/// @brief Number of live or tombstone target slots.
static size_t s_target_occupied = 0;
/// @brief Whether the target index represents every wrapper with a target.
static bool s_target_index_complete = false;

/// @brief Owned hash buckets keyed by owner-widget identity.
static rt_gui_subhandle_t **s_owner_buckets = NULL;
/// @brief Power-of-two owner-bucket count.
static size_t s_owner_capacity = 0;
/// @brief Number of wrappers linked through owner buckets.
static size_t s_owner_indexed_count = 0;
/// @brief Whether the owner index represents every wrapper with an owner.
static bool s_owner_index_complete = false;

/// @brief Probe count consumed by the latest target lookup.
static size_t s_target_last_probes = 0;
/// @brief Largest bounded target lookup probe count observed.
static size_t s_target_max_probes = 0;

/// @brief Mix an address and subhandle kind into a stable process-local hash.
/// @details The hash never dereferences the address and is valid for live targets and retained
///          tombstones alike. Capacity masking requires a power-of-two table size.
/// @param kind Runtime subhandle kind discriminator.
/// @param ptr Opaque lower-toolkit target address.
/// @return Mixed machine-word hash value.
static size_t rt_gui_subhandle_target_hash(rt_gui_subhandle_kind_t kind, const void *ptr) {
    uint64_t value = (uint64_t)(uintptr_t)ptr;
    value ^= (uint64_t)(uint32_t)kind * UINT64_C(0x9E3779B185EBCA87);
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31;
    return (size_t)value;
}

/// @brief Hash an owner widget address for the owner-linked bucket index.
/// @param owner Widget address; never dereferenced.
/// @return Mixed machine-word hash value.
static size_t rt_gui_subhandle_owner_hash(const vg_widget_t *owner) {
    uint64_t value = (uint64_t)(uintptr_t)owner;
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return (size_t)value;
}

/// @brief Insert one target into an already allocated open-addressed table.
/// @details The caller guarantees power-of-two capacity and at least one available slot. Tombstone
///          reuse keeps occupied-count semantics correct during churn.
/// @param slots Destination table.
/// @param capacity Destination slot count.
/// @param handle Wrapper to index; its target must be non-NULL.
/// @return true on insertion, false only when bounded probing found no slot.
static bool rt_gui_subhandle_target_insert_raw(rt_gui_subhandle_slot_t *slots,
                                               size_t capacity,
                                               rt_gui_subhandle_t *handle) {
    if (!slots || capacity == 0 || !handle || !handle->ptr)
        return false;
    size_t mask = capacity - 1;
    size_t index =
        rt_gui_subhandle_target_hash((rt_gui_subhandle_kind_t)handle->kind, handle->ptr) & mask;
    size_t first_tombstone = SIZE_MAX;
    for (size_t probes = 0; probes < capacity; ++probes) {
        rt_gui_subhandle_slot_t *slot = &slots[index];
        if (slot->state == RT_GUI_SUBHANDLE_SLOT_EMPTY) {
            if (first_tombstone != SIZE_MAX)
                slot = &slots[first_tombstone];
            slot->ptr = handle->ptr;
            slot->kind = handle->kind;
            slot->handle = handle;
            slot->state = RT_GUI_SUBHANDLE_SLOT_LIVE;
            return true;
        }
        if (slot->state == RT_GUI_SUBHANDLE_SLOT_TOMBSTONE && first_tombstone == SIZE_MAX)
            first_tombstone = index;
        index = (index + 1) & mask;
    }
    if (first_tombstone != SIZE_MAX) {
        rt_gui_subhandle_slot_t *slot = &slots[first_tombstone];
        slot->ptr = handle->ptr;
        slot->kind = handle->kind;
        slot->handle = handle;
        slot->state = RT_GUI_SUBHANDLE_SLOT_LIVE;
        return true;
    }
    return false;
}

/// @brief Rebuild the complete target index at a requested power-of-two capacity.
/// @details Allocation failure leaves the old index untouched and callers retain the intrusive
///          list fallback. Successful rebuilds remove all probe tombstones and mark every wrapper
///          with a retained target as indexed.
/// @param capacity New slot count; values below 64 are promoted.
/// @return true on success, false on overflow or allocation failure.
static bool rt_gui_subhandle_target_rehash(size_t capacity) {
    if (capacity < 64)
        capacity = 64;
    if ((capacity & (capacity - 1)) != 0 || capacity > RT_GUI_SUBHANDLE_MAX_INDEX_CAPACITY ||
        capacity > SIZE_MAX / sizeof(*s_target_slots))
        return false;
    rt_gui_subhandle_slot_t *slots = (rt_gui_subhandle_slot_t *)calloc(capacity, sizeof(*slots));
    if (!slots)
        return false;

    size_t live = 0;
    for (rt_gui_subhandle_t *handle = s_gui_subhandles; handle; handle = handle->next) {
        if (!handle->ptr)
            continue;
        if (!rt_gui_subhandle_target_insert_raw(slots, capacity, handle)) {
            free(slots);
            return false;
        }
        ++live;
    }
    free(s_target_slots);
    s_target_slots = slots;
    s_target_capacity = capacity;
    s_target_live = live;
    s_target_occupied = live;
    s_target_index_complete = true;
    for (rt_gui_subhandle_t *handle = s_gui_subhandles; handle; handle = handle->next)
        handle->target_indexed = handle->ptr != NULL;
    return true;
}

/// @brief Ensure the target index has room for one additional live key.
/// @return true when insertion can be attempted, false when allocation failed and no table exists.
static bool rt_gui_subhandle_target_ensure_capacity(void) {
    if (s_target_capacity == 0)
        return rt_gui_subhandle_target_rehash(64);
    bool crowded = (s_target_occupied + 1) * 10 >= s_target_capacity * 7;
    bool tombstone_heavy = s_target_occupied > s_target_live * 2 + 16;
    if (!crowded && !tombstone_heavy)
        return true;
    size_t next = crowded && s_target_capacity <= RT_GUI_SUBHANDLE_MAX_INDEX_CAPACITY / 2u
                      ? s_target_capacity * 2u
                      : s_target_capacity;
    if (crowded && next == s_target_capacity)
        return false;
    return rt_gui_subhandle_target_rehash(next);
}

/// @brief Lookup one target through the bounded hash index with an OOM fallback list.
/// @param kind Required target kind.
/// @param ptr Required non-NULL target address.
/// @return Stable wrapper for the key, or NULL.
static rt_gui_subhandle_t *rt_gui_find_subhandle(rt_gui_subhandle_kind_t kind, const void *ptr) {
    if (!ptr)
        return NULL;
    size_t probes_used = 0;
    if (s_target_slots && s_target_capacity > 0) {
        size_t mask = s_target_capacity - 1;
        size_t index = rt_gui_subhandle_target_hash(kind, ptr) & mask;
        for (; probes_used < s_target_capacity; ++probes_used) {
            rt_gui_subhandle_slot_t *slot = &s_target_slots[index];
            if (slot->state == RT_GUI_SUBHANDLE_SLOT_EMPTY) {
                ++probes_used;
                break;
            }
            if (slot->state == RT_GUI_SUBHANDLE_SLOT_LIVE && slot->kind == (uint32_t)kind &&
                slot->ptr == ptr) {
                ++probes_used;
                s_target_last_probes = probes_used;
                if (probes_used > s_target_max_probes)
                    s_target_max_probes = probes_used;
                return slot->handle;
            }
            index = (index + 1) & mask;
        }
    }
    s_target_last_probes = probes_used;
    if (probes_used > s_target_max_probes)
        s_target_max_probes = probes_used;
    if (s_target_index_complete)
        return NULL;
    for (rt_gui_subhandle_t *handle = s_gui_subhandles; handle; handle = handle->next) {
        if (handle->magic == RT_GUI_SUBHANDLE_MAGIC && handle->kind == (uint32_t)kind &&
            handle->ptr == ptr) {
            return handle;
        }
    }
    return NULL;
}

/// @brief Add a wrapper's current target to the hash index.
/// @param handle Newly linked wrapper with a non-NULL target.
static void rt_gui_subhandle_target_link(rt_gui_subhandle_t *handle) {
    if (!handle || !handle->ptr)
        return;
    if (!rt_gui_subhandle_target_ensure_capacity()) {
        handle->target_indexed = false;
        s_target_index_complete = false;
        return;
    }
    if (handle->target_indexed)
        return;
    size_t occupied_before = s_target_occupied;
    size_t mask = s_target_capacity - 1;
    size_t index =
        rt_gui_subhandle_target_hash((rt_gui_subhandle_kind_t)handle->kind, handle->ptr) & mask;
    size_t first_tombstone = SIZE_MAX;
    for (size_t probes = 0; probes < s_target_capacity; ++probes) {
        rt_gui_subhandle_slot_t *slot = &s_target_slots[index];
        if (slot->state == RT_GUI_SUBHANDLE_SLOT_EMPTY ||
            slot->state == RT_GUI_SUBHANDLE_SLOT_TOMBSTONE) {
            if (slot->state == RT_GUI_SUBHANDLE_SLOT_TOMBSTONE && first_tombstone == SIZE_MAX)
                first_tombstone = index;
            if (slot->state == RT_GUI_SUBHANDLE_SLOT_TOMBSTONE) {
                index = (index + 1) & mask;
                continue;
            }
            if (first_tombstone != SIZE_MAX)
                slot = &s_target_slots[first_tombstone];
            else
                ++s_target_occupied;
            slot->ptr = handle->ptr;
            slot->kind = handle->kind;
            slot->handle = handle;
            slot->state = RT_GUI_SUBHANDLE_SLOT_LIVE;
            handle->target_indexed = true;
            ++s_target_live;
            return;
        }
        index = (index + 1) & mask;
    }
    if (first_tombstone != SIZE_MAX) {
        rt_gui_subhandle_slot_t *slot = &s_target_slots[first_tombstone];
        slot->ptr = handle->ptr;
        slot->kind = handle->kind;
        slot->handle = handle;
        slot->state = RT_GUI_SUBHANDLE_SLOT_LIVE;
        handle->target_indexed = true;
        ++s_target_live;
        return;
    }
    s_target_occupied = occupied_before;
    handle->target_indexed = false;
    s_target_index_complete = false;
}

/// @brief Remove a wrapper from the target index without touching its retained payload pointer.
/// @param handle Indexed or fallback-only wrapper; NULL is ignored.
static void rt_gui_subhandle_target_unlink(rt_gui_subhandle_t *handle) {
    if (!handle || !handle->target_indexed || !s_target_slots || s_target_capacity == 0)
        return;
    size_t mask = s_target_capacity - 1;
    size_t index =
        rt_gui_subhandle_target_hash((rt_gui_subhandle_kind_t)handle->kind, handle->ptr) & mask;
    for (size_t probes = 0; probes < s_target_capacity; ++probes) {
        rt_gui_subhandle_slot_t *slot = &s_target_slots[index];
        if (slot->state == RT_GUI_SUBHANDLE_SLOT_EMPTY)
            break;
        if (slot->state == RT_GUI_SUBHANDLE_SLOT_LIVE && slot->handle == handle) {
            slot->ptr = NULL;
            slot->handle = NULL;
            slot->kind = 0;
            slot->state = RT_GUI_SUBHANDLE_SLOT_TOMBSTONE;
            if (s_target_live > 0)
                --s_target_live;
            handle->target_indexed = false;
            return;
        }
        index = (index + 1) & mask;
    }
    handle->target_indexed = false;
    s_target_index_complete = false;
}

/// @brief Rebuild owner buckets from the global wrapper list.
/// @param capacity Power-of-two bucket count; values below 32 are promoted.
/// @return true on success, false on invalid capacity or allocation failure.
static bool rt_gui_subhandle_owner_rehash(size_t capacity) {
    if (capacity < 32)
        capacity = 32;
    // cppcheck-suppress divideSizeof
    // The allocation is intentionally an array of wrapper pointers, not wrappers.
    if ((capacity & (capacity - 1)) != 0 || capacity > RT_GUI_SUBHANDLE_MAX_INDEX_CAPACITY ||
        capacity > SIZE_MAX / sizeof(*s_owner_buckets))
        return false;
    rt_gui_subhandle_t **buckets = (rt_gui_subhandle_t **)calloc(capacity, sizeof(*buckets));
    if (!buckets)
        return false;
    size_t count = 0;
    for (rt_gui_subhandle_t *handle = s_gui_subhandles; handle; handle = handle->next) {
        handle->owner_prev = NULL;
        handle->owner_next = NULL;
        handle->owner_indexed = false;
        if (!handle->owner_widget)
            continue;
        size_t bucket = rt_gui_subhandle_owner_hash(handle->owner_widget) & (capacity - 1);
        handle->owner_next = buckets[bucket];
        if (buckets[bucket])
            buckets[bucket]->owner_prev = handle;
        buckets[bucket] = handle;
        handle->owner_indexed = true;
        ++count;
    }
    free(s_owner_buckets);
    s_owner_buckets = buckets;
    s_owner_capacity = capacity;
    s_owner_indexed_count = count;
    s_owner_index_complete = true;
    return true;
}

/// @brief Link one wrapper into the hash bucket for its owner widget.
/// @param handle Wrapper with an optional owner.
static void rt_gui_subhandle_owner_link(rt_gui_subhandle_t *handle) {
    if (!handle || !handle->owner_widget)
        return;
    if (s_owner_capacity == 0 && !rt_gui_subhandle_owner_rehash(32)) {
        s_owner_index_complete = false;
        return;
    }
    if (handle->owner_indexed)
        return;
    if (s_owner_capacity > 0 && (s_owner_indexed_count + 1) * 2 > s_owner_capacity * 3) {
        size_t next = s_owner_capacity <= RT_GUI_SUBHANDLE_MAX_INDEX_CAPACITY / 2u
                          ? s_owner_capacity * 2u
                          : s_owner_capacity;
        if (next > s_owner_capacity)
            (void)rt_gui_subhandle_owner_rehash(next);
    }
    if (handle->owner_indexed)
        return;
    if (!s_owner_buckets || s_owner_capacity == 0) {
        s_owner_index_complete = false;
        return;
    }
    size_t bucket = rt_gui_subhandle_owner_hash(handle->owner_widget) & (s_owner_capacity - 1);
    handle->owner_prev = NULL;
    handle->owner_next = s_owner_buckets[bucket];
    if (handle->owner_next)
        handle->owner_next->owner_prev = handle;
    s_owner_buckets[bucket] = handle;
    handle->owner_indexed = true;
    ++s_owner_indexed_count;
}

/// @brief Unlink one wrapper from its owner bucket in constant time.
/// @param handle Wrapper whose current `owner_widget` still names its bucket key.
static void rt_gui_subhandle_owner_unlink(rt_gui_subhandle_t *handle) {
    if (!handle || !handle->owner_indexed || !handle->owner_widget || !s_owner_buckets ||
        s_owner_capacity == 0) {
        if (handle) {
            handle->owner_next = NULL;
            handle->owner_prev = NULL;
            handle->owner_indexed = false;
        }
        return;
    }
    size_t bucket = rt_gui_subhandle_owner_hash(handle->owner_widget) & (s_owner_capacity - 1);
    if (handle->owner_prev)
        handle->owner_prev->owner_next = handle->owner_next;
    else if (s_owner_buckets[bucket] == handle)
        s_owner_buckets[bucket] = handle->owner_next;
    else
        s_owner_index_complete = false;
    if (handle->owner_next)
        handle->owner_next->owner_prev = handle->owner_prev;
    handle->owner_next = NULL;
    handle->owner_prev = NULL;
    handle->owner_indexed = false;
    if (s_owner_indexed_count > 0)
        --s_owner_indexed_count;
}

/// @brief Reclaim retired lower subobjects belonging to one live owner widget.
/// @param owner Widget whose retired item lists should be inspected.
static void rt_gui_collect_retired_for_owner(vg_widget_t *owner);

/// @brief Detach a subhandle from the global intrusive list.
/// @details Target and owner indexes must be unlinked before this helper. The global list remains
///          the allocation-failure fallback and authoritative source for index rebuilds.
/// @param handle Wrapper to detach; NULL is ignored.
static void rt_gui_subhandle_global_unlink(rt_gui_subhandle_t *handle) {
    if (!handle)
        return;
    if (handle->prev)
        handle->prev->next = handle->next;
    else if (s_gui_subhandles == handle)
        s_gui_subhandles = handle->next;
    if (handle->next)
        handle->next->prev = handle->prev;
    handle->next = NULL;
    handle->prev = NULL;
    if (s_gui_subhandle_count > 0)
        --s_gui_subhandle_count;
    if (s_gui_subhandle_count == 0) {
        free(s_target_slots);
        s_target_slots = NULL;
        s_target_capacity = 0;
        s_target_live = 0;
        s_target_occupied = 0;
        s_target_index_complete = false;
        free(s_owner_buckets);
        s_owner_buckets = NULL;
        s_owner_capacity = 0;
        s_owner_indexed_count = 0;
        s_owner_index_complete = false;
    }
}

/// @brief Return whether a retained target still represents a live lower-toolkit subobject.
/// @details The caller must first establish that `owner_widget` is live; retired payload storage is
///          retained by that owner, making the type-specific magic check safe. Unknown kinds are
///          treated as retired.
/// @param handle Wrapper with a non-NULL target.
/// @return true only for a currently live target of the wrapper's declared kind.
static bool rt_gui_subhandle_payload_is_live(const rt_gui_subhandle_t *handle) {
    if (!handle || !handle->ptr)
        return false;
    switch ((rt_gui_subhandle_kind_t)handle->kind) {
        case RT_GUI_HANDLE_TREE_NODE:
            return vg_tree_node_is_live((const vg_tree_node_t *)handle->ptr);
        case RT_GUI_HANDLE_TAB:
            return vg_tab_is_live((const vg_tab_t *)handle->ptr);
        case RT_GUI_HANDLE_LISTBOX_ITEM:
            return vg_listbox_item_is_live((const vg_listbox_item_t *)handle->ptr);
        case RT_GUI_HANDLE_MENU:
            return vg_menu_is_live((const vg_menu_t *)handle->ptr);
        case RT_GUI_HANDLE_MENU_ITEM:
            return vg_menu_item_is_live((const vg_menu_item_t *)handle->ptr);
        case RT_GUI_HANDLE_CONTEXTMENU: {
            const vg_contextmenu_t *menu = (const vg_contextmenu_t *)handle->ptr;
            return vg_contextmenu_is_live(menu) && menu->base.type == VG_WIDGET_MENU;
        }
        case RT_GUI_HANDLE_STATUSBAR_ITEM:
            return vg_statusbar_item_is_live((const vg_statusbar_item_t *)handle->ptr);
        case RT_GUI_HANDLE_TOOLBAR_ITEM:
            return vg_toolbar_item_is_live((const vg_toolbar_item_t *)handle->ptr);
        default:
            return false;
    }
}

/// @brief Mark a wrapper as a retained tombstone without discarding its indexed identity.
/// @details Keeping the target address indexed prevents duplicate wrappers and lets the finalizer
///          trigger exact automatic reclamation. Public conversions reject the wrapper once this
///          bit is set and therefore never expose or dereference the retired payload.
/// @param handle Wrapper whose lower target was retired.
static void rt_gui_subhandle_mark_retired(rt_gui_subhandle_t *handle) {
    if (handle && handle->magic == RT_GUI_SUBHANDLE_MAGIC)
        handle->retired = 1;
}

/// @brief Permanently detach a wrapper from a target and owner before payload memory is freed.
/// @details This is used for owner destruction and explicit prune operations. Removing both
///          indexes before clearing the addresses permits future allocations to reuse the same
///          address with a new generation while old script wrappers remain inert.
/// @param handle Wrapper to invalidate; NULL and finalized wrappers are ignored.
static void rt_gui_subhandle_invalidate(rt_gui_subhandle_t *handle) {
    if (!handle || handle->magic != RT_GUI_SUBHANDLE_MAGIC)
        return;
    rt_gui_subhandle_target_unlink(handle);
    rt_gui_subhandle_owner_unlink(handle);
    handle->ptr = NULL;
    handle->owner_widget = NULL;
    handle->owner_widget_id = 0;
    handle->retired = 1;
}

/// @brief GC finalizer for a subhandle and automatic tombstone reclamation trigger.
/// @details Index links are weak and therefore removed before the runtime object is freed. If the
///          owner is still live and the lower target has become a retained tombstone, collecting
///          that owner after unlinking can reclaim the exact retirement group now that the last
///          stable wrapper has disappeared.
/// @param obj GC-managed @ref rt_gui_subhandle_t being finalized; NULL is a no-op.
static void rt_gui_subhandle_finalize(void *obj) {
    rt_gui_subhandle_t *handle = (rt_gui_subhandle_t *)obj;
    if (!handle)
        return;
    vg_widget_t *owner = handle->owner_widget;
    bool owner_live = owner && vg_widget_is_live(owner) && owner->id == handle->owner_widget_id;
    bool retired =
        owner_live && handle->ptr && (handle->retired || !rt_gui_subhandle_payload_is_live(handle));
    rt_gui_subhandle_target_unlink(handle);
    rt_gui_subhandle_owner_unlink(handle);
    rt_gui_subhandle_global_unlink(handle);
    handle->magic = 0;
    handle->generation = 0;
    handle->ptr = NULL;
    handle->owner_widget = NULL;
    handle->owner_widget_id = 0;
    handle->retired = 1;
    if (retired)
        rt_gui_collect_retired_for_owner(owner);
}

/// @brief Safe-cast an opaque handle to a subhandle of the expected @p kind.
/// @details Runtime heap metadata is validated before any private wrapper fields are read.
/// @param handle Candidate opaque runtime object.
/// @param kind Exact subhandle discriminator required by the caller.
/// @return Borrowed matching subhandle, or NULL for a foreign, finalized, or wrong-kind object.
static rt_gui_subhandle_t *rt_gui_subhandle_checked(void *handle, rt_gui_subhandle_kind_t kind) {
    if (!rt_obj_is_instance(handle, 0, sizeof(rt_gui_subhandle_t)))
        return NULL;
    rt_gui_subhandle_t *sub = (rt_gui_subhandle_t *)handle;
    if (sub->magic != RT_GUI_SUBHANDLE_MAGIC || sub->kind != (uint32_t)kind)
        return NULL;
    return sub;
}

/// @brief True if the subhandle's owner widget is still alive (or it has no owner).
/// @details A dead owner triggers hard invalidation and a false return, so callers never
///          dereference a sub-object whose widget storage has already been destroyed.
/// @param handle Candidate authenticated subhandle.
/// @return True for an ownerless handle or matching live owner generation, otherwise false.
static bool rt_gui_subhandle_owner_is_live(rt_gui_subhandle_t *handle) {
    if (!handle)
        return false;
    if (!handle->owner_widget)
        return true;
    if (vg_widget_is_live(handle->owner_widget) &&
        handle->owner_widget->id == handle->owner_widget_id)
        return true;
    rt_gui_subhandle_invalidate(handle);
    return false;
}

/// @brief Find the top-level widget that owns a menu item — its context menu, or
///        the menubar reached through its parent menu. NULL if free-floating.
/// @param item Borrowed live menu item.
/// @return Borrowed owner widget, preferring ContextMenu, or NULL when detached.
static vg_widget_t *rt_gui_owner_widget_for_menu_item(vg_menu_item_t *item) {
    if (!item)
        return NULL;
    if (item->owner_contextmenu)
        return &item->owner_contextmenu->base;
    if (item->parent_menu && item->parent_menu->owner_menubar)
        return &item->parent_menu->owner_menubar->base;
    return NULL;
}

/// @brief The menubar widget that owns a menu, or NULL if the menu isn't in a menubar.
/// @param menu Borrowed live or detached menu.
/// @return Borrowed owning MenuBar widget, or NULL.
static vg_widget_t *rt_gui_owner_widget_for_menu(vg_menu_t *menu) {
    return menu && menu->owner_menubar ? &menu->owner_menubar->base : NULL;
}

/// @brief Get-or-create the GC subhandle that wraps a widget sub-object.
/// @details The entry point of the subhandle system: returns the existing handle for
///          (@p kind, @p ptr) — refreshing its owner — or allocates a new one and links
///          it into the global list. Reuse guarantees a stable identity per sub-object
///          across calls. Returns NULL on NULL ptr or allocation failure.
/// @param kind Lower payload kind recorded in the wrapper and target index.
/// @param ptr Borrowed live lower-toolkit payload address.
/// @param owner_widget Borrowed owner widget used for generation validation; may be NULL.
/// @return Existing or newly allocated GC-managed stable wrapper, or NULL on invalid input or
///         allocation failure.
static void *rt_gui_wrap_subhandle(rt_gui_subhandle_kind_t kind,
                                   void *ptr,
                                   vg_widget_t *owner_widget) {
    if (!ptr)
        return NULL;
    rt_gui_subhandle_t *existing = rt_gui_find_subhandle(kind, ptr);
    if (existing && existing->owner_widget &&
        (!vg_widget_is_live(existing->owner_widget) ||
         existing->owner_widget->id != existing->owner_widget_id)) {
        // A lower-toolkit-only owner teardown can bypass the runtime's proactive invalidation.
        // Reject that stale generation before a newly allocated payload reuses its address.
        rt_gui_subhandle_invalidate(existing);
        existing = NULL;
    }
    if (existing) {
        if (existing->owner_widget != owner_widget) {
            rt_gui_subhandle_owner_unlink(existing);
            existing->owner_widget = owner_widget;
            existing->owner_widget_id = owner_widget ? owner_widget->id : 0;
            rt_gui_subhandle_owner_link(existing);
        }
        /* Every return hands the caller one reference (ADR 0314): the
           existing wrapper is retained exactly like a freshly allocated one. */
        rt_obj_retain_maybe(existing);
        return existing;
    }
    if (s_gui_subhandle_count >= RT_GUI_MAX_COLLECTION_ITEMS)
        return NULL;
    rt_gui_subhandle_t *handle =
        (rt_gui_subhandle_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_gui_subhandle_t));
    if (!handle)
        return NULL;
    handle->magic = RT_GUI_SUBHANDLE_MAGIC;
    handle->generation = s_gui_subhandle_generation++;
    if (s_gui_subhandle_generation == 0)
        s_gui_subhandle_generation = 1;
    handle->kind = (uint32_t)kind;
    handle->retired = 0;
    handle->ptr = ptr;
    handle->owner_widget = owner_widget;
    handle->owner_widget_id = owner_widget ? owner_widget->id : 0;
    handle->prev = NULL;
    handle->next = s_gui_subhandles;
    if (s_gui_subhandles)
        s_gui_subhandles->prev = handle;
    s_gui_subhandles = handle;
    ++s_gui_subhandle_count;
    rt_gui_subhandle_target_link(handle);
    rt_gui_subhandle_owner_link(handle);
    rt_obj_set_finalizer(handle, rt_gui_subhandle_finalize);
    return handle;
}

/// @brief Return the stable managed wrapper for a TreeView node.
/// @param node Borrowed live node whose TreeView becomes the owner generation.
/// @return Stable GC-managed TreeNode handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_tree_node(vg_tree_node_t *node) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_TREE_NODE, node, node && node->owner ? &node->owner->base : NULL);
}

/// @brief Return the stable managed wrapper for a TabBar tab.
/// @param tab Borrowed live tab whose TabBar becomes the owner generation.
/// @return Stable GC-managed Tab handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_tab(vg_tab_t *tab) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_TAB, tab, tab && tab->owner ? &tab->owner->base : NULL);
}

/// @brief Return the stable managed wrapper for a ListBox item.
/// @param item Borrowed live item whose ListBox becomes the owner generation.
/// @return Stable GC-managed ListBoxItem handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_listbox_item(vg_listbox_item_t *item) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_LISTBOX_ITEM, item, item && item->owner ? &item->owner->base : NULL);
}

/// @brief Return the stable managed wrapper for a regular Menu record.
/// @param menu Borrowed live menu associated with its owning MenuBar when present.
/// @return Stable GC-managed Menu handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_menu(vg_menu_t *menu) {
    return rt_gui_wrap_subhandle(RT_GUI_HANDLE_MENU, menu, rt_gui_owner_widget_for_menu(menu));
}

/// @brief Return the stable managed wrapper for a regular or context-menu item.
/// @param item Borrowed live item associated with its ContextMenu/MenuBar owner.
/// @return Stable GC-managed MenuItem handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_menu_item(vg_menu_item_t *item) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_MENU_ITEM, item, rt_gui_owner_widget_for_menu_item(item));
}

/// @brief Return the stable managed wrapper for a ContextMenu root.
/// @param menu Borrowed live ContextMenu, which also serves as the owner widget.
/// @return Stable GC-managed ContextMenu handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_contextmenu(vg_contextmenu_t *menu) {
    return rt_gui_wrap_subhandle(RT_GUI_HANDLE_CONTEXTMENU, menu, menu ? &menu->base : NULL);
}

/// @brief Return the stable managed wrapper for a StatusBar item.
/// @param item Borrowed live item whose StatusBar becomes the owner generation.
/// @return Stable GC-managed StatusBarItem handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_statusbar_item(vg_statusbar_item_t *item) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_STATUSBAR_ITEM, item, item && item->owner ? &item->owner->base : NULL);
}

/// @brief Return the stable managed wrapper for a Toolbar item.
/// @param item Borrowed live item whose Toolbar becomes the owner generation.
/// @return Stable GC-managed ToolbarItem handle, or NULL for invalid input/allocation failure.
void *rt_gui_wrap_toolbar_item(vg_toolbar_item_t *item) {
    return rt_gui_wrap_subhandle(
        RT_GUI_HANDLE_TOOLBAR_ITEM, item, item && item->owner ? &item->owner->base : NULL);
}

/// @brief Resolve a managed TreeNode handle to its live lower node.
/// @details Rejects retired/wrong-kind wrappers, validates the owner generation, and lazily marks
///          the wrapper retired when the lower liveness registry no longer recognizes its target.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live node, or NULL when validation fails.
vg_tree_node_t *rt_gui_tree_node_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_TREE_NODE);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_tree_node_t *node = (vg_tree_node_t *)sub->ptr;
    if (!vg_tree_node_is_live(node)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return node;
}

/// @brief Resolve a managed Tab handle to its live lower tab.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live tab, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_tab_t *rt_gui_tab_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_TAB);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_tab_t *tab = (vg_tab_t *)sub->ptr;
    if (!vg_tab_is_live(tab)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return tab;
}

/// @brief Resolve a managed ListBoxItem handle to its live lower item.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live item, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_listbox_item_t *rt_gui_listbox_item_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_LISTBOX_ITEM);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_listbox_item_t *item = (vg_listbox_item_t *)sub->ptr;
    if (!vg_listbox_item_is_live(item)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return item;
}

/// @brief Resolve a managed Menu handle to its live lower menu record.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live menu, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_menu_t *rt_gui_menu_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_MENU);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_menu_t *menu = (vg_menu_t *)sub->ptr;
    if (!vg_menu_is_live(menu)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return menu;
}

/// @brief Resolve a managed MenuItem handle to its live lower item record.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live item, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_menu_item_t *rt_gui_menu_item_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_MENU_ITEM);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_menu_item_t *item = (vg_menu_item_t *)sub->ptr;
    if (!vg_menu_item_is_live(item)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return item;
}

/// @brief Resolve a managed ContextMenu handle to its live lower menu widget.
/// @details In addition to owner/payload liveness, requires the lower base type to remain
///          @c VG_WIDGET_MENU before returning it.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live ContextMenu, or NULL when validation fails.
vg_contextmenu_t *rt_gui_contextmenu_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_CONTEXTMENU);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_contextmenu_t *menu = (vg_contextmenu_t *)sub->ptr;
    if (!vg_contextmenu_is_live(menu) || menu->base.type != VG_WIDGET_MENU) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return menu;
}

/// @brief Resolve a managed StatusBarItem handle to its live lower item.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live item, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_statusbar_item_t *rt_gui_statusbar_item_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_STATUSBAR_ITEM);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_statusbar_item_t *item = (vg_statusbar_item_t *)sub->ptr;
    if (!vg_statusbar_item_is_live(item)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return item;
}

/// @brief Resolve a managed ToolbarItem handle to its live lower item.
/// @param handle Candidate opaque runtime handle.
/// @return Borrowed live item, or NULL for wrong-kind, retired, stale-owner, or dead payload state.
vg_toolbar_item_t *rt_gui_toolbar_item_from_handle(void *handle) {
    rt_gui_subhandle_t *sub = rt_gui_subhandle_checked(handle, RT_GUI_HANDLE_TOOLBAR_ITEM);
    if (!sub || !sub->ptr || sub->retired)
        return NULL;
    if (!rt_gui_subhandle_owner_is_live(sub))
        return NULL;
    vg_toolbar_item_t *item = (vg_toolbar_item_t *)sub->ptr;
    if (!vg_toolbar_item_is_live(item)) {
        rt_gui_subhandle_mark_retired(sub);
        return NULL;
    }
    return item;
}

/// @brief Return whether a stable wrapper still references one retained target address.
/// @param kind Subhandle kind corresponding to the lower record type.
/// @param ptr Candidate retained record address.
/// @return true when the target index or its allocation-failure fallback contains the key.
static bool rt_gui_subhandle_target_has_wrapper(rt_gui_subhandle_kind_t kind, const void *ptr) {
    return ptr && rt_gui_find_subhandle(kind, ptr) != NULL;
}

/// @brief Test whether any node in one retired TreeView subtree still has a wrapper.
/// @details Walks the retained child links without allocation. The lower toolkit preserves this
///          topology until the group root is reclaimed.
/// @param root Retired subtree root.
/// @return true when a wrapper targets the root or any descendant.
static bool rt_gui_retired_tree_has_wrapper(vg_tree_node_t *root) {
    if (!root)
        return false;
    vg_tree_node_t *node = root;
    while (node) {
        if (rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_TREE_NODE, node))
            return true;
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != root && !node->next_sibling)
            node = node->parent;
        if (!node || node == root)
            break;
        node = node->next_sibling;
    }
    return false;
}

/// @brief Reclaim every retirement record for one owner that has no remaining wrapper.
/// @details Reclamation is per item/tab/menu or per TreeView subtree group, so one intentionally
///          retained stale wrapper cannot cause unrelated future churn to accumulate without
///          bound. The owner must be live for the complete call. No allocations are performed.
/// @param owner Exact live owner widget.
static void rt_gui_collect_retired_for_owner(vg_widget_t *owner) {
    if (!owner || !vg_widget_is_live(owner))
        return;
    switch (owner->type) {
        case VG_WIDGET_LISTBOX: {
            vg_listbox_t *listbox = (vg_listbox_t *)owner;
            vg_listbox_item_t *item = listbox->retired_items;
            while (item) {
                vg_listbox_item_t *next = item->retired_next;
                if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_LISTBOX_ITEM, item))
                    (void)vg_listbox_reclaim_retired_item(listbox, item);
                item = next;
            }
            break;
        }
        case VG_WIDGET_TREEVIEW: {
            vg_treeview_t *tree = (vg_treeview_t *)owner;
            vg_tree_node_t *root = tree->retired_nodes;
            while (root) {
                vg_tree_node_t *next = root->retired_next;
                if (!rt_gui_retired_tree_has_wrapper(root))
                    (void)vg_treeview_reclaim_retired_subtree(tree, root);
                root = next;
            }
            break;
        }
        case VG_WIDGET_TABBAR: {
            vg_tabbar_t *tabbar = (vg_tabbar_t *)owner;
            vg_tab_t *tab = tabbar->retired_tabs;
            while (tab) {
                vg_tab_t *next = tab->retired_next;
                if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_TAB, tab))
                    (void)vg_tabbar_reclaim_retired_tab(tabbar, tab);
                tab = next;
            }
            break;
        }
        case VG_WIDGET_MENUBAR: {
            vg_menubar_t *menubar = (vg_menubar_t *)owner;
            for (vg_menu_t *menu = menubar->first_menu; menu; menu = menu->next) {
                vg_menu_item_t *item = menu->retired_items;
                while (item) {
                    vg_menu_item_t *next = item->retired_next;
                    if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_MENU_ITEM, item))
                        (void)vg_menu_reclaim_retired_item(menu, item);
                    item = next;
                }
            }
            vg_menu_t *menu = menubar->retired_menus;
            while (menu) {
                vg_menu_t *next = menu->retired_next;
                vg_menu_item_t *item = menu->retired_items;
                while (item) {
                    vg_menu_item_t *item_next = item->retired_next;
                    if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_MENU_ITEM, item))
                        (void)vg_menu_reclaim_retired_item(menu, item);
                    item = item_next;
                }
                bool menu_wrapped = rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_MENU, menu);
                if (!menu_wrapped && menu->retired_items == NULL)
                    (void)vg_menubar_reclaim_retired_menu(menubar, menu);
                menu = next;
            }
            break;
        }
        case VG_WIDGET_MENU: {
            vg_contextmenu_t *menu = (vg_contextmenu_t *)owner;
            vg_menu_item_t *item = menu->retired_items;
            while (item) {
                vg_menu_item_t *next = item->retired_next;
                if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_MENU_ITEM, item))
                    (void)vg_contextmenu_reclaim_retired_item(menu, item);
                item = next;
            }
            break;
        }
        case VG_WIDGET_STATUSBAR: {
            vg_statusbar_t *bar = (vg_statusbar_t *)owner;
            vg_statusbar_item_t *item = bar->retired_items;
            while (item) {
                vg_statusbar_item_t *next = item->retired_next;
                if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_STATUSBAR_ITEM, item))
                    (void)vg_statusbar_reclaim_retired_item(bar, item);
                item = next;
            }
            break;
        }
        case VG_WIDGET_TOOLBAR: {
            vg_toolbar_t *bar = (vg_toolbar_t *)owner;
            vg_toolbar_item_t *item = bar->retired_items;
            while (item) {
                vg_toolbar_item_t *next = item->retired_next;
                if (!rt_gui_subhandle_target_has_wrapper(RT_GUI_HANDLE_TOOLBAR_ITEM, item))
                    (void)vg_toolbar_reclaim_retired_item(bar, item);
                item = next;
            }
            break;
        }
        default:
            break;
    }
}

/// @brief Automatically reclaim unwrapped retirement records beneath a live widget subtree.
/// @details Traverses owner widgets without allocation and applies per-owner collection. This is
///          called after explicit removals and event-dispatch boundaries; it never invalidates a
///          still-referenced wrapper and therefore preserves stale-handle safety.
/// @param subtree Live widget root to collect; NULL is a no-op.
void rt_gui_collect_retired_subhandles(vg_widget_t *subtree) {
    if (!subtree || !vg_widget_is_live(subtree))
        return;
    vg_widget_t *node = subtree;
    while (node) {
        rt_gui_collect_retired_for_owner(node);
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != subtree && !node->next_sibling)
            node = node->parent;
        if (!node || node == subtree)
            break;
        node = node->next_sibling;
    }
}

/// @brief Return the number of currently allocated stable subhandle wrappers.
/// @return Process-global wrapper count; intended for regression/performance diagnostics.
size_t rt_gui_subhandle_debug_live_count(void) {
    return s_gui_subhandle_count;
}

/// @brief Return the target index's current slot capacity.
/// @return Power-of-two slot count, or zero when no wrappers are allocated.
size_t rt_gui_subhandle_debug_index_capacity(void) {
    return s_target_capacity;
}

/// @brief Return the largest bounded probe count observed since the last reset.
/// @return Maximum target-index slots inspected by one lookup.
size_t rt_gui_subhandle_debug_max_probes(void) {
    return s_target_max_probes;
}

/// @brief Reset target-index probe diagnostics without changing wrapper/index state.
void rt_gui_subhandle_debug_reset_probes(void) {
    s_target_last_probes = 0;
    s_target_max_probes = 0;
}

/// @brief Hard-invalidate owner-indexed wrappers matching an optional kind/retirement filter.
/// @details A complete owner index touches only the target owner's bucket chain. The global list is
///          retained solely as the allocation-failure fallback. Iteration saves the next link
///          before invalidation because invalidation unlinks the current wrapper in constant time.
/// @param owner Exact live owner widget whose wrappers are selected.
/// @param kind Required kind, or zero to select every kind.
/// @param retired_only When true, preserve wrappers whose lower payload is still live.
static void rt_gui_subhandle_invalidate_owner_matches(vg_widget_t *owner,
                                                      uint32_t kind,
                                                      bool retired_only) {
    if (!owner)
        return;
    if (s_owner_index_complete && s_owner_buckets && s_owner_capacity > 0) {
        size_t bucket = rt_gui_subhandle_owner_hash(owner) & (s_owner_capacity - 1);
        rt_gui_subhandle_t *handle = s_owner_buckets[bucket];
        while (handle) {
            rt_gui_subhandle_t *next = handle->owner_next;
            if (handle->owner_widget == owner && handle->owner_widget_id == owner->id &&
                (kind == 0 || handle->kind == kind) &&
                (!retired_only || handle->retired || !rt_gui_subhandle_payload_is_live(handle))) {
                rt_gui_subhandle_invalidate(handle);
            }
            handle = next;
        }
        return;
    }
    rt_gui_subhandle_t *handle = s_gui_subhandles;
    while (handle) {
        rt_gui_subhandle_t *next = handle->next;
        if (handle->owner_widget == owner && handle->owner_widget_id == owner->id &&
            (kind == 0 || handle->kind == kind) &&
            (!retired_only || handle->retired || !rt_gui_subhandle_payload_is_live(handle))) {
            rt_gui_subhandle_invalidate(handle);
        }
        handle = next;
    }
}

/// @brief Hard-invalidate wrappers owned by every widget in a subtree using owner indexes.
/// @details The traversal runs before widget destruction and never inspects child payloads. Each
///          exact owner lookup is expected O(1), replacing the former global handle-list scan.
/// @param subtree Live root whose owner generations and descendants are being retired; NULL is a
///                no-op.
void rt_gui_invalidate_widget_subhandles(vg_widget_t *subtree) {
    if (!subtree)
        return;
    vg_widget_t *node = subtree;
    while (node) {
        rt_gui_subhandle_invalidate_owner_matches(node, 0, false);
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != subtree && !node->next_sibling)
            node = node->parent;
        if (!node || node == subtree)
            break;
        node = node->next_sibling;
    }
}

/// @brief Invalidate managed wrappers that target retired nodes owned by @p tree.
/// @details Retired node storage is deliberately still allocated at this point, so checking its
///          live marker is safe. Clearing the runtime wrapper before the lower toolkit drains the
///          retired list ensures subsequent script calls stop at the NULL target and never inspect
///          reclaimed memory. Wrappers for nodes that remain live are left untouched.
/// @param tree Tree about to prune its retired-node list; NULL is a no-op.
void rt_gui_invalidate_retired_tree_node_subhandles(vg_treeview_t *tree) {
    if (!tree)
        return;
    rt_gui_subhandle_invalidate_owner_matches(&tree->base, RT_GUI_HANDLE_TREE_NODE, true);
}

/// @brief Invalidate managed wrappers that target retired tabs owned by @p tabbar.
/// @details Retired tab storage is still allocated while this function runs. The live-marker check
///          therefore distinguishes retired tabs without reading freed memory. Invalidating those
///          wrappers before the lower toolkit drains its retired list keeps all later script calls
///          inert while preserving wrappers for tabs that remain in the bar.
/// @param tabbar Tab bar about to prune its retired-tab list; NULL is a no-op.
void rt_gui_invalidate_retired_tab_subhandles(vg_tabbar_t *tabbar) {
    if (!tabbar)
        return;
    rt_gui_subhandle_invalidate_owner_matches(&tabbar->base, RT_GUI_HANDLE_TAB, true);
}

/// @brief Invalidate item and nested-submenu wrappers owned by a ContextMenu.
/// @details Retires direct item wrappers through the owner index, then recursively invalidates each
///          submenu tree while deliberately preserving the root wrapper for @p menu.
/// @param menu Live ContextMenu whose contents are about to be cleared; NULL is a no-op.
void rt_gui_invalidate_contextmenu_contents(vg_contextmenu_t *menu) {
    if (!menu)
        return;

    rt_gui_subhandle_invalidate_owner_matches(&menu->base, RT_GUI_HANDLE_MENU_ITEM, false);

    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item && item->submenu)
            rt_gui_invalidate_contextmenu_tree((vg_contextmenu_t *)item->submenu);
    }
}

/// @brief Invalidate a complete ContextMenu wrapper tree including its root.
/// @details Invalidates all item/submenu descendants first, then hard-invalidates the stable
///          ContextMenu wrapper owned by the root widget.
/// @param menu Live ContextMenu being destroyed; NULL is a no-op.
void rt_gui_invalidate_contextmenu_tree(vg_contextmenu_t *menu) {
    if (!menu)
        return;
    rt_gui_invalidate_contextmenu_contents(menu);
    rt_gui_subhandle_invalidate_owner_matches(&menu->base, RT_GUI_HANDLE_CONTEXTMENU, false);
}
