//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_scene.c
// Purpose: Hierarchical scene graph for Zanna games. Manages a tree of named
//   nodes (entities), each with a 2D transform (position, scale, rotation)
//   that accumulates parent transforms. Provides name-based lookup, child
//   iteration, and batched Draw(canvas, camera) that traverses the tree in
//   order and renders each visible node's sprite or custom draw callback.
//
// Key invariants:
//   - Node names are retained identifiers but need not be unique. Lookup
//     returns the first exact match in depth-first pre-order.
//   - World position applies parent scale and rotation to the local offset,
//     then adds parent translation. Rotations add in degrees and scales
//     multiply as integer percentages.
//   - The implicit scene root has no parent. Each child Seq retains its direct
//     children; detachment releases that ownership without changing unrelated
//     caller-held references.
//   - Hierarchy, lookup, update, draw, and dirty propagation are iterative.
//     Parent-chain validation is capped at SCENE_NODE_MAX_PARENT_CHAIN.
//   - Scene draws are depth-sorted globally. Nodes with equal depth preserve
//     traversal order so sibling/insertion order remains stable for ties.
//
// Ownership/Lifetime:
//   - Scene and SceneNode objects are runtime reference-counted allocations.
//     The scene owns its root and every parent owns its child Seq entries.
//   - Finalizing the scene releases the complete ownership tree; nodes still
//     retained elsewhere survive with cleared parent links.
//   - Name, sprite, root, child-Seq, and Option results follow explicit retain
//     ownership; direct property and lookup getters return borrowed values.
//
// Links: src/runtime/graphics/2d/rt_scene.h (public API),
//        src/runtime/graphics/2d/rt_sprite.h (node sprite payload),
//        docs/zannalib/game.md (Scene section)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the retained hierarchical 2D sprite scene graph.
///
/// Local transforms invalidate cached world transforms lazily. Public draws
/// either traverse one subtree in pre-order or collect the complete scene into
/// reusable scratch for stable global depth sorting. All structural walks avoid
/// recursion so deep valid hierarchies consume checked explicit storage rather
/// than the C call stack.

#include "rt_scene.h"
#include "rt_camera.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_sprite.h"
#include "rt_string.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration from rt_io.c
#include "rt_trap.h"

/// Maximum accepted ancestor depth before a hierarchy is treated as corrupt.
#define SCENE_NODE_MAX_PARENT_CHAIN 8192
/// Private initialized-payload cookies for scene objects.
#define RT_SCENE_NODE_STATE_MAGIC UINT64_C(0x5A53434E4E4F4432)
#define RT_SCENE_STATE_MAGIC UINT64_C(0x5A5343454E453032)

//=============================================================================
// Internal Structures
//=============================================================================

/// @brief Private retained payload for one scene-graph node.
/// @details Local transform fields are authoritative. Cached world fields are
///          refreshed on demand after `transform_dirty` propagation.
typedef struct scene_node_impl {
    uint64_t state_magic;
    // Local transform (relative to parent)
    int64_t x;
    int64_t y;
    int64_t scale_x;  // 100 = 100%
    int64_t scale_y;  // 100 = 100%
    int64_t rotation; // degrees
    int64_t depth;    // Z-order
    int8_t visible;   // visibility flag

    // Cached world transform
    int64_t world_x;
    int64_t world_y;
    int64_t world_scale_x;
    int64_t world_scale_y;
    int64_t world_rotation;
    int8_t transform_dirty;          // this node's local transform changed
    uint64_t world_revision;         // increments whenever this cache changes
    uint64_t cached_parent_revision; // parent revision used by this cache
    uint64_t draw_revision;          // root-owned draw-order invalidation epoch
    uint64_t name_revision;          // root-owned name-index invalidation epoch

    // Hierarchy
    struct scene_node_impl *parent;
    void *children; // Seq of child nodes

    // Content
    void *sprite;   // Attached sprite (nullable)
    rt_string name; // Tag/identifier
} scene_node_impl;

/// @brief Private scene container owning the implicit root and draw scratch.
typedef struct scene_impl {
    uint64_t state_magic;
    scene_node_impl *root;
    /* Reusable draw-order scratch (node_sort_entry array). Persisted across frames
     * so a static scene performs no per-frame allocation for the collect+sort pass;
     * grows on demand and is freed in scene_finalize. Typed void* here because
     * node_sort_entry is defined later in this file. */
    void *draw_scratch;
    int64_t draw_scratch_cap;
    int64_t draw_scratch_count;
    uint64_t draw_cache_revision;
    scene_node_impl **name_index;
    int64_t name_index_cap;
    int64_t name_index_count;
    uint64_t name_cache_revision;
} scene_impl;

/// @brief Validate the private Seq used as a node's owning child list.
static int8_t scene_children_are_valid(void *children) {
    if (!rt_seq_internal_is_valid(children))
        return 0;
    const rt_seq_impl *seq = (const rt_seq_impl *)children;
    return seq->len >= 0 && seq->cap > 0 && seq->len <= seq->cap && seq->items &&
           seq->owns_elements == 1 &&
           (uint64_t)seq->cap <= (uint64_t)SIZE_MAX / sizeof(*seq->items);
}

/// @brief Validate a node payload without recursively walking its subtree.
static int8_t scene_node_state_is_valid(const scene_node_impl *node) {
    if (!node || node->state_magic != RT_SCENE_NODE_STATE_MAGIC || node->scale_x < 1 ||
        node->scale_y < 1 || node->world_scale_x < 1 || node->world_scale_y < 1 ||
        (node->visible != 0 && node->visible != 1) ||
        (node->transform_dirty != 0 && node->transform_dirty != 1) ||
        !scene_children_are_valid(node->children))
        return 0;
    if (node->parent &&
        (!rt_obj_is_instance(node->parent, RT_SCENE_NODE_CLASS_ID, sizeof(scene_node_impl)) ||
         node->parent->state_magic != RT_SCENE_NODE_STATE_MAGIC))
        return 0;
    if (node->name && !rt_string_is_handle(node->name))
        return 0;
    return 1;
}

/// @brief Validate-and-return a SceneNode pointer; NULL for NULL or wrong class.
/// @details Soft check used by every public SceneNode entry point.
/// @param node_ptr Opaque candidate SceneNode handle.
/// @return Validated private payload, or null without trapping.
static scene_node_impl *scene_node_checked_or_null(void *node_ptr) {
    if (!node_ptr || !rt_obj_is_instance(node_ptr, RT_SCENE_NODE_CLASS_ID, sizeof(scene_node_impl)))
        return NULL;
    scene_node_impl *node = (scene_node_impl *)node_ptr;
    return scene_node_state_is_valid(node) ? node : NULL;
}

/// @brief Validate-and-return a Scene pointer; NULL for NULL or wrong class.
/// @details Soft check used by every public Scene entry point.
/// @param scene_ptr Opaque candidate Scene handle.
/// @return Validated private payload, or null without trapping.
static scene_impl *scene_checked_or_null(void *scene_ptr) {
    if (!scene_ptr || !rt_obj_is_instance(scene_ptr, RT_SCENE_CLASS_ID, sizeof(scene_impl)))
        return NULL;
    scene_impl *scene = (scene_impl *)scene_ptr;
    if (scene->state_magic != RT_SCENE_STATE_MAGIC || !scene_node_state_is_valid(scene->root) ||
        scene->root->parent || scene->draw_scratch_cap < 0 || scene->draw_scratch_count < 0 ||
        scene->draw_scratch_count > scene->draw_scratch_cap || scene->name_index_cap < 0 ||
        scene->name_index_count < 0 || scene->name_index_count > scene->name_index_cap ||
        (scene->draw_scratch_cap == 0) != (scene->draw_scratch == NULL) ||
        (scene->name_index_cap == 0) != (scene->name_index == NULL) ||
        (uint64_t)scene->draw_scratch_cap >
            (uint64_t)SIZE_MAX / (sizeof(void *) + 2u * sizeof(int64_t)) ||
        (uint64_t)scene->name_index_cap > (uint64_t)SIZE_MAX / sizeof(scene_node_impl *))
        return NULL;
    if (scene->name_index_cap > 0 && (scene->name_index_cap & (scene->name_index_cap - 1)) != 0)
        return NULL;
    return scene;
}

/// @brief Add two int64 values, saturating at INT64_MIN/MAX instead of wrapping.
/// @details Used to compose accumulated transforms (parent + child position)
///          without UB on overflow. Negative @p b correctly saturates at MIN.
/// @param a First addend.
/// @param b Second addend.
/// @return `a + b` clamped to the signed 64-bit range.
static int64_t scene_add_saturating(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Round a long double to int64 with halves away from zero.
/// @details Used as the final step of every long-double world-transform
///          calculation so the result lands cleanly in int64 storage. Out-
///          of-range inputs clamp to INT64_MIN/MAX rather than producing UB.
/// @param value Extended-precision value to round.
/// @return Rounded result clamped to the signed 64-bit range.
static int64_t scene_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (isinf(value))
        return signbit(value) ? INT64_MIN : INT64_MAX;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0L ? value + 0.5L : value - 0.5L);
}

/// @brief Return the unsigned magnitude of a signed 64-bit value without overflow.
static uint64_t scene_i64_magnitude(int64_t value) {
    return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

/// @brief Compute `(value * multiplier) / 100`, rounded halves away from zero.
/// @details Decomposes both magnitudes into quotient/remainder terms before
///          multiplication. This stays exact on every supported C compiler,
///          including MSVC where `long double` has only binary64 precision.
/// @param value Multiplicand.
/// @param multiplier Percentage multiplier.
/// @return Rounded product divided by 100, saturated to the signed range.
static int64_t scene_mul_percent_saturating(int64_t value, int64_t multiplier) {
    if (value == 0 || multiplier == 0)
        return 0;
    int negative = (value < 0) != (multiplier < 0);
    uint64_t a = scene_i64_magnitude(value);
    uint64_t b = scene_i64_magnitude(multiplier);
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t b_quotient = b / 100u;
    uint64_t b_remainder = b % 100u;
    if (b_quotient != 0 && a > limit / b_quotient)
        return negative ? INT64_MIN : INT64_MAX;
    uint64_t result = a * b_quotient;
    uint64_t a_quotient = a / 100u;
    uint64_t a_remainder = a % 100u;
    if (b_remainder != 0 && a_quotient > (limit - result) / b_remainder)
        return negative ? INT64_MIN : INT64_MAX;
    result += a_quotient * b_remainder;
    uint64_t residual_product = a_remainder * b_remainder;
    uint64_t residual_quotient = residual_product / 100u;
    if (residual_quotient > limit - result)
        return negative ? INT64_MIN : INT64_MAX;
    result += residual_quotient;
    if (residual_product % 100u >= 50u) {
        if (result == limit)
            return negative ? INT64_MIN : INT64_MAX;
        result++;
    }
    if (!negative)
        return (int64_t)result;
    if (result >= (uint64_t)INT64_MAX + 1u)
        return INT64_MIN;
    return -(int64_t)result;
}

/// @brief Keep local node scale positive so draw-time scale normalization is explicit.
/// @param scale Requested integer percentage.
/// @return @p scale when positive; otherwise one.
static int64_t scene_normalize_scale(int64_t scale) {
    return scale < 1 ? 1 : scale;
}

/// @brief Obtain an exact byte view for a validated runtime string.
static int8_t scene_string_view(rt_string value, const char **bytes_out, size_t *length_out) {
    if (!value || !bytes_out || !length_out || !rt_string_is_handle(value))
        return 0;
    int64_t raw_length = rt_str_len(value);
    if (raw_length < 0 || (uint64_t)raw_length > (uint64_t)SIZE_MAX)
        return 0;
    const char *bytes = rt_string_cstr(value);
    if (!bytes)
        return 0;
    *bytes_out = bytes;
    *length_out = (size_t)raw_length;
    return 1;
}

/// @brief Subtract @p b from @p a with signed-overflow checks.
/// @details Used by camera-relative rotation where the exact difference can
///          exceed the signed range.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return Rounded `a - b` clamped to the signed 64-bit range.
static int64_t scene_sub_saturating(int64_t a, int64_t b) {
    if (b > 0 && a < INT64_MIN + b)
        return INT64_MIN;
    if (b < 0 && a > INT64_MAX + b)
        return INT64_MAX;
    return a - b;
}

/// @brief Add periodic degree angles without allowing irrelevant full turns to overflow.
static int64_t scene_add_rotations(int64_t a, int64_t b) {
    return ((a % 360) + (b % 360)) % 360;
}

// Forward declarations
static void mark_transform_dirty(scene_node_impl *node);
static void update_world_transform(scene_node_impl *node);

/// @brief Return whether a bounded parent-chain walk reaches @p target.
/// @details Normal scene trees are acyclic, so this walk is short. The depth
///          cap prevents corrupted parent pointers or cycles from causing an
///          unbounded traversal in hierarchy mutation and transform updates.
/// @param start Node at which to start walking ancestors.
/// @param target Node to search for.
/// @return 1 if @p target is found or the chain is rejected as too deep; otherwise 0.
static int scene_parent_chain_contains(scene_node_impl *start, scene_node_impl *target) {
    int64_t depth = 0;
    for (scene_node_impl *cur = start; cur; cur = cur->parent) {
        if (!scene_node_state_is_valid(cur)) {
            rt_trap("SceneNode: invalid parent chain");
            return 1;
        }
        if (cur == target)
            return 1;
        depth++;
        if (depth >= SCENE_NODE_MAX_PARENT_CHAIN && cur->parent) {
            rt_trap("SceneNode: parent chain too deep or cyclic");
            return 1;
        }
    }
    return 0;
}

/// @brief Return the topmost ancestor for cache-epoch invalidation.
static scene_node_impl *scene_tree_root(scene_node_impl *node) {
    int64_t depth = 0;
    while (node && node->parent && depth++ < SCENE_NODE_MAX_PARENT_CHAIN)
        node = node->parent;
    return node;
}

/// @brief Advance a nonzero cache revision, tolerating theoretical wraparound.
static void scene_bump_revision(uint64_t *revision) {
    if (!revision)
        return;
    (*revision)++;
    if (*revision == 0)
        *revision = 1;
}

static void mark_draw_cache_dirty(scene_node_impl *node) {
    scene_node_impl *root = scene_tree_root(node);
    if (root)
        scene_bump_revision(&root->draw_revision);
}

static void mark_name_cache_dirty(scene_node_impl *node) {
    scene_node_impl *root = scene_tree_root(node);
    if (root)
        scene_bump_revision(&root->name_revision);
}

static int compare_depth(const void *a, const void *b);
static void release_owned_ref(void **slot);
static void scene_node_finalize(void *obj);
static void scene_finalize(void *obj);

/// @brief Explicit traversal stack with a 64-node inline fast path.
typedef struct scene_node_stack {
    scene_node_impl **items;
    scene_node_impl *inline_items[64];
    int64_t count;
    int64_t capacity;
} scene_node_stack;

/// @brief Initialize an explicit traversal stack backed by its inline buffer
///        (no heap until it grows) — used for non-recursive scene-graph walks.
/// @param stack Uninitialized stack storage to prepare.
static void scene_node_stack_init(scene_node_stack *stack) {
    stack->items = stack->inline_items;
    stack->count = 0;
    stack->capacity = (int64_t)(sizeof(stack->inline_items) / sizeof(stack->inline_items[0]));
}

/// @brief Free any heap-grown storage and reset the stack back to its inline
///        buffer (safe to call whether or not it ever grew).
/// @param stack Initialized stack to reset.
static void scene_node_stack_destroy(scene_node_stack *stack) {
    if (stack->items != stack->inline_items)
        free(stack->items);
    stack->items = stack->inline_items;
    stack->count = 0;
    stack->capacity = (int64_t)(sizeof(stack->inline_items) / sizeof(stack->inline_items[0]));
}

/// @brief Push @p node, growing (inline -> heap, then doubling) as needed.
/// @details NULL @p node is a no-op success. @return 1 on success, 0 only on
///          allocation failure or capacity overflow.
/// @param stack Initialized destination stack.
/// @param node Node to push, or null.
static int8_t scene_node_stack_push(scene_node_stack *stack, scene_node_impl *node) {
    if (!node)
        return 1;
    if (stack->count >= stack->capacity) {
        if (stack->capacity > INT64_MAX / 2 ||
            (uint64_t)(stack->capacity * 2) > (uint64_t)SIZE_MAX / sizeof(*stack->items))
            return 0;
        int64_t new_capacity = stack->capacity * 2;
        scene_node_impl **grown = NULL;
        if (stack->items == stack->inline_items) {
            grown = (scene_node_impl **)malloc((size_t)new_capacity * sizeof(*grown));
            if (grown)
                memcpy(grown, stack->items, (size_t)stack->count * sizeof(*grown));
        } else {
            grown =
                (scene_node_impl **)realloc(stack->items, (size_t)new_capacity * sizeof(*grown));
        }
        if (!grown)
            return 0;
        stack->items = grown;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = node;
    return 1;
}

/// @brief Pop the top node, or NULL if the stack is empty/NULL.
/// @param stack Initialized source stack, or null.
/// @return The most recently pushed node, or null when empty or invalid.
static scene_node_impl *scene_node_stack_pop(scene_node_stack *stack) {
    if (!stack || stack->count <= 0)
        return NULL;
    return stack->items[--stack->count];
}

/// @brief Push @p node's children in reverse order so a subsequent pop loop
///        visits them left-to-right (depth-first pre-order traversal).
/// @param stack Initialized destination stack.
/// @param node Node whose direct children should be pushed.
/// @return 1 on success, 0 if a push failed (allocation/overflow).
static int8_t scene_node_stack_push_children_reverse(scene_node_stack *stack,
                                                     scene_node_impl *node) {
    if (!scene_node_state_is_valid(node))
        return 0;
    int64_t count = rt_seq_len(node->children);
    for (int64_t i = count; i > 0; i--) {
        scene_node_impl *child = scene_node_checked_or_null(rt_seq_get(node->children, i - 1));
        if (!child || child->parent != node || !scene_node_stack_push(stack, child))
            return 0;
    }
    return 1;
}

/// @brief Release a GC reference stored in @p slot and NULL it.
/// @details If the reference count drops to zero after release, frees the object
///   immediately.  Nulling the slot prevents double-free if called again.
/// @param slot Address of an owned object slot; null and empty slots are
///        ignored.
static void release_owned_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_heap_is_payload(*slot) && rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief GC finalizer for a scene node — release children, sprite, and name.
/// @details Before releasing the children Seq, clears every child's parent pointer
///   so that children being freed during Seq teardown don't attempt a dangling
///   remove_child back on this node.  Called automatically when the node's
///   reference count reaches zero.
/// @param obj Finalizing SceneNode runtime object.
static void scene_node_finalize(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_SCENE_NODE_CLASS_ID, sizeof(scene_node_impl)))
        return;
    scene_node_impl *node = (scene_node_impl *)obj;
    if (node->state_magic != RT_SCENE_NODE_STATE_MAGIC)
        return;

    if (scene_children_are_valid(node->children)) {
        int64_t count = rt_seq_len(node->children);
        for (int64_t i = 0; i < count; i++) {
            scene_node_impl *child = scene_node_checked_or_null(rt_seq_get(node->children, i));
            if (child)
                child->parent = NULL;
        }
        release_owned_ref(&node->children);
    } else {
        node->children = NULL;
    }

    node->state_magic = 0;
    release_owned_ref(&node->sprite);
    release_owned_ref((void **)&node->name);
}

/// @brief GC finalizer for the scene container — release the root node.
/// @details Clears root->parent before releasing so the root's finalizer doesn't
///   try to call remove_child on a now-dead parent.  Releasing root triggers
///   a cascade release of the whole node tree. Reusable draw scratch is
///   malloc-owned and freed directly.
/// @param obj Finalizing Scene runtime object.
static void scene_finalize(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_SCENE_CLASS_ID, sizeof(scene_impl)))
        return;
    scene_impl *scene = (scene_impl *)obj;
    if (scene->state_magic != RT_SCENE_STATE_MAGIC)
        return;
    scene->state_magic = 0;
    if (scene_node_checked_or_null(scene->root))
        scene->root->parent = NULL;
    release_owned_ref((void **)&scene->root);
    free(scene->draw_scratch);
    scene->draw_scratch = NULL;
    scene->draw_scratch_cap = 0;
    scene->draw_scratch_count = 0;
    free(scene->name_index);
    scene->name_index = NULL;
    scene->name_index_cap = 0;
    scene->name_index_count = 0;
}

//=============================================================================
// Scene Node Creation
//=============================================================================

/// @brief Create an empty 2D scene node positioned at the origin with identity transform.
/// @details Scale is stored as a percentage (100 = 1.0x). The node owns an
///          element-owning child Seq, begins visible and transform-dirty, has
///          no sprite or parent, and retains an empty name. Allocation failure
///          traps and releases any partially created node.
/// @return A caller-owned SceneNode handle, or null after allocation failure.
void *rt_scene_node_new(void) {
    scene_node_impl *node =
        (scene_node_impl *)rt_obj_new_i64(RT_SCENE_NODE_CLASS_ID, (int64_t)sizeof(scene_node_impl));
    if (!node) {
        rt_trap("SceneNode: allocation failed");
        return NULL;
    }
    memset(node, 0, sizeof(scene_node_impl));

    node->x = 0;
    node->y = 0;
    node->scale_x = 100;
    node->scale_y = 100;
    node->rotation = 0;
    node->depth = 0;
    node->visible = 1;

    node->world_x = 0;
    node->world_y = 0;
    node->world_scale_x = 100;
    node->world_scale_y = 100;
    node->world_rotation = 0;
    node->transform_dirty = 1;
    node->world_revision = 0;
    node->cached_parent_revision = 0;
    node->draw_revision = 1;
    node->name_revision = 1;

    node->parent = NULL;
    node->children = rt_seq_new();
    if (!node->children) {
        if (rt_obj_release_check0(node))
            rt_obj_free(node);
        rt_trap("SceneNode: child list allocation failed");
        return NULL;
    }
    rt_seq_set_owns_elements(node->children, 1);
    node->state_magic = RT_SCENE_NODE_STATE_MAGIC;
    node->sprite = NULL;
    node->name = NULL;

    rt_obj_set_finalizer(node, scene_node_finalize);
    rt_scene_node_set_name(node, rt_const_cstr(""));

    return node;
}

/// @brief Convenience constructor: create a scene node and attach @p sprite to it.
/// @details A nonnull sprite is validated and retained by
///          `rt_scene_node_set_sprite()`. Failure to attach does not discard a
///          successfully created empty node.
/// @param sprite Optional Sprite handle to attach.
/// @return A caller-owned SceneNode handle, or null when node allocation fails.
void *rt_scene_node_from_sprite(void *sprite) {
    scene_node_impl *node = (scene_node_impl *)rt_scene_node_new();
    if (node && sprite)
        rt_scene_node_set_sprite(node, sprite);
    return node;
}

//=============================================================================
// Transform Management
//=============================================================================

/// @brief Mark one node's local transform cache stale in constant time.
/// @details Descendants compare their cached parent revision on demand, so a
///          parent edit never walks the subtree. A requested descendant cache
///          is refreshed by the top-down ancestor pass below.
/// @param node Node whose local transform changed, or null.
static void mark_transform_dirty(scene_node_impl *node) {
    if (node)
        node->transform_dirty = 1;
}

/// @brief Compute and store the world transform for @p node, assuming its parent's world
///        transform is already up-to-date.
/// @details For non-root nodes the world position is derived by scaling the local offset by
///          the parent's world scale, then rotating it by the parent's world rotation angle.
///          World scale and rotation accumulate multiplicatively from the root. For root nodes
///          the world transform equals the local transform directly. Clears `transform_dirty`.
/// @param node Dirty node whose parent cache is already current.
static void apply_node_transform(scene_node_impl *node) {
    if (node->parent) {
        node->world_scale_x =
            scene_mul_percent_saturating(node->parent->world_scale_x, node->scale_x);
        node->world_scale_y =
            scene_mul_percent_saturating(node->parent->world_scale_y, node->scale_y);
        node->world_rotation = scene_add_rotations(node->parent->world_rotation, node->rotation);

        int64_t scaled_x = scene_mul_percent_saturating(node->x, node->parent->world_scale_x);
        int64_t scaled_y = scene_mul_percent_saturating(node->y, node->parent->world_scale_y);

        if (node->parent->world_rotation == 0) {
            node->world_x = scene_add_saturating(node->parent->world_x, scaled_x);
            node->world_y = scene_add_saturating(node->parent->world_y, scaled_y);
        } else {
            double rad =
                (double)(node->parent->world_rotation % 360) * 3.14159265358979323846 / 180.0;
            double cos_r = cos(rad);
            double sin_r = sin(rad);

            int64_t rx = scene_ld_to_i64_sat((long double)scaled_x * (long double)cos_r -
                                             (long double)scaled_y * (long double)sin_r);
            int64_t ry = scene_ld_to_i64_sat((long double)scaled_x * (long double)sin_r +
                                             (long double)scaled_y * (long double)cos_r);
            node->world_x = scene_add_saturating(node->parent->world_x, rx);
            node->world_y = scene_add_saturating(node->parent->world_y, ry);
        }
    } else {
        node->world_x = node->x;
        node->world_y = node->y;
        node->world_scale_x = node->scale_x;
        node->world_scale_y = node->scale_y;
        node->world_rotation = node->rotation % 360;
    }

    node->cached_parent_revision = node->parent ? node->parent->world_revision : 0;
    node->transform_dirty = 0;
    node->world_revision++;
    if (node->world_revision == 0)
        node->world_revision = 1;
}

/// @brief Propagate world transforms from the highest dirty ancestor down to @p node.
/// @details Iterative replacement for the former recursive implementation. Walks up the
///          ancestor chain collecting all dirty nodes into a small inline buffer (spills to
///          heap for hierarchies deeper than 64 nodes), then applies `apply_node_transform`
///          top-down so each parent is always clean before its child is processed.
///          No-op when @p node is NULL or its transform is already clean.
/// @param node Node whose cached world transform is required.
static void update_world_transform(scene_node_impl *node) {
    if (!node)
        return;

    // Collect the complete ancestor chain. A clean-looking descendant may still
    // depend on a parent whose local transform changed since its last request.
    // Use a small fixed inline buffer; spill to heap for deep hierarchies.
    scene_node_impl *inline_buf[64];
    scene_node_impl **heap_chain = NULL;
    scene_node_impl **chain = inline_buf;
    int64_t capacity = 64;
    int64_t depth = 0;

    scene_node_impl *cur = node;
    while (cur) {
        if (!scene_node_state_is_valid(cur)) {
            free(heap_chain);
            rt_trap("SceneNode: invalid transform chain");
            return;
        }
        if (depth >= SCENE_NODE_MAX_PARENT_CHAIN) {
            free(heap_chain);
            rt_trap("SceneNode: transform chain too deep or cyclic");
            return;
        }
        if (depth >= capacity) {
            if (capacity > INT64_MAX / 2 ||
                (uint64_t)(capacity * 2) > (uint64_t)SIZE_MAX / sizeof(scene_node_impl *)) {
                free(heap_chain);
                rt_trap("SceneNode: transform chain too deep");
                return;
            }
            int64_t new_cap = capacity * 2;
            scene_node_impl **grown = malloc((size_t)new_cap * sizeof(*grown));
            if (!grown) {
                free(heap_chain);
                return;
            }
            memcpy(grown, chain, (size_t)depth * sizeof(*grown));
            free(heap_chain);
            heap_chain = grown;
            chain = grown;
            capacity = new_cap;
        }
        chain[depth++] = cur;
        cur = cur->parent;
    }

    // Process top-down, recalculating only locally dirty nodes or nodes whose
    // cached parent generation is stale.
    for (int64_t i = depth - 1; i >= 0; i--) {
        scene_node_impl *current = chain[i];
        uint64_t parent_revision = current->parent ? current->parent->world_revision : 0;
        if (current->transform_dirty || current->cached_parent_revision != parent_revision)
            apply_node_transform(current);
    }

    free(heap_chain);
}

//=============================================================================
// Scene Node Properties - Position
//=============================================================================

/// @brief Return the node's local X position relative to its parent (or world origin if root).
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored local X coordinate, or zero for invalid input.
int64_t rt_scene_node_get_x(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return node->x;
}

/// @brief Set the node's local X position and mark the subtree's world transforms dirty.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param x New local X coordinate.
void rt_scene_node_set_x(void *node_ptr, int64_t x) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->x == x)
        return;
    node->x = x;
    mark_transform_dirty(node);
}

/// @brief Return the node's local Y position relative to its parent (or world origin if root).
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored local Y coordinate, or zero for invalid input.
int64_t rt_scene_node_get_y(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return node->y;
}

/// @brief Set the node's local Y position and mark the subtree's world transforms dirty.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param y New local Y coordinate.
void rt_scene_node_set_y(void *node_ptr, int64_t y) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->y == y)
        return;
    node->y = y;
    mark_transform_dirty(node);
}

/// @brief Return the node's computed world-space X position, updating dirty transforms first.
/// @param node_ptr Opaque SceneNode handle.
/// @return Cached or recomputed world X coordinate, or zero for invalid input.
int64_t rt_scene_node_get_world_x(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    update_world_transform(node);
    return node->world_x;
}

/// @brief Return the node's computed world-space Y position, updating dirty transforms first.
/// @param node_ptr Opaque SceneNode handle.
/// @return Cached or recomputed world Y coordinate, or zero for invalid input.
int64_t rt_scene_node_get_world_y(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    update_world_transform(node);
    return node->world_y;
}

//=============================================================================
// Scene Node Properties - Scale
//=============================================================================

/// @brief Return the node's local X scale as a percentage (100 = 1.0×).
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored positive X-scale percentage, or 100 for invalid input.
int64_t rt_scene_node_get_scale_x(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 100;
    return node->scale_x;
}

/// @brief Set the node's local X scale (percentage) and mark the subtree's transforms dirty.
/// @details Values below one clamp to one.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param scale New local X-scale percentage.
void rt_scene_node_set_scale_x(void *node_ptr, int64_t scale) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    scale = scene_normalize_scale(scale);
    if (node->scale_x == scale)
        return;
    node->scale_x = scale;
    mark_transform_dirty(node);
}

/// @brief Return the node's local Y scale as a percentage (100 = 1.0×).
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored positive Y-scale percentage, or 100 for invalid input.
int64_t rt_scene_node_get_scale_y(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 100;
    return node->scale_y;
}

/// @brief Set the node's local Y scale (percentage) and mark the subtree's transforms dirty.
/// @details Values below one clamp to one.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param scale New local Y-scale percentage.
void rt_scene_node_set_scale_y(void *node_ptr, int64_t scale) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    scale = scene_normalize_scale(scale);
    if (node->scale_y == scale)
        return;
    node->scale_y = scale;
    mark_transform_dirty(node);
}

/// @brief Return the node's accumulated world-space X scale (parent scales multiplied in).
/// @param node_ptr Opaque SceneNode handle.
/// @return Lazily recomputed world X-scale percentage, or 100 for invalid
///         input.
int64_t rt_scene_node_get_world_scale_x(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 100;
    update_world_transform(node);
    return node->world_scale_x;
}

/// @brief Return the node's accumulated world-space Y scale (parent scales multiplied in).
/// @param node_ptr Opaque SceneNode handle.
/// @return Lazily recomputed world Y-scale percentage, or 100 for invalid
///         input.
int64_t rt_scene_node_get_world_scale_y(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 100;
    update_world_transform(node);
    return node->world_scale_y;
}

//=============================================================================
// Scene Node Properties - Rotation
//=============================================================================

/// @brief Return the node's local rotation in whole degrees.
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored local angle without normalization, or zero for invalid input.
int64_t rt_scene_node_get_rotation(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return node->rotation;
}

/// @brief Set the node's local rotation in whole degrees and mark the subtree's transforms dirty.
/// @details Angles are stored without modulo normalization.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param degrees New local angle.
void rt_scene_node_set_rotation(void *node_ptr, int64_t degrees) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->rotation == degrees)
        return;
    node->rotation = degrees;
    mark_transform_dirty(node);
}

/// @brief Return the node's accumulated world-space rotation (sum of all ancestor rotations).
/// @param node_ptr Opaque SceneNode handle.
/// @return Lazily recomputed saturated world angle, or zero for invalid input.
int64_t rt_scene_node_get_world_rotation(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    update_world_transform(node);
    return node->world_rotation;
}

//=============================================================================
// Scene Node Properties - Visibility & Depth
//=============================================================================

/// @brief Return whether the node (and its subtree) will be rendered.
/// @details A node whose visible flag is 0 is skipped entirely during draw traversal,
///   including all of its descendants.
/// @param node_ptr Opaque SceneNode handle.
/// @return The node's own normalized visibility flag, or zero for invalid
///         input. This does not inspect ancestor visibility.
int8_t rt_scene_node_get_visible(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return node->visible;
}

/// @brief Show or hide the node and its entire subtree.
/// @details Setting visible to 0 prevents the node from being collected during
///   draw traversal, including descendants, without changing their stored
///   visibility flags or detaching them.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param visible Zero to hide this subtree; any nonzero value to show it
///        subject to ancestor visibility.
void rt_scene_node_set_visible(void *node_ptr, int8_t visible) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    int8_t normalized = visible ? 1 : 0;
    if (node->visible == normalized)
        return;
    node->visible = normalized;
    mark_draw_cache_dirty(node);
}

/// @brief Return the node's Z-order depth used for depth-sorted rendering.
/// @details Higher values render on top of lower values.  Siblings with equal depth
///   are drawn in traversal order.
/// @param node_ptr Opaque SceneNode handle.
/// @return Stored depth key, or zero for invalid input.
int64_t rt_scene_node_get_depth(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return node->depth;
}

/// @brief Set the node's Z-order depth for depth-sorted rendering.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param depth New signed depth key.
void rt_scene_node_set_depth(void *node_ptr, int64_t depth) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->depth == depth)
        return;
    node->depth = depth;
    mark_draw_cache_dirty(node);
}

//=============================================================================
// Scene Node Properties - Name & Sprite
//=============================================================================

/// @brief Return the node's name string (borrowed — do not release the returned value).
/// @param node_ptr Opaque SceneNode handle.
/// @return Borrowed retained name, or a borrowed empty constant for invalid
///         input.
rt_string rt_scene_node_get_name(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return rt_const_cstr("");
    return node->name;
}

/// @brief Set the node's name string, retaining the new value and releasing the old.
/// @details Retains @p name before releasing the old name so the value is safe even
///   when the old and new strings happen to be the same object.  Empty string is
///   substituted when @p name is NULL.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param name Runtime string to retain, or null to assign the empty constant.
void rt_scene_node_set_name(void *node_ptr, rt_string name) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (!name)
        name = rt_const_cstr("");
    const char *name_bytes = NULL;
    size_t name_length = 0;
    if (!scene_string_view(name, &name_bytes, &name_length))
        return;
    (void)name_bytes;
    (void)name_length;
    if (node->name == name)
        return;
    rt_obj_retain_maybe(name);
    release_owned_ref((void **)&node->name);
    node->name = name;
    mark_name_cache_dirty(node);
}

/// @brief Return the sprite attached to the node (borrowed reference — do not release).
/// @details Returns NULL if no sprite has been set.  The sprite is retained by the
///   node; callers that need to hold a long-lived reference must retain it themselves.
/// @param node_ptr Opaque SceneNode handle.
/// @return Borrowed Sprite handle, or null when absent or invalid.
void *rt_scene_node_get_sprite(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return NULL;
    return node->sprite;
}

/// @brief Attach a sprite to the node, retaining it and releasing the previous sprite.
/// @details The node takes ownership: the sprite is retained on assignment and
///   released when the node is finalized or a new sprite is set. A nonnull
///   value of the wrong runtime class traps and leaves the old sprite intact.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param sprite Sprite handle to retain, or null to detach the current sprite.
void rt_scene_node_set_sprite(void *node_ptr, void *sprite) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (sprite && !rt_obj_is_instance(sprite, RT_SPRITE_CLASS_ID, 0)) {
        rt_trap("SceneNode.SetSprite: invalid sprite");
        return;
    }
    if (node->sprite == sprite)
        return;
    int presence_changed = (node->sprite == NULL) != (sprite == NULL);
    rt_obj_retain_maybe(sprite);
    release_owned_ref(&node->sprite);
    node->sprite = sprite;
    if (presence_changed)
        mark_draw_cache_dirty(node);
}

//=============================================================================
// Scene Node Hierarchy
//=============================================================================

/// @brief Attach @p child_ptr as a child of @p node_ptr in the scene hierarchy.
/// @details Guards against cycles by walking the ancestor chain before attaching.
///   The new parent takes ownership before the old parent releases it, so an
///   insertion failure preserves the original hierarchy. Marks the child's
///   world transforms dirty after a successful reparent. Re-adding a direct
///   child is an idempotent no-op. Null, wrong-class, cyclic, corrupt-chain,
///   or child-list insertion failure is a no-op.
/// @param node_ptr Opaque parent SceneNode handle.
/// @param child_ptr Opaque child SceneNode handle.
void rt_scene_node_add_child(void *node_ptr, void *child_ptr) {
    if (!node_ptr || !child_ptr)
        return;

    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    scene_node_impl *child = scene_node_checked_or_null(child_ptr);
    if (!node || !child)
        return;

    if (scene_parent_chain_contains(node, child))
        return; // Would create a cycle or uses a corrupt ancestor chain.

    if (child->parent == node)
        return;
    if (!node->children)
        return;

    /* Append first so a failed allocation leaves the old hierarchy intact.
     * The new owning Seq reference also keeps the child alive while its old
     * parent releases ownership. */
    int64_t before = rt_seq_len(node->children);
    if (before < 0 || before == INT64_MAX)
        return;
    rt_seq_push(node->children, child);
    if (rt_seq_len(node->children) != before + 1 || rt_seq_get(node->children, before) != child)
        return;

    scene_node_impl *old_parent = child->parent;
    if (old_parent)
        rt_scene_node_remove_child(old_parent, child);
    child->parent = node;
    mark_transform_dirty(child);
    mark_draw_cache_dirty(node);
    mark_name_cache_dirty(node);
}

/// @brief Detach @p child_ptr from @p node_ptr and release the node's reference to it.
/// @details Clears child->parent before removing from the Seq so the child's finalizer
///   cannot call back into this parent during teardown.  Frees the child if the release
///   drops its reference count to zero.  No-op if the child is not a direct child.
/// @param node_ptr Opaque parent SceneNode handle.
/// @param child_ptr Opaque direct-child handle; it may finalize during removal
///        when no other owner retains it.
void rt_scene_node_remove_child(void *node_ptr, void *child_ptr) {
    if (!node_ptr || !child_ptr)
        return;

    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    scene_node_impl *child = scene_node_checked_or_null(child_ptr);
    if (!node || !child)
        return;

    int64_t count = rt_seq_len(node->children);
    for (int64_t i = 0; i < count; i++) {
        if (rt_seq_get(node->children, i) == child) {
            mark_draw_cache_dirty(node);
            mark_name_cache_dirty(node);
            int was_parent = child->parent == node;
            if (was_parent)
                child->parent = NULL;
            void *removed = rt_seq_remove(node->children, i);
            if (was_parent)
                mark_transform_dirty(child);
            if (removed && rt_obj_release_check0(removed))
                rt_obj_free(removed);
            return;
        }
    }
}

/// @brief Return the number of direct children attached to @p node_ptr.
/// @param node_ptr Opaque SceneNode handle.
/// @return Direct child count, or zero for invalid input.
int64_t rt_scene_node_child_count(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return 0;
    return rt_seq_len(node->children);
}

/// @brief Get the child at @p index in the node's child list (NULL if out of range).
/// @param node_ptr Opaque parent SceneNode handle.
/// @param index Zero-based direct-child index.
/// @return Borrowed child handle, or null for invalid input or an out-of-range
///         index.
void *rt_scene_node_get_child(void *node_ptr, int64_t index) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return NULL;
    if (index < 0 || index >= rt_seq_len(node->children))
        return NULL;
    return rt_seq_get(node->children, index);
}

/// @brief Return the node's parent in the scene tree (NULL for unparented or root).
/// @param node_ptr Opaque SceneNode handle.
/// @return Borrowed parent handle, or null when unparented or invalid.
void *rt_scene_node_get_parent(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return NULL;
    return node->parent;
}

/// @brief Iterative depth-first search for a node with @p name beneath @p node_ptr.
/// @details Returns the first exact byte-string match in pre-order, including
///          the start node. Duplicate names are allowed. Stack growth failure
///          traps and terminates the search.
/// @param node_ptr Opaque starting SceneNode handle.
/// @param name Runtime string to match; null or invalid C-string storage yields
///        no match.
/// @return Borrowed first matching SceneNode, or null.
void *rt_scene_node_find(void *node_ptr, rt_string name) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node || !name)
        return NULL;

    const char *search = NULL;
    size_t search_length = 0;
    if (!scene_string_view(name, &search, &search_length))
        return NULL;

    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, node)) {
        rt_trap("SceneNode.Find: stack allocation failed");
        scene_node_stack_destroy(&stack);
        return NULL;
    }

    while (stack.count > 0) {
        scene_node_impl *cur = scene_node_stack_pop(&stack);
        if (!cur)
            continue;
        const char *node_name = NULL;
        size_t node_name_length = 0;
        if (scene_string_view(cur->name, &node_name, &node_name_length) &&
            node_name_length == search_length && memcmp(node_name, search, search_length) == 0) {
            scene_node_stack_destroy(&stack);
            return cur;
        }
        if (!scene_node_stack_push_children_reverse(&stack, cur)) {
            rt_trap("SceneNode.Find: stack allocation failed");
            break;
        }
    }

    scene_node_stack_destroy(&stack);
    return NULL;
}

/// @brief Find a descendant node by name as an Option.
/// @details Wraps the borrowed node returned by @ref rt_scene_node_find in
///          `Some(node)` when found. Missing names and invalid receivers return
///          None.
/// @param node_ptr Starting SceneNode handle.
/// @param name Node name to search for.
/// @return Opaque Zanna.Option containing the first matching node, or None.
void *rt_scene_node_find_option(void *node_ptr, rt_string name) {
    void *found = rt_scene_node_find(node_ptr, name);
    return found ? rt_option_some(found) : rt_option_none();
}

/// @brief Detach the node from its parent, if any.
/// @details Convenience wrapper that calls remove_child on the node's current parent.
///   No-op for root, unparented, or invalid nodes. The node may finalize during
///   detachment if the parent held its only reference.
/// @param node_ptr Opaque SceneNode handle to detach.
void rt_scene_node_detach(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->parent) {
        rt_scene_node_remove_child(node->parent, node);
    }
}

//=============================================================================
// Scene Node Methods
//=============================================================================

/// @brief Iteratively draw this node and all its descendants to @p canvas.
/// @details Skips invisible nodes (and their subtrees).  Each visible node with a sprite
///   is rendered using its computed world-space transform.  Children are drawn in
///   insertion order after their parent, so siblings stack naturally. This
///   subtree operation does not perform the scene container's global depth
///   sort. Explicit-stack allocation failure traps after any earlier nodes
///   have already drawn.
/// @param node_ptr Opaque root SceneNode handle; null or invalid input is
///        ignored.
/// @param canvas Target Canvas handle; null is ignored.
void rt_scene_node_draw(void *node_ptr, void *canvas) {
    if (!node_ptr || !canvas)
        return;

    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;

    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, node)) {
        rt_trap("SceneNode.Draw: stack allocation failed");
        scene_node_stack_destroy(&stack);
        return;
    }

    while (stack.count > 0) {
        scene_node_impl *cur = scene_node_stack_pop(&stack);
        if (!cur || !cur->visible)
            continue;

        update_world_transform(cur);

        if (cur->sprite)
            rt_sprite_draw_transformed(cur->sprite,
                                       canvas,
                                       cur->world_x,
                                       cur->world_y,
                                       cur->world_scale_x,
                                       cur->world_scale_y,
                                       cur->world_rotation,
                                       -1,
                                       255);

        if (!scene_node_stack_push_children_reverse(&stack, cur)) {
            rt_trap("SceneNode.Draw: stack allocation failed");
            break;
        }
    }

    scene_node_stack_destroy(&stack);
}

/// @brief Draw a subtree in pre-order with an optional camera transform.
/// @details Same traversal as rt_scene_node_draw, but each node's world position is
///   converted to screen space via the camera, camera zoom multiplies world
///   scale, and camera rotation is subtracted from node rotation. Null camera
///   means identity view. This operation is iterative and does not globally
///   depth-sort the subtree.
/// @param node_ptr Opaque root SceneNode handle; null or invalid input is
///        ignored.
/// @param canvas Target Canvas handle; null is ignored.
/// @param camera Optional Camera handle.
void rt_scene_node_draw_with_camera(void *node_ptr, void *canvas, void *camera) {
    if (!node_ptr || !canvas)
        return;

    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;

    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, node)) {
        rt_trap("SceneNode.DrawWithCamera: stack allocation failed");
        scene_node_stack_destroy(&stack);
        return;
    }

    while (stack.count > 0) {
        scene_node_impl *cur = scene_node_stack_pop(&stack);
        if (!cur || !cur->visible)
            continue;

        update_world_transform(cur);

        if (cur->sprite) {
            int64_t screen_x = cur->world_x;
            int64_t screen_y = cur->world_y;
            int64_t scale_x = cur->world_scale_x;
            int64_t scale_y = cur->world_scale_y;
            int64_t rotation = cur->world_rotation;

            if (camera) {
                rt_camera_world_to_screen(camera, cur->world_x, cur->world_y, &screen_x, &screen_y);
                int64_t zoom = rt_camera_get_zoom(camera);
                scale_x = scene_mul_percent_saturating(cur->world_scale_x, zoom);
                scale_y = scene_mul_percent_saturating(cur->world_scale_y, zoom);
                rotation = scene_sub_saturating(rotation, rt_camera_get_rotation(camera));
            }

            rt_sprite_draw_transformed(
                cur->sprite, canvas, screen_x, screen_y, scale_x, scale_y, rotation, -1, 255);
        }

        if (!scene_node_stack_push_children_reverse(&stack, cur)) {
            rt_trap("SceneNode.DrawWithCamera: stack allocation failed");
            break;
        }
    }

    scene_node_stack_destroy(&stack);
}

/// @brief Advance this node's state by one tick and recursively update all children.
/// @details Calls rt_sprite_update on any attached sprite to advance its frame animation,
///   then propagates the update to all children in iterative pre-order.
///   Visibility does not suppress updates. Stack allocation failure traps
///   after any earlier nodes have updated.
/// @param node_ptr Opaque root SceneNode handle; invalid input is ignored.
void rt_scene_node_update(void *node_ptr) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;

    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, node)) {
        rt_trap("SceneNode.Update: stack allocation failed");
        scene_node_stack_destroy(&stack);
        return;
    }

    while (stack.count > 0) {
        scene_node_impl *cur = scene_node_stack_pop(&stack);
        if (!cur)
            continue;
        if (cur->sprite)
            rt_sprite_update(cur->sprite);
        if (!scene_node_stack_push_children_reverse(&stack, cur)) {
            rt_trap("SceneNode.Update: stack allocation failed");
            break;
        }
    }

    scene_node_stack_destroy(&stack);
}

/// @brief Translate the node by (dx, dy) relative to its current local position.
/// @details Both additions saturate independently and the complete subtree is
///          marked transform-dirty.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param dx Signed local X displacement.
/// @param dy Signed local Y displacement.
void rt_scene_node_move(void *node_ptr, int64_t dx, int64_t dy) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    int64_t new_x = scene_add_saturating(node->x, dx);
    int64_t new_y = scene_add_saturating(node->y, dy);
    if (new_x == node->x && new_y == node->y)
        return;
    node->x = new_x;
    node->y = new_y;
    mark_transform_dirty(node);
}

/// @brief Set both local position coordinates and invalidate world transforms.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param x New local X coordinate.
/// @param y New local Y coordinate.
void rt_scene_node_set_position(void *node_ptr, int64_t x, int64_t y) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    if (node->x == x && node->y == y)
        return;
    node->x = x;
    node->y = y;
    mark_transform_dirty(node);
}

/// @brief Set a uniform local scale and invalidate world transforms.
/// @details Values below one clamp to one before assignment to both axes.
/// @param node_ptr Opaque SceneNode handle; invalid input is ignored.
/// @param scale New uniform percentage.
void rt_scene_node_set_scale(void *node_ptr, int64_t scale) {
    scene_node_impl *node = scene_node_checked_or_null(node_ptr);
    if (!node)
        return;
    scale = scene_normalize_scale(scale);
    if (node->scale_x == scale && node->scale_y == scale)
        return;
    node->scale_x = scale;
    node->scale_y = scale;
    mark_transform_dirty(node);
}

//=============================================================================
// Scene (Root Container)
//=============================================================================

/// @brief Create an empty 2D scene with a single root node named "root".
/// @details The scene owns the new identity root and reusable draw scratch,
///          installs a finalizer, and balances the temporary retained root-name
///          string. Partial allocation failure traps and releases created
///          runtime objects.
/// @return A caller-owned Scene handle, or null after allocation failure.
void *rt_scene_new(void) {
    scene_impl *scene =
        (scene_impl *)rt_obj_new_i64(RT_SCENE_CLASS_ID, (int64_t)sizeof(scene_impl));
    if (!scene) {
        rt_trap("Scene: allocation failed");
        return NULL;
    }
    memset(scene, 0, sizeof(scene_impl));

    scene->root = (scene_node_impl *)rt_scene_node_new();
    if (!scene->root) {
        if (rt_obj_release_check0(scene))
            rt_obj_free(scene);
        rt_trap("Scene: root allocation failed");
        return NULL;
    }
    /* rt_const_cstr allocates a +1 reference for non-empty input; set_name retains
     * its own, so release the creation reference to avoid leaking one string per
     * Scene. */
    rt_string root_name = rt_const_cstr("root");
    rt_scene_node_set_name(scene->root, root_name);
    rt_string_unref(root_name);
    scene->state_magic = RT_SCENE_STATE_MAGIC;
    rt_obj_set_finalizer(scene, scene_finalize);

    return scene;
}

/// @brief Return the implicit root node so callers can attach children directly.
/// @param scene_ptr Opaque Scene handle.
/// @return Borrowed root SceneNode handle, or null for invalid input.
void *rt_scene_get_root(void *scene_ptr) {
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene)
        return NULL;
    return scene->root;
}

/// @brief Add a top-level node to the scene by attaching it as a child of the root.
/// @details Equivalent to rt_scene_node_add_child(scene->root, node).  The node's
///   world transform will inherit the root's identity transform.
/// @param scene_ptr  Scene handle.
/// @param node_ptr   Scene node to attach; silently ignored if NULL.
void rt_scene_add(void *scene_ptr, void *node_ptr) {
    if (!scene_ptr || !node_ptr)
        return;
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;
    rt_scene_node_add_child(scene->root, node_ptr);
}

/// @brief Remove a top-level node from the scene (detach from the root).
/// @details Only direct children of the scene root are removed.  Nodes that are
///   grandchildren or deeper are not affected.
/// @param scene_ptr  Scene handle.
/// @param node_ptr   Node to detach; silently ignored if NULL or not a direct child.
void rt_scene_remove(void *scene_ptr, void *node_ptr) {
    if (!scene_ptr || !node_ptr)
        return;
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;
    rt_scene_node_remove_child(scene->root, node_ptr);
}

/// @brief Hash an exact scene-node name byte span using FNV-1a.
static uint64_t scene_name_hash(const char *bytes, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/// @brief Grow and rehash the scene's open-addressed name index.
static int8_t scene_name_index_reserve(scene_impl *scene, int64_t needed) {
    if (!scene || needed < 0)
        return 0;
    if (scene->name_index_cap > 0 && needed <= scene->name_index_cap / 2)
        return 1;
    int64_t capacity = scene->name_index_cap > 0 ? scene->name_index_cap : 64;
    while (needed > capacity / 2) {
        if (capacity > INT64_MAX / 2 ||
            (uint64_t)(capacity * 2) > SIZE_MAX / sizeof(scene_node_impl *))
            return 0;
        capacity *= 2;
    }
    scene_node_impl **grown = (scene_node_impl **)calloc((size_t)capacity, sizeof(*grown));
    if (!grown)
        return 0;
    for (int64_t i = 0; i < scene->name_index_cap; ++i) {
        scene_node_impl *node = scene->name_index[i];
        if (!node)
            continue;
        const char *bytes = NULL;
        size_t length = 0;
        if (!scene_string_view(node->name, &bytes, &length)) {
            free(grown);
            return 0;
        }
        size_t slot = (size_t)(scene_name_hash(bytes, length) & (uint64_t)(capacity - 1));
        while (grown[slot])
            slot = (slot + 1) & (size_t)(capacity - 1);
        grown[slot] = node;
    }
    free(scene->name_index);
    scene->name_index = grown;
    scene->name_index_cap = capacity;
    return 1;
}

/// @brief Insert a node only when its name has no earlier pre-order match.
static int8_t scene_name_index_insert(scene_impl *scene, scene_node_impl *node) {
    const char *bytes = NULL;
    size_t length = 0;
    if (!scene || !node || !scene_string_view(node->name, &bytes, &length) ||
        !scene_name_index_reserve(scene, scene->name_index_count + 1))
        return 0;
    size_t slot = (size_t)(scene_name_hash(bytes, length) & (uint64_t)(scene->name_index_cap - 1));
    while (scene->name_index[slot]) {
        scene_node_impl *existing = scene->name_index[slot];
        const char *existing_bytes = NULL;
        size_t existing_length = 0;
        if (!scene_string_view(existing->name, &existing_bytes, &existing_length))
            return 0;
        if (existing_length == length && memcmp(existing_bytes, bytes, length) == 0)
            return 1;
        slot = (slot + 1) & (size_t)(scene->name_index_cap - 1);
    }
    scene->name_index[slot] = node;
    scene->name_index_count++;
    return 1;
}

/// @brief Rebuild the first-preorder-match name index after name/tree mutation.
static int8_t scene_rebuild_name_index(scene_impl *scene) {
    if (!scene)
        return 0;
    if (scene->name_index && scene->name_index_cap > 0)
        memset(scene->name_index, 0, (size_t)scene->name_index_cap * sizeof(scene_node_impl *));
    scene->name_index_count = 0;

    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, scene->root)) {
        scene_node_stack_destroy(&stack);
        return 0;
    }
    while (stack.count > 0) {
        scene_node_impl *node = scene_node_stack_pop(&stack);
        if (!node || !scene_name_index_insert(scene, node) ||
            !scene_node_stack_push_children_reverse(&stack, node)) {
            scene_node_stack_destroy(&stack);
            if (scene->name_index && scene->name_index_cap > 0)
                memset(scene->name_index,
                       0,
                       (size_t)scene->name_index_cap * sizeof(scene_node_impl *));
            scene->name_index_count = 0;
            scene->name_cache_revision = 0;
            return 0;
        }
    }
    scene_node_stack_destroy(&stack);
    scene->name_cache_revision = scene->root->name_revision;
    return 1;
}

/// @brief Search the scene's node tree for the first node matching @p name.
/// @details Uses a lazily rebuilt scene-owned hash index. Duplicate names map
///          to the first exact depth-first pre-order match, including the root.
/// @param scene_ptr Opaque Scene handle.
/// @param name Runtime string to match.
/// @return Borrowed first matching SceneNode, or null for invalid input or no
///         match.
void *rt_scene_find(void *scene_ptr, rt_string name) {
    if (!scene_ptr)
        return NULL;
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return NULL;
    const char *bytes = NULL;
    size_t length = 0;
    if (!scene_string_view(name, &bytes, &length))
        return NULL;
    if (scene->name_cache_revision != scene->root->name_revision &&
        !scene_rebuild_name_index(scene)) {
        rt_trap("Scene.Find: name index allocation failed");
        return NULL;
    }
    if (scene->name_index_cap <= 0)
        return NULL;
    size_t slot = (size_t)(scene_name_hash(bytes, length) & (uint64_t)(scene->name_index_cap - 1));
    for (int64_t probe = 0; probe < scene->name_index_cap; ++probe) {
        scene_node_impl *node = scene->name_index[slot];
        if (!node)
            return NULL;
        const char *node_bytes = NULL;
        size_t node_length = 0;
        if (!scene_string_view(node->name, &node_bytes, &node_length))
            return NULL;
        if (node_length == length && memcmp(node_bytes, bytes, length) == 0)
            return node;
        slot = (slot + 1) & (size_t)(scene->name_index_cap - 1);
    }
    return NULL;
}

/// @brief Search the scene's node tree for a matching name as an Option.
/// @param scene_ptr SceneGraph handle.
/// @param name Node name to search for.
/// @return Opaque Zanna.Option containing the first matching node, or None.
void *rt_scene_find_option(void *scene_ptr, rt_string name) {
    void *found = rt_scene_find(scene_ptr, name);
    return found ? rt_option_some(found) : rt_option_none();
}

//=============================================================================
// Depth-sorted rendering helpers
//=============================================================================

/// @brief One sortable visible-sprite entry in reusable scene draw scratch.
typedef struct {
    scene_node_impl *node;
    int64_t effective_depth;
    int64_t traversal_order;
} node_sort_entry;

/// @brief qsort comparator for node_sort_entry — sorts by depth ascending, ties by traversal order.
/// @details Preserving traversal order for equal-depth nodes guarantees that sibling
///   insertion order and tree-traversal order remain stable across frames even when
///   many nodes share the same depth value.
/// @param a Pointer to the first `node_sort_entry`.
/// @param b Pointer to the second `node_sort_entry`.
/// @return Negative, zero, or positive according to ascending effective depth
///         and then traversal order.
static int compare_depth(const void *a, const void *b) {
    const node_sort_entry *na = (const node_sort_entry *)a;
    const node_sort_entry *nb = (const node_sort_entry *)b;

    if (na->effective_depth < nb->effective_depth)
        return -1;
    if (na->effective_depth > nb->effective_depth)
        return 1;
    if (na->traversal_order < nb->traversal_order)
        return -1;
    if (na->traversal_order > nb->traversal_order)
        return 1;
    return 0;
}

/// @brief Collect visible sprite nodes (with depth/traversal keys) into the scene's
///        reusable draw scratch, growing it as needed. Returns the entry count, or
///        -1 on allocation failure. Reusing @p *arr across frames avoids the
///        per-frame rt_seq + malloc the previous implementation paid every draw.
/// @param root Root of the subtree to traverse.
/// @param arr In/out malloc-owned entry array.
/// @param cap In/out number of allocated entries in @p *arr.
/// @return Number of collected visible sprite nodes, or -1 on stack or scratch
///         growth failure.
static int64_t scene_collect_draw_entries(scene_node_impl *root,
                                          node_sort_entry **arr,
                                          int64_t *cap) {
    scene_node_stack stack;
    scene_node_stack_init(&stack);
    if (!scene_node_stack_push(&stack, root)) {
        scene_node_stack_destroy(&stack);
        return -1;
    }
    int64_t count = 0;
    while (stack.count > 0) {
        scene_node_impl *cur = scene_node_stack_pop(&stack);
        if (!cur || !cur->visible)
            continue;
        if (cur->sprite) {
            if (count >= *cap) {
                if (*cap < 0 || (*cap > 0 && *cap > INT64_MAX / 2)) {
                    scene_node_stack_destroy(&stack);
                    return -1;
                }
                int64_t new_cap = *cap > 0 ? *cap * 2 : 64;
                if (new_cap > INT64_MAX / (int64_t)sizeof(node_sort_entry) ||
                    (uint64_t)new_cap > (uint64_t)(SIZE_MAX / sizeof(node_sort_entry))) {
                    scene_node_stack_destroy(&stack);
                    return -1;
                }
                node_sort_entry *grown =
                    (node_sort_entry *)realloc(*arr, (size_t)new_cap * sizeof(node_sort_entry));
                if (!grown) {
                    scene_node_stack_destroy(&stack);
                    return -1;
                }
                *arr = grown;
                *cap = new_cap;
            }
            (*arr)[count].node = cur;
            (*arr)[count].effective_depth = cur->depth;
            (*arr)[count].traversal_order = count;
            count++;
        }
        if (!scene_node_stack_push_children_reverse(&stack, cur)) {
            scene_node_stack_destroy(&stack);
            return -1;
        }
    }
    scene_node_stack_destroy(&stack);
    return count;
}

/// @brief Return the scene's cached stable depth order, rebuilding only after
///        structural, visibility, sprite-presence, or depth mutations.
static int8_t scene_get_draw_entries(scene_impl *scene,
                                     node_sort_entry **entries_out,
                                     int64_t *count_out) {
    if (!scene || !entries_out || !count_out)
        return 0;
    if (scene->draw_cache_revision != scene->root->draw_revision) {
        node_sort_entry *entries = (node_sort_entry *)scene->draw_scratch;
        int64_t capacity = scene->draw_scratch_cap;
        int64_t count = scene_collect_draw_entries(scene->root, &entries, &capacity);
        scene->draw_scratch = entries;
        scene->draw_scratch_cap = capacity;
        if (count < 0)
            return 0;
        if (count > 1)
            qsort(entries, (size_t)count, sizeof(node_sort_entry), compare_depth);
        scene->draw_scratch_count = count;
        scene->draw_cache_revision = scene->root->draw_revision;
    }
    *entries_out = (node_sort_entry *)scene->draw_scratch;
    *count_out = scene->draw_scratch_count;
    return 1;
}

/// @brief Draw all visible nodes to @p canvas, sorted by depth.
/// @details Collects every visible node that has a sprite into a temporary list,
///   stable-sorts them by depth (ascending), and renders each in order using
///   world-space transforms.  Nodes with equal depth are drawn in traversal (insertion) order.
/// @param scene_ptr  Scene handle.
/// @param canvas     Target 2D canvas to draw to.
void rt_scene_draw(void *scene_ptr, void *canvas) {
    if (!scene_ptr || !canvas)
        return;

    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;

    node_sort_entry *arr = NULL;
    int64_t count = 0;
    if (!scene_get_draw_entries(scene, &arr, &count) || count <= 0)
        return;

    // Draw in depth order
    for (int64_t i = 0; i < count; i++) {
        scene_node_impl *node = arr[i].node;
        update_world_transform(node);

        if (node->sprite)
            rt_sprite_draw_transformed(node->sprite,
                                       canvas,
                                       node->world_x,
                                       node->world_y,
                                       node->world_scale_x,
                                       node->world_scale_y,
                                       node->world_rotation,
                                       -1,
                                       255);
    }
}

/// @brief Draw all visible nodes to @p canvas with camera-space transform applied.
/// @details Same depth-sorted traversal as rt_scene_draw, but each node's world position
///   is converted to screen space via rt_camera_world_to_screen and the camera zoom is
///   multiplied into the scale before rendering.  Camera rotation is subtracted from each
///   node's world rotation so nodes counter-rotate relative to the viewport.
/// @param scene_ptr  Scene handle.
/// @param canvas     Target 2D canvas.
/// @param camera     Camera that provides the view transform; may be NULL (identity view).
void rt_scene_draw_with_camera(void *scene_ptr, void *canvas, void *camera) {
    if (!scene_ptr || !canvas)
        return;

    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;

    node_sort_entry *arr = NULL;
    int64_t count = 0;
    if (!scene_get_draw_entries(scene, &arr, &count) || count <= 0)
        return;

    // Draw in depth order
    for (int64_t i = 0; i < count; i++) {
        scene_node_impl *node = arr[i].node;
        update_world_transform(node);

        if (node->sprite) {
            int64_t screen_x = node->world_x;
            int64_t screen_y = node->world_y;
            int64_t final_sx = node->world_scale_x;
            int64_t final_sy = node->world_scale_y;
            int64_t rotation = node->world_rotation;

            if (camera) {
                rt_camera_world_to_screen(
                    camera, node->world_x, node->world_y, &screen_x, &screen_y);
                int64_t zoom = rt_camera_get_zoom(camera);
                final_sx = scene_mul_percent_saturating(node->world_scale_x, zoom);
                final_sy = scene_mul_percent_saturating(node->world_scale_y, zoom);
                rotation = scene_sub_saturating(rotation, rt_camera_get_rotation(camera));
            }

            rt_sprite_draw_transformed(
                node->sprite, canvas, screen_x, screen_y, final_sx, final_sy, rotation, -1, 255);
        }
    }
}

/// @brief Advance all nodes in the scene by one frame — call once per game tick.
/// @details Recursively calls rt_scene_node_update on the root, which propagates to
///   every child, advancing sprite frame animations and any per-node update logic.
/// @param scene_ptr  Scene handle.
void rt_scene_update(void *scene_ptr) {
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;
    rt_scene_node_update(scene->root);
}

/// @brief Return the number of direct children attached to the scene root.
/// @details Only counts immediate children of the root node, not the entire tree.
///   Returns 0 for a NULL or invalid scene.
/// @param scene_ptr Opaque Scene handle.
/// @return Top-level node count, or zero for invalid input.
int64_t rt_scene_node_count(void *scene_ptr) {
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return 0;
    return rt_seq_len(scene->root->children);
}

/// @brief Remove all direct children from the scene root, releasing their references.
/// @details Clears every child's parent pointer before releasing the Seq entries to
///   prevent remove_child callbacks from firing during teardown.  Deep children are
///   freed transitively as their parent nodes lose their last reference.
/// @param scene_ptr  Scene handle.
void rt_scene_clear(void *scene_ptr) {
    scene_impl *scene = scene_checked_or_null(scene_ptr);
    if (!scene || !scene->root)
        return;

    // Clear parent pointers before removing children to avoid stale references
    int64_t n = rt_seq_len(scene->root->children);
    if (n > 0) {
        mark_draw_cache_dirty(scene->root);
        mark_name_cache_dirty(scene->root);
    }
    for (int64_t i = 0; i < n; i++) {
        scene_node_impl *child = (scene_node_impl *)rt_seq_get(scene->root->children, i);
        if (child)
            child->parent = NULL;
    }
    while (rt_seq_len(scene->root->children) > 0) {
        void *child = rt_seq_pop(scene->root->children);
        if (rt_obj_release_check0(child))
            rt_obj_free(child);
    }
}
