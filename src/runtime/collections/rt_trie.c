//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_trie.c
/// @file
/// @brief Implements the GC-managed bytewise Trie collection.
///
// Purpose: Implements a prefix tree (Trie) for string keys with associated
//   object values. Each trie node stores up to TRIE_ALPHABET_SIZE (256) child
//   pointers indexed by ASCII byte value. Supports exact lookup, prefix search
//   (all keys with a given prefix), longest-prefix-match, and lexicographic
//   key enumeration.
//
// Key invariants:
//   - Each node has a fixed 256-element children array indexed by byte value,
//     supporting the full byte range (0-255) including UTF-8 multi-byte sequences.
//   - The root node is allocated at construction and freed iteratively by the
//     trie finalizer to avoid C stack growth on deep keys.
//   - is_terminal marks nodes where a complete key ends. A node can be both
//     terminal (a key ends here) and internal (a longer key passes through).
//   - Values are retained (rt_obj_retain) on insert and released (rt_obj_free)
//     on overwrite or node deletion in the iterative free_node function.
//   - Deletion prunes the now-empty suffix while preserving terminal ancestors
//     and shared branches; clear/finalize free the whole node tree iteratively.
//   - KeysWithPrefix returns a Seq of all matching key strings (GC-managed).
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Trie objects are GC-managed (rt_obj_new_i64). All trie nodes are
//     malloc'd individually and freed iteratively by the GC finalizer via
//     free_node. Values stored in nodes are released on node free.
//   - Stored keys are represented structurally and are not retained as string
//     objects. Returned key strings and owning Seq snapshots are independent
//     of the trie. Exact lookup returns a borrowed value.
//
// Links: src/runtime/collections/rt_trie.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_trie.h"

#include "rt_option.h"

#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// @brief Installs @p buf as the current thread's non-local trap recovery target.
/// @param buf Jump buffer that receives control when a runtime trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Removes the current thread's trap recovery target.
void rt_trap_clear_recovery(void);

/// @brief Returns the diagnostic text associated with the latest runtime trap.
/// @return Borrowed null-terminated diagnostic text, if available.
const char *rt_trap_get_error(void);

/// @brief Number of possible one-byte edges from each trie node.
#define TRIE_ALPHABET_SIZE 256

/// @brief Trie node representing one byte of a structurally stored key.
///
/// The root represents the empty prefix. A terminal node may hold a null value,
/// so terminal status—not value nullness—determines whether a key exists.
typedef struct rt_trie_node {
    struct rt_trie_node *children[TRIE_ALPHABET_SIZE];
    void *value;         // Non-NULL if this node marks end of a key
    uint8_t is_terminal; // 1 if a key ends here (value may legitimately be
                         // NULL even at a terminal node)
} rt_trie_node;

/// @brief Runtime object payload for a Trie.
typedef struct rt_trie_impl {
    void **vptr;
    rt_trie_node *root;
    size_t count;
} rt_trie_impl;

/// @brief Explicit depth-first traversal frame used for freeing and GC tracing.
typedef struct {
    rt_trie_node *node;
    size_t next_child;
} trie_walk_frame;

/// @brief Explicit depth-first traversal frame used while reconstructing keys.
typedef struct {
    rt_trie_node *node;
    size_t next_child;
    size_t depth;
    int8_t emitted;
} trie_collect_frame;

/// @brief Source/destination node pair awaiting iterative deep cloning.
typedef struct {
    rt_trie_node *src;
    rt_trie_node *dst;
} trie_clone_pair;

/// @brief Parent edge recorded while locating a key for suffix pruning.
typedef struct {
    rt_trie_node *parent;
    unsigned char edge;
} trie_remove_path_entry;

/// @brief Checked cast of an opaque handle to the Trie implementation;
///        traps with @p what if @p obj is NULL or not a Trie.
/// @param obj Opaque runtime object to validate.
/// @param what Diagnostic raised when validation fails.
/// @return Validated Trie implementation, or `NULL` after raising a trap.
static rt_trie_impl *as_trie(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_TRIE_CLASS_ID, sizeof(rt_trie_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_trie_impl *)obj;
}

/// @brief Releases one retained runtime object and frees it at reference count zero.
/// @param obj Nullable object reference to release.
static void trie_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Copies the active trap diagnostic into a fixed caller-owned buffer.
/// @param buffer Destination for the null-terminated message.
/// @param buffer_size Capacity of @p buffer in bytes.
/// @param fallback Message used when no nonempty trap diagnostic is available.
static void trie_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Allocates a zero-initialized, nonterminal trie node.
/// @return New node, or `NULL` after raising an out-of-memory trap.
static rt_trie_node *new_node(void) {
    rt_trie_node *n = (rt_trie_node *)calloc(1, sizeof(rt_trie_node));
    if (!n) {
        rt_trap("rt_trie: memory allocation failed");
        return NULL;
    }
    return n;
}

/// @brief Validate a nullable runtime string and borrow its bytes and length.
/// @details Null continues to represent the documented empty key. A nonnull
///          stale or forged handle traps and returns failure instead of being
///          silently normalized to the empty key when an embedder hook returns.
/// @param s String to inspect; null denotes an empty byte string.
/// @param diagnostic Diagnostic raised for a nonnull invalid handle.
/// @param out_data Receives borrowed bytes, initialized to a stable empty string.
/// @param out_len Receives the byte length, initialized to zero.
/// @return Nonzero for null or a live string handle; otherwise zero after trapping.
static int trie_string_data(rt_string s,
                            const char *diagnostic,
                            const char **out_data,
                            size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!s)
        return 1;
    if (!rt_string_is_handle(s)) {
        rt_trap(diagnostic);
        return 0;
    }

    int64_t len = rt_str_len(s);
    if (len <= 0)
        return 1;

    const char *data = rt_string_cstr(s);
    if (!data) {
        rt_trap(diagnostic);
        return 0;
    }

    *out_data = data;
    *out_len = (size_t)len;
    return 1;
}

/// @brief Compute the next traversal-stack capacity (double, or 64 from
///        empty); traps on size overflow. Shared by the explicit-stack walks.
/// @param cap Current element capacity.
/// @param elem_size Size of one stack element in bytes.
/// @return New element capacity, or zero after raising an overflow trap.
static size_t grow_stack_capacity(size_t cap, size_t elem_size) {
    size_t new_cap = cap ? cap * 2 : 64;
    if (cap && cap > SIZE_MAX / 2) {
        rt_trap("rt_trie: traversal stack overflow");
        return 0;
    }
    if (new_cap > SIZE_MAX / elem_size) {
        rt_trap("rt_trie: traversal stack allocation overflow");
        return 0;
    }
    return new_cap;
}

/// @brief Push a node onto the explicit DFS walk stack, growing it if full
///        (NULL node ignored). Traps on OOM.
/// @param stack Address of the heap-allocated frame buffer.
/// @param len Address of the current frame count.
/// @param cap Address of the current frame capacity.
/// @param node Node to push; null is ignored.
static void push_walk_frame(trie_walk_frame **stack, size_t *len, size_t *cap, rt_trie_node *node) {
    if (!node)
        return;
    if (*len == *cap) {
        size_t new_cap = grow_stack_capacity(*cap, sizeof(**stack));
        if (new_cap == 0)
            return;
        trie_walk_frame *new_stack = (trie_walk_frame *)realloc(*stack, new_cap * sizeof(**stack));
        if (!new_stack) {
            rt_trap("rt_trie: memory allocation failed");
            return;
        }
        *stack = new_stack;
        *cap = new_cap;
    }
    (*stack)[(*len)++] = (trie_walk_frame){node, 0};
}

/// @brief Push a node (with key depth) onto the key-collection stack,
///        growing it if full. Traps on OOM.
/// @param stack Heap-allocated collection-frame buffer, or NULL.
/// @param len Address of the current frame count.
/// @param cap Address of the current frame capacity.
/// @param node Node to push; null is ignored.
/// @param depth Reconstructed key length represented by @p node.
/// @return Current buffer, updated when growth relocates it.
static trie_collect_frame *push_collect_frame(
    trie_collect_frame *stack, size_t *len, size_t *cap, rt_trie_node *node, size_t depth) {
    if (!node)
        return stack;
    if (*len == *cap) {
        size_t new_cap = grow_stack_capacity(*cap, sizeof(*stack));
        if (new_cap == 0)
            return stack;
        trie_collect_frame *new_stack =
            (trie_collect_frame *)realloc(stack, new_cap * sizeof(*stack));
        if (!new_stack) {
            rt_trap("rt_trie: memory allocation failed");
            return stack;
        }
        stack = new_stack;
        *cap = new_cap;
    }
    stack[(*len)++] = (trie_collect_frame){node, 0, depth, 0};
    return stack;
}

/// @brief Push a (src,dst) node pair onto the deep-clone stack, growing it
///        if full (ignored unless both nodes are non-NULL). Traps on OOM.
/// @param stack Heap-allocated clone-pair buffer, or NULL.
/// @param len Address of the current pair count.
/// @param cap Address of the current pair capacity.
/// @param src Source node whose children remain to be copied.
/// @param dst Corresponding destination node.
/// @return Current buffer, updated when growth relocates it.
static trie_clone_pair *push_clone_pair(
    trie_clone_pair *stack, size_t *len, size_t *cap, rt_trie_node *src, rt_trie_node *dst) {
    if (!src || !dst)
        return stack;
    if (*len == *cap) {
        size_t new_cap = grow_stack_capacity(*cap, sizeof(*stack));
        if (new_cap == 0)
            return stack;
        trie_clone_pair *new_stack = (trie_clone_pair *)realloc(stack, new_cap * sizeof(*stack));
        if (!new_stack) {
            rt_trap("rt_trie: memory allocation failed");
            return stack;
        }
        stack = new_stack;
        *cap = new_cap;
    }
    stack[(*len)++] = (trie_clone_pair){src, dst};
    return stack;
}

/// @brief Grow the reusable key-reconstruction buffer to >= @p needed bytes
///        (doubling). Traps on overflow/OOM.
/// @param buf Address of the heap buffer pointer to grow.
/// @param buf_cap Address of the buffer capacity in bytes.
/// @param needed Minimum required capacity in bytes.
/// @return Nonzero on success; zero after raising an overflow/allocation trap.
static int ensure_key_buf(char **buf, size_t *buf_cap, size_t needed) {
    if (needed <= *buf_cap)
        return 1;
    size_t new_cap = *buf_cap ? *buf_cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            rt_trap("rt_trie: key buffer overflow");
            return 0;
        }
        new_cap *= 2;
    }
    char *new_buf = (char *)realloc(*buf, new_cap);
    if (!new_buf) {
        rt_trap("rt_trie: memory allocation failed");
        return 0;
    }
    *buf = new_buf;
    *buf_cap = new_cap;
    return 1;
}

/// @brief Free a subtree iteratively without allocating cleanup storage.
/// @details Destruction repurposes each detached node's value slot as a parent
///          backlink and its terminal byte as a child cursor. Each child edge
///          is cleared before descent, so the walk needs neither recursion nor
///          a fallible traversal stack. This is safe because the subtree is
///          already exclusively owned by the cleanup path.
/// @param node Subtree root to destroy; null is ignored.
static void free_node(rt_trie_node *node) {
    if (!node)
        return;

    if (node->value)
        trie_release_object(node->value);
    node->value = NULL;
    node->is_terminal = 0;

    while (node) {
        unsigned child_index = node->is_terminal;
        rt_trie_node *child = node->children[child_index];
        if (child) {
            node->children[child_index] = NULL;
            if (child_index < TRIE_ALPHABET_SIZE - 1)
                node->is_terminal = (uint8_t)(child_index + 1);

            if (child->value)
                trie_release_object(child->value);
            child->value = node;
            child->is_terminal = 0;
            node = child;
            continue;
        }

        if (child_index < TRIE_ALPHABET_SIZE - 1) {
            node->is_terminal = (uint8_t)(child_index + 1);
            continue;
        }

        rt_trie_node *parent = (rt_trie_node *)node->value;
        free(node);
        node = parent;
    }
}

/// @brief Iteratively visit every terminal node's value under @p node
///        (explicit-stack DFS, GC tracing helper).
/// @param node Root of the subtree to trace.
/// @param visitor GC callback invoked for each terminal node's nullable value.
/// @param ctx Opaque context forwarded unchanged to @p visitor.
static void traverse_node(rt_trie_node *node, rt_gc_visitor_t visitor, void *ctx) {
    if (!node || !visitor)
        return;
    trie_walk_frame *stack = NULL;
    size_t len = 0;
    size_t cap = 0;
    push_walk_frame(&stack, &len, &cap, node);
    while (len > 0) {
        trie_walk_frame *frame = &stack[len - 1];
        if (frame->next_child == 0 && frame->node->is_terminal)
            visitor(frame->node->value, ctx);
        if (frame->next_child < TRIE_ALPHABET_SIZE) {
            rt_trie_node *child = frame->node->children[frame->next_child++];
            if (child)
                push_walk_frame(&stack, &len, &cap, child);
            continue;
        }
        len--;
    }
    free(stack);
}

/// @brief GC traversal entry point: visit every stored value in the trie.
/// @param obj Trie whose outgoing references are being traced.
/// @param visitor Callback invoked for every stored nullable value.
/// @param ctx Opaque context forwarded unchanged to @p visitor.
static void rt_trie_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_trie_impl *trie = as_trie(obj, "Trie: invalid Trie object");
    if (!trie)
        return;
    traverse_node(trie->root, visitor, ctx);
}

/// @brief Collects all terminal keys in a subtree into an owning sequence.
///
/// Traversal follows child indexes from 0 through 255, producing bytewise
/// lexicographic order. On a recovered construction/append trap, the helper
/// frees its traversal state and returns the diagnostic to its caller so the
/// caller can release its own buffer and sequence before re-raising.
///
/// @param node Subtree root corresponding to the prefix already in @p buf.
/// @param buf Address of the reusable key reconstruction buffer.
/// @param buf_cap Address of the current buffer capacity in bytes.
/// @param depth Prefix length and starting depth for @p node.
/// @param seq Owning Seq receiving copied runtime strings.
/// @param error Caller-owned diagnostic buffer populated on failure.
/// @param error_size Capacity of @p error in bytes.
/// @return Nonzero on success; zero after cleanup of helper-owned state.
static int collect_keys(rt_trie_node *node,
                        char **buf,
                        size_t *buf_cap,
                        size_t depth,
                        void *seq,
                        char *error,
                        size_t error_size) {
    if (!node)
        return 1;
    trie_collect_frame *volatile stack = NULL;
    size_t len = 0;
    size_t cap = 0;
    rt_string volatile key = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        trie_save_trap_error(error, error_size, "rt_trie: failed to collect keys");
        rt_trap_clear_recovery();
        if (key)
            rt_str_release_maybe((rt_string)key);
        free((void *)stack);
        return 0;
    }

    stack = push_collect_frame(stack, &len, &cap, node, depth);
    while (len > 0) {
        trie_collect_frame *frame = &((trie_collect_frame *)stack)[len - 1];
        if (!frame->emitted) {
            frame->emitted = 1;
            if (frame->node->is_terminal) {
                key = rt_string_from_bytes(*buf, frame->depth);
                rt_seq_push(seq, (void *)key);
                rt_str_release_maybe((rt_string)key);
                key = NULL;
            }
        }
        if (frame->next_child < TRIE_ALPHABET_SIZE) {
            size_t child_index = frame->next_child++;
            rt_trie_node *child = frame->node->children[child_index];
            if (child) {
                if (!ensure_key_buf(buf, buf_cap, frame->depth + 1)) {
                    free((void *)stack);
                    return 0;
                }
                (*buf)[frame->depth] = (char)child_index;
                stack = push_collect_frame(stack, &len, &cap, child, frame->depth + 1);
            }
            continue;
        }
        len--;
    }
    rt_trap_clear_recovery();
    free((void *)stack);
    return 1;
}

/// @brief Checks whether a node or any descendant terminates a stored key.
/// @param node Subtree root to search.
/// @return Nonzero if a terminal node exists, otherwise zero.
/// @note Uses an explicit stack and may raise an allocation trap.
static int has_any_key(rt_trie_node *node) {
    if (!node)
        return 0;
    trie_walk_frame *stack = NULL;
    size_t len = 0;
    size_t cap = 0;
    push_walk_frame(&stack, &len, &cap, node);
    while (len > 0) {
        trie_walk_frame *frame = &stack[len - 1];
        if (frame->next_child == 0 && frame->node->is_terminal) {
            free(stack);
            return 1;
        }
        if (frame->next_child < TRIE_ALPHABET_SIZE) {
            rt_trie_node *child = frame->node->children[frame->next_child++];
            if (child)
                push_walk_frame(&stack, &len, &cap, child);
            continue;
        }
        len--;
    }
    free(stack);
    return 0;
}

/// @brief Return non-zero when a trie node has at least one child edge.
/// @details Scans the fixed byte alphabet and exits on the first live child.
///          Removal uses this after clearing a terminal marker to decide how
///          much of the key's now-unreachable suffix can be reclaimed.
/// @param node Node to inspect, or NULL.
/// @return One when a child exists; zero for NULL or a leaf node.
static int trie_node_has_children(const rt_trie_node *node) {
    if (!node)
        return 0;
    for (size_t index = 0; index < TRIE_ALPHABET_SIZE; ++index) {
        if (node->children[index])
            return 1;
    }
    return 0;
}

/// @brief GC finalizer: free the entire node tree and reset the trie.
/// @param obj Trie instance being finalized; null is ignored.
static void rt_trie_finalize(void *obj) {
    if (!obj)
        return;
    rt_trie_impl *trie = as_trie(obj, "Trie: invalid Trie object");
    if (!trie)
        return;
    free_node(trie->root);
    trie->root = NULL;
    trie->count = 0;
}

/// @brief Construct an empty trie. Each node has a 256-way child array (one slot per byte
/// value), so memory cost is high per node but lookups are O(key length).
/// @return New GC-managed Trie, or `NULL` after an allocation trap.
/// @note The returned object must not be freed directly by callers.
void *rt_trie_new(void) {
    rt_trie_impl *trie =
        (rt_trie_impl *)rt_obj_new_i64(RT_TRIE_CLASS_ID, (int64_t)sizeof(rt_trie_impl));
    if (!trie) {
        rt_trap("rt_trie: memory allocation failed");
        return NULL;
    }
    trie->vptr = NULL;
    trie->root = NULL;
    trie->count = 0;
    rt_obj_set_finalizer(trie, rt_trie_finalize);
    rt_gc_track(trie, rt_trie_traverse);
    trie->root = (rt_trie_node *)calloc(1, sizeof(rt_trie_node));
    if (!trie->root) {
        if (rt_obj_release_check0(trie))
            rt_obj_free(trie);
        rt_trap("rt_trie: memory allocation failed");
        return NULL;
    }
    return trie;
}

/// @brief Number of keys (terminal nodes) currently stored in the trie.
/// @param obj Trie handle, or null to query an empty trie.
/// @return Number of exact keys, including a stored empty key.
/// @note A nonnull object of the wrong runtime class raises a trap.
int64_t rt_trie_len(void *obj) {
    if (!obj)
        return 0;
    rt_trie_impl *trie = as_trie(obj, "Trie.Len: invalid Trie object");
    return trie ? (int64_t)trie->count : 0;
}

/// @brief Returns 1 if the trie has no keys.
/// @param obj Trie handle, or null to query an empty trie.
/// @return One when no terminal keys exist; otherwise zero.
int8_t rt_trie_is_empty(void *obj) {
    return rt_trie_len(obj) == 0;
}

/// @brief Insert or update `key → value`. Walks the trie one byte at a time, allocating new
/// nodes for each missing branch. Existing key replaces (and releases) the old value. O(|key|).
/// @param obj Trie to mutate; null is ignored.
/// @param key Byte string to store; null denotes the empty key.
/// @param value Nullable object retained while the key remains stored.
/// @note Raises a trap for invalid objects, allocation failure, or count overflow.
void rt_trie_set(void *obj, rt_string key, void *value) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_trie_impl *trie = as_trie(obj, "Trie.Set: invalid Trie object");
    if (!trie) {
        rt_gc_mutator_exit();
        return;
    }
    if (!trie->root) {
        rt_gc_mutator_exit();
        return;
    }

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(key, "Trie.Set: invalid key", &cstr, &len)) {
        rt_gc_mutator_exit();
        return;
    }

    rt_trie_node *node = trie->root;
    size_t missing_index = len;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c]) {
            missing_index = i;
            break;
        }
        node = node->children[c];
    }

    int adding_key = missing_index < len || !node->is_terminal;
    if (adding_key && trie->count >= (size_t)INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Trie.Set: maximum size reached");
        return;
    }

    rt_trie_node *volatile branch_root = NULL;
    rt_trie_node *branch_tail = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        trie_save_trap_error(saved_error, sizeof(saved_error), "Trie.Set: operation failed");
        rt_trap_clear_recovery();
        free_node((rt_trie_node *)branch_root);
        rt_trap(saved_error);
        return;
    }

    for (size_t i = missing_index; i < len; ++i) {
        rt_trie_node *child = new_node();
        if (!branch_root) {
            branch_root = child;
        } else {
            branch_tail->children[(unsigned char)cstr[i]] = child;
        }
        branch_tail = child;
    }

    rt_obj_retain_maybe(value);
    if (branch_root) {
        node->children[(unsigned char)cstr[missing_index]] = (rt_trie_node *)branch_root;
        node = branch_tail;
        branch_root = NULL;
    }

    void *old = node->value;
    node->value = value;
    if (!node->is_terminal)
        trie->count++;
    node->is_terminal = 1;
    rt_trap_clear_recovery();
    rt_gc_mutator_exit();

    if (old && rt_obj_release_check0(old))
        rt_obj_free(old);
}

/// @brief Look up `key`. Returns the borrowed value or NULL if not present. O(|key|).
/// @param obj Trie to search; null returns `NULL`.
/// @param key Exact byte string to find; null denotes the empty key.
/// @return Borrowed stored value, or `NULL` for absent and null-valued keys.
/// @note Use rt_trie_has() when an absent key must be distinguished from a
///       key whose stored value is null.
void *rt_trie_get(void *obj, rt_string key) {
    if (!obj)
        return NULL;
    rt_trie_impl *trie = as_trie(obj, "Trie.Get: invalid Trie object");
    if (!trie || !trie->root)
        return NULL;

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(key, "Trie.Get: invalid key", &cstr, &len))
        return NULL;

    rt_trie_node *node = trie->root;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            return NULL;
        node = node->children[c];
    }
    return node->is_terminal ? node->value : NULL;
}

/// @brief Returns 1 if `key` is exactly stored (terminal). Distinct from `_has_prefix`.
/// @param obj Trie to search; null returns false.
/// @param key Exact byte string to find; null denotes the empty key.
/// @return One for an exact terminal match, otherwise zero.
int8_t rt_trie_has(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_trie_impl *trie = as_trie(obj, "Trie.Has: invalid Trie object");
    if (!trie || !trie->root)
        return 0;

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(key, "Trie.Has: invalid key", &cstr, &len))
        return 0;

    rt_trie_node *node = trie->root;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            return 0;
        node = node->children[c];
    }
    return node->is_terminal;
}

/// @brief Returns 1 if any stored key starts with `prefix` (or `prefix` itself is stored).
/// Useful for autocomplete "any matches?" probes.
/// @param obj Trie to search; null returns false.
/// @param prefix Byte prefix to locate; null denotes the empty prefix.
/// @return One when the located subtree contains any terminal key.
/// @note Querying the empty prefix is equivalent to asking whether the trie
///       contains at least one key.
int8_t rt_trie_has_prefix(void *obj, rt_string prefix) {
    if (!obj)
        return 0;
    rt_trie_impl *trie = as_trie(obj, "Trie.HasPrefix: invalid Trie object");
    if (!trie || !trie->root)
        return 0;

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(prefix, "Trie.HasPrefix: invalid prefix", &cstr, &len))
        return 0;

    rt_trie_node *node = trie->root;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            return 0;
        node = node->children[c];
    }
    return has_any_key(node) ? 1 : 0;
}

/// @brief Return a Seq of all stored keys that start with `prefix` (the typical autocomplete
/// query). Walks the subtrie rooted at the prefix node and reconstructs full keys via DFS.
/// @param obj Trie to search; null produces an empty sequence.
/// @param prefix Byte prefix to match; null denotes the empty prefix.
/// @return New owning Seq of copied strings in bytewise lexicographic order.
/// @note Snapshot allocation/collection failures raise a trap and clean up the
///       partially constructed snapshot.
void *rt_trie_with_prefix(void *obj, rt_string prefix) {
    void *result = rt_seq_new();
    if (!result)
        return NULL;
    rt_seq_set_owns_elements(result, 1);
    if (!obj)
        return result;
    rt_trie_impl *trie = as_trie(obj, "Trie.WithPrefix: invalid Trie object");
    if (!trie || !trie->root)
        return result;

    size_t plen = 0;
    const char *cstr = NULL;
    if (!trie_string_data(prefix, "Trie.WithPrefix: invalid prefix", &cstr, &plen)) {
        trie_release_object(result);
        return NULL;
    }

    // Navigate to prefix node
    rt_trie_node *node = trie->root;
    for (size_t i = 0; i < plen; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            return result;
        node = node->children[c];
    }

    // Collect all keys under this node; buffer grows as needed
    size_t buf_cap = plen + 1 > 4096 ? plen + 1 : 4096;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) {
        rt_trap("rt_trie: memory allocation failed");
        return result;
    }
    if (plen > 0)
        memcpy(buf, cstr, plen);
    char error[256] = {0};
    if (!collect_keys(node, &buf, &buf_cap, plen, result, error, sizeof(error))) {
        free(buf);
        trie_release_object(result);
        rt_trap(error);
        return NULL;
    }
    free(buf);
    return result;
}

/// @brief Find the longest stored key that is a prefix of `str`. Useful for tokenizers, IP
/// routing, and dictionary-based segmentation. Empty result if no prefix matches.
/// @param obj Trie to search; null produces a newly allocated empty string.
/// @param str Byte string whose prefixes are examined; null denotes empty.
/// @return New string containing the longest matching key, or a new empty
///         string when no stored key is a prefix.
/// @note Because the empty key is valid, this API cannot distinguish an
///       empty-key match from no match; use rt_trie_longest_prefix_option().
rt_string rt_trie_longest_prefix(void *obj, rt_string str) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_trie_impl *trie = as_trie(obj, "Trie.LongestPrefix: invalid Trie object");
    if (!trie || !trie->root)
        return rt_string_from_bytes("", 0);

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(str, "Trie.LongestPrefix: invalid source", &cstr, &len))
        return NULL;

    rt_trie_node *node = trie->root;
    size_t last_match = 0;
    int found = 0;

    if (node->is_terminal) {
        found = 1;
        last_match = 0;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            break;
        node = node->children[c];
        if (node->is_terminal) {
            found = 1;
            last_match = i + 1;
        }
    }

    if (!found)
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(cstr, last_match);
}

/// @brief LongestPrefix preserving the empty-key match: Some(prefix) when any
///        stored key (including "") is a prefix of `str`, None when nothing
///        matches (VDOC-103).
/// @param obj Trie to search; null returns `None`.
/// @param str Byte string whose prefixes are examined; null denotes empty.
/// @return New Option containing an owned matching string, or a new `None`.
void *rt_trie_longest_prefix_option(void *obj, rt_string str) {
    if (!obj)
        return rt_option_none();
    rt_trie_impl *trie = as_trie(obj, "Trie.LongestPrefix: invalid Trie object");
    if (!trie || !trie->root)
        return rt_option_none();

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(str, "Trie.LongestPrefix: invalid source", &cstr, &len))
        return NULL;

    rt_trie_node *node = trie->root;
    size_t last_match = 0;
    int found = 0;

    if (node->is_terminal) {
        found = 1;
        last_match = 0;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c])
            break;
        node = node->children[c];
        if (node->is_terminal) {
            found = 1;
            last_match = i + 1;
        }
    }

    if (!found)
        return rt_option_none();
    rt_string match = rt_string_from_bytes(cstr, last_match);
    void *opt = rt_option_some_str(match);
    rt_string_unref(match);
    return opt;
}

/// @brief Remove `key`, release its value, and prune its empty structural suffix.
/// @details Records each parent edge while navigating. After clearing the
///          terminal node, walks that path backward and frees leaf nodes until
///          reaching either a terminal ancestor or a node shared by another
///          key. The root node is permanent, including for the empty key.
/// @param obj Trie object, or NULL for an absent result.
/// @param key Exact byte-string key to remove.
/// @return One if a terminal key was removed; zero when absent.
int8_t rt_trie_remove(void *obj, rt_string key) {
    if (!obj)
        return 0;
    rt_gc_mutator_enter();
    rt_trie_impl *trie = as_trie(obj, "Trie.Remove: invalid Trie object");
    if (!trie || !trie->root) {
        rt_gc_mutator_exit();
        return 0;
    }

    size_t len = 0;
    const char *cstr = NULL;
    if (!trie_string_data(key, "Trie.Remove: invalid key", &cstr, &len)) {
        rt_gc_mutator_exit();
        return 0;
    }

    if (len > SIZE_MAX / sizeof(trie_remove_path_entry)) {
        rt_gc_mutator_exit();
        rt_trap("Trie.Remove: key path size overflow");
        return 0;
    }
    trie_remove_path_entry *path = NULL;
    if (len > 0) {
        path = (trie_remove_path_entry *)malloc(len * sizeof(*path));
        if (!path) {
            rt_gc_mutator_exit();
            rt_trap("Trie.Remove: memory allocation failed");
            return 0;
        }
    }

    // Navigate to the node
    rt_trie_node *node = trie->root;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)cstr[i];
        if (!node->children[c]) {
            free(path);
            rt_gc_mutator_exit();
            return 0;
        }
        path[i] = (trie_remove_path_entry){node, c};
        node = node->children[c];
    }

    if (!node->is_terminal) {
        free(path);
        rt_gc_mutator_exit();
        return 0;
    }

    // Remove terminal mark and release value
    node->is_terminal = 0;
    void *value = node->value;
    node->value = NULL;
    trie->count--;

    for (size_t depth = len; depth > 0; --depth) {
        trie_remove_path_entry entry = path[depth - 1];
        rt_trie_node *child = entry.parent->children[entry.edge];
        if (!child || child->is_terminal || trie_node_has_children(child))
            break;
        entry.parent->children[entry.edge] = NULL;
        free(child);
    }
    free(path);
    rt_gc_mutator_exit();

    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
    return 1;
}

/// @brief Free every node and reset the trie to empty. Releases all stored values.
/// @param obj Trie to clear; null is ignored.
/// @note A fresh root is allocated before the old tree is destroyed, so an
///       allocation trap leaves the original trie unchanged.
void rt_trie_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_trie_impl *trie = as_trie(obj, "Trie.Clear: invalid Trie object");
    if (!trie) {
        rt_gc_mutator_exit();
        return;
    }
    rt_trie_node *replacement = new_node();
    if (!replacement) {
        rt_gc_mutator_exit();
        return;
    }
    rt_trie_node *old_root = trie->root;
    trie->root = replacement;
    trie->count = 0;
    rt_gc_mutator_exit();
    free_node(old_root);
}

/// @brief Return a Seq of every stored key in lexicographic order. Owned-element Seq (releases
/// the strings on its own destruction). Implemented via DFS over the trie.
/// @param obj Trie to snapshot; null produces an empty sequence.
/// @return New owning Seq of copied keys in bytewise lexicographic order, or
///         `NULL` if the initial sequence allocation fails.
/// @note Later trie mutations do not affect the returned strings.
void *rt_trie_keys(void *obj) {
    void *result = rt_seq_new();
    if (!result)
        return NULL;
    rt_seq_set_owns_elements(result, 1);
    if (!obj)
        return result;
    rt_trie_impl *trie = as_trie(obj, "Trie.Keys: invalid Trie object");
    if (!trie)
        return result;
    if (!trie->root)
        return result;

    size_t buf_cap = 4096;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) {
        rt_trap("rt_trie: memory allocation failed");
        return result;
    }
    char error[256] = {0};
    if (!collect_keys(trie->root, &buf, &buf_cap, 0, result, error, sizeof(error))) {
        free(buf);
        trie_release_object(result);
        rt_trap(error);
        return NULL;
    }
    free(buf);
    return result;
}

/// @brief Iteratively clone a trie node and all its descendants.
/// @param src Root of the source subtree; null produces a null result.
/// @param error Caller-owned diagnostic buffer populated on failure.
/// @param error_size Capacity of @p error in bytes.
/// @return Newly allocated structural copy whose values have been retained,
///         or `NULL` after an allocation trap.
/// @note A failed clone releases all nodes and value references already copied.
static rt_trie_node *clone_node(rt_trie_node *src, char *error, size_t error_size) {
    if (!src)
        return NULL;

    rt_trie_node *volatile dst = NULL;
    trie_clone_pair *volatile stack = NULL;
    size_t len = 0;
    size_t cap = 0;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        trie_save_trap_error(error, error_size, "rt_trie: clone failed");
        rt_trap_clear_recovery();
        free_node((rt_trie_node *)dst);
        free((void *)stack);
        return NULL;
    }

    dst = new_node();
    if (src->value)
        rt_obj_retain_maybe(src->value);
    ((rt_trie_node *)dst)->is_terminal = src->is_terminal;
    ((rt_trie_node *)dst)->value = src->value;

    stack = push_clone_pair(stack, &len, &cap, src, (rt_trie_node *)dst);
    while (len > 0) {
        trie_clone_pair pair = ((trie_clone_pair *)stack)[--len];
        for (int i = 0; i < TRIE_ALPHABET_SIZE; ++i) {
            rt_trie_node *child_src = pair.src->children[i];
            if (!child_src)
                continue;
            rt_trie_node *child_dst = new_node();
            pair.dst->children[i] = child_dst;
            if (child_src->value)
                rt_obj_retain_maybe(child_src->value);
            child_dst->is_terminal = child_src->is_terminal;
            child_dst->value = child_src->value;
            stack = push_clone_pair(stack, &len, &cap, child_src, child_dst);
        }
    }
    rt_trap_clear_recovery();
    free((void *)stack);

    return (rt_trie_node *)dst;
}

/// @brief Deep-copy the trie structure (all nodes); values are shared via reference-count
/// retain. Modifying the clone's structure (insertions/removals) doesn't affect the source.
/// @param obj Source Trie; null creates a new empty Trie.
/// @return New GC-managed Trie with independent nodes and retained shared
///         values, or `NULL` after an allocation failure.
void *rt_trie_clone(void *obj) {
    if (!obj)
        return rt_trie_new();

    rt_trie_impl *src = as_trie(obj, "Trie.Clone: invalid Trie object");
    if (!src)
        return NULL;
    rt_trie_impl *dst = (rt_trie_impl *)rt_trie_new();
    if (!dst)
        return NULL;

    char error[256] = {0};
    rt_trie_node *cloned_root = clone_node(src->root, error, sizeof(error));
    if (!cloned_root) {
        trie_release_object(dst);
        rt_trap(error);
        return NULL;
    }

    // Free the default empty root from rt_trie_new, replace with deep copy
    free_node(dst->root);
    dst->root = cloned_root;
    dst->count = src->count;

    return dst;
}
