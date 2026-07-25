//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_scene.h
// Purpose: Scene graph for hierarchical sprite management with local and world transforms computed
// by composing ancestor transforms.
//
// Key invariants:
//   - Each scene node has at most one parent; the root has no parent.
//   - World position applies ancestor scale and rotation to local offsets before
//     translation; scale multiplies as percentages and rotation adds in degrees.
//   - Adding a child retains it in the parent's owning child sequence and
//     automatically detaches it from any prior parent.
//   - Node names need not be unique; lookup returns the first exact pre-order
//     match.
//   - Subtree draws use pre-order, while Scene draws apply one stable global
//     depth ordering across all visible sprite nodes.
//
// Ownership/Lifetime:
//   - Scene and SceneNode objects are runtime reference-counted.
//   - The scene owns its root and each parent retains its direct children.
//     Detachment releases parent ownership and may finalize an otherwise
//     unreferenced subtree.
//   - Root, parent, child, name, sprite, and direct find getters return borrowed
//     references. Option-returning find functions retain a match in `Some`.
//
// Links: src/runtime/graphics/2d/rt_scene.c (implementation),
//        src/runtime/graphics/2d/rt_camera.h (view transform),
//        src/runtime/graphics/2d/rt_sprite.h (renderable payload),
//        src/runtime/graphics/2d/rt_graphics2d.h (Canvas surface)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the retained hierarchical 2D sprite scene API.
///
/// Invalid opaque handles are soft failures for this legacy surface: getters
/// return documented neutral values and mutators do nothing. Allocation and
/// detected hierarchy/transform corruption may still report runtime traps.
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Runtime class identifier stored in Scene object headers.
#define RT_SCENE_CLASS_ID INT64_C(-0x520105)
/// Runtime class identifier stored in SceneNode object headers.
#define RT_SCENE_NODE_CLASS_ID INT64_C(-0x520106)

//=========================================================================
// Scene Node Creation
//=========================================================================

/// @brief Create a new scene node.
/// @details Initializes local identity transform, depth zero, visible state,
///          an empty retained name, no sprite or parent, and an owning empty
///          child sequence.
/// @return New caller-owned SceneNode object, or null after allocation failure.
void *rt_scene_node_new(void);

/// @brief Create a scene node with a sprite.
/// @param sprite Optional valid Sprite handle to retain on the new node.
/// @return New caller-owned SceneNode object; null only when node allocation
///         fails. Invalid nonnull sprite input traps and leaves the new node
///         unattached.
void *rt_scene_node_from_sprite(void *sprite);

//=========================================================================
// Scene Node Properties
//=========================================================================

/// @brief Get node local X position (relative to parent).
/// @param node The SceneNode object.
/// @return The node's local X position in pixels.
int64_t rt_scene_node_get_x(void *node);

/// @brief Set node local X position (relative to parent).
/// @param node The SceneNode object.
/// @param x The new local X position in pixels.
void rt_scene_node_set_x(void *node, int64_t x);

/// @brief Get node local Y position (relative to parent).
/// @param node The SceneNode object.
/// @return The node's local Y position in pixels.
int64_t rt_scene_node_get_y(void *node);

/// @brief Set node local Y position (relative to parent).
/// @param node The SceneNode object.
/// @param y The new local Y position in pixels.
void rt_scene_node_set_y(void *node, int64_t y);

/// @brief Get node world X position (absolute).
/// @details Lazily composes all dirty ancestor transforms, including parent
///          scale and rotation of local offsets.
/// @param node The SceneNode object.
/// @return The node's computed world X position, accounting for all
///         ancestor translations.
int64_t rt_scene_node_get_world_x(void *node);

/// @brief Get node world Y position (absolute).
/// @details Lazily composes all dirty ancestor transforms, including parent
///          scale and rotation of local offsets.
/// @param node The SceneNode object.
/// @return The node's computed world Y position, accounting for all
///         ancestor translations.
int64_t rt_scene_node_get_world_y(void *node);

/// @brief Get node local scale X (100 = 100%).
/// @param node The SceneNode object.
/// @return The node's local X scale factor (100 = unscaled).
int64_t rt_scene_node_get_scale_x(void *node);

/// @brief Set node local scale X (100 = 100%).
/// @param node The SceneNode object.
/// @param scale The new local X scale factor; values below one clamp to one
///              (100 = unscaled).
void rt_scene_node_set_scale_x(void *node, int64_t scale);

/// @brief Get node local scale Y (100 = 100%).
/// @param node The SceneNode object.
/// @return The node's local Y scale factor (100 = unscaled).
int64_t rt_scene_node_get_scale_y(void *node);

/// @brief Set node local scale Y (100 = 100%).
/// @param node The SceneNode object.
/// @param scale The new local Y scale factor; values below one clamp to one
///              (100 = unscaled).
void rt_scene_node_set_scale_y(void *node, int64_t scale);

/// @brief Get node world scale X (combined with ancestors).
/// @param node The SceneNode object.
/// @return The node's computed world X scale, combining all ancestor
///         scale factors.
int64_t rt_scene_node_get_world_scale_x(void *node);

/// @brief Get node world scale Y (combined with ancestors).
/// @param node The SceneNode object.
/// @return The node's computed world Y scale, combining all ancestor
///         scale factors.
int64_t rt_scene_node_get_world_scale_y(void *node);

/// @brief Get node local rotation in degrees.
/// @param node The SceneNode object.
/// @return The node's local rotation angle in degrees.
int64_t rt_scene_node_get_rotation(void *node);

/// @brief Set node local rotation in degrees.
/// @param node The SceneNode object.
/// @param degrees The new local rotation angle in degrees.
void rt_scene_node_set_rotation(void *node, int64_t degrees);

/// @brief Get node world rotation (combined with ancestors).
/// @param node The SceneNode object.
/// @return The node's computed world rotation in degrees, combining
///         all ancestor rotations.
int64_t rt_scene_node_get_world_rotation(void *node);

/// @brief Get node visibility.
/// @param node The SceneNode object.
/// @return 1 if the node is visible, 0 if hidden.
int8_t rt_scene_node_get_visible(void *node);

/// @brief Set node visibility (affects children too).
/// @details Changes only this node's stored flag. A hidden node prunes its
///          complete subtree during drawing without rewriting descendant
///          flags; updates still traverse hidden nodes.
/// @param node The SceneNode object.
/// @param visible 1 to make visible, 0 to hide (hides all descendants).
void rt_scene_node_set_visible(void *node, int8_t visible);

/// @brief Get node depth (Z-order for sorting).
/// @param node The SceneNode object.
/// @return The node's depth value used for draw-order sorting.
int64_t rt_scene_node_get_depth(void *node);

/// @brief Set node depth (higher values drawn later/on top).
/// @param node The SceneNode object.
/// @param depth The new depth value; higher values are drawn on top
///              of lower values during scene rendering.
void rt_scene_node_set_depth(void *node, int64_t depth);

/// @brief Get node name/tag for identification.
/// @param node The SceneNode object.
/// @return Borrowed node name string, or a borrowed empty constant for invalid
///         input. Do not release the returned reference.
rt_string rt_scene_node_get_name(void *node);

/// @brief Set node name/tag for identification.
/// @details Retains @p name before releasing the previous value; null assigns
///          an empty string. Names are not required to be unique.
/// @param node The SceneNode object.
/// @param name The name string to assign to this node.
void rt_scene_node_set_name(void *node, rt_string name);

/// @brief Get the sprite attached to this node.
/// @param node The SceneNode object.
/// @return Borrowed attached Sprite object, or NULL if absent or invalid.
///         Retain it separately if it must outlive the node.
void *rt_scene_node_get_sprite(void *node);

/// @brief Attach a sprite to this node.
/// @details Retains the new Sprite before releasing the previous one. An
///          invalid nonnull runtime object traps and leaves the old attachment.
/// @param node The SceneNode object.
/// @param sprite The sprite to attach, or NULL to detach the current sprite.
void rt_scene_node_set_sprite(void *node, void *sprite);

//=========================================================================
// Scene Node Hierarchy
//=========================================================================

/// @brief Add a child node.
/// @details Rejects self/ancestor cycles and corrupt over-deep parent chains.
///          The new parent's child sequence retains the child. Reparenting
///          first detaches from the old parent while a temporary retain keeps
///          the handle alive.
/// @param node Parent node.
/// @param child Child node to add. The child is detached from any previous
///              parent before being added.
void rt_scene_node_add_child(void *node, void *child);

/// @brief Remove a child node.
/// @details Releases the parent's retained reference after clearing the parent
///          link. The child or its subtree may finalize immediately if no
///          other owner retains it.
/// @param node Parent node.
/// @param child Child node to remove. The child's parent becomes NULL.
void rt_scene_node_remove_child(void *node, void *child);

/// @brief Get the number of children.
/// @param node The SceneNode object.
/// @return The number of direct child nodes.
int64_t rt_scene_node_child_count(void *node);

/// @brief Get a child by index.
/// @param node The parent SceneNode.
/// @param index Zero-based index of the child (0 to child_count-1).
/// @return Borrowed child SceneNode at @p index, or NULL if out of range or
///         invalid. Do not release the borrowed reference.
void *rt_scene_node_get_child(void *node, int64_t index);

/// @brief Get the parent node.
/// @param node The SceneNode object.
/// @return Borrowed parent SceneNode, or NULL if this node has no parent or is
///         invalid.
void *rt_scene_node_get_parent(void *node);

/// @brief Find a descendant node by name.
/// @details Searches the start node and descendants in iterative depth-first
///          pre-order using exact byte-string equality. Duplicate names return
///          the first traversal match.
/// @param node Starting node to search from (searches this node's subtree).
/// @param name Name to search for.
/// @return Borrowed first matching node, or NULL if not found.
void *rt_scene_node_find(void *node, rt_string name);

/// @brief Find a descendant node by name as an Option.
/// @details Returns `Some(SceneNode)` when a match exists and `None` when
///          absent, avoiding the legacy NULL sentinel. `Some` retains the
///          matched node for the Option's lifetime.
/// @param node Starting node to search from.
/// @param name Name to search for.
/// @return Opaque Zanna.Option containing the matching node, or None.
void *rt_scene_node_find_option(void *node, rt_string name);

/// @brief Remove this node from its parent.
/// @details Releases parent ownership and may finalize the node if no other
///          strong reference exists.
/// @param node The SceneNode to detach. After detaching, the node's
///             parent becomes NULL.
void rt_scene_node_detach(void *node);

//=========================================================================
// Scene Node Methods
//=========================================================================

/// @brief Draw this node and all children to a canvas.
/// @details Traverses visible nodes in parent-before-children insertion order
///          and does not apply the Scene container's global depth sort.
/// @param node Node to draw.
/// @param canvas Canvas to draw on.
void rt_scene_node_draw(void *node, void *canvas);

/// @brief Draw this node and children with camera transform.
/// @details Uses the same pre-order traversal as `rt_scene_node_draw()`.
///          Camera zoom multiplies world scale and camera rotation is
///          subtracted from node rotation; null camera is identity.
/// @param node Node to draw.
/// @param canvas Canvas to draw on.
/// @param camera Camera for world-to-screen transform.
void rt_scene_node_draw_with_camera(void *node, void *canvas, void *camera);

/// @brief Update node and all children (for animations).
/// @details Iteratively advances every attached sprite once, including hidden
///          subtrees.
/// @param node The SceneNode to update. Recursively updates all
///             descendant nodes.
void rt_scene_node_update(void *node);

/// @brief Move the node by delta amounts.
/// @param node The SceneNode to move.
/// @param dx Horizontal displacement in pixels to add to the current
///           X position.
/// @param dy Vertical displacement in pixels to add to the current
///           Y position.
void rt_scene_node_move(void *node, int64_t dx, int64_t dy);

/// @brief Set both position components at once.
/// @param node The SceneNode object.
/// @param x The new local X position in pixels.
/// @param y The new local Y position in pixels.
void rt_scene_node_set_position(void *node, int64_t x, int64_t y);

/// @brief Set both scale components at once.
/// @param node The SceneNode object.
/// @param scale The uniform scale factor to apply to both X and Y; values below
///              one clamp to one (100 = unscaled).
void rt_scene_node_set_scale(void *node, int64_t scale);

//=========================================================================
// Scene (Root Container)
//=========================================================================

/// @brief Create a new scene (root container for nodes).
/// @details Creates and owns an identity root named `"root"` plus reusable
///          depth-sort scratch.
/// @return A new caller-owned Scene object, or null after allocation failure.
void *rt_scene_new(void);

/// @brief Get the root node of a scene.
/// @param scene The Scene object.
/// @return Borrowed root SceneNode serving as the hierarchy top, or null for
///         invalid input.
void *rt_scene_get_root(void *scene);

/// @brief Add a node to the scene root.
/// @details Equivalent to `rt_scene_node_add_child(rt_scene_get_root(scene),
///          node)` and therefore reparents and retains on success.
/// @param scene The Scene object.
/// @param node The SceneNode to add as a child of the scene root.
void rt_scene_add(void *scene, void *node);

/// @brief Remove a node from the scene.
/// @details Removes only a direct child of the implicit root. Descendants must
///          be detached from their actual parent.
/// @param scene The Scene object.
/// @param node The SceneNode to remove from the scene root.
void rt_scene_remove(void *scene, void *node);

/// @brief Find a node in the scene by name.
/// @details Includes the implicit root and returns the first exact depth-first
///          pre-order match.
/// @param scene The Scene object.
/// @param name The name to search for in the entire scene hierarchy.
/// @return Borrowed first matching SceneNode, or NULL if not found.
void *rt_scene_find(void *scene, rt_string name);

/// @brief Find a node in the scene by name as an Option.
/// @details Returns `Some(SceneNode)` when a match exists and `None` when
///          absent. `Some` retains the matched node for the Option's lifetime.
/// @param scene The Scene object.
/// @param name The name to search for in the entire scene hierarchy.
/// @return Opaque Zanna.Option containing the matching node, or None.
void *rt_scene_find_option(void *scene, rt_string name);

/// @brief Draw all nodes in the scene (depth-sorted).
/// @details Collects visible nodes with sprites, prunes hidden subtrees, and
///          sorts globally by ascending depth with traversal order as an
///          explicit tie-breaker. Scratch allocation is retained by the scene
///          for later frames.
/// @param scene The Scene object.
/// @param canvas The canvas to draw all visible nodes onto, sorted
///               by depth. Equal depths preserve traversal order.
void rt_scene_draw(void *scene, void *canvas);

/// @brief Draw scene with camera transform (depth-sorted).
/// @details Uses the same global ordering as `rt_scene_draw()`, then applies
///          camera world-to-screen translation, zoom, and inverse view
///          rotation. Null camera uses identity view.
/// @param scene The Scene object.
/// @param canvas The canvas to draw onto.
/// @param camera The camera providing the world-to-screen transform. Equal
///               depths preserve traversal order.
void rt_scene_draw_with_camera(void *scene, void *canvas, void *camera);

/// @brief Update all nodes in the scene.
/// @param scene The Scene object. Iteratively updates all nodes in the
///              hierarchy, including hidden subtrees.
void rt_scene_update(void *scene);

/// @brief Get the number of direct children of the scene's root node.
/// @param scene The Scene object.
/// @return The count of top-level nodes (direct children of the implicit root);
///         nested descendants are not included. 0 if @p scene is invalid.
int64_t rt_scene_node_count(void *scene);

/// @brief Clear all nodes from the scene.
/// @details Clears direct-child parent links before releasing the root's
///          owning Seq entries. Descendants are released transitively, while
///          externally retained nodes survive detached.
/// @param scene The Scene object whose root children are removed.
void rt_scene_clear(void *scene);

#ifdef __cplusplus
}
#endif
