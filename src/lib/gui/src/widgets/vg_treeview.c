//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements retained and provider-backed hierarchical tree views.
/// @details The retained path owns a hierarchy of stable node records with
///          selection, expansion, lazy-loading, icons, scrolling, and optional
///          drag-and-drop state. Removed subtrees become inert tombstones so
///          external wrappers can test stale handles before reclamation. The
///          virtual path instead materializes only visible row descriptors
///          through application callbacks and allocates no per-row nodes.
///
///          Both paths share viewport metrics, scrollbar interaction, themed
///          painting, scheduler-visible state edges, and keyboard/pointer
///          behavior. Application-directed drag modes expose validated drop
///          latches without mutating the retained hierarchy internally.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_treeview.c
// Purpose: Hierarchical tree-view widget with lazy loading, drag-and-drop
//          reordering, icon support, keyboard navigation, and alternating row
//          backgrounds.
// Key invariants:
//   - Every live node carries VG_TREE_NODE_MAGIC and a non-NULL owner pointer.
//     Retired nodes carry VG_TREE_NODE_RETIRED_MAGIC with owner == NULL so that
//     stale external handles fail vg_tree_node_is_live() safely.
//   - Removed nodes are not freed immediately; they are placed on retired_nodes
//     (via retired_next) so stale external handles fail vg_tree_node_is_live()
//     safely. Call vg_treeview_prune_retired_nodes only when callers no longer
//     retain removed node handles; destroy always drains the retired list.
//   - scroll_y is always re-clamped after collapse and selection changes to
//     prevent blank space at the bottom of the visible area.
//   - Scrollbar cleanup releases input capture only when the scrollbar owned
//     it; retained-tree drag-and-drop uses the same widget capture independently.
//   - drag-and-drop: drop position is classified as BEFORE/INTO/AFTER based on
//     where in the target row's height (< 30% → BEFORE, > 70% → AFTER, else
//     INTO); drops are vetoed by treeview_drop_is_valid.
//   - Retained multi-selection stores one primary and one range anchor while
//     every selected node carries its own flag; range traversal is iterative.
//   - Application-directed DnD mode 1 preserves container-only INTO drops;
//     mode 2 exposes row-aware BEFORE/INTO/AFTER latches.
// Ownership/Lifetime:
//   - vg_tree_node_t instances are allocated by vg_treeview_add_node and owned
//     by the tree. Callers must not free nodes directly.
//   - node->user_data is freed on retire only if owns_user_data is true.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h,
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VG_TREE_NODE_MAGIC UINT64_C(0x564754524E4F4445)
#define VG_TREE_NODE_RETIRED_MAGIC UINT64_C(0x5647545244524F50)

//=============================================================================
// Forward Declarations
//=============================================================================

static void treeview_destroy(vg_widget_t *widget);
static void treeview_measure(vg_widget_t *widget, float available_width, float available_height);
static void treeview_paint(vg_widget_t *widget, void *canvas);
static bool treeview_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool treeview_can_focus(vg_widget_t *widget);

static void free_node(vg_tree_node_t *node);
static void free_retired_nodes(vg_treeview_t *tree);
static void retire_node_subtree(vg_treeview_t *tree, vg_tree_node_t *node);
static int count_visible_nodes(vg_tree_node_t *node);
static vg_tree_node_t *get_node_at_index(vg_tree_node_t *root, int index, int *current);
static int get_node_index(vg_tree_node_t *root, vg_tree_node_t *target, int *current);
static bool node_in_subtree(const vg_tree_node_t *root, const vg_tree_node_t *candidate);
static vg_tree_node_t *treeview_next_retained(vg_tree_node_t *root,
                                              vg_tree_node_t *node,
                                              bool visible_only);
static bool treeview_clear_retained_selection(vg_treeview_t *tree, vg_tree_node_t *keep);
static vg_tree_node_t *treeview_first_selected(vg_treeview_t *tree);
static bool treeview_subtree_has_selection(vg_tree_node_t *root);
static void treeview_select_interactive(vg_treeview_t *tree,
                                        vg_tree_node_t *node,
                                        bool toggle,
                                        bool range);
static void treeview_clamp_scroll(vg_treeview_t *tree);
static float treeview_content_height(const vg_treeview_t *tree);
static float treeview_max_scroll(const vg_treeview_t *tree);
static bool treeview_scrollbar_geometry(const vg_treeview_t *tree,
                                        float *out_width,
                                        float *out_thumb_y,
                                        float *out_thumb_height);
static bool treeview_handle_scrollbar_event(vg_treeview_t *tree, vg_event_t *event);
static void treeview_paint_scrollbar(vg_treeview_t *tree, void *canvas);
static float treeview_scale(void);
static float treeview_outer_padding(void);
static void treeview_sync_metrics(vg_treeview_t *tree);
static float treeview_text_baseline(vg_treeview_t *tree, float row_y);
static void treeview_encode_glyph(uint32_t codepoint, char out[8]);
static char *treeview_fit_text(vg_treeview_t *tree, const char *text, float max_width);
static bool treeview_drop_is_valid(vg_treeview_t *tree,
                                   vg_tree_node_t *source,
                                   vg_tree_node_t *target,
                                   vg_tree_drop_position_t position);
static void treeview_update_drop_target(vg_treeview_t *tree, float local_y);
static void treeview_paint_icon(vg_treeview_t *tree,
                                void *canvas,
                                vg_tree_node_t *node,
                                float icon_x,
                                float row_y,
                                uint32_t color);
static void treeview_notify_virtual_unbound(vg_treeview_t *tree);
static void treeview_compute_virtual_range(vg_treeview_t *tree,
                                           size_t *out_start,
                                           size_t *out_count);
static void treeview_paint_virtual(vg_treeview_t *tree, void *canvas);
static void treeview_edit_finish(vg_treeview_t *tree, bool commit);
static void treeview_edit_sync(vg_treeview_t *tree);
static bool treeview_handle_virtual_event(vg_treeview_t *tree, vg_event_t *event);

//=============================================================================
// TreeView VTable
//=============================================================================

static vg_widget_vtable_t g_treeview_vtable = {.destroy = treeview_destroy,
                                               .measure = treeview_measure,
                                               .arrange = NULL,
                                               .paint = treeview_paint,
                                               .handle_event = treeview_handle_event,
                                               .can_focus = treeview_can_focus,
                                               .on_focus = NULL};

//=============================================================================
// Helper Functions
//=============================================================================

typedef struct tree_node_stack {
    vg_tree_node_t **items;
    size_t count;
    size_t cap;
} tree_node_stack_t;

/// @brief Release storage owned by a temporary iterative traversal stack.
/// @param stack Stack to clear; `NULL` is ignored.
static void tree_node_stack_destroy(tree_node_stack_t *stack) {
    if (!stack)
        return;
    free(stack->items);
    stack->items = NULL;
    stack->count = 0;
    stack->cap = 0;
}

/// @brief Push a node onto a growable traversal stack.
/// @details Capacity doubles from 64 entries with arithmetic-overflow checks.
///          A `NULL` stack or node is treated as having no work to enqueue.
/// @param stack Stack receiving the node.
/// @param node Node pointer to append.
/// @return `true` when the node is queued or intentionally ignored; `false`
///         only when capacity cannot be grown safely.
static bool tree_node_stack_push(tree_node_stack_t *stack, vg_tree_node_t *node) {
    if (!stack || !node)
        return true;
    if (stack->count == stack->cap) {
        size_t new_cap = stack->cap ? stack->cap * 2 : 64;
        if (new_cap < stack->cap || new_cap > SIZE_MAX / sizeof(*stack->items))
            return false;
        vg_tree_node_t **items = (vg_tree_node_t **)realloc(stack->items, new_cap * sizeof(*items));
        if (!items)
            return false;
        stack->items = items;
        stack->cap = new_cap;
    }
    stack->items[stack->count++] = node;
    return true;
}

/// @brief Pop the most recently queued traversal node.
/// @param stack Stack to consume; may be `NULL`.
/// @return Popped node pointer, or `NULL` for an empty/absent stack.
static vg_tree_node_t *tree_node_stack_pop(tree_node_stack_t *stack) {
    if (!stack || stack->count == 0)
        return NULL;
    return stack->items[--stack->count];
}

/// @brief Release resources owned directly by one node record.
/// @details Text, icon text, stable identifier, and icon descriptors are
///          destroyed. User data is freed only when `owns_user_data` is set;
///          child and sibling records are not traversed here.
/// @param node Node whose payload is cleared; `NULL` is ignored.
static void free_node_payload(vg_tree_node_t *node) {
    if (!node)
        return;
    free(node->text);
    node->text = NULL;
    node->text_len = 0;
    free(node->icon_text);
    node->icon_text = NULL;
    node->icon_text_len = 0;
    free(node->icon_spec);
    node->icon_spec = NULL;
    free(node->stable_id);
    node->stable_id = NULL;
    node->stable_id_len = 0;
    vg_icon_destroy(&node->icon);
    vg_icon_destroy(&node->expanded_icon);
    if (node->owns_user_data && node->user_data) {
        free(node->user_data);
        node->user_data = NULL;
    }
    node->owns_user_data = false;
}

/// @brief Iteratively destroy a node subtree and its owned payloads.
/// @details Leaf records are detached and freed while walking back toward the
///          supplied root, avoiding recursion for arbitrarily deep trees.
///          Parent child counts and sibling links are repaired during removal.
/// @param node Root of the subtree to destroy; `NULL` is ignored.
static void free_node(vg_tree_node_t *node) {
    if (!node)
        return;

    while (node) {
        if (node->first_child) {
            node = node->first_child;
            continue;
        }

        vg_tree_node_t *parent = node->parent;
        vg_tree_node_t *next = node->next_sibling;

        if (node->prev_sibling)
            node->prev_sibling->next_sibling = next;
        if (next)
            next->prev_sibling = node->prev_sibling;
        if (parent) {
            if (parent->first_child == node)
                parent->first_child = next;
            if (parent->last_child == node)
                parent->last_child = node->prev_sibling;
            if (parent->child_count > 0)
                parent->child_count--;
            parent->has_children = parent->first_child != NULL;
        }

        free_node_payload(node);
        free(node);
        node = next ? next : parent;
    }
}

/// @brief Convert every record in a subtree into an inert tombstone.
/// @details Payload ownership is released, selection/loading state is cleared,
///          and owner/magic fields are changed without freeing record storage
///          or destroying the hierarchy used for later reclamation.
/// @param node Root of the subtree to retire; `NULL` is ignored.
static void mark_node_subtree_retired(vg_tree_node_t *node) {
    if (!node)
        return;

    vg_tree_node_t *root = node;
    while (node) {
        vg_tree_node_t *next = NULL;
        if (node->first_child) {
            next = node->first_child;
        } else {
            vg_tree_node_t *cursor = node;
            while (cursor && cursor != root && !cursor->next_sibling)
                cursor = cursor->parent;
            if (cursor && cursor != root)
                next = cursor->next_sibling;
        }

        free_node_payload(node);
        node->selected = false;
        node->loading = false;
        node->owner = NULL;
        node->magic = VG_TREE_NODE_RETIRED_MAGIC;
        node = next;
    }
}

/// @brief Retire a detached subtree for deferred reclamation.
/// @details The subtree is tombstoned, its external parent/sibling links are
///          cleared, and its root is prepended to the TreeView's retired-root
///          list.
/// @param tree TreeView that owns the retired list.
/// @param node Detached subtree root to retire.
static void retire_node_subtree(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !node)
        return;
    mark_node_subtree_retired(node);
    node->parent = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
    node->retired_next = tree->retired_nodes;
    tree->retired_nodes = node;
}

/// @brief Destroy every subtree on a TreeView's retired list.
/// @param tree TreeView whose tombstones are reclaimed; `NULL` is ignored.
static void free_retired_nodes(vg_treeview_t *tree) {
    if (!tree)
        return;
    vg_tree_node_t *node = tree->retired_nodes;
    while (node) {
        vg_tree_node_t *next = node->retired_next;
        node->retired_next = NULL;
        free_node(node);
        node = next;
    }
    tree->retired_nodes = NULL;
}

/// @brief Test whether a node handle still denotes a live retained-tree node.
/// @param node Node handle to inspect; may be `NULL`.
/// @return `true` only when the active magic value and non-null owner are both
///         present.
bool vg_tree_node_is_live(const vg_tree_node_t *node) {
    return node && node->magic == VG_TREE_NODE_MAGIC && node->owner != NULL;
}

/// @brief Count currently visible retained rows below a sentinel or node.
/// @details Children are traversed iteratively and descendants are queued only
///          for expanded nodes. Allocation failure or integer overflow
///          saturates the result at `INT_MAX`.
/// @param node Parent whose visible descendants are counted; may be `NULL`.
/// @return Visible row count, zero for `NULL`, or `INT_MAX` when saturated.
static int count_visible_nodes(vg_tree_node_t *node) {
    if (!node)
        return 0;

    tree_node_stack_t stack = {0};
    for (vg_tree_node_t *child = node->first_child; child; child = child->next_sibling) {
        if (!tree_node_stack_push(&stack, child)) {
            tree_node_stack_destroy(&stack);
            return INT_MAX;
        }
    }

    int count = 0;
    while ((node = tree_node_stack_pop(&stack)) != NULL) {
        if (count < INT_MAX)
            count++;
        if (node->expanded) {
            for (vg_tree_node_t *child = node->first_child; child; child = child->next_sibling) {
                if (!tree_node_stack_push(&stack, child)) {
                    tree_node_stack_destroy(&stack);
                    return INT_MAX;
                }
            }
        }
    }
    tree_node_stack_destroy(&stack);
    return count;
}

/// @brief Resolve a flattened visible-row index to its retained node.
/// @details Iterative preorder traversal descends only through expanded nodes
///          and advances the caller-provided running index.
/// @param root Sentinel or subtree parent whose children define the row domain.
/// @param target_index Zero-based flattened visible index.
/// @param current In/out traversal index initialized by the caller.
/// @return Borrowed node at @p target_index, or `NULL` when out of range or
///         traversal storage cannot grow.
static vg_tree_node_t *get_node_at_index(vg_tree_node_t *root, int target_index, int *current) {
    if (!root || !current || target_index < 0)
        return NULL;

    tree_node_stack_t stack = {0};
    for (vg_tree_node_t *child = root->last_child; child; child = child->prev_sibling) {
        if (!tree_node_stack_push(&stack, child)) {
            tree_node_stack_destroy(&stack);
            return NULL;
        }
    }

    vg_tree_node_t *node = NULL;
    while ((node = tree_node_stack_pop(&stack)) != NULL) {
        if (*current == target_index) {
            tree_node_stack_destroy(&stack);
            return node;
        }
        if (*current < INT_MAX)
            (*current)++;
        if (node->expanded) {
            for (vg_tree_node_t *child = node->last_child; child; child = child->prev_sibling) {
                if (!tree_node_stack_push(&stack, child)) {
                    tree_node_stack_destroy(&stack);
                    return NULL;
                }
            }
        }
    }
    tree_node_stack_destroy(&stack);
    return NULL;
}

/// @brief Resolve a retained node to its flattened visible-row index.
/// @details Iterative preorder traversal descends only through expanded nodes
///          and advances the caller-provided running index.
/// @param root Sentinel or subtree parent whose children define the row domain.
/// @param target Exact node pointer to locate.
/// @param current In/out traversal index initialized by the caller.
/// @return Zero-based visible index, or `-1` when the target is hidden, absent,
///         or traversal storage cannot grow.
static int get_node_index(vg_tree_node_t *root, vg_tree_node_t *target, int *current) {
    if (!root || !target || !current)
        return -1;

    tree_node_stack_t stack = {0};
    for (vg_tree_node_t *child = root->last_child; child; child = child->prev_sibling) {
        if (!tree_node_stack_push(&stack, child)) {
            tree_node_stack_destroy(&stack);
            return -1;
        }
    }

    vg_tree_node_t *node = NULL;
    while ((node = tree_node_stack_pop(&stack)) != NULL) {
        if (node == target) {
            int found = *current;
            tree_node_stack_destroy(&stack);
            return found;
        }
        if (*current < INT_MAX)
            (*current)++;
        if (node->expanded) {
            for (vg_tree_node_t *child = node->last_child; child; child = child->prev_sibling) {
                if (!tree_node_stack_push(&stack, child)) {
                    tree_node_stack_destroy(&stack);
                    return -1;
                }
            }
        }
    }
    tree_node_stack_destroy(&stack);
    return -1;
}

/// @brief Record one TreeView selection transition in both event domains.
/// @details The selection-specific compatibility counter remains independent from the common
///          `WasChanged` edge. Both saturate rather than wrap, and the common helper also advances
///          the non-consuming widget revision.
/// @param tree Tree whose logical selection changed; may be NULL.
static void treeview_note_selection_changed(vg_treeview_t *tree) {
    if (!tree)
        return;
    if (tree->selection_revision < UINT64_MAX)
        tree->selection_revision++;
    vg_widget_note_change(&tree->base);
}

/// @brief Clear and invoke one external virtual-model detach callback.
/// @details Callback fields are cleared before invocation, preventing duplicate notifications if
///          model cleanup indirectly enters another TreeView cleanup path.
/// @param tree TreeView being detached; NULL is ignored.
static void treeview_notify_virtual_unbound(vg_treeview_t *tree) {
    if (!tree)
        return;
    vg_treeview_virtual_unbind_callback_t callback = tree->virtual_unbind;
    void *user_data = tree->virtual_model_user_data;
    tree->virtual_unbind = NULL;
    if (callback)
        callback(tree, user_data);
}

/// @brief Test whether a retained node belongs to a subtree.
/// @param root Candidate subtree root.
/// @param candidate Node whose parent chain is inspected.
/// @return `true` when @p candidate is @p root or one of its descendants.
static bool node_in_subtree(const vg_tree_node_t *root, const vg_tree_node_t *candidate) {
    if (!root || !candidate)
        return false;

    for (const vg_tree_node_t *node = candidate; node; node = node->parent) {
        if (node == root)
            return true;
    }
    return false;
}

/// @brief Advance through retained preorder, optionally skipping collapsed descendants.
/// @details Parent links keep traversal iterative and allocation-free for arbitrarily deep trees.
/// @param root Boundary node that traversal must not leave.
/// @param node Current node.
/// @param visible_only When `true`, do not descend into collapsed nodes.
/// @return Next node in preorder, or `NULL` at the subtree boundary.
static vg_tree_node_t *treeview_next_retained(vg_tree_node_t *root,
                                              vg_tree_node_t *node,
                                              bool visible_only) {
    if (!root || !node)
        return NULL;
    if (node->first_child && (!visible_only || node->expanded))
        return node->first_child;
    while (node && node != root) {
        if (node->next_sibling)
            return node->next_sibling;
        node = node->parent;
    }
    return NULL;
}

/// @brief Clear retained selection flags except for an optional node.
/// @param tree TreeView whose complete retained hierarchy is scanned.
/// @param keep Node whose selected flag is preserved; may be `NULL`.
/// @return `true` when at least one selection flag changed.
static bool treeview_clear_retained_selection(vg_treeview_t *tree, vg_tree_node_t *keep) {
    if (!tree || !tree->root)
        return false;
    bool changed = false;
    for (vg_tree_node_t *node = tree->root->first_child; node;
         node = treeview_next_retained(tree->root, node, false)) {
        if (node != keep && node->selected) {
            node->selected = false;
            changed = true;
        }
    }
    return changed;
}

/// @brief Find the first selected node in complete retained preorder.
/// @param tree TreeView to scan.
/// @return Borrowed selected node pointer, or `NULL` when none is selected.
static vg_tree_node_t *treeview_first_selected(vg_treeview_t *tree) {
    if (!tree || !tree->root)
        return NULL;
    for (vg_tree_node_t *node = tree->root->first_child; node;
         node = treeview_next_retained(tree->root, node, false)) {
        if (node->selected)
            return node;
    }
    return NULL;
}

/// @brief Determine whether a retained subtree contains a selection.
/// @param root Subtree root to inspect; may be `NULL`.
/// @return `true` when @p root or one of its descendants is selected.
static bool treeview_subtree_has_selection(vg_tree_node_t *root) {
    if (!root)
        return false;
    for (vg_tree_node_t *node = root; node; node = treeview_next_retained(root, node, false)) {
        if (node->selected)
            return true;
    }
    return false;
}

/// @brief Publish a retained selection transition and preserve primary visibility.
/// @details A real change advances revision edges, scrolls the primary
///          selection into view or clamps existing scroll, requests repaint,
///          and notifies the registered selection callback.
/// @param tree TreeView whose selection state was evaluated.
/// @param changed Whether the operation changed logical selection state.
static void treeview_publish_retained_selection(vg_treeview_t *tree, bool changed) {
    if (!tree || !changed)
        return;
    treeview_note_selection_changed(tree);
    if (tree->selected)
        vg_treeview_scroll_to(tree, tree->selected);
    else
        treeview_clamp_scroll(tree);
    tree->base.needs_paint = true;
    if (tree->on_select && tree->selected)
        tree->on_select(&tree->base, tree->selected, tree->on_select_data);
}

/// @brief Apply interactive single, toggle, or visible-range selection.
/// @details Single selection clears peers and establishes a new range anchor.
///          Toggle selection independently flips one node in multi-select mode.
///          Range selection spans flattened visible preorder between the
///          retained anchor and target.
/// @param tree TreeView expected to own @p node.
/// @param node Live target node.
/// @param toggle Whether modifier input requests independent toggle semantics.
/// @param range Whether modifier input requests anchor-to-target range
///        selection.
static void treeview_select_interactive(vg_treeview_t *tree,
                                        vg_tree_node_t *node,
                                        bool toggle,
                                        bool range) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree)
        return;

    vg_tree_node_t *old_primary = tree->selected;
    bool changed = false;
    if (!tree->multi_select || (!toggle && !range)) {
        changed = treeview_clear_retained_selection(tree, node);
        changed = changed || !node->selected || tree->selected != node;
        node->selected = true;
        tree->selected = node;
        tree->anchor_selected = node;
        treeview_publish_retained_selection(tree, changed);
        return;
    }

    if (range) {
        vg_tree_node_t *anchor = tree->anchor_selected &&
                                         vg_tree_node_is_live(tree->anchor_selected) &&
                                         tree->anchor_selected->owner == tree
                                     ? tree->anchor_selected
                                     : (tree->selected ? tree->selected : node);
        int current = 0;
        int anchor_index = get_node_index(tree->root, anchor, &current);
        current = 0;
        int target_index = get_node_index(tree->root, node, &current);
        if (anchor_index < 0 || target_index < 0) {
            changed = treeview_clear_retained_selection(tree, node);
            changed = changed || !node->selected || tree->selected != node;
            node->selected = true;
            tree->selected = node;
            tree->anchor_selected = node;
            treeview_publish_retained_selection(tree, changed);
            return;
        }

        changed = treeview_clear_retained_selection(tree, NULL);
        int start = anchor_index < target_index ? anchor_index : target_index;
        int end = anchor_index > target_index ? anchor_index : target_index;
        int index = 0;
        for (vg_tree_node_t *candidate = tree->root->first_child; candidate;
             candidate = treeview_next_retained(tree->root, candidate, true), ++index) {
            if (index >= start && index <= end) {
                changed = changed || !candidate->selected;
                candidate->selected = true;
            }
            if (index >= end)
                break;
        }
        tree->selected = node;
        changed = changed || old_primary != node;
        treeview_publish_retained_selection(tree, changed);
        return;
    }

    bool was_selected = node->selected;
    node->selected = !node->selected;
    if (node->selected) {
        tree->selected = node;
        tree->anchor_selected = node;
    } else {
        if (tree->selected == node)
            tree->selected = treeview_first_selected(tree);
        if (tree->anchor_selected == node)
            tree->anchor_selected = tree->selected;
    }
    changed = was_selected != node->selected || old_primary != tree->selected;
    treeview_publish_retained_selection(tree, changed);
}

/// @brief Compute total flattened content height without floating-point overflow.
/// @param tree TreeView whose retained or virtual row domain is measured.
/// @return Nonnegative content height, saturated at `FLT_MAX`.
static float treeview_content_height(const vg_treeview_t *tree) {
    if (!tree)
        return 0.0f;
    size_t visible =
        tree->virtual_mode ? tree->virtual_row_count : (size_t)count_visible_nodes(tree->root);
    double content_height = (double)visible * (double)tree->row_height;
    if (!isfinite(content_height) || content_height > (double)FLT_MAX)
        return FLT_MAX;
    if (content_height < 0.0)
        return 0.0f;
    return (float)content_height;
}

/// @brief Compute the largest valid vertical scroll offset.
/// @param tree TreeView supplying content and viewport heights.
/// @return Nonnegative maximum offset, saturated at `FLT_MAX`.
static float treeview_max_scroll(const vg_treeview_t *tree) {
    if (!tree)
        return 0.0f;
    double max_scroll_double = (double)treeview_content_height(tree) - (double)tree->base.height;
    float max_scroll = max_scroll_double > (double)FLT_MAX ? FLT_MAX : (float)max_scroll_double;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    return max_scroll;
}

/// @brief Clamp a TreeView's vertical scroll offset to its current content.
/// @param tree TreeView whose `scroll_y` is normalized; `NULL` is ignored.
static void treeview_clamp_scroll(vg_treeview_t *tree) {
    if (!tree)
        return;
    if (!isfinite(tree->scroll_y) || tree->scroll_y < 0.0f)
        tree->scroll_y = 0.0f;
    float max_scroll = treeview_max_scroll(tree);
    if (tree->scroll_y > max_scroll)
        tree->scroll_y = max_scroll;
}

/// @brief Compute the visible vertical scrollbar and thumb geometry.
/// @param tree TreeView supplying viewport, content, and scroll state.
/// @param out_width Optional destination for the scrollbar gutter width.
/// @param out_thumb_y Optional destination for the thumb's local top edge.
/// @param out_thumb_height Optional destination for the thumb height.
/// @return `true` only when content overflows the current viewport.
static bool treeview_scrollbar_geometry(const vg_treeview_t *tree,
                                        float *out_width,
                                        float *out_thumb_y,
                                        float *out_thumb_height) {
    if (!tree || tree->base.width <= 0.0f || tree->base.height <= 0.0f)
        return false;
    float content_height = treeview_content_height(tree);
    float viewport_height = tree->base.height;
    if (content_height <= viewport_height)
        return false;

    vg_theme_t *theme = vg_theme_get_current();
    float width = theme ? theme->scrollbar.width : 10.0f;
    if (!isfinite(width) || width <= 0.0f)
        width = 10.0f;
    if (width > tree->base.width)
        width = tree->base.width;

    float thumb_height = viewport_height * (viewport_height / content_height);
    float min_thumb = theme ? theme->scrollbar.min_thumb_size : 24.0f;
    if (!isfinite(min_thumb) || min_thumb < 1.0f)
        min_thumb = 1.0f;
    if (thumb_height < min_thumb)
        thumb_height = min_thumb;
    if (thumb_height > viewport_height)
        thumb_height = viewport_height;

    float travel = viewport_height - thumb_height;
    float max_scroll = treeview_max_scroll(tree);
    float thumb_y =
        (travel > 0.0f && max_scroll > 0.0f) ? (tree->scroll_y / max_scroll) * travel : 0.0f;
    if (thumb_y < 0.0f)
        thumb_y = 0.0f;
    if (thumb_y > travel)
        thumb_y = travel;

    if (out_width)
        *out_width = width;
    if (out_thumb_y)
        *out_thumb_y = thumb_y;
    if (out_thumb_height)
        *out_thumb_height = thumb_height;
    return true;
}

/// @brief Handle scrollbar hover, track clicks, thumb dragging, and click suppression.
/// @details Mouse coordinates are widget-local on the event path. This handler runs before
/// retained/virtual row handling so the scrollbar gutter never selects or activates a file row.
/// @param tree TreeView whose scrollbar state is updated.
/// @param event Candidate pointer event.
/// @return `true` when scrollbar interaction consumes the event.
static bool treeview_handle_scrollbar_event(vg_treeview_t *tree, vg_event_t *event) {
    if (!tree || !event)
        return false;
    if (event->type != VG_EVENT_MOUSE_DOWN && event->type != VG_EVENT_MOUSE_UP &&
        event->type != VG_EVENT_MOUSE_MOVE && event->type != VG_EVENT_MOUSE_LEAVE &&
        event->type != VG_EVENT_CLICK && event->type != VG_EVENT_DOUBLE_CLICK) {
        return false;
    }

    float width = 0.0f;
    float thumb_y = 0.0f;
    float thumb_height = 0.0f;
    bool visible = treeview_scrollbar_geometry(tree, &width, &thumb_y, &thumb_height);
    float gutter_left = tree->base.width - width;
    bool in_gutter = visible && event->mouse.x >= gutter_left &&
                     event->mouse.x < tree->base.width && event->mouse.y >= 0.0f &&
                     event->mouse.y < tree->base.height;

    if (!visible) {
        bool was_scrollbar_dragging = tree->scrollbar_dragging;
        bool changed = tree->scrollbar_hovered || tree->scrollbar_dragging;
        tree->scrollbar_hovered = false;
        tree->scrollbar_dragging = false;
        tree->scrollbar_suppress_click = false;
        // TreeView drag-and-drop and the scrollbar both capture through the
        // widget handle. Only tear capture down when an active thumb drag lost
        // its scrollbar (for example after content collapsed). Otherwise this
        // move event is the one that must promote a pressed file into a drag.
        if (was_scrollbar_dragging && vg_widget_get_input_capture() == &tree->base)
            vg_widget_release_input_capture();
        if (changed)
            tree->base.needs_paint = true;
        return false;
    }

    if (event->type == VG_EVENT_MOUSE_DOWN) {
        if (!in_gutter || event->mouse.button != VG_MOUSE_LEFT) {
            tree->scrollbar_suppress_click = false;
            return false;
        }
        tree->scrollbar_suppress_click = true;
        if (event->mouse.y >= thumb_y && event->mouse.y < thumb_y + thumb_height) {
            tree->scrollbar_dragging = true;
            tree->scrollbar_drag_offset = event->mouse.y - thumb_y;
            vg_widget_set_input_capture(&tree->base);
        } else {
            float travel = tree->base.height - thumb_height;
            float target = event->mouse.y - thumb_height * 0.5f;
            if (target < 0.0f)
                target = 0.0f;
            if (target > travel)
                target = travel;
            tree->scroll_y = travel > 0.0f ? (target / travel) * treeview_max_scroll(tree) : 0.0f;
            treeview_clamp_scroll(tree);
        }
        tree->base.needs_paint = true;
        return true;
    }

    if (event->type == VG_EVENT_MOUSE_MOVE) {
        if (tree->scrollbar_dragging) {
            float travel = tree->base.height - thumb_height;
            float target = event->mouse.y - tree->scrollbar_drag_offset;
            if (target < 0.0f)
                target = 0.0f;
            if (target > travel)
                target = travel;
            tree->scroll_y = travel > 0.0f ? (target / travel) * treeview_max_scroll(tree) : 0.0f;
            treeview_clamp_scroll(tree);
            tree->base.needs_paint = true;
            return true;
        }
        if (tree->scrollbar_hovered != in_gutter) {
            tree->scrollbar_hovered = in_gutter;
            tree->base.needs_paint = true;
        }
        if (in_gutter) {
            tree->hovered = NULL;
            tree->virtual_hovered_index = SIZE_MAX;
            return true;
        }
        return false;
    }

    if (event->type == VG_EVENT_MOUSE_UP && tree->scrollbar_dragging) {
        tree->scrollbar_dragging = false;
        if (vg_widget_get_input_capture() == &tree->base)
            vg_widget_release_input_capture();
        tree->base.needs_paint = true;
        return true;
    }

    if (event->type == VG_EVENT_CLICK) {
        if (tree->scrollbar_suppress_click || in_gutter) {
            tree->scrollbar_suppress_click = false;
            return true;
        }
        return false;
    }

    if (event->type == VG_EVENT_DOUBLE_CLICK && in_gutter)
        return true;

    if (event->type == VG_EVENT_MOUSE_LEAVE && !tree->scrollbar_dragging &&
        tree->scrollbar_hovered) {
        tree->scrollbar_hovered = false;
        tree->base.needs_paint = true;
    }
    return false;
}

/// @brief Compute the bounded virtual-row range needed for the viewport.
/// @details The range begins at the row intersecting `scroll_y`, includes two
///          rows of overscan, never exceeds the model suffix, and is capped at
///          4096 descriptors per paint.
/// @param tree Virtual-mode TreeView supplying row and viewport metrics.
/// @param out_start Optional destination for the first flattened row index.
/// @param out_count Optional destination for the number of rows to materialize.
static void treeview_compute_virtual_range(vg_treeview_t *tree,
                                           size_t *out_start,
                                           size_t *out_count) {
    size_t start = 0;
    size_t count = 0;
    if (tree && tree->virtual_mode && tree->virtual_row_count > 0 && tree->row_height > 0.0f &&
        isfinite(tree->row_height)) {
        treeview_clamp_scroll(tree);
        start = (size_t)(tree->scroll_y / tree->row_height);
        if (start >= tree->virtual_row_count)
            start = tree->virtual_row_count - 1u;
        double rows =
            tree->base.height > 0.0f ? (double)tree->base.height / (double)tree->row_height : 0.0;
        count = rows > 4094.0 ? 4096u : (size_t)rows + 2u;
        if (count > tree->virtual_row_count - start)
            count = tree->virtual_row_count - start;
        if (count > 4096u)
            count = 4096u;
    }
    if (out_start)
        *out_start = start;
    if (out_count)
        *out_count = count;
}

/// @brief Convert a viewport-local Y coordinate to a virtual row index.
/// @param tree Virtual-mode TreeView supplying scroll and row metrics.
/// @param local_y Y coordinate relative to the widget's top edge.
/// @param out_index Destination for the resolved flattened row index.
/// @return `true` when the coordinate maps to a model row.
static bool treeview_virtual_row_at_y(vg_treeview_t *tree, float local_y, size_t *out_index) {
    if (!tree || !tree->virtual_mode || !out_index || tree->row_height <= 0.0f ||
        !isfinite(local_y))
        return false;
    float content_y = local_y + tree->scroll_y;
    if (content_y < 0.0f)
        return false;
    size_t index = (size_t)(content_y / tree->row_height);
    if (index >= tree->virtual_row_count)
        return false;
    *out_index = index;
    return true;
}

/// @brief Scroll minimally to make a virtual row fully visible.
/// @param tree Virtual-mode TreeView whose scroll position is updated.
/// @param index Flattened row index to reveal.
static void treeview_scroll_virtual_to(vg_treeview_t *tree, size_t index) {
    if (!tree || !tree->virtual_mode || index >= tree->virtual_row_count)
        return;
    double row_top_double = (double)index * (double)tree->row_height;
    float row_top = row_top_double > (double)FLT_MAX ? FLT_MAX : (float)row_top_double;
    if (row_top < tree->scroll_y)
        tree->scroll_y = row_top;
    else if (row_top + tree->row_height > tree->scroll_y + tree->base.height)
        tree->scroll_y = row_top + tree->row_height - tree->base.height;
    treeview_clamp_scroll(tree);
}

/// @brief Obtain a positive UI scale for TreeView geometry.
/// @return Current theme scale when positive, otherwise `1.0`.
static float treeview_scale(void) {
    vg_theme_t *theme = vg_theme_get_current();
    return (theme && theme->ui_scale > 0.0f) ? theme->ui_scale : 1.0f;
}

/// @brief Compute the scaled horizontal inset around row content.
/// @return Ten base pixels multiplied by the current UI scale.
static float treeview_outer_padding(void) {
    return 10.0f * treeview_scale();
}

/// @brief Recompute scaled row metrics from theme and font configuration.
/// @details Row height is at least 28 scaled pixels and grows when the selected
///          font's line height plus vertical padding requires more space.
/// @param tree TreeView whose metric fields are updated; `NULL` is ignored.
static void treeview_sync_metrics(vg_treeview_t *tree) {
    if (!tree)
        return;

    float scale = treeview_scale();
    tree->indent_size = 18.0f * scale;
    tree->icon_size = 16.0f * scale;
    tree->icon_gap = 8.0f * scale;

    float row_height = 28.0f * scale;
    if (tree->font) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(tree->font, tree->font_size, &metrics);
        float metrics_height = (float)metrics.line_height + 8.0f * scale;
        if (metrics_height > row_height)
            row_height = metrics_height;
    }
    tree->row_height = row_height;
}

/// @brief Compute a vertically centered text baseline for one row.
/// @param tree TreeView supplying font metrics and row height.
/// @param row_y Row's top edge in the same coordinate space as the result.
/// @return Baseline Y coordinate, or @p row_y when no font is configured.
static float treeview_text_baseline(vg_treeview_t *tree, float row_y) {
    if (!tree || !tree->font)
        return row_y;

    vg_font_metrics_t metrics = {0};
    vg_font_get_metrics(tree->font, tree->font_size, &metrics);
    return row_y + (tree->row_height + (float)metrics.ascent + (float)metrics.descent) / 2.0f;
}

/// @brief Create a width-constrained display copy of row text.
/// @details Text that already fits is duplicated unchanged. Otherwise bytes are
///          removed from the end until an ASCII ellipsis fits; this helper
///          measures byte prefixes and does not itself enforce UTF-8 boundary
///          truncation.
/// @param tree TreeView supplying font and size.
/// @param text Source label; `NULL` is treated as empty.
/// @param max_width Maximum measured width in layout units.
/// @return Newly allocated fitted text owned by the caller, or `NULL` on
///         allocation failure.
static char *treeview_fit_text(vg_treeview_t *tree, const char *text, float max_width) {
    if (!text)
        return vg_strdup("");
    if (!tree->font || max_width <= 0.0f)
        return vg_strdup("");

    vg_text_metrics_t metrics = {0};
    vg_font_measure_text(tree->font, tree->font_size, text, &metrics);
    if (metrics.width <= max_width)
        return vg_strdup(text);

    vg_text_metrics_t ellipsis_metrics = {0};
    vg_font_measure_text(tree->font, tree->font_size, "...", &ellipsis_metrics);
    if (ellipsis_metrics.width > max_width)
        return vg_strdup("");

    size_t len = strlen(text);
    char *buf = (char *)malloc(len + 4);
    if (!buf)
        return NULL;

    while (len > 0) {
        memcpy(buf, text, len);
        memcpy(buf + len, "...", 4);
        vg_font_measure_text(tree->font, tree->font_size, buf, &metrics);
        if (metrics.width <= max_width)
            return buf;
        len--;
    }

    memcpy(buf, "...", 4);
    return buf;
}

/// @brief Encode a codepoint as a null-terminated UTF-8 byte sequence.
/// @param codepoint Value to encode.
/// @param out Eight-byte output buffer; `NULL` is ignored.
static void treeview_encode_glyph(uint32_t codepoint, char out[8]) {
    if (!out)
        return;

    memset(out, 0, 8);
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
    } else if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
    }
}

/// @brief Paint a retained node's icon within its row slot.
/// @details Loading dots take precedence, followed by UTF-8 icon text, expanded
///          or base vector/glyph icons, and finally a neutral fallback dot.
/// @param tree TreeView supplying font and icon metrics.
/// @param canvas Destination graphics canvas.
/// @param node Retained node whose icon state is rendered.
/// @param icon_x Left edge of the icon slot.
/// @param row_y Top edge of the row.
/// @param color Foreground color for icon content.
static void treeview_paint_icon(vg_treeview_t *tree,
                                void *canvas,
                                vg_tree_node_t *node,
                                float icon_x,
                                float row_y,
                                uint32_t color) {
    if (!tree || !node)
        return;

    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    float icon_center_y = row_y + tree->row_height * 0.5f;
    vg_icon_t icon = node->icon;
    if (node->expanded && node->expanded_icon.type != VG_ICON_NONE)
        icon = node->expanded_icon;

    if (node->loading) {
        int32_t dot_r = (int32_t)(tree->icon_size * 0.12f);
        if (dot_r < 1)
            dot_r = 1;
        int32_t cy = (int32_t)icon_center_y;
        int32_t cx = (int32_t)(icon_x + tree->icon_size * 0.5f);
        int32_t gap = (int32_t)(tree->icon_size * 0.28f);
        vgfx_fill_circle(win, cx - gap, cy, dot_r, color);
        vgfx_fill_circle(win, cx, cy, dot_r, vg_color_lighten(color, 0.08f));
        vgfx_fill_circle(win, cx + gap, cy, dot_r, color);
        return;
    }

    if (node->icon_text && node->icon_text[0] != '\0' && tree->font) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(tree->font, tree->icon_size, &metrics);
        vg_font_draw_text(
            canvas,
            tree->font,
            tree->icon_size,
            icon_x,
            row_y + (tree->row_height + (float)metrics.ascent + (float)metrics.descent) * 0.5f,
            node->icon_text,
            color);
        return;
    }

    if (icon.type == VG_ICON_VECTOR) {
        float icon_y = icon_center_y - tree->icon_size * 0.5f;
        vg_icon_vector_draw(win,
                            icon.data.vector_id,
                            (int32_t)(icon_x + 0.5f),
                            (int32_t)(icon_y + 0.5f),
                            (int32_t)(tree->icon_size + 0.5f),
                            color);
        return;
    }

    if (icon.type == VG_ICON_GLYPH && tree->font) {
        char glyph[8];
        vg_font_metrics_t metrics = {0};
        treeview_encode_glyph(icon.data.glyph, glyph);
        vg_font_get_metrics(tree->font, tree->icon_size, &metrics);
        vg_font_draw_text(
            canvas,
            tree->font,
            tree->icon_size,
            icon_x,
            row_y + (tree->row_height + (float)metrics.ascent + (float)metrics.descent) * 0.5f,
            glyph,
            color);
        return;
    }

    vgfx_fill_circle(win,
                     (int32_t)(icon_x + tree->icon_size * 0.5f),
                     (int32_t)icon_center_y,
                     (int32_t)(tree->icon_size * 0.18f),
                     vg_color_blend(color, theme->colors.bg_primary, 0.15f));
}

//=============================================================================
// TreeView Implementation
//=============================================================================

/// @brief Create a tree view widget, optionally as a child of parent.
///
/// @details Allocates a vg_treeview_t, initialises a hidden sentinel root node
///          (depth = -1, always expanded), seeds metrics from the current theme,
///          and adds the widget to parent if non-NULL. The sentinel and all
///          subsequently added retained nodes are owned by the TreeView.
///
/// @param parent Optional parent widget to attach to; may be NULL.
/// @return       Heap-allocated tree view, or NULL on allocation failure.
vg_treeview_t *vg_treeview_create(vg_widget_t *parent) {
    vg_treeview_t *tree = calloc(1, sizeof(vg_treeview_t));
    if (!tree)
        return NULL;

    // Initialize base widget
    vg_widget_init(&tree->base, VG_WIDGET_TREEVIEW, &g_treeview_vtable);

    // Create root node
    tree->root = calloc(1, sizeof(vg_tree_node_t));
    if (!tree->root) {
        vg_widget_destroy(&tree->base);
        return NULL;
    }
    tree->root->expanded = true; // Root is always expanded
    tree->root->depth = -1;
    tree->root->magic = VG_TREE_NODE_MAGIC;
    tree->root->owner = tree;

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize treeview-specific fields
    tree->selected = NULL;
    tree->font = theme->typography.font_regular;
    tree->font_size = theme->typography.size_normal;

    // Appearance
    treeview_sync_metrics(tree);
    tree->text_color = theme->colors.fg_primary;
    tree->selected_bg = theme->colors.bg_selected;
    tree->hover_bg = theme->colors.bg_hover;

    // Scrolling
    tree->scroll_y = 0;
    tree->visible_start = 0;
    tree->visible_count = 0;
    tree->scrollbar_hovered = false;
    tree->scrollbar_dragging = false;
    tree->scrollbar_suppress_click = false;
    tree->scrollbar_drag_offset = 0.0f;
    tree->virtual_selected_index = SIZE_MAX;
    tree->virtual_hovered_index = SIZE_MAX;

    // Callbacks
    tree->on_select = NULL;
    tree->on_select_data = NULL;
    tree->on_expand = NULL;
    tree->on_expand_data = NULL;
    tree->on_activate = NULL;
    tree->on_activate_data = NULL;

    // State
    tree->hovered = NULL;

    // Set minimum size
    tree->base.constraints.min_width = 100.0f;
    tree->base.constraints.min_height = 100.0f;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &tree->base);
    }

    return tree;
}

/// @brief Release all resources owned by a TreeView.
/// @details The virtual model receives its single detach notification before
///          both the live retained hierarchy and deferred tombstone subtrees
///          are destroyed.
/// @param widget TreeView base widget being destroyed.
static void treeview_destroy(vg_widget_t *widget) {
    vg_treeview_t *tree = (vg_treeview_t *)widget;

    treeview_notify_virtual_unbound(tree);

    // The editor child widget is destroyed by the widget tree; only the
    // latched commit text is owned directly here.
    free(tree->edit_text);
    tree->edit_text = NULL;
    tree->edit_node = NULL;
    tree->edit_committed_node = NULL;
    tree->edit_input = NULL;

    if (tree->root) {
        free_node(tree->root);
        tree->root = NULL;
    }
    free_retired_nodes(tree);
}

/// @brief Measure a TreeView against available viewport dimensions.
/// @details Width fills a positive available constraint or defaults to 200.
///          Retained mode uses all visible rows for intrinsic height, while
///          virtual mode caps intrinsic height at five rows to avoid requesting
///          model-sized geometry. Widget minimum constraints are applied last.
/// @param widget TreeView base widget receiving measured dimensions.
/// @param available_width Width constraint from the parent.
/// @param available_height Height constraint from the parent.
static void treeview_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_treeview_t *tree = (vg_treeview_t *)widget;

    size_t visible =
        tree->virtual_mode ? tree->virtual_row_count : (size_t)count_visible_nodes(tree->root);
    size_t measured_rows = tree->virtual_mode && visible > 5u ? 5u : visible;
    float content_height = (float)measured_rows * tree->row_height;

    widget->measured_width = available_width > 0 ? available_width : 200;
    widget->measured_height = content_height;

    if (widget->measured_height < available_height && available_height > 0) {
        widget->measured_height = available_height;
    }

    // Apply constraints
    if (widget->measured_width < widget->constraints.min_width) {
        widget->measured_width = widget->constraints.min_width;
    }
    if (widget->measured_height < widget->constraints.min_height) {
        widget->measured_height = widget->constraints.min_height;
    }
}

/// @brief Paint visible retained rows beneath a hierarchy node.
/// @details The traversal advances content-space Y for every flattened visible
///          row, clips painting by viewport tests, and recursively descends only
///          into expanded nodes. Row rendering includes zebra backgrounds,
///          selection/hover treatment, disclosure arrows, icons, fitted text,
///          and drag-target indicators.
/// @param tree TreeView supplying state and presentation metrics.
/// @param canvas Destination graphics canvas.
/// @param node Parent whose child rows are traversed.
/// @param x Base row-content X coordinate.
/// @param y In/out content-space row cursor.
/// @param width Paintable row width excluding any scrollbar gutter.
static void paint_node(
    vg_treeview_t *tree, void *canvas, vg_tree_node_t *node, float x, float *y, float width) {
    vg_theme_t *theme = vg_theme_get_current();
    float outer_padding = treeview_outer_padding();

    for (vg_tree_node_t *child = node->first_child; child; child = child->next_sibling) {
        float row_y = *y;
        int row_index = (int)(row_y / tree->row_height);

        // Check if visible
        if (row_y + tree->row_height >= tree->scroll_y &&
            row_y < tree->scroll_y + tree->base.height) {
            float display_y = tree->base.y + row_y - tree->scroll_y;
            float arrow_slot_x = x + outer_padding + child->depth * tree->indent_size;
            float icon_x = arrow_slot_x + tree->indent_size;
            float text_x = icon_x + tree->icon_size + tree->icon_gap;
            float arrow_size = 8.0f * treeview_scale();
            uint32_t zebra_bg =
                ((row_index & 1) == 0)
                    ? theme->colors.bg_primary
                    : vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.35f);
            uint32_t row_fg = tree->text_color;
            bool row_sel = child->selected;
            bool row_primary = child == tree->selected;
            bool row_hov = (child == tree->hovered);
            if (row_sel)
                row_fg = theme->colors.fg_primary;

            // Flat zebra base, then a modern rounded selection/hover pill inset
            // from the edges (replaces the old flat full-width highlight bar).
            vgfx_fill_rect((vgfx_window_t)canvas,
                           (int32_t)tree->base.x,
                           (int32_t)display_y,
                           (int32_t)width,
                           (int32_t)tree->row_height,
                           zebra_bg);
            if (row_sel || row_hov) {
                // Clearly inset, rounded "pill". Selected rows are brightened
                // toward the accent so the highlight reads at a glance.
                uint32_t pill =
                    row_sel ? vg_color_blend(tree->selected_bg, theme->colors.accent_primary, 0.28f)
                            : tree->hover_bg;
                vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                        tree->base.x + 8.0f,
                                        display_y + 2.0f,
                                        width - 16.0f,
                                        tree->row_height - 4.0f,
                                        theme->radius.lg,
                                        pill);
                if (row_primary)
                    vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                            tree->base.x + 11.0f,
                                            display_y + 6.0f,
                                            3.0f,
                                            tree->row_height - 12.0f,
                                            1.5f,
                                            theme->colors.accent_primary);
            }

            // Draw expand/collapse arrow if has children
            if (child->has_children || child->first_child) {
                int32_t ax = (int32_t)(arrow_slot_x + (tree->icon_size - arrow_size) * 0.5f);
                int32_t ay = (int32_t)(display_y + (tree->row_height - arrow_size) / 2.0f);
                uint32_t arrow_color =
                    child->selected ? theme->colors.fg_primary : theme->colors.fg_secondary;
                if (child->expanded) {
                    // ▼ downward triangle
                    vgfx_line(
                        (vgfx_window_t)canvas, ax, ay, ax + (int32_t)arrow_size, ay, arrow_color);
                    vgfx_line((vgfx_window_t)canvas,
                              ax,
                              ay,
                              ax + (int32_t)(arrow_size * 0.5f),
                              ay + (int32_t)(arrow_size * 0.75f),
                              arrow_color);
                    vgfx_line((vgfx_window_t)canvas,
                              ax + (int32_t)arrow_size,
                              ay,
                              ax + (int32_t)(arrow_size * 0.5f),
                              ay + (int32_t)(arrow_size * 0.75f),
                              arrow_color);
                } else {
                    // ▶ rightward triangle
                    vgfx_line(
                        (vgfx_window_t)canvas, ax, ay, ax, ay + (int32_t)arrow_size, arrow_color);
                    vgfx_line((vgfx_window_t)canvas,
                              ax,
                              ay,
                              ax + (int32_t)(arrow_size * 0.75f),
                              ay + (int32_t)(arrow_size * 0.5f),
                              arrow_color);
                    vgfx_line((vgfx_window_t)canvas,
                              ax,
                              ay + (int32_t)arrow_size,
                              ax + (int32_t)(arrow_size * 0.75f),
                              ay + (int32_t)(arrow_size * 0.5f),
                              arrow_color);
                }
            }

            treeview_paint_icon(tree,
                                canvas,
                                child,
                                icon_x,
                                display_y,
                                child->loading ? theme->colors.accent_primary : row_fg);

            // Draw text; the inline row editor covers the edited row's label.
            if (tree->font && child->text && !(tree->edit_active && child == tree->edit_node)) {
                float text_max_width =
                    tree->base.width - (text_x - tree->base.x) - treeview_outer_padding();
                if (text_max_width < 0.0f)
                    text_max_width = 0.0f;
                char *fit = treeview_fit_text(tree, child->text, text_max_width);
                vg_font_draw_text(canvas,
                                  tree->font,
                                  tree->font_size,
                                  text_x,
                                  treeview_text_baseline(tree, display_y),
                                  fit ? fit : child->text,
                                  row_fg);
                free(fit);
            }

            if (tree->is_dragging && child == tree->drop_target) {
                if (tree->drop_position == VG_TREE_DROP_INTO) {
                    vgfx_rect((vgfx_window_t)canvas,
                              (int32_t)tree->base.x + 2,
                              (int32_t)display_y + 2,
                              (int32_t)width - 4,
                              (int32_t)tree->row_height - 4,
                              theme->colors.accent_primary);
                } else {
                    int32_t line_y =
                        (int32_t)(display_y + (tree->drop_position == VG_TREE_DROP_BEFORE
                                                   ? 1.0f
                                                   : tree->row_height - 2.0f));
                    vgfx_fill_rect((vgfx_window_t)canvas,
                                   (int32_t)tree->base.x + 2,
                                   line_y,
                                   (int32_t)width - 4,
                                   2,
                                   theme->colors.accent_primary);
                }
            }
        }

        *y += tree->row_height;

        // Paint children if expanded
        if (child->expanded && child->first_child) {
            paint_node(tree, canvas, child, x, y, width);
        }
    }
}

/// @brief Paint only the flattened virtual-tree rows intersecting the current viewport.
/// @details Provider descriptors are borrowed and consumed synchronously. No per-model-row
///          allocation or retained node is created by this path.
/// @param tree Virtual-mode TreeView to paint.
/// @param canvas Destination graphics canvas.
static void treeview_paint_virtual(vg_treeview_t *tree, void *canvas) {
    if (!tree || !tree->virtual_mode)
        return;

    vg_theme_t *theme = vg_theme_get_current();
    size_t start = 0;
    size_t count = 0;
    treeview_compute_virtual_range(tree, &start, &count);
    tree->visible_start = start > (size_t)INT_MAX ? INT_MAX : (int)start;
    tree->visible_count = count > (size_t)INT_MAX ? INT_MAX : (int)count;
    float scrollbar_width = 0.0f;
    float content_width = tree->base.width;
    if (treeview_scrollbar_geometry(tree, &scrollbar_width, NULL, NULL))
        content_width -= scrollbar_width;
    if (content_width < 0.0f)
        content_width = 0.0f;

    for (size_t offset = 0; offset < count; offset++) {
        size_t index = start + offset;
        vg_treeview_virtual_row_t row = {0};
        bool found = tree->virtual_provider &&
                     tree->virtual_provider(tree, index, &row, tree->virtual_model_user_data);
        if (!found)
            row.text = "";

        float content_y = (float)((double)index * (double)tree->row_height);
        float display_y = tree->base.y + content_y - tree->scroll_y;
        if (display_y + tree->row_height <= tree->base.y ||
            display_y >= tree->base.y + tree->base.height)
            continue;

        uint32_t zebra =
            ((index & 1u) == 0u)
                ? theme->colors.bg_primary
                : vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.35f);
        bool selected = index == tree->virtual_selected_index;
        bool hovered = index == tree->virtual_hovered_index;
        uint32_t foreground = selected ? theme->colors.fg_primary : tree->text_color;
        vgfx_fill_rect((vgfx_window_t)canvas,
                       (int32_t)tree->base.x,
                       (int32_t)display_y,
                       (int32_t)content_width,
                       (int32_t)tree->row_height,
                       zebra);
        if (selected || hovered) {
            uint32_t pill =
                selected ? vg_color_blend(tree->selected_bg, theme->colors.accent_primary, 0.28f)
                         : tree->hover_bg;
            vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                    tree->base.x + 8.0f,
                                    display_y + 2.0f,
                                    content_width - 16.0f,
                                    tree->row_height - 4.0f,
                                    theme->radius.lg,
                                    pill);
            if (selected)
                vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                        tree->base.x + 11.0f,
                                        display_y + 6.0f,
                                        3.0f,
                                        tree->row_height - 12.0f,
                                        1.5f,
                                        theme->colors.accent_primary);
        }

        float depth = row.depth > (size_t)INT_MAX ? (float)INT_MAX : (float)row.depth;
        float arrow_x = tree->base.x + treeview_outer_padding() + depth * tree->indent_size;
        float icon_x = arrow_x + tree->indent_size;
        float text_x = icon_x + tree->icon_size + tree->icon_gap;
        float arrow_size = 8.0f * treeview_scale();
        if (row.has_children) {
            int32_t ax = (int32_t)(arrow_x + (tree->icon_size - arrow_size) * 0.5f);
            int32_t ay = (int32_t)(display_y + (tree->row_height - arrow_size) * 0.5f);
            uint32_t arrow_color = selected ? theme->colors.fg_primary : theme->colors.fg_secondary;
            if (row.expanded) {
                vgfx_line((vgfx_window_t)canvas, ax, ay, ax + (int32_t)arrow_size, ay, arrow_color);
                vgfx_line((vgfx_window_t)canvas,
                          ax,
                          ay,
                          ax + (int32_t)(arrow_size * 0.5f),
                          ay + (int32_t)(arrow_size * 0.75f),
                          arrow_color);
                vgfx_line((vgfx_window_t)canvas,
                          ax + (int32_t)arrow_size,
                          ay,
                          ax + (int32_t)(arrow_size * 0.5f),
                          ay + (int32_t)(arrow_size * 0.75f),
                          arrow_color);
            } else {
                vgfx_line((vgfx_window_t)canvas, ax, ay, ax, ay + (int32_t)arrow_size, arrow_color);
                vgfx_line((vgfx_window_t)canvas,
                          ax,
                          ay,
                          ax + (int32_t)(arrow_size * 0.75f),
                          ay + (int32_t)(arrow_size * 0.5f),
                          arrow_color);
                vgfx_line((vgfx_window_t)canvas,
                          ax,
                          ay + (int32_t)arrow_size,
                          ax + (int32_t)(arrow_size * 0.75f),
                          ay + (int32_t)(arrow_size * 0.5f),
                          arrow_color);
            }
        }

        if (row.loading) {
            int32_t cy = (int32_t)(display_y + tree->row_height * 0.5f);
            int32_t cx = (int32_t)(icon_x + tree->icon_size * 0.5f);
            int32_t radius = (int32_t)(tree->icon_size * 0.12f);
            if (radius < 1)
                radius = 1;
            vgfx_fill_circle((vgfx_window_t)canvas, cx - radius * 3, cy, radius, foreground);
            vgfx_fill_circle(
                (vgfx_window_t)canvas, cx, cy, radius, vg_color_lighten(foreground, 0.08f));
            vgfx_fill_circle((vgfx_window_t)canvas, cx + radius * 3, cy, radius, foreground);
        }

        if (tree->font && row.text) {
            float max_width = content_width - (text_x - tree->base.x) - treeview_outer_padding();
            if (max_width < 0.0f)
                max_width = 0.0f;
            char *fitted = treeview_fit_text(tree, row.text, max_width);
            vg_font_draw_text(canvas,
                              tree->font,
                              tree->font_size,
                              text_x,
                              treeview_text_baseline(tree, display_y),
                              fitted ? fitted : row.text,
                              foreground);
            free(fitted);
        }
    }
}

/// @brief Paint the TreeView's vertical scrollbar overlay.
/// @details The thumb uses active, hovered, or normal theme colors according to
///          current interaction state. Nothing is drawn when content fits.
/// @param tree TreeView supplying scrollbar geometry and state.
/// @param canvas Destination graphics canvas.
static void treeview_paint_scrollbar(vg_treeview_t *tree, void *canvas) {
    float width = 0.0f;
    float thumb_y = 0.0f;
    float thumb_height = 0.0f;
    if (!treeview_scrollbar_geometry(tree, &width, &thumb_y, &thumb_height))
        return;

    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    float track_x = tree->base.x + tree->base.width - width;
    float track_y = tree->base.y;
    uint32_t track_color =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.5f);
    uint32_t thumb_color =
        tree->scrollbar_hovered || tree->scrollbar_dragging
            ? vg_color_blend(theme->colors.bg_hover, theme->colors.accent_primary, 0.2f)
            : vg_color_blend(theme->colors.bg_tertiary, theme->colors.border_primary, 0.35f);
    vgfx_fill_rect(win,
                   (int32_t)track_x,
                   (int32_t)track_y,
                   (int32_t)width,
                   (int32_t)tree->base.height,
                   track_color);
    vgfx_rect(win,
              (int32_t)track_x,
              (int32_t)track_y,
              (int32_t)width,
              (int32_t)tree->base.height,
              theme->colors.border_secondary);
    float inset = width > 9.0f ? 2.0f : 1.0f;
    float thumb_width = width - inset * 2.0f;
    if (thumb_width < 1.0f)
        thumb_width = 1.0f;
    vg_draw_round_rect_fill(win,
                            track_x + inset,
                            track_y + thumb_y,
                            thumb_width,
                            thumb_height,
                            theme->scrollbar.border_radius,
                            thumb_color);
}

/// @brief Paint TreeView background, rows, scrollbar, and focus border.
/// @details The content clip excludes a visible scrollbar gutter. Retained or
///          virtual row rendering is selected by mode, after which the
///          scrollbar and themed outer border are overlaid.
/// @param widget TreeView base widget to paint.
/// @param canvas Destination graphics canvas.
static void treeview_paint(vg_widget_t *widget, void *canvas) {
    vg_treeview_t *tree = (vg_treeview_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    // Paint is the reliable poll point for inline-edit upkeep: the focused
    // row editor repaints continuously and focus transitions repaint both
    // widgets, so submit, blur-commit, and scroll re-placement land here.
    treeview_edit_sync(tree);

    // Draw background
    vgfx_fill_rect((vgfx_window_t)canvas,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   theme->colors.bg_secondary);

    // Paint rows inside the content area. Reserve the right gutter when a
    // scrollbar is visible so text ellipsis and selection pills remain clear.
    float y = 0;
    float scrollbar_width = 0.0f;
    float content_width = widget->width;
    if (treeview_scrollbar_geometry(tree, &scrollbar_width, NULL, NULL))
        content_width -= scrollbar_width;
    if (content_width < 0.0f)
        content_width = 0.0f;
    if (content_width > 2.0f && widget->height > 2.0f)
        vgfx_set_clip((vgfx_window_t)canvas,
                      (int32_t)widget->x + 1,
                      (int32_t)widget->y + 1,
                      (int32_t)content_width - 2,
                      (int32_t)widget->height - 2);
    if (tree->virtual_mode)
        treeview_paint_virtual(tree, canvas);
    else
        paint_node(tree, canvas, tree->root, widget->x, &y, content_width);
    if (content_width > 2.0f && widget->height > 2.0f)
        vgfx_clear_clip((vgfx_window_t)canvas);

    treeview_paint_scrollbar(tree, canvas);

    vgfx_rect((vgfx_window_t)canvas,
              (int32_t)widget->x,
              (int32_t)widget->y,
              (int32_t)widget->width,
              (int32_t)widget->height,
              (widget->state & VG_STATE_FOCUSED) ? theme->colors.border_focus
                                                 : theme->colors.border_primary);
}

/// @brief Resolve a retained content-space Y coordinate to a visible node.
/// @param tree TreeView supplying row height.
/// @param node Parent whose visible descendants are searched.
/// @param target_y Content-space Y coordinate.
/// @param current_y In/out flattened content-space row cursor.
/// @return Borrowed node covering @p target_y, or `NULL` outside all rows.
static vg_tree_node_t *find_node_at_y(vg_treeview_t *tree,
                                      vg_tree_node_t *node,
                                      float target_y,
                                      float *current_y) {
    for (vg_tree_node_t *child = node->first_child; child; child = child->next_sibling) {
        float row_start = *current_y;
        float row_end = row_start + tree->row_height;

        if (target_y >= row_start && target_y < row_end) {
            return child;
        }

        *current_y += tree->row_height;

        if (child->expanded && child->first_child) {
            vg_tree_node_t *found = find_node_at_y(tree, child, target_y, current_y);
            if (found)
                return found;
        }
    }
    return NULL;
}

/// @brief Hit-test a screen-space point against retained TreeView rows.
/// @details Virtual mode and the scrollbar gutter deliberately return no
///          retained node. The result is borrowed from the TreeView.
/// @param tree TreeView to query.
/// @param x Screen-space X coordinate.
/// @param y Screen-space Y coordinate.
/// @return Visible retained node at the point, or `NULL`.
vg_tree_node_t *vg_treeview_node_at(vg_treeview_t *tree, float x, float y) {
    if (!tree || tree->virtual_mode)
        return NULL;

    vg_widget_t *widget = &tree->base;
    if (x < widget->x || y < widget->y || x >= widget->x + widget->width ||
        y >= widget->y + widget->height) {
        return NULL;
    }
    float scrollbar_width = 0.0f;
    if (treeview_scrollbar_geometry(tree, &scrollbar_width, NULL, NULL) &&
        x >= widget->x + widget->width - scrollbar_width) {
        return NULL;
    }

    float local_y = y - widget->y;
    float target_y = local_y + tree->scroll_y;
    float current_y = 0.0f;
    return find_node_at_y(tree, tree->root, target_y, &current_y);
}

/// @brief Validate a candidate retained-tree drop.
/// @details Self-drops and drops into the source subtree are rejected before
///          the optional application predicate is consulted.
/// @param tree TreeView owning both node handles.
/// @param source Dragged subtree root.
/// @param target Candidate target node.
/// @param position Proposed before, into, or after placement.
/// @return Application predicate result when present, otherwise `true` for a
///         structurally valid candidate.
static bool treeview_drop_is_valid(vg_treeview_t *tree,
                                   vg_tree_node_t *source,
                                   vg_tree_node_t *target,
                                   vg_tree_drop_position_t position) {
    if (!tree || !source || !target || source == target)
        return false;
    if (node_in_subtree(source, target))
        return false;
    if (tree->can_drop)
        return tree->can_drop(source, target, position, tree->drag_user_data);
    return true;
}

/// @brief Recompute the retained drag target from a local cursor Y coordinate.
/// @details Row thirds map to before/into/after in row-aware mode. Legacy
///          application-directed mode permits only into drops on container
///          targets. Invalid candidates clear the current target.
/// @param tree TreeView with an active retained drag.
/// @param local_y Cursor Y relative to the widget.
static void treeview_update_drop_target(vg_treeview_t *tree, float local_y) {
    if (!tree || !tree->is_dragging || !tree->drag_node) {
        if (tree) {
            tree->drop_target = NULL;
            tree->drop_position = VG_TREE_DROP_INTO;
        }
        return;
    }

    float y = local_y + tree->scroll_y;
    float current_y = 0.0f;
    vg_tree_node_t *target = find_node_at_y(tree, tree->root, y, &current_y);
    if (!target) {
        tree->drop_target = NULL;
        tree->drop_position = VG_TREE_DROP_INTO;
        return;
    }

    float row_top = current_y - tree->scroll_y;
    float local_row_y = local_y - row_top;
    vg_tree_drop_position_t position = VG_TREE_DROP_INTO;
    if (tree->app_directed_dnd_mode == VG_TREEVIEW_APP_DND_LEGACY_INTO) {
        // Legacy poll-mode DnD remains INTO-only on expandable containers so
        // existing Explorer behavior does not change.
        if (!target->has_children && target->first_child == NULL) {
            tree->drop_target = NULL;
            return;
        }
        position = VG_TREE_DROP_INTO;
    } else {
        if (local_row_y < tree->row_height * 0.3f)
            position = VG_TREE_DROP_BEFORE;
        else if (local_row_y > tree->row_height * 0.7f)
            position = VG_TREE_DROP_AFTER;
    }

    if (!treeview_drop_is_valid(tree, tree->drag_node, target, position)) {
        tree->drop_target = NULL;
        return;
    }

    tree->drop_target = target;
    tree->drop_position = position;
}

/// @brief Install one virtual selection and optionally notify the model.
/// @details A changed local index is scrolled into view, advances selection
///          revision state, repaints, and invokes the compatibility selection
///          callback with no retained node.
/// @param tree Virtual-mode TreeView.
/// @param index Flattened model row to select.
/// @param notify_model Whether to emit `VG_TREEVIEW_VIRTUAL_SELECT`.
static void treeview_select_virtual_internal(vg_treeview_t *tree, size_t index, bool notify_model) {
    if (!tree || !tree->virtual_mode || index >= tree->virtual_row_count)
        return;
    if (tree->virtual_selected_index != index) {
        tree->virtual_selected_index = index;
        treeview_scroll_virtual_to(tree, index);
        treeview_note_selection_changed(tree);
        tree->base.needs_paint = true;
        if (tree->on_select)
            tree->on_select(&tree->base, NULL, tree->on_select_data);
    }
    if (notify_model && tree->virtual_action)
        tree->virtual_action(
            tree, index, VG_TREEVIEW_VIRTUAL_SELECT, tree->virtual_model_user_data);
}

/// @brief Handle provider-backed TreeView interaction without retained nodes.
/// @details Pointer hover/click, disclosure toggles, activation, directional
///          navigation, parent/child requests, and wheel scrolling operate on
///          flattened indices and synchronously queried row descriptors.
/// @param tree Virtual-mode TreeView receiving input.
/// @param event Event in widget-local coordinates where applicable.
/// @return `true` when the virtual path consumes the event.
static bool treeview_handle_virtual_event(vg_treeview_t *tree, vg_event_t *event) {
    if (!tree || !tree->virtual_mode || !event)
        return false;

    vg_widget_t *widget = &tree->base;
    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            size_t index = SIZE_MAX;
            size_t old_hover = tree->virtual_hovered_index;
            if (treeview_virtual_row_at_y(tree, event->mouse.y, &index))
                tree->virtual_hovered_index = index;
            else
                tree->virtual_hovered_index = SIZE_MAX;
            if (old_hover != tree->virtual_hovered_index)
                widget->needs_paint = true;
            return false;
        }
        case VG_EVENT_MOUSE_LEAVE:
            if (tree->virtual_hovered_index != SIZE_MAX) {
                tree->virtual_hovered_index = SIZE_MAX;
                widget->needs_paint = true;
            }
            return false;
        case VG_EVENT_CLICK: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            size_t index = 0;
            if (!treeview_virtual_row_at_y(tree, event->mouse.y, &index))
                return false;
            vg_treeview_virtual_row_t row = {0};
            bool found = tree->virtual_provider &&
                         tree->virtual_provider(tree, index, &row, tree->virtual_model_user_data);
            float depth = row.depth > (size_t)INT_MAX ? (float)INT_MAX : (float)row.depth;
            float arrow_left = treeview_outer_padding() + depth * tree->indent_size;
            float row_content_y = (float)((double)index * (double)tree->row_height);
            float row_local_y = row_content_y - tree->scroll_y;
            float arrow_size = 8.0f * treeview_scale();
            float arrow_top = row_local_y + (tree->row_height - arrow_size) * 0.5f;
            if (found && row.has_children && event->mouse.x >= arrow_left &&
                event->mouse.x < arrow_left + tree->icon_size && event->mouse.y >= arrow_top &&
                event->mouse.y < arrow_top + arrow_size) {
                if (tree->virtual_action)
                    tree->virtual_action(
                        tree, index, VG_TREEVIEW_VIRTUAL_TOGGLE, tree->virtual_model_user_data);
            } else {
                treeview_select_virtual_internal(tree, index, true);
            }
            widget->needs_paint = true;
            return true;
        }
        case VG_EVENT_DOUBLE_CLICK:
            if (event->mouse.button != VG_MOUSE_LEFT ||
                tree->virtual_selected_index >= tree->virtual_row_count)
                return false;
            vg_widget_note_activation(widget);
            if (tree->on_activate)
                tree->on_activate(widget, NULL, tree->on_activate_data);
            if (tree->virtual_action)
                tree->virtual_action(tree,
                                     tree->virtual_selected_index,
                                     VG_TREEVIEW_VIRTUAL_ACTIVATE,
                                     tree->virtual_model_user_data);
            return true;
        case VG_EVENT_KEY_DOWN: {
            if (tree->virtual_row_count == 0)
                return false;
            size_t selected = tree->virtual_selected_index;
            bool has_selection = selected < tree->virtual_row_count;
            size_t target = has_selection ? selected : 0u;
            bool select_target = false;
            switch (event->key.key) {
                case VG_KEY_UP:
                    target = has_selection && selected > 0 ? selected - 1u : 0u;
                    select_target = true;
                    break;
                case VG_KEY_DOWN:
                    target = has_selection && selected + 1u < tree->virtual_row_count
                                 ? selected + 1u
                                 : (has_selection ? selected : 0u);
                    select_target = true;
                    break;
                case VG_KEY_HOME:
                    target = 0u;
                    select_target = true;
                    break;
                case VG_KEY_END:
                    target = tree->virtual_row_count - 1u;
                    select_target = true;
                    break;
                case VG_KEY_LEFT:
                case VG_KEY_RIGHT: {
                    if (!has_selection)
                        return false;
                    vg_treeview_virtual_row_t row = {0};
                    bool found =
                        tree->virtual_provider &&
                        tree->virtual_provider(tree, selected, &row, tree->virtual_model_user_data);
                    if (!found)
                        return false;
                    if (event->key.key == VG_KEY_LEFT) {
                        if (row.expanded && row.has_children) {
                            if (tree->virtual_action)
                                tree->virtual_action(tree,
                                                     selected,
                                                     VG_TREEVIEW_VIRTUAL_TOGGLE,
                                                     tree->virtual_model_user_data);
                        } else if (tree->virtual_action) {
                            tree->virtual_action(tree,
                                                 selected,
                                                 VG_TREEVIEW_VIRTUAL_PARENT,
                                                 tree->virtual_model_user_data);
                        }
                    } else if (!row.expanded && row.has_children) {
                        if (tree->virtual_action)
                            tree->virtual_action(tree,
                                                 selected,
                                                 VG_TREEVIEW_VIRTUAL_TOGGLE,
                                                 tree->virtual_model_user_data);
                    } else if (selected + 1u < tree->virtual_row_count) {
                        vg_treeview_virtual_row_t next = {0};
                        if (tree->virtual_provider &&
                            tree->virtual_provider(
                                tree, selected + 1u, &next, tree->virtual_model_user_data) &&
                            next.depth > row.depth) {
                            treeview_select_virtual_internal(tree, selected + 1u, true);
                        }
                    }
                    widget->needs_paint = true;
                    return true;
                }
                case VG_KEY_ENTER:
                    if (!has_selection)
                        return false;
                    vg_widget_note_activation(widget);
                    if (tree->on_activate)
                        tree->on_activate(widget, NULL, tree->on_activate_data);
                    if (tree->virtual_action)
                        tree->virtual_action(tree,
                                             selected,
                                             VG_TREEVIEW_VIRTUAL_ACTIVATE,
                                             tree->virtual_model_user_data);
                    return true;
                default:
                    return false;
            }
            if (select_target)
                treeview_select_virtual_internal(tree, target, true);
            return true;
        }
        case VG_EVENT_MOUSE_WHEEL: {
            if (!isfinite(event->wheel.delta_y) || event->wheel.delta_y == 0.0f)
                return false;
            float old_scroll = tree->scroll_y;
            tree->scroll_y -= event->wheel.delta_y * tree->row_height * vg_get_wheel_speed();
            treeview_clamp_scroll(tree);
            if (tree->scroll_y == old_scroll)
                return false;
            widget->needs_paint = true;
            return true;
        }
        default:
            return false;
    }
}

/// @brief Route retained or virtual TreeView input.
/// @details Disabled widgets reject input. Scrollbar handling has first
///          priority, followed by virtual dispatch or retained hover,
///          selection, disclosure, activation, keyboard navigation, wheel
///          scrolling, and drag/drop latching.
/// @param widget TreeView base widget receiving the event.
/// @param event Event in widget-local coordinates where applicable.
/// @return `true` when the TreeView consumes the event.
static bool treeview_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_treeview_t *tree = (vg_treeview_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    // Escape cancels an inline row edit. The focused row editor declines the
    // key, so it propagates here through the ancestor chain.
    if (tree->edit_active && event->type == VG_EVENT_KEY_DOWN &&
        event->key.key == VG_KEY_ESCAPE) {
        treeview_edit_finish(tree, false);
        return true;
    }

    if (treeview_handle_scrollbar_event(tree, event))
        return true;

    if (tree->virtual_mode)
        return treeview_handle_virtual_event(tree, event);

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN: {
            if (!tree->drag_enabled)
                return false;
            float y = event->mouse.y + tree->scroll_y;
            float current_y = 0.0f;
            vg_tree_node_t *pressed = find_node_at_y(tree, tree->root, y, &current_y);
            if (!pressed)
                return false;
            if (tree->can_drag && !tree->can_drag(pressed, tree->drag_user_data))
                return false;
            tree->drag_node = pressed;
            tree->drag_start_x = (int)event->mouse.x;
            tree->drag_start_y = (int)event->mouse.y;
            tree->is_dragging = false;
            tree->drop_target = NULL;
            tree->drop_position = VG_TREE_DROP_INTO;
            vg_widget_set_input_capture(widget);
            return false;
        }

        case VG_EVENT_MOUSE_MOVE: {
            if (vg_widget_get_input_capture() == widget && tree->drag_node) {
                float scale = treeview_scale();
                int dx = (int)event->mouse.x - tree->drag_start_x;
                int dy = (int)event->mouse.y - tree->drag_start_y;
                if (!tree->is_dragging &&
                    (dx * dx + dy * dy) >= (int)((6.0f * scale) * (6.0f * scale))) {
                    tree->is_dragging = true;
                }
                if (tree->is_dragging) {
                    treeview_update_drop_target(tree, event->mouse.y);
                    widget->needs_paint = true;
                    return true;
                }
            }

            // Find node at position
            float y = event->mouse.y + tree->scroll_y;
            float current_y = 0;
            vg_tree_node_t *old_hover = tree->hovered;
            tree->hovered = find_node_at_y(tree, tree->root, y, &current_y);
            if (old_hover != tree->hovered) {
                widget->needs_paint = true;
            }
            return false;
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (!tree->is_dragging && tree->hovered) {
                tree->hovered = NULL;
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_CLICK: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            if (tree->suppress_click) {
                tree->suppress_click = false;
                return true;
            }
            float y = event->mouse.y + tree->scroll_y;
            float current_y = 0;
            vg_tree_node_t *clicked = find_node_at_y(tree, tree->root, y, &current_y);

            if (clicked) {
                // Check if clicked on expand arrow
                float arrow_left = treeview_outer_padding() + clicked->depth * tree->indent_size;
                float arrow_size = 8.0f * treeview_scale();
                float row_local_y = current_y - tree->scroll_y;
                float arrow_top = row_local_y + (tree->row_height - arrow_size) * 0.5f;
                if (event->mouse.x >= arrow_left && event->mouse.x < arrow_left + tree->icon_size &&
                    event->mouse.y >= arrow_top && event->mouse.y < arrow_top + arrow_size) {
                    // Toggle expand
                    vg_treeview_toggle(tree, clicked);
                } else {
                    uint32_t modifiers = event->modifiers;
                    bool toggle = tree->multi_select && ((modifiers & VG_MOD_CTRL) != 0 ||
                                                         (modifiers & VG_MOD_SUPER) != 0);
                    bool range = tree->multi_select && (modifiers & VG_MOD_SHIFT) != 0;
                    treeview_select_interactive(tree, clicked, toggle, range);
                }
                return true;
            }
            return false;
        }

        case VG_EVENT_DOUBLE_CLICK: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            if (tree->selected) {
                tree->last_activated = tree->selected;
                vg_widget_note_activation(widget);
                if (tree->on_activate)
                    tree->on_activate(widget, tree->selected, tree->on_activate_data);
            }
            return true;
        }

        case VG_EVENT_KEY_DOWN:
            if (tree->selected) {
                switch (event->key.key) {
                    case VG_KEY_UP: {
                        // Select previous node
                        int current = 0;
                        int index = get_node_index(tree->root, tree->selected, &current);
                        if (index > 0) {
                            current = 0;
                            vg_tree_node_t *prev =
                                get_node_at_index(tree->root, index - 1, &current);
                            if (prev) {
                                treeview_select_interactive(
                                    tree,
                                    prev,
                                    false,
                                    tree->multi_select && (event->modifiers & VG_MOD_SHIFT) != 0);
                            }
                        }
                        return true;
                    }
                    case VG_KEY_DOWN: {
                        // Select next node
                        int current = 0;
                        int index = get_node_index(tree->root, tree->selected, &current);
                        current = 0;
                        vg_tree_node_t *next = get_node_at_index(tree->root, index + 1, &current);
                        if (next) {
                            treeview_select_interactive(tree,
                                                        next,
                                                        false,
                                                        tree->multi_select &&
                                                            (event->modifiers & VG_MOD_SHIFT) != 0);
                        }
                        return true;
                    }
                    case VG_KEY_LEFT:
                        // Collapse or go to parent
                        if (tree->selected->expanded && tree->selected->first_child) {
                            vg_treeview_collapse(tree, tree->selected);
                        } else if (tree->selected->parent && tree->selected->parent != tree->root) {
                            treeview_select_interactive(tree, tree->selected->parent, false, false);
                        }
                        return true;
                    case VG_KEY_RIGHT:
                        // Expand or go to first child
                        if (!tree->selected->expanded &&
                            (tree->selected->has_children || tree->selected->first_child)) {
                            vg_treeview_expand(tree, tree->selected);
                        } else if (tree->selected->first_child) {
                            treeview_select_interactive(
                                tree, tree->selected->first_child, false, false);
                        }
                        return true;
                    case VG_KEY_ENTER:
                        tree->last_activated = tree->selected;
                        vg_widget_note_activation(widget);
                        if (tree->on_activate)
                            tree->on_activate(widget, tree->selected, tree->on_activate_data);
                        return true;
                    default:
                        break;
                }
            }
            return false;

        case VG_EVENT_MOUSE_WHEEL: {
            if (!isfinite(event->wheel.delta_y) || event->wheel.delta_y == 0.0f)
                return false;
            float old_scroll = tree->scroll_y;
            tree->scroll_y -= event->wheel.delta_y * tree->row_height * vg_get_wheel_speed();
            treeview_clamp_scroll(tree);
            if (tree->scroll_y == old_scroll)
                return false;
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_MOUSE_UP: {
            bool was_dragging = tree->is_dragging;
            if (vg_widget_get_input_capture() == widget)
                vg_widget_release_input_capture();
            if (tree->is_dragging && tree->drag_node && tree->drop_target) {
                if (tree->app_directed_dnd_mode != VG_TREEVIEW_APP_DND_DISABLED) {
                    // Latch for polling; the application performs the move and
                    // refreshes the tree. Do NOT self-reorder or fire on_drop.
                    tree->drop_latched = true;
                    tree->latched_src = tree->drag_node;
                    tree->latched_tgt = tree->drop_target;
                    tree->latched_pos = tree->drop_position;
                } else if (tree->on_drop) {
                    tree->on_drop(tree->drag_node,
                                  tree->drop_target,
                                  tree->drop_position,
                                  tree->drag_user_data);
                }
            }
            tree->drag_node = NULL;
            tree->drop_target = NULL;
            tree->is_dragging = false;
            tree->suppress_click = was_dragging;
            if (was_dragging) {
                widget->needs_paint = true;
                return true;
            }
            return false;
        }

        default:
            break;
    }

    return false;
}

/// @brief Determine whether a TreeView may accept keyboard focus.
/// @param widget TreeView base widget to inspect.
/// @return `true` when the widget is enabled and visible.
static bool treeview_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

//=============================================================================
// TreeView API
//=============================================================================

/// @brief Bind a flattened external model without retained row allocation.
/// @details Any previous virtual model is notified before callback fields are
///          replaced. Selection, hover, scrolling, and visible-range state are
///          reset, while the retained hierarchy remains available for later
///          restoration.
/// @param tree TreeView to switch into virtual mode.
/// @param row_count Current flattened model row count.
/// @param provider Required synchronous row-descriptor provider.
/// @param action Optional callback for selection, toggle, activation, and
///        parent navigation requests.
/// @param user_data Opaque pointer forwarded to virtual callbacks.
/// @param on_unbind Optional one-shot detach notification.
/// @return `true` when the model was bound; `false` for invalid input.
bool vg_treeview_bind_virtual_model(vg_treeview_t *tree,
                                    size_t row_count,
                                    vg_treeview_virtual_provider_t provider,
                                    vg_treeview_virtual_action_callback_t action,
                                    void *user_data,
                                    vg_treeview_virtual_unbind_callback_t on_unbind) {
    if (!tree || !provider)
        return false;

    treeview_notify_virtual_unbound(tree);
    tree->virtual_mode = true;
    tree->virtual_row_count = row_count;
    tree->virtual_selected_index = SIZE_MAX;
    tree->virtual_hovered_index = SIZE_MAX;
    tree->virtual_provider = provider;
    tree->virtual_action = action;
    tree->virtual_unbind = on_unbind;
    tree->virtual_model_user_data = user_data;
    tree->scroll_y = 0.0f;
    tree->visible_start = 0;
    tree->visible_count = 0;
    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);
    return true;
}

/// @brief Detach the current external model and restore retained rendering.
/// @details Callback pointers and virtual indices are cleared, scrolling
///          returns to the top, and change/revision edges reflect whether a
///          virtual selection existed.
/// @param tree TreeView to detach; `NULL` is ignored.
void vg_treeview_clear_virtual_model(vg_treeview_t *tree) {
    if (!tree)
        return;
    bool was_virtual = tree->virtual_mode;
    bool had_selection = tree->virtual_selected_index != SIZE_MAX;
    treeview_notify_virtual_unbound(tree);
    tree->virtual_mode = false;
    tree->virtual_row_count = 0;
    tree->virtual_selected_index = SIZE_MAX;
    tree->virtual_hovered_index = SIZE_MAX;
    tree->virtual_provider = NULL;
    tree->virtual_action = NULL;
    tree->virtual_model_user_data = NULL;
    tree->scroll_y = 0.0f;
    tree->visible_start = 0;
    tree->visible_count = 0;
    if (was_virtual) {
        if (had_selection)
            treeview_note_selection_changed(tree);
        else
            vg_widget_note_revision(&tree->base);
        tree->base.needs_layout = true;
        tree->base.needs_paint = true;
    }
}

/// @brief Update the flattened virtual-model row count.
/// @details Selection and hover indices beyond the new suffix are cleared,
///          scroll is reclamped, and layout/repaint plus revision state are
///          invalidated.
/// @param tree Virtual-mode TreeView to update.
/// @param row_count New flattened row count.
void vg_treeview_set_virtual_row_count(vg_treeview_t *tree, size_t row_count) {
    if (!tree || !tree->virtual_mode || tree->virtual_row_count == row_count)
        return;
    tree->virtual_row_count = row_count;
    if (tree->virtual_selected_index >= row_count) {
        tree->virtual_selected_index = SIZE_MAX;
        treeview_note_selection_changed(tree);
    } else {
        vg_widget_note_revision(&tree->base);
    }
    if (tree->virtual_hovered_index >= row_count)
        tree->virtual_hovered_index = SIZE_MAX;
    treeview_clamp_scroll(tree);
    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
}

/// @brief Synchronize virtual selection without a model action callback.
/// @param tree Virtual-mode TreeView.
/// @param index Row to select, or `SIZE_MAX` to clear selection.
void vg_treeview_select_virtual_index(vg_treeview_t *tree, size_t index) {
    if (!tree || !tree->virtual_mode)
        return;
    if (index == SIZE_MAX) {
        if (tree->virtual_selected_index != SIZE_MAX) {
            tree->virtual_selected_index = SIZE_MAX;
            treeview_note_selection_changed(tree);
            tree->base.needs_paint = true;
        }
        return;
    }
    treeview_select_virtual_internal(tree, index, false);
}

/// @brief Read the current virtual selection.
/// @param tree TreeView to inspect.
/// @return Selected flattened row index, or `SIZE_MAX` outside virtual mode or
///         when no row is selected.
size_t vg_treeview_get_virtual_selected_index(const vg_treeview_t *tree) {
    return tree && tree->virtual_mode ? tree->virtual_selected_index : SIZE_MAX;
}

/// @brief Compute the first virtual row intersecting the viewport.
/// @param tree TreeView whose virtual range is queried.
/// @return First flattened row index, or zero when no range is active.
size_t vg_treeview_get_visible_first(vg_treeview_t *tree) {
    size_t first = 0;
    treeview_compute_virtual_range(tree, &first, NULL);
    return first;
}

/// @brief Compute the bounded virtual viewport materialization count.
/// @param tree TreeView whose virtual range is queried.
/// @return Number of provider rows needed for the current viewport.
size_t vg_treeview_get_visible_count(vg_treeview_t *tree) {
    size_t count = 0;
    treeview_compute_virtual_range(tree, NULL, &count);
    return count;
}

/// @brief Invalidate externally supplied virtual row descriptors.
/// @details No model callbacks are invoked; repaint and the non-consuming
///          widget revision are advanced for the next render.
/// @param tree Virtual-mode TreeView to invalidate.
void vg_treeview_invalidate_virtual_rows(vg_treeview_t *tree) {
    if (!tree || !tree->virtual_mode)
        return;
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);
}

/// @brief Return the hidden sentinel root node of the tree.
///
/// @param tree The tree view to query; may be NULL (returns NULL).
/// @return     Internal root node; pass as parent to vg_treeview_add_node for top-level items.
vg_tree_node_t *vg_treeview_get_root(vg_treeview_t *tree) {
    return tree ? tree->root : NULL;
}

/// @brief Add a new text node as the last child of parent (or of root if parent is NULL).
///
/// @details Allocates a vg_tree_node_t, stamps VG_TREE_NODE_MAGIC, copies text,
///          links the node into parent's child list, and triggers layout/paint.
///          parent must be live and belong to tree; an invalid parent causes the
///          new node to be freed and NULL returned.
///
/// @param tree   The tree view that will own the node; may be NULL (returns NULL).
/// @param parent Parent node, or NULL to append at the root level.
/// @param text   Display text; copied internally.
/// @return       New node handle, or NULL on allocation failure or invalid parent.
vg_tree_node_t *vg_treeview_add_node(vg_treeview_t *tree,
                                     vg_tree_node_t *parent,
                                     const char *text) {
    if (!tree)
        return NULL;

    vg_tree_node_t *node = calloc(1, sizeof(vg_tree_node_t));
    if (!node)
        return NULL;

    node->text = text ? vg_strdup(text) : vg_strdup("");
    if (!node->text) {
        free(node);
        return NULL;
    }
    node->text_len = strlen(node->text);
    node->magic = VG_TREE_NODE_MAGIC;
    node->owner = tree;
    node->expanded = false;
    node->selected = false;
    node->has_children = false;

    // Add to parent
    vg_tree_node_t *actual_parent = parent ? parent : tree->root;
    if (!vg_tree_node_is_live(actual_parent) || actual_parent->owner != tree) {
        free_node(node);
        return NULL;
    }
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);
    node->parent = actual_parent;
    node->depth = actual_parent->depth + 1;

    if (actual_parent->last_child) {
        actual_parent->last_child->next_sibling = node;
        node->prev_sibling = actual_parent->last_child;
        actual_parent->last_child = node;
    } else {
        actual_parent->first_child = node;
        actual_parent->last_child = node;
    }
    actual_parent->child_count++;
    actual_parent->has_children = true;

    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);

    return node;
}

/// @brief Remove a node and its entire subtree from the tree, retiring them for deferred free.
///
/// @details Clears selection, hovered, drag, and drop references that point into the
///          removed subtree. Unlinks node from its parent's child list and retires the
///          subtree; the freed memory is deferred until free_retired_nodes runs.
///
/// @param tree The owning tree view; may be NULL (no-op).
/// @param node The node to remove; must be live, owned by tree, and not the root.
void vg_treeview_remove_node(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree || node == tree->root)
        return;
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);

    // Update selection if needed. A removed non-primary selection is still a
    // logical selection change and must emit exactly one edge.
    bool removed_had_selection = treeview_subtree_has_selection(node);
    if (node_in_subtree(node, tree->selected))
        tree->selected = NULL;
    if (node_in_subtree(node, tree->anchor_selected))
        tree->anchor_selected = NULL;
    if (node_in_subtree(node, tree->prev_selected)) {
        tree->prev_selected = NULL;
    }
    if (node_in_subtree(node, tree->hovered)) {
        tree->hovered = NULL;
    }
    if (node_in_subtree(node, tree->drag_node))
        tree->drag_node = NULL;
    if (node_in_subtree(node, tree->drop_target))
        tree->drop_target = NULL;
    if (node_in_subtree(node, tree->latched_src) || node_in_subtree(node, tree->latched_tgt)) {
        tree->latched_src = NULL;
        tree->latched_tgt = NULL;
        tree->drop_latched = false;
    }
    if (node_in_subtree(node, tree->last_activated))
        tree->last_activated = NULL;
    if (node_in_subtree(node, tree->last_load_requested))
        tree->last_load_requested = NULL;
    if (node_in_subtree(node, tree->edit_node))
        treeview_edit_finish(tree, false);
    if (node_in_subtree(node, tree->edit_committed_node))
        tree->edit_committed_node = NULL;

    // Remove from parent's child list
    vg_tree_node_t *parent = node->parent;
    if (parent) {
        if (node->prev_sibling) {
            node->prev_sibling->next_sibling = node->next_sibling;
        } else {
            parent->first_child = node->next_sibling;
        }
        if (node->next_sibling) {
            node->next_sibling->prev_sibling = node->prev_sibling;
        } else {
            parent->last_child = node->prev_sibling;
        }
        parent->child_count--;
        parent->has_children = parent->first_child != NULL;
    }

    if (removed_had_selection) {
        for (vg_tree_node_t *candidate = node; candidate;
             candidate = treeview_next_retained(node, candidate, false)) {
            candidate->selected = false;
        }
        if (!tree->selected)
            tree->selected = treeview_first_selected(tree);
        if (!tree->anchor_selected)
            tree->anchor_selected = tree->selected;
        treeview_note_selection_changed(tree);
    }

    retire_node_subtree(tree, node);

    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);
}

/// @brief Remove all nodes from the tree, retiring them for deferred free.
///
/// @details All children of root are retired (stale external handles become
///          safely inert via the magic-field check). Selection, hover, and
///          scroll state are reset to zero.
///
/// @param tree The tree view to clear; may be NULL.
void vg_treeview_clear(vg_treeview_t *tree) {
    if (!tree)
        return;
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);

    bool had_nodes = tree->root && tree->root->first_child != NULL;
    bool had_selection = treeview_first_selected(tree) != NULL;

    treeview_edit_finish(tree, false);
    tree->edit_committed_node = NULL;

    // Retire all children of root so stale node handles remain safely inert
    // until the tree itself is destroyed.
    vg_tree_node_t *child = tree->root->first_child;
    while (child) {
        vg_tree_node_t *next = child->next_sibling;
        retire_node_subtree(tree, child);
        child = next;
    }

    tree->root->first_child = NULL;
    tree->root->last_child = NULL;
    tree->root->child_count = 0;
    tree->root->has_children = false;
    if (had_selection)
        treeview_note_selection_changed(tree);
    tree->selected = NULL;
    tree->prev_selected = NULL;
    tree->anchor_selected = NULL;
    tree->hovered = NULL;
    tree->drag_node = NULL;
    tree->drop_target = NULL;
    tree->latched_src = NULL;
    tree->latched_tgt = NULL;
    tree->drop_latched = false;
    tree->last_activated = NULL;
    tree->last_load_requested = NULL;
    tree->scroll_y = 0;

    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
    if (had_nodes)
        vg_widget_note_revision(&tree->base);
}

/// @brief Free retired node tombstones once callers have released stale handles.
///
/// @details Removed node handles are normally kept as inert tombstones until the
///          tree is destroyed so vg_tree_node_is_live() can reject stale handles
///          without dereferencing freed memory. This explicit pruning hook lets
///          owners reclaim that memory after they have discarded all node handles
///          returned before the corresponding remove/clear calls.
/// @param tree TreeView whose complete retired list is reclaimed; `NULL` is
///        ignored.
void vg_treeview_prune_retired_nodes(vg_treeview_t *tree) {
    free_retired_nodes(tree);
}

/// @brief Unlink and destroy one exact retired TreeView root subtree.
/// @details Only roots are linked in `retired_nodes`; descendants remain connected for one
///          iterative `free_node` call after the embedding runtime proves the group unreferenced.
/// @param tree TreeView whose retired-root list is searched.
/// @param retired_root Exact tombstone root pointer to reclaim.
/// @return `true` when the root was found and destroyed.
bool vg_treeview_reclaim_retired_subtree(vg_treeview_t *tree, vg_tree_node_t *retired_root) {
    if (!tree || !retired_root)
        return false;
    vg_tree_node_t **link = &tree->retired_nodes;
    while (*link) {
        vg_tree_node_t *candidate = *link;
        if (candidate == retired_root) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_node(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Expand node to show its children, triggering lazy load if needed.
///
/// @details If node has has_children set but no actual child nodes, fires
///          on_load_children to populate them and sets node->loading = true
///          until the callback completes. Always fires on_expand(true) if set.
///
/// @param tree The owning tree view; may be NULL.
/// @param node The node to expand; must be live and owned by tree.
void vg_treeview_expand(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree)
        return;
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);

    if (!node->expanded) {
        node->expanded = true;
        tree->base.needs_layout = true;
        tree->base.needs_paint = true;
        vg_widget_note_revision(&tree->base);

        // Lazy loading: if node has children flag but no actual children, load them
        if (node->has_children && node->child_count == 0 && !node->loading) {
            node->loading = true;
            tree->last_load_requested = node;
            if (tree->load_request_revision < UINT64_MAX)
                tree->load_request_revision++;
            vg_widget_note_revision(&tree->base);
            if (tree->on_load_children)
                tree->on_load_children(tree, node, tree->on_load_children_data);
            // Callback should add children and then set loading=false
        }

        if (tree->on_expand) {
            tree->on_expand(&tree->base, node, true, tree->on_expand_data);
        }
    }
}

/// @brief Collapse node to hide its children, re-clamping scroll_y.
///
/// @details Calls treeview_clamp_scroll after collapsing so blank space at the
///          bottom of the view is eliminated. Fires on_expand(false) if set.
///
/// @param tree The owning tree view; may be NULL.
/// @param node The node to collapse; must be live and owned by tree.
void vg_treeview_collapse(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree)
        return;
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);

    if (node->expanded) {
        node->expanded = false;
        tree->base.needs_layout = true;
        tree->base.needs_paint = true;
        vg_widget_note_revision(&tree->base);

        // Re-clamp scroll_y against the new (smaller) content. Without this
        // the view can sit past the last visible row, leaving blank space at
        // the bottom and desynchronizing arrow-key navigation.
        treeview_clamp_scroll(tree);

        if (tree->on_expand) {
            tree->on_expand(&tree->base, node, false, tree->on_expand_data);
        }
    }
}

/// @brief Toggle a node's expanded state — expands if collapsed, collapses if expanded.
///
/// @param tree The owning tree view; may be NULL.
/// @param node The node to toggle; must be live and owned by tree.
void vg_treeview_toggle(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree)
        return;

    if (node->expanded) {
        vg_treeview_collapse(tree, node);
    } else {
        vg_treeview_expand(tree, node);
    }
}

/// @brief Select a node, updating visual state and firing the on_select callback.
///
/// @details Replaces selection in single-select mode, adds node and makes it primary in
///          multi-select mode, or clears every selected flag for NULL. Programmatic additive
///          behavior lets callers reconstruct a stable selection after rebuilding retained rows.
///
/// @param tree The owning tree view; may be NULL.
/// @param node Node to select; must be live and owned by tree, or NULL to deselect.
void vg_treeview_select(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree)
        return;
    if (node && (!vg_tree_node_is_live(node) || node->owner != tree))
        return;
    if (tree->virtual_mode)
        vg_treeview_clear_virtual_model(tree);

    vg_tree_node_t *old_primary = tree->selected;
    bool changed = false;
    if (!node) {
        changed = treeview_clear_retained_selection(tree, NULL);
        tree->selected = NULL;
        tree->anchor_selected = NULL;
        changed = changed || old_primary != NULL;
    } else if (!tree->multi_select) {
        changed = treeview_clear_retained_selection(tree, node);
        changed = changed || !node->selected || old_primary != node;
        node->selected = true;
        tree->selected = node;
        tree->anchor_selected = node;
    } else {
        changed = !node->selected || old_primary != node;
        node->selected = true;
        tree->selected = node;
        tree->anchor_selected = node;
    }
    treeview_publish_retained_selection(tree, changed);
}

/// @brief Configure retained-node multi-selection.
/// @details Disabling the mode preserves the live primary selection when
///          possible, clears every other selected flag, resets the range anchor,
///          and publishes a selection revision if state changed.
/// @param tree TreeView to configure; `NULL` is ignored.
/// @param enabled Whether independent and range selection are permitted.
void vg_treeview_set_multi_select(vg_treeview_t *tree, bool enabled) {
    if (!tree || tree->multi_select == enabled)
        return;
    tree->multi_select = enabled;
    if (enabled)
        return;

    vg_tree_node_t *keep = tree->selected;
    if (!keep || !vg_tree_node_is_live(keep) || keep->owner != tree || !keep->selected)
        keep = treeview_first_selected(tree);
    bool changed = treeview_clear_retained_selection(tree, keep);
    if (keep && !keep->selected) {
        keep->selected = true;
        changed = true;
    }
    if (tree->selected != keep) {
        tree->selected = keep;
        changed = true;
    }
    tree->anchor_selected = keep;
    if (changed) {
        treeview_note_selection_changed(tree);
        tree->base.needs_paint = true;
    }
}

/// @brief Scroll the view so node is visible, adjusting scroll_y minimally.
///
/// @details If node's row top is above the viewport, scrolls up to it. If the
///          row bottom is below the viewport, scrolls down to show it. Does nothing
///          if node is already fully visible.
///
/// @param tree The owning tree view; may be NULL.
/// @param node The node to bring into view; must be live and owned by tree.
void vg_treeview_scroll_to(vg_treeview_t *tree, vg_tree_node_t *node) {
    if (!tree || !vg_tree_node_is_live(node) || node->owner != tree)
        return;

    // Get node index
    int current = 0;
    int index = get_node_index(tree->root, node, &current);
    if (index < 0)
        return;

    float node_y = index * tree->row_height;

    // Scroll if needed
    float old_scroll_y = tree->scroll_y;
    if (node_y < tree->scroll_y) {
        tree->scroll_y = node_y;
    } else if (node_y + tree->row_height > tree->scroll_y + tree->base.height) {
        tree->scroll_y = node_y + tree->row_height - tree->base.height;
    }
    treeview_clamp_scroll(tree);

    tree->base.needs_paint = true;
    if (tree->scroll_y != old_scroll_y)
        vg_widget_note_revision(&tree->base);
}

/// @brief Replace a live node's display text atomically.
/// @details The input is copied before existing storage is released. A changed
///          value invalidates layout, paint, and widget revision state.
/// @param node Live node to update.
/// @param text New label; `NULL` is stored as an empty string.
/// @return `true` on success or an unchanged value; `false` for a stale node or
///         allocation failure.
bool vg_tree_node_set_text(vg_tree_node_t *node, const char *text) {
    if (!vg_tree_node_is_live(node))
        return false;
    const char *value = text ? text : "";
    if (node->text && strcmp(node->text, value) == 0)
        return true;
    char *copy = vg_strdup(value);
    if (!copy)
        return false;
    free(node->text);
    node->text = copy;
    node->text_len = strlen(copy);
    node->owner->base.needs_layout = true;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
    return true;
}

/// @brief Replace a live node's UTF-8 icon text and clear any resource-backed icon.
/// @details The value "vector:<name>" (optionally "vector:<name>|<expanded>")
///          selects named scalable icons from the vg_icon_vector library
///          instead of literal text, so the existing TreeView.Node.SetIcon
///          runtime surface reaches vector icons with no new API (plan 04).
///          Unknown names degrade to no icon (the fallback dot renders).
/// @param node Live node to update.
/// @param icon_text UTF-8 icon label, vector specification, or `NULL` to clear.
/// @return `true` on success or an unchanged value; `false` for a stale node or
///         allocation failure.
bool vg_tree_node_set_icon_text(vg_tree_node_t *node, const char *icon_text) {
    if (!vg_tree_node_is_live(node))
        return false;
    const char *value = icon_text ? icon_text : "";

    if (strncmp(value, "vector:", 7) == 0) {
        const char *name = value + 7;
        const char *sep = strchr(name, '|');
        int32_t base_id = VG_ICON_VECTOR_INVALID;
        int32_t expanded_id = VG_ICON_VECTOR_INVALID;
        if (sep) {
            size_t base_len = (size_t)(sep - name);
            char base_name[64];
            if (base_len < sizeof(base_name)) {
                memcpy(base_name, name, base_len);
                base_name[base_len] = '\0';
                base_id = vg_icon_vector_find(base_name);
            }
            expanded_id = vg_icon_vector_find(sep + 1);
        } else {
            base_id = vg_icon_vector_find(name);
        }
        free(node->icon_text);
        node->icon_text = NULL;
        node->icon_text_len = 0;
        free(node->icon_spec);
        node->icon_spec = vg_strdup(value);
        vg_icon_destroy(&node->icon);
        vg_icon_destroy(&node->expanded_icon);
        if (base_id != VG_ICON_VECTOR_INVALID)
            node->icon = vg_icon_from_vector(base_id);
        if (expanded_id != VG_ICON_VECTOR_INVALID)
            node->expanded_icon = vg_icon_from_vector(expanded_id);
        node->owner->base.needs_layout = true;
        node->owner->base.needs_paint = true;
        vg_widget_note_revision(&node->owner->base);
        return true;
    }

    if (node->icon_text && strcmp(node->icon_text, value) == 0 && node->icon.type == VG_ICON_NONE)
        return true;
    if (!node->icon_text && value[0] == '\0' && node->icon.type == VG_ICON_NONE)
        return true;

    char *copy = value[0] != '\0' ? vg_strdup(value) : NULL;
    if (value[0] != '\0' && !copy)
        return false;

    free(node->icon_text);
    node->icon_text = copy;
    node->icon_text_len = copy ? strlen(copy) : 0;
    free(node->icon_spec);
    node->icon_spec = NULL;
    vg_icon_destroy(&node->icon);
    node->owner->base.needs_layout = true;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
    return true;
}

/// @brief Return a live node's borrowed UTF-8 icon text.
/// @details Vector icons set through the "vector:" protocol round-trip: the
///          original spec string is returned so callers and probes can read
///          back what they assigned.
/// @param node Node to inspect.
/// @return Borrowed icon-text or "vector:" spec pointer, or `NULL` for stale
///         nodes or nodes using directly assigned resource icons.
const char *vg_tree_node_get_icon_text(const vg_tree_node_t *node) {
    if (!vg_tree_node_is_live(node))
        return NULL;
    return node->icon_text ? node->icon_text : node->icon_spec;
}

/// @brief Replace a live node's application-stable identifier atomically.
/// @details Empty input clears the allocation. A successful change advances
///          widget revision without affecting layout or paint.
/// @param node Live node to update.
/// @param stable_id Identifier to copy, or `NULL` to clear.
/// @return `true` on success or an unchanged value; `false` for a stale node or
///         allocation failure.
bool vg_tree_node_set_stable_id(vg_tree_node_t *node, const char *stable_id) {
    if (!vg_tree_node_is_live(node))
        return false;
    const char *value = stable_id ? stable_id : "";
    if (node->stable_id && strcmp(node->stable_id, value) == 0)
        return true;
    if (!node->stable_id && value[0] == '\0')
        return true;
    char *copy = value[0] != '\0' ? vg_strdup(value) : NULL;
    if (value[0] != '\0' && !copy)
        return false;
    free(node->stable_id);
    node->stable_id = copy;
    node->stable_id_len = copy ? strlen(copy) : 0;
    vg_widget_note_revision(&node->owner->base);
    return true;
}

/// @brief Return a node's application-stable identifier.
/// @param node Node to inspect.
/// @return Borrowed identifier for a live node, otherwise a static empty
///         string.
const char *vg_tree_node_get_stable_id(const vg_tree_node_t *node) {
    return vg_tree_node_is_live(node) && node->stable_id ? node->stable_id : "";
}

/// @brief Associate arbitrary user data with a node (not owned — caller manages lifetime).
///
/// @details Frees any previously set user data if owns_user_data was true, then
///          stores data with owns_user_data = false so the caller retains ownership.
///
/// @param node The node to update; must be live.
/// @param data Caller-owned pointer; not freed on retire.
void vg_tree_node_set_data(vg_tree_node_t *node, void *data) {
    if (!vg_tree_node_is_live(node))
        return;

    if (node->owns_user_data && node->user_data) {
        free(node->user_data);
    }
    node->user_data = data;
    node->owns_user_data = false;
}

/// @brief Set the font and size for all node labels, then resync layout metrics.
///
/// @details The font pointer is borrowed. A null font disables label and glyph
///          drawing; a nonpositive size selects the theme's normal size.
/// @param tree The tree view to configure; may be NULL.
/// @param font Font to use for labels; may be `NULL`.
/// @param size Font size in points; if <= 0, the theme's normal size is used.
void vg_treeview_set_font(vg_treeview_t *tree, vg_font_t *font, float size) {
    if (!tree)
        return;

    tree->font = font;
    tree->font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    treeview_sync_metrics(tree);
    tree->base.needs_layout = true;
    tree->base.needs_paint = true;
}

/// @brief Register the callback fired when the selected node changes.
///
/// @param tree      The tree view to configure; may be NULL.
/// @param callback  Called with (widget, node, user_data) on selection change; NULL to clear.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_treeview_set_on_select(vg_treeview_t *tree,
                               vg_tree_select_callback_t callback,
                               void *user_data) {
    if (!tree)
        return;

    tree->on_select = callback;
    tree->on_select_data = user_data;
}

/// @brief Register the callback fired when a node is expanded or collapsed.
///
/// @param tree      The tree view to configure; may be NULL.
/// @param callback  Called with (widget, node, expanded, user_data); NULL to clear.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_treeview_set_on_expand(vg_treeview_t *tree,
                               vg_tree_expand_callback_t callback,
                               void *user_data) {
    if (!tree)
        return;

    tree->on_expand = callback;
    tree->on_expand_data = user_data;
}

/// @brief Register the callback fired when a node is double-clicked or Enter is pressed.
///
/// @param tree      The tree view to configure; may be NULL.
/// @param callback  Called with (widget, node, user_data) on activation; NULL to clear.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_treeview_set_on_activate(vg_treeview_t *tree,
                                 vg_tree_activate_callback_t callback,
                                 void *user_data) {
    if (!tree)
        return;

    tree->on_activate = callback;
    tree->on_activate_data = user_data;
}

//=============================================================================
// Icon Support
//=============================================================================

/// @brief Set the icon displayed when the node is collapsed (or always, if no expanded_icon is
/// set).
///
/// @details Ownership of @p icon transfers to this function. Literal icon text
///          and the previous base icon are cleared; stale-node input destroys
///          the incoming icon instead.
/// @param node The node to update; must be live.
/// @param icon Icon value; VG_ICON_NONE removes the icon.
void vg_tree_node_set_icon(vg_tree_node_t *node, vg_icon_t icon) {
    if (!vg_tree_node_is_live(node)) {
        vg_icon_destroy(&icon);
        return;
    }
    free(node->icon_text);
    node->icon_text = NULL;
    node->icon_text_len = 0;
    free(node->icon_spec);
    node->icon_spec = NULL;
    vg_icon_destroy(&node->icon);
    node->icon = icon;
    node->owner->base.needs_layout = true;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
}

/// @brief Set the icon displayed when the node is expanded; overrides the base icon when visible.
///
/// @details Ownership of @p icon transfers to this function. Stale-node input
///          destroys the incoming icon instead.
/// @param node The node to update; must be live.
/// @param icon Icon shown when node->expanded is true; VG_ICON_NONE falls back to the base icon.
void vg_tree_node_set_expanded_icon(vg_tree_node_t *node, vg_icon_t icon) {
    if (!vg_tree_node_is_live(node)) {
        vg_icon_destroy(&icon);
        return;
    }
    vg_icon_destroy(&node->expanded_icon);
    node->expanded_icon = icon;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
}

//=============================================================================
// Drag and Drop
//=============================================================================

/// @brief Enable or disable drag-and-drop reordering in the tree view.
///
/// @details This flag controls retained drag recognition; callback registration
///          remains independent.
/// @param tree    The tree view to configure; may be NULL.
/// @param enabled true to allow node dragging (requires drag callbacks to be set).
void vg_treeview_set_drag_enabled(vg_treeview_t *tree, bool enabled) {
    if (!tree)
        return;
    tree->drag_enabled = enabled;
}

/// @brief Enable legacy application-directed into-only drag/drop mode.
/// @param tree TreeView to configure; `NULL` is ignored.
/// @param enabled `true` for legacy polling mode, `false` for callback-directed
///        mode with dragging disabled.
void vg_treeview_set_app_directed_dnd(vg_treeview_t *tree, bool enabled) {
    vg_treeview_set_app_directed_dnd_mode(
        tree, enabled ? VG_TREEVIEW_APP_DND_LEGACY_INTO : VG_TREEVIEW_APP_DND_DISABLED);
}

/// @brief Select the application-directed drag/drop policy.
/// @details Changing policy cancels any active drag, releases TreeView input
///          capture, clears pending drop latches, and synchronizes
///          `drag_enabled` with whether the selected mode is disabled.
/// @param tree TreeView to configure; `NULL` is ignored.
/// @param mode One of `VG_TREEVIEW_APP_DND_DISABLED`,
///        `VG_TREEVIEW_APP_DND_LEGACY_INTO`, or
///        `VG_TREEVIEW_APP_DND_ROW_AWARE`; out-of-range values are ignored.
void vg_treeview_set_app_directed_dnd_mode(vg_treeview_t *tree, int mode) {
    if (!tree)
        return;
    if (mode < (int)VG_TREEVIEW_APP_DND_DISABLED || mode > (int)VG_TREEVIEW_APP_DND_ROW_AWARE)
        return;
    vg_treeview_app_dnd_mode_t requested = (vg_treeview_app_dnd_mode_t)mode;
    if (tree->app_directed_dnd_mode == requested &&
        (requested != VG_TREEVIEW_APP_DND_DISABLED || !tree->drag_enabled))
        return;

    if (vg_widget_get_input_capture() == &tree->base)
        vg_widget_release_input_capture();
    tree->drag_node = NULL;
    tree->drop_target = NULL;
    tree->is_dragging = false;
    tree->suppress_click = false;
    tree->drop_latched = false;
    tree->latched_src = NULL;
    tree->latched_tgt = NULL;
    tree->latched_pos = VG_TREE_DROP_INTO;
    tree->app_directed_dnd_mode = requested;
    tree->drag_enabled = requested != VG_TREEVIEW_APP_DND_DISABLED;
    tree->base.needs_paint = true;
}

/// @brief Test whether application-directed drag/drop has latched a drop.
/// @param tree TreeView to inspect; may be `NULL`.
/// @return `true` while an unconsumed source/target/position latch exists.
bool vg_treeview_has_pending_drop(const vg_treeview_t *tree) {
    return tree && tree->drop_latched;
}

/// @brief Read the source node from the pending application-directed drop.
/// @param tree TreeView to inspect; may be `NULL`.
/// @return Borrowed latched source pointer, or `NULL`.
vg_tree_node_t *vg_treeview_drop_source(vg_treeview_t *tree) {
    return tree ? tree->latched_src : NULL;
}

/// @brief Read the target node from the pending application-directed drop.
/// @param tree TreeView to inspect; may be `NULL`.
/// @return Borrowed latched target pointer, or `NULL`.
vg_tree_node_t *vg_treeview_drop_target_node(vg_treeview_t *tree) {
    return tree ? tree->latched_tgt : NULL;
}

/// @brief Read the placement from the pending application-directed drop.
/// @param tree TreeView to inspect; may be `NULL`.
/// @return Latched `vg_tree_drop_position_t` represented as an integer, or
///         `VG_TREE_DROP_INTO` when no tree is supplied.
int vg_treeview_drop_position_value(const vg_treeview_t *tree) {
    return tree ? (int)tree->latched_pos : (int)VG_TREE_DROP_INTO;
}

/// @brief Consume and clear an application-directed drop latch.
/// @param tree TreeView whose latched source, target, and position are reset;
///        `NULL` is ignored.
void vg_treeview_clear_drop(vg_treeview_t *tree) {
    if (!tree)
        return;
    tree->drop_latched = false;
    tree->latched_src = NULL;
    tree->latched_tgt = NULL;
    tree->latched_pos = VG_TREE_DROP_INTO;
}

/// @brief Set all drag-and-drop callbacks and user data in one call.
///
/// @details Predicates are consulted during retained drag recognition and
///          target validation. `on_drop` is bypassed in application-directed
///          modes, where the same result is exposed through polling latches.
/// @param tree      The tree view to configure; may be NULL.
/// @param can_drag  Predicate: return true if a node is draggable; NULL allows all.
/// @param can_drop  Predicate: return true if (source, target, position) is a valid drop; NULL
/// allows all.
/// @param on_drop   Called when a drop is confirmed with (source, target, position, user_data).
/// @param user_data Opaque pointer forwarded to all three callbacks.
void vg_treeview_set_drag_callbacks(vg_treeview_t *tree,
                                    vg_tree_can_drag_callback_t can_drag,
                                    vg_tree_can_drop_callback_t can_drop,
                                    vg_tree_on_drop_callback_t on_drop,
                                    void *user_data) {
    if (!tree)
        return;

    tree->can_drag = can_drag;
    tree->can_drop = can_drop;
    tree->on_drop = on_drop;
    tree->drag_user_data = user_data;
}

//=============================================================================
// Lazy Loading
//=============================================================================

/// @brief Register the callback invoked when an unexpanded node with has_children is expanded.
///
/// @details The callback should call vg_treeview_add_node to populate children and then
///          set node->loading = false and trigger a repaint.
///
/// @param tree      The tree view to configure; may be NULL.
/// @param callback  Called with (tree, node, user_data) on lazy-expand; NULL to disable.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_treeview_set_on_load_children(vg_treeview_t *tree,
                                      vg_tree_load_children_callback_t callback,
                                      void *user_data) {
    if (!tree)
        return;

    tree->on_load_children = callback;
    tree->on_load_children_data = user_data;
}

/// @brief Mark a node as having children without actually adding any, enabling the expand arrow.
///
/// @details When has_children is true and the node is expanded, the lazy-load
///          callback fires if no children have been added yet.
///
/// @param node        The node to update; must be live.
/// @param has_children true to show the expand arrow even if child_count is zero.
void vg_tree_node_set_has_children(vg_tree_node_t *node, bool has_children) {
    if (!vg_tree_node_is_live(node))
        return;
    bool effective = has_children || node->child_count > 0;
    if (node->has_children == effective)
        return;
    node->has_children = effective;
    node->owner->base.needs_layout = true;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
}

/// @brief Set the loading animation state for a node.
///
/// @details When true, the node's icon slot shows three animated dots instead of
///          the normal icon, indicating an async child-load is in progress.
///
/// @param node    The node to update; must be live.
/// @param loading true to show the loading animation; false when children are ready.
void vg_tree_node_set_loading(vg_tree_node_t *node, bool loading) {
    if (!vg_tree_node_is_live(node))
        return;
    if (node->loading == loading)
        return;
    node->loading = loading;
    node->owner->base.needs_paint = true;
    vg_widget_note_revision(&node->owner->base);
}

/// @brief Return whether a live node has materialized or lazily advertised children.
/// @param node Node to inspect.
/// @return `true` when a live node has a child record or its advertised
///         `has_children` flag is set.
bool vg_tree_node_has_children(const vg_tree_node_t *node) {
    return vg_tree_node_is_live(node) && (node->has_children || node->child_count > 0);
}

/// @brief Return whether a live node currently displays the loading indicator.
/// @param node Node to inspect.
/// @return `true` when @p node is live and marked as loading.
bool vg_tree_node_is_loading(const vg_tree_node_t *node) {
    return vg_tree_node_is_live(node) && node->loading;
}

/// @brief Consume the independent lazy-child-request edge.
/// @details Each load-request revision is reported at most once through this
///          compatibility polling API.
/// @param tree Live TreeView to inspect.
/// @return `true` exactly once for each unreported lazy-child request.
bool vg_treeview_was_load_children_requested(vg_treeview_t *tree) {
    if (!tree || !vg_widget_is_live(&tree->base) ||
        tree->reported_load_request_revision == tree->load_request_revision)
        return false;
    tree->reported_load_request_revision = tree->load_request_revision;
    return true;
}

/// @brief Return the most recently requested lazy-child node when it remains live.
/// @param tree Live TreeView to inspect.
/// @return Borrowed requested node owned by @p tree, or `NULL` if the record is
///         absent, stale, or foreign.
vg_tree_node_t *vg_treeview_get_load_requested_node(vg_treeview_t *tree) {
    if (!tree || !vg_widget_is_live(&tree->base) ||
        !vg_tree_node_is_live(tree->last_load_requested) ||
        tree->last_load_requested->owner != tree)
        return NULL;
    return tree->last_load_requested;
}

/// @brief Return the most recently activated node when it remains live.
/// @param tree Live TreeView to inspect.
/// @return Borrowed activated node owned by @p tree, or `NULL` if the record is
///         absent, stale, or foreign.
vg_tree_node_t *vg_treeview_get_activated_node(vg_treeview_t *tree) {
    if (!tree || !vg_widget_is_live(&tree->base) || !vg_tree_node_is_live(tree->last_activated) ||
        tree->last_activated->owner != tree)
        return NULL;
    return tree->last_activated;
}

//=============================================================================
// Inline row editing
//=============================================================================

/// @brief Arrange the row editor over the edited node's current row.
/// @details Coordinates are parent-relative; the walker adds the tree origin.
///          Rows are addressed by flattened visible index, so scrolling simply
///          re-places the editor on the next sync.
/// @param tree Tree with an active edit.
/// @return true when the edited row remains visible in the flattened order.
static bool treeview_edit_place(vg_treeview_t *tree) {
    int current = 0;
    int index = get_node_index(tree->root, tree->edit_node, &current);
    if (index < 0)
        return false;

    float outer_padding = treeview_outer_padding();
    float x = outer_padding + (float)tree->edit_node->depth * tree->indent_size +
              tree->indent_size + tree->icon_size + tree->icon_gap;
    float y = (float)index * tree->row_height - tree->scroll_y;
    float width = tree->base.width - x - outer_padding;
    if (width < 40.0f)
        width = 40.0f;
    vg_widget_arrange(
        &tree->edit_input->base, x, y + 2.0f, width, tree->row_height - 4.0f);
    return true;
}

/// @brief End the active inline edit, optionally latching a commit.
/// @param tree Tree whose edit ends; callers verify edit_active.
/// @param commit true latches the editor text and node for consumption.
static void treeview_edit_finish(vg_treeview_t *tree, bool commit) {
    if (!tree->edit_active)
        return;
    if (commit && tree->edit_input) {
        const char *text = vg_textinput_get_text(tree->edit_input);
        char *copy = text ? vg_strdup(text) : NULL;
        if (copy) {
            free(tree->edit_text);
            tree->edit_text = copy;
            tree->edit_committed = true;
            tree->edit_committed_node = tree->edit_node;
        }
    }
    tree->edit_active = false;
    tree->edit_node = NULL;
    if (tree->edit_input) {
        if (tree->edit_input->base.state & VG_STATE_FOCUSED)
            vg_widget_set_focus(&tree->base);
        vg_widget_set_visible(&tree->edit_input->base, false);
    }
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);
}

/// @brief Per-paint edit upkeep: submit/blur commits, liveness, placement.
/// @details Paint is the widget's reliable poll point — the focused editor
///          repaints continuously (cursor blink) and every focus transition
///          repaints both widgets involved.
/// @param tree Tree to synchronize; inactive trees return immediately.
static void treeview_edit_sync(vg_treeview_t *tree) {
    if (!tree->edit_active)
        return;
    if (!vg_tree_node_is_live(tree->edit_node) || tree->edit_node->owner != tree ||
        !tree->edit_input) {
        treeview_edit_finish(tree, false);
        return;
    }
    if (vg_textinput_was_submitted(tree->edit_input)) {
        treeview_edit_finish(tree, true);
        return;
    }
    if (!(tree->edit_input->base.state & VG_STATE_FOCUSED)) {
        treeview_edit_finish(tree, true);
        return;
    }
    if (!treeview_edit_place(tree))
        treeview_edit_finish(tree, true);
}

/// @brief Begin an inline edit of one visible row (see header contract).
/// @copydetails vg_treeview_begin_edit
bool vg_treeview_begin_edit(vg_treeview_t *tree,
                            vg_tree_node_t *node,
                            const char *initial_text) {
    if (!tree || !vg_widget_is_live(&tree->base) || tree->virtual_mode ||
        !vg_tree_node_is_live(node) || node->owner != tree)
        return false;

    if (tree->edit_active)
        treeview_edit_finish(tree, true);

    if (!tree->edit_input) {
        tree->edit_input = vg_textinput_create(&tree->base);
        if (!tree->edit_input)
            return false;
    }
    if (tree->font)
        vg_textinput_set_font(tree->edit_input, tree->font, tree->font_size);

    vg_treeview_scroll_to(tree, node);
    tree->edit_node = node;
    tree->edit_active = true;
    tree->edit_committed = false;
    tree->edit_committed_node = NULL;

    vg_textinput_set_text(tree->edit_input, initial_text ? initial_text : "");
    vg_textinput_select_all(tree->edit_input);
    vg_widget_set_visible(&tree->edit_input->base, true);
    if (!treeview_edit_place(tree)) {
        treeview_edit_finish(tree, false);
        return false;
    }
    vg_widget_set_focus(&tree->edit_input->base);
    /* Consume any stale submit latched before this edit began. */
    (void)vg_textinput_was_submitted(tree->edit_input);
    tree->base.needs_paint = true;
    vg_widget_note_revision(&tree->base);
    return true;
}

/// @brief Return whether an inline row edit is in progress (see header).
/// @copydetails vg_treeview_is_editing
bool vg_treeview_is_editing(const vg_treeview_t *tree) {
    return tree && tree->edit_active;
}

/// @brief Consume the latched inline-edit commit edge (see header).
/// @copydetails vg_treeview_was_edit_committed
bool vg_treeview_was_edit_committed(vg_treeview_t *tree) {
    if (!tree || !tree->edit_committed)
        return false;
    tree->edit_committed = false;
    return true;
}

/// @brief Return the most recently committed inline-edit text (see header).
/// @copydetails vg_treeview_get_edit_text
const char *vg_treeview_get_edit_text(const vg_treeview_t *tree) {
    return tree ? tree->edit_text : NULL;
}

/// @brief Return the node whose inline edit most recently committed (see header).
/// @copydetails vg_treeview_get_edited_node
vg_tree_node_t *vg_treeview_get_edited_node(vg_treeview_t *tree) {
    if (!tree || !vg_tree_node_is_live(tree->edit_committed_node) ||
        tree->edit_committed_node->owner != tree)
        return NULL;
    return tree->edit_committed_node;
}

/// @brief Cancel any inline edit in progress without committing (see header).
/// @copydetails vg_treeview_cancel_edit
void vg_treeview_cancel_edit(vg_treeview_t *tree) {
    if (!tree)
        return;
    treeview_edit_finish(tree, false);
}
