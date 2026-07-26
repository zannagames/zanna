//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_disabled_runtime_stubs.c
/// @brief Supplies deterministic fallback symbols for graphics-disabled runtime builds.
///
/// @details
/// This translation unit preserves ABI compatibility for GUI, command,
/// rendering, scene, asset, physics, and navigation helpers that are normally
/// provided by graphics-enabled components. Mutators retain no state, queries
/// return explicit empty defaults, and mutation probes report failure.
///
// File: src/runtime/graphics/common/rt_disabled_runtime_stubs.c
// Purpose: Supplemental exported runtime stubs for graphics-disabled builds.
// Key invariants:
//   - Every supplemental public symbol remains link-compatible with the full runtime.
//   - Scene mutation probes return failure when no graphics scene graph exists.
// Ownership/Lifetime:
//   - Stubs retain no graphics handles and allocate only documented fallback values.
// Links: src/runtime/graphics/3d/scene/rt_scene3d.h,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md
//
//===----------------------------------------------------------------------===//

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

typedef struct rt_gltf_preload_bundle rt_gltf_preload_bundle;

/// @brief Create an owned empty runtime string for disabled-feature fallbacks.
///
/// @return A newly created empty runtime string.
static rt_string disabled_empty_string(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Ignore a preferred-size update when the GUI runtime is unavailable.
/// @param widget Widget handle (ignored).
/// @param width Preferred width (ignored).
/// @param height Preferred height (ignored).
void rt_widget_set_preferred_size(void *widget, double width, double height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Ignore a maximum-size update when the GUI runtime is unavailable.
/// @param widget Widget handle (ignored).
/// @param width Maximum width (ignored).
/// @param height Maximum height (ignored).
void rt_widget_set_max_size(void *widget, double width, double height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Ignore a progress-bar style update in a disabled GUI build.
/// @param progress ProgressBar handle (ignored).
/// @param style Style identifier (ignored).
void rt_progressbar_set_style(void *progress, int64_t style) {
    (void)progress;
    (void)style;
}

/// @brief Ignore a progress-bar percentage-visibility update.
/// @param progress ProgressBar handle (ignored).
/// @param show Non-zero to show the percentage label (ignored).
void rt_progressbar_show_percentage(void *progress, int64_t show) {
    (void)progress;
    (void)show;
}

/// @brief Return no GUI test harness when graphics and GUI support are disabled.
/// @return `NULL`.
void *rt_gui_test_harness_new(void) {
    return NULL;
}

/// @brief Ignore a request to clear a disabled GUI test harness.
/// @param harness GUI test-harness handle (ignored).
void rt_gui_test_harness_clear(void *harness) {
    (void)harness;
}

/// @brief Report that a disabled GUI test harness advanced no work.
/// @param harness GUI test-harness handle (ignored).
/// @param ms Elapsed virtual time in milliseconds (ignored).
/// @return `0`.
int64_t rt_gui_test_harness_tick(void *harness, int64_t ms) {
    (void)harness;
    (void)ms;
    return 0;
}

/// @brief Ignore registration of a widget with the disabled GUI test harness.
/// @param harness GUI test-harness handle (ignored).
/// @param id Stable widget identifier (ignored).
/// @param name Widget name (ignored).
/// @param type Widget type name (ignored).
/// @param x Widget X coordinate (ignored).
/// @param y Widget Y coordinate (ignored).
/// @param w Widget width (ignored).
/// @param h Widget height (ignored).
void rt_gui_test_harness_register_widget(void *harness,
                                         rt_string id,
                                         rt_string name,
                                         rt_string type,
                                         int64_t x,
                                         int64_t y,
                                         int64_t w,
                                         int64_t h) {
    (void)harness;
    (void)id;
    (void)name;
    (void)type;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

/// @brief Return no widget for an identifier lookup in the disabled harness.
/// @param harness GUI test-harness handle (ignored).
/// @param id Widget identifier (ignored).
/// @return `NULL`.
void *rt_gui_test_harness_find_by_id(void *harness, rt_string id) {
    (void)harness;
    (void)id;
    return NULL;
}

/// @brief Return no widget for a name lookup in the disabled harness.
/// @param harness GUI test-harness handle (ignored).
/// @param name Widget name (ignored).
/// @return `NULL`.
void *rt_gui_test_harness_find_by_name(void *harness, rt_string name) {
    (void)harness;
    (void)name;
    return NULL;
}

/// @brief Return no widget for a type lookup in the disabled harness.
/// @param harness GUI test-harness handle (ignored).
/// @param type Widget type name (ignored).
/// @return `NULL`.
void *rt_gui_test_harness_find_by_type(void *harness, rt_string type) {
    (void)harness;
    (void)type;
    return NULL;
}

/// @brief Ignore synthetic key input sent to a disabled GUI harness.
/// @param harness GUI test-harness handle (ignored).
/// @param id Target widget identifier (ignored).
/// @param key Key code (ignored).
void rt_gui_test_harness_send_key(void *harness, rt_string id, int64_t key) {
    (void)harness;
    (void)id;
    (void)key;
}

/// @brief Ignore synthetic mouse input sent to a disabled GUI harness.
/// @param harness GUI test-harness handle (ignored).
/// @param id Target widget identifier (ignored).
/// @param x Mouse X coordinate (ignored).
/// @param y Mouse Y coordinate (ignored).
/// @param buttons Mouse-button bitmask (ignored).
void rt_gui_test_harness_send_mouse(
    void *harness, rt_string id, int64_t x, int64_t y, int64_t buttons) {
    (void)harness;
    (void)id;
    (void)x;
    (void)y;
    (void)buttons;
}

/// @brief Return the empty focus identifier from a disabled GUI harness.
/// @param harness GUI test-harness handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_gui_test_harness_get_focus(void *harness) {
    (void)harness;
    return disabled_empty_string();
}

/// @brief Return no focus-order collection from a disabled GUI harness.
/// @param harness GUI test-harness handle (ignored).
/// @return `NULL`.
void *rt_gui_test_harness_focus_order(void *harness) {
    (void)harness;
    return NULL;
}

/// @brief Return no framebuffer capture from a disabled GUI harness.
/// @param harness GUI test-harness handle (ignored).
/// @param x Capture-region X coordinate (ignored).
/// @param y Capture-region Y coordinate (ignored).
/// @param w Capture width (ignored).
/// @param h Capture height (ignored).
/// @return `NULL`.
void *rt_gui_test_harness_capture_region(
    void *harness, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)harness;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return NULL;
}

/// @brief Report that a disabled GUI harness has no nonblank capture.
/// @param harness GUI test-harness handle (ignored).
/// @return `0`.
int8_t rt_gui_test_harness_assert_nonblank(void *harness) {
    (void)harness;
    return 0;
}

/// @brief Return no virtual-list model when GUI support is disabled.
/// @param row_count Logical row count (ignored).
/// @param row_height Row height in pixels (ignored).
/// @param viewport_height Viewport height in pixels (ignored).
/// @return `NULL`.
void *rt_virtual_list_new(int64_t row_count, int64_t row_height, int64_t viewport_height) {
    (void)row_count;
    (void)row_height;
    (void)viewport_height;
    return NULL;
}

/// @brief Ignore a virtual-list row-count update.
/// @param list Virtual-list handle (ignored).
/// @param count New row count (ignored).
void rt_virtual_list_set_count(void *list, int64_t count) {
    (void)list;
    (void)count;
}

/// @brief Ignore assignment of a stable identifier to a virtual-list row.
/// @param list Virtual-list handle (ignored).
/// @param row Row index (ignored).
/// @param id Stable row identifier (ignored).
void rt_virtual_list_set_row_id(void *list, int64_t row, rt_string id) {
    (void)list;
    (void)row;
    (void)id;
}

/// @brief Return no visible-range descriptor from a disabled virtual list.
/// @param list Virtual-list handle (ignored).
/// @param scroll_y Vertical scroll offset (ignored).
/// @return `NULL`.
void *rt_virtual_list_visible_range(void *list, int64_t scroll_y) {
    (void)list;
    (void)scroll_y;
    return NULL;
}

/// @brief Ignore virtual-list selection by stable identifier.
/// @param list Virtual-list handle (ignored).
/// @param id Row identifier (ignored).
void rt_virtual_list_select_id(void *list, rt_string id) {
    (void)list;
    (void)id;
}

/// @brief Return the empty selected-row identifier from a disabled virtual list.
/// @param list Virtual-list handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_virtual_list_get_selected_id(void *list) {
    (void)list;
    return disabled_empty_string();
}

/// @brief Return the invalid selected index from a disabled virtual list.
/// @param list Virtual-list handle (ignored).
/// @return `-1`.
int64_t rt_virtual_list_get_selected_index(void *list) {
    (void)list;
    return -1;
}

/// @brief Return no virtual-tree model when GUI support is disabled.
/// @return `NULL`.
void *rt_virtual_tree_new(void) {
    return NULL;
}

/// @brief Ignore insertion of a node into a disabled virtual tree.
/// @param tree Virtual-tree handle (ignored).
/// @param id Stable node identifier (ignored).
/// @param parent Parent identifier, or empty for a root (ignored).
/// @param label Display label (ignored).
void rt_virtual_tree_add_node(void *tree, rt_string id, rt_string parent, rt_string label) {
    (void)tree;
    (void)id;
    (void)parent;
    (void)label;
}

/// @brief Return no expansion result for a disabled virtual-tree node.
/// @param tree Virtual-tree handle (ignored).
/// @param id Node identifier (ignored).
/// @return `NULL`.
void *rt_virtual_tree_expand(void *tree, rt_string id) {
    (void)tree;
    (void)id;
    return NULL;
}

/// @brief Ignore collapse of a disabled virtual-tree node.
/// @param tree Virtual-tree handle (ignored).
/// @param id Node identifier (ignored).
void rt_virtual_tree_collapse(void *tree, rt_string id) {
    (void)tree;
    (void)id;
}

/// @brief Ignore virtual-tree selection by stable identifier.
/// @param tree Virtual-tree handle (ignored).
/// @param id Node identifier (ignored).
void rt_virtual_tree_select_id(void *tree, rt_string id) {
    (void)tree;
    (void)id;
}

/// @brief Return the empty selected-node identifier from a disabled virtual tree.
/// @param tree Virtual-tree handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_virtual_tree_get_selected_id(void *tree) {
    (void)tree;
    return disabled_empty_string();
}

/// @brief Return no visible-row collection from a disabled virtual tree.
/// @param tree Virtual-tree handle (ignored).
/// @return `NULL`.
void *rt_virtual_tree_visible_rows(void *tree) {
    (void)tree;
    return NULL;
}

/// @brief Ignore refresh of a disabled virtual-tree subtree.
/// @param tree Virtual-tree handle (ignored).
/// @param id Subtree root identifier (ignored).
void rt_virtual_tree_refresh_subtree(void *tree, rt_string id) {
    (void)tree;
    (void)id;
}

/// @brief Return no command-state model when GUI command support is disabled.
/// @param id Stable command identifier (ignored).
/// @param label Accessible display label (ignored).
/// @return `NULL`.
void *rt_command_state_new(rt_string id, rt_string label) {
    (void)id;
    (void)label;
    return NULL;
}

/// @brief Ignore an enabled-state update on a disabled command state.
/// @param state Command-state handle (ignored).
/// @param enabled Requested enabled flag (ignored).
void rt_command_state_set_enabled(void *state, int8_t enabled) {
    (void)state;
    (void)enabled;
}

/// @brief Report that a disabled command state is not enabled.
/// @param state Command-state handle (ignored).
/// @return `0`.
int8_t rt_command_state_get_enabled(void *state) {
    (void)state;
    return 0;
}

/// @brief Ignore a checked-state update on a disabled command state.
/// @param state Command-state handle (ignored).
/// @param checked Requested checked flag (ignored).
void rt_command_state_set_checked(void *state, int8_t checked) {
    (void)state;
    (void)checked;
}

/// @brief Report that a disabled command state is not checked.
/// @param state Command-state handle (ignored).
/// @return `0`.
int8_t rt_command_state_get_checked(void *state) {
    (void)state;
    return 0;
}

/// @brief Ignore accessible text assigned to a disabled command state.
/// @param state Command-state handle (ignored).
/// @param label Accessible label (ignored).
/// @param description Accessible description (ignored).
void rt_command_state_set_accessible(void *state, rt_string label, rt_string description) {
    (void)state;
    (void)label;
    (void)description;
}

/// @brief Return no serialized snapshot for a disabled command state.
/// @param state Command-state handle (ignored).
/// @return `NULL`.
void *rt_command_state_snapshot(void *state) {
    (void)state;
    return NULL;
}

/// @brief Return no command object when GUI command support is disabled.
/// @param id Stable command identifier (ignored).
/// @param title Display title (ignored).
/// @return `NULL`.
void *rt_command_new(rt_string id, rt_string title) {
    (void)id;
    (void)title;
    return NULL;
}

/// @brief Return the empty identifier for a disabled command.
/// @param command Command handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_command_get_id(void *command) {
    (void)command;
    return disabled_empty_string();
}

/// @brief Return the empty title for a disabled command.
/// @param command Command handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_command_get_title(void *command) {
    (void)command;
    return disabled_empty_string();
}

/// @brief Ignore assignment of a keyboard shortcut to a disabled command.
/// @param command Command handle (ignored).
/// @param keys Shortcut description (ignored).
void rt_command_set_shortcut(void *command, rt_string keys) {
    (void)command;
    (void)keys;
}

/// @brief Return the empty shortcut for a disabled command.
/// @param command Command handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_command_get_shortcut(void *command) {
    (void)command;
    return disabled_empty_string();
}

/// @brief Ignore an enabled-state update on a disabled command.
/// @param command Command handle (ignored).
/// @param enabled Requested enabled flag (ignored).
void rt_command_set_enabled(void *command, int8_t enabled) {
    (void)command;
    (void)enabled;
}

/// @brief Report that a disabled command is not enabled.
/// @param command Command handle (ignored).
/// @return `0`.
int8_t rt_command_is_enabled(void *command) {
    (void)command;
    return 0;
}

/// @brief Ignore a checkable-state update on a disabled command.
/// @param command Command handle (ignored).
/// @param checkable Requested checkable flag (ignored).
void rt_command_set_checkable(void *command, int8_t checkable) {
    (void)command;
    (void)checkable;
}

/// @brief Report that a disabled command is not checkable.
/// @param command Command handle (ignored).
/// @return `0`.
int8_t rt_command_is_checkable(void *command) {
    (void)command;
    return 0;
}

/// @brief Ignore a checked-state update on a disabled command.
/// @param command Command handle (ignored).
/// @param checked Requested checked flag (ignored).
void rt_command_set_checked(void *command, int8_t checked) {
    (void)command;
    (void)checked;
}

/// @brief Report that a disabled command is not checked.
/// @param command Command handle (ignored).
/// @return `0`.
int8_t rt_command_is_checked(void *command) {
    (void)command;
    return 0;
}

/// @brief Ignore binding a disabled command to a menu item.
/// @param command Command handle (ignored).
/// @param item Menu-item handle (ignored).
void rt_command_bind_menu_item(void *command, void *item) {
    (void)command;
    (void)item;
}

/// @brief Ignore binding a disabled command to a toolbar item.
/// @param command Command handle (ignored).
/// @param item Toolbar-item handle (ignored).
void rt_command_bind_toolbar_item(void *command, void *item) {
    (void)command;
    (void)item;
}

/// @brief Report no pending invocation while polling a disabled command.
/// @param command Command handle (ignored).
/// @return `0`.
int8_t rt_command_poll(void *command) {
    (void)command;
    return 0;
}

/// @brief Report that a disabled command was not invoked.
/// @param command Command handle (ignored).
/// @return `0`.
int8_t rt_command_was_invoked(void *command) {
    (void)command;
    return 0;
}

/// @brief Return no serialized snapshot for a disabled command.
/// @param command Command handle (ignored).
/// @return `NULL`.
void *rt_command_snapshot(void *command) {
    (void)command;
    return NULL;
}

/// @brief Return no command registry when GUI command support is disabled.
/// @return `NULL`.
void *rt_command_registry_new(void) {
    return NULL;
}

/// @brief Ignore insertion into a disabled command registry.
/// @param registry Command-registry handle (ignored).
/// @param command Command handle (ignored).
void rt_command_registry_add(void *registry, void *command) {
    (void)registry;
    (void)command;
}

/// @brief Return the command count of a disabled registry.
/// @param registry Command-registry handle (ignored).
/// @return `0`.
int64_t rt_command_registry_count(void *registry) {
    (void)registry;
    return 0;
}

/// @brief Return no command from a disabled registry lookup.
/// @param registry Command-registry handle (ignored).
/// @param id Command identifier (ignored).
/// @return `NULL`.
void *rt_command_registry_find(void *registry, rt_string id) {
    (void)registry;
    (void)id;
    return NULL;
}

/// @brief Ignore binding a disabled command registry to a palette.
/// @param registry Command-registry handle (ignored).
/// @param palette Command-palette handle (ignored).
void rt_command_registry_bind_palette(void *registry, void *palette) {
    (void)registry;
    (void)palette;
}

/// @brief Return no invoked command identifier from a disabled registry.
/// @param registry Command-registry handle (ignored).
/// @return An owned empty runtime string.
rt_string rt_command_registry_poll(void *registry) {
    (void)registry;
    return disabled_empty_string();
}

/// @brief Ignore clearing a disabled command registry.
/// @param registry Command-registry handle (ignored).
void rt_command_registry_clear(void *registry) {
    (void)registry;
}

/// @brief Return the neutral fallback contrast ratio for unavailable accessibility color math.
/// @param fg_rgb Packed foreground RGB color (ignored).
/// @param bg_rgb Packed background RGB color (ignored).
/// @return `1.0`.
double rt_accessibility_contrast_ratio(int64_t fg_rgb, int64_t bg_rgb) {
    (void)fg_rgb;
    (void)bg_rgb;
    return 1.0;
}

/// @brief Evaluate a contrast threshold against the neutral fallback ratio.
/// @param fg_rgb Packed foreground RGB color (ignored).
/// @param bg_rgb Packed background RGB color (ignored).
/// @param min_ratio Required contrast ratio.
/// @return `1` when `min_ratio` is at most `1.0`; otherwise `0`.
int8_t rt_accessibility_meets_contrast(int64_t fg_rgb, int64_t bg_rgb, double min_ratio) {
    (void)fg_rgb;
    (void)bg_rgb;
    return min_ratio <= 1.0 ? 1 : 0;
}

/// @brief Return no high-contrast token map in a disabled GUI build.
/// @return `NULL`.
void *rt_accessibility_high_contrast_tokens(void) {
    return NULL;
}

/// @brief Ignore entry into a Canvas3D overlay pass.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_begin_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore completion of a Canvas3D overlay pass.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_end_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore clearing a disabled Canvas3D overlay.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_clear_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore finalization of a disabled Canvas3D frame.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_finalize_frame(void *obj) {
    (void)obj;
}

/// @brief Return the fallback GPU frame time.
/// @param obj Canvas3D handle (ignored).
/// @return `0` microseconds.
int64_t rt_canvas3d_get_frame_gpu_time_us(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the number of draws submitted by the disabled renderer.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_draws_submitted(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's AABB-transform count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_aabb_transforms(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's sort-pass count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_sort_passes(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's backend state-change count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_backend_state_changes(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return no finalized-frame screenshot from a disabled renderer.
/// @param obj Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_screenshot_final(void *obj) {
    (void)obj;
    return NULL;
}

/// @brief Report that no Canvas3D frame has been finalized.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_frame_finalized(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Ignore a Mesh3D capacity reservation without graphics support.
/// @param obj Mesh3D handle (ignored).
/// @param vertex_count Requested vertex capacity (ignored).
/// @param triangle_count Requested triangle capacity (ignored).
void rt_mesh3d_reserve(void *obj, int64_t vertex_count, int64_t triangle_count) {
    (void)obj;
    (void)vertex_count;
    (void)triangle_count;
}

/// @brief Ignore a Material3D anisotropy update.
/// @param obj Material3D handle (ignored).
/// @param anisotropy Requested anisotropy level (ignored).
void rt_material3d_set_anisotropy(void *obj, int64_t anisotropy) {
    (void)obj;
    (void)anisotropy;
}

/// @brief Return the default material anisotropy level.
/// @param obj Material3D handle (ignored).
/// @return `1`.
int64_t rt_material3d_get_anisotropy(void *obj) {
    (void)obj;
    return 1;
}

/// @brief Ignore rebasing the origin of a disabled scene.
/// @param scene Scene3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_scene3d_rebase_origin(void *scene, double dx, double dy, double dz) {
    (void)scene;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Report that a child was not added to a disabled scene node.
/// @param node Parent SceneNode3D handle (ignored).
/// @param child Child SceneNode3D handle (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_add_child(void *node, void *child) {
    (void)node;
    (void)child;
    return 0;
}

/// @brief Report that preserve-world reparenting was not performed.
/// @param node Parent SceneNode3D handle (ignored).
/// @param child Child SceneNode3D handle (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_add_child_preserve_world(void *node, void *child) {
    (void)node;
    (void)child;
    return 0;
}

/// @brief Report that a world matrix was not assigned to a disabled scene node.
/// @param node SceneNode3D handle (ignored).
/// @param world_matrix Matrix4 world transform (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_set_world_matrix(void *node, void *world_matrix) {
    (void)node;
    (void)world_matrix;
    return 0;
}

/// @brief Initialize world-position outputs for a disabled scene node.
/// @param node SceneNode3D handle (ignored).
/// @param x Optional X output receiving `0.0`.
/// @param y Optional Y output receiving `0.0`.
/// @param z Optional Z output receiving `0.0`.
/// @return `0`, indicating that no world position was available.
int8_t rt_scene_node3d_get_world_position_components(void *node, double *x, double *y, double *z) {
    (void)node;
    if (x)
        *x = 0.0;
    if (y)
        *y = 0.0;
    if (z)
        *z = 0.0;
    return 0;
}

/// @brief Return the scene count of an unavailable model.
/// @param model Model3D handle (ignored).
/// @return `0`.
int64_t rt_model3d_get_scene_count(void *model) {
    (void)model;
    return 0;
}

/// @brief Return the camera count of an unavailable model.
/// @param model Model3D handle (ignored).
/// @return `0`.
int64_t rt_model3d_get_camera_count(void *model) {
    (void)model;
    return 0;
}

/// @brief Return no camera from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param index Camera index (ignored).
/// @return `NULL`.
void *rt_model3d_get_camera(void *model, int64_t index) {
    (void)model;
    (void)index;
    return NULL;
}

/// @brief Return the empty scene name from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param index Scene index (ignored).
/// @return An owned empty runtime string.
rt_string rt_model3d_get_scene_name(void *model, int64_t index) {
    (void)model;
    (void)index;
    return disabled_empty_string();
}

/// @brief Return no instantiated scene from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param index Scene index (ignored).
/// @return `NULL`.
void *rt_model3d_instantiate_scene_at(void *model, int64_t index) {
    (void)model;
    (void)index;
    return NULL;
}

/// @brief Return no model from a preloaded glTF bundle in a disabled build.
/// @param path Logical source path (ignored).
/// @param bundle Predecoded glTF bundle (ignored).
/// @param asset_path Non-zero when `path` names a packed asset (ignored).
/// @return `NULL`.
void *rt_model3d_load_preloaded_gltf_bundle(rt_string path,
                                            rt_gltf_preload_bundle *bundle,
                                            int8_t asset_path) {
    (void)path;
    (void)bundle;
    (void)asset_path;
    return NULL;
}

/// @brief Return no model from preloaded FBX bytes in a disabled build.
/// @param path Logical source path (ignored).
/// @param data FBX byte buffer (ignored).
/// @param size Buffer length in bytes (ignored).
/// @param asset_path Non-zero when `path` names a packed asset (ignored).
/// @return `NULL`.
void *rt_model3d_load_preloaded_fbx(rt_string path,
                                    const uint8_t *data,
                                    size_t size,
                                    int8_t asset_path) {
    (void)path;
    (void)data;
    (void)size;
    (void)asset_path;
    return NULL;
}

/// @brief Report that a disabled physics world contains no body.
/// @param world Physics3DWorld handle (ignored).
/// @param body Body3D handle (ignored).
/// @return `0`.
int8_t rt_world3d_contains_body(void *world, void *body) {
    (void)world;
    (void)body;
    return 0;
}

/// @brief Return the most recent disabled-world CCD clamped-body count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_last_ccd_clamped_body_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Return the cumulative disabled-world CCD clamped-body count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_ccd_substep_clamped_body_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Return the disabled world's broadphase fallback count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_broadphase_fallback_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Return the disabled world's query broadphase rebuild count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_query_broadphase_rebuild_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Ignore rebasing the origin of a disabled physics world.
/// @param world Physics3DWorld handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_world3d_rebase_origin(void *world, double dx, double dy, double dz) {
    (void)world;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Report that a disabled navmesh was not exported.
/// @param navmesh NavMesh3D handle (ignored).
/// @param path Destination path (ignored).
/// @return `0`.
int8_t rt_navmesh3d_export(void *navmesh, rt_string path) {
    (void)navmesh;
    (void)path;
    return 0;
}

/// @brief Return no imported navmesh when graphics support is disabled.
/// @param path Source path (ignored).
/// @return `NULL`.
void *rt_navmesh3d_import(rt_string path) {
    (void)path;
    return NULL;
}

/// @brief Ignore the ground normal assigned to a disabled IK solver.
/// @param solver IKSolver3D handle (ignored).
/// @param normal Vec3 ground normal (ignored).
void rt_ik_solver3d_set_ground_normal(void *solver, void *normal) {
    (void)solver;
    (void)normal;
}

/// @brief Return the fallback playback time for a disabled animation controller.
/// @param controller AnimController3D handle (ignored).
/// @return `0.0` seconds.
double rt_anim_controller3d_get_state_time(void *controller) {
    (void)controller;
    return 0.0;
}

/// @brief Report that a disabled animation controller is not playing a state.
/// @param controller AnimController3D handle (ignored).
/// @return `0`.
int8_t rt_anim_controller3d_is_state_playing(void *controller) {
    (void)controller;
    return 0;
}

/// @brief Ignore a bone-LOD update on a disabled animation controller.
/// @param controller AnimController3D handle (ignored).
/// @param lod Bone LOD level (ignored).
void rt_anim_controller3d_set_bone_lod(void *controller, int64_t lod) {
    (void)controller;
    (void)lod;
}

/// @brief Ignore rebasing a disabled particle system.
/// @param particles Particles3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_particles3d_rebase_origin(void *particles, double dx, double dy, double dz) {
    (void)particles;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Ignore rebasing a disabled decal.
/// @param decal Decal3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_decal3d_rebase_origin(void *decal, double dx, double dy, double dz) {
    (void)decal;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Ignore rebasing a disabled 3D sprite.
/// @param sprite Sprite3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_sprite3d_rebase_origin(void *sprite, double dx, double dy, double dz) {
    (void)sprite;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Return no synthesized heightmap Pixels from a disabled terrain.
/// @param terrain Terrain3D handle (ignored).
/// @return `NULL`.
void *rt_terrain3d_build_heightmap_pixels(void *terrain) {
    (void)terrain;
    return NULL;
}

/// @brief Report that two disabled terrain edges were not stitched.
/// @param terrain Terrain3D handle (ignored).
/// @param edge Local edge identifier (ignored).
/// @param neighbor Neighbor Terrain3D handle (ignored).
/// @param neighbor_edge Neighbor edge identifier (ignored).
/// @return `0`.
int64_t rt_terrain3d_stitch_edge(void *terrain,
                                 int64_t edge,
                                 void *neighbor,
                                 int64_t neighbor_edge) {
    (void)terrain;
    (void)edge;
    (void)neighbor;
    (void)neighbor_edge;
    return 0;
}

/// @brief Return no navigation mesh built from a disabled terrain.
/// @param terrain Terrain3D handle (ignored).
/// @param step Heightmap sampling stride (ignored).
/// @return `NULL`.
void *rt_terrain3d_build_nav_mesh(void *terrain, int64_t step) {
    (void)terrain;
    (void)step;
    return NULL;
}

/// @brief Ignore drawing a disabled terrain at a world-space offset.
/// @param canvas Canvas3D handle (ignored).
/// @param terrain Terrain3D handle (ignored).
/// @param x World-space X position (ignored).
/// @param y World-space Y position (ignored).
/// @param z World-space Z position (ignored).
void rt_canvas3d_draw_terrain_at(void *canvas, void *terrain, double x, double y, double z) {
    (void)canvas;
    (void)terrain;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Return no glTF preload bundle in a graphics-disabled build.
/// @param path Null-terminated source path (ignored).
/// @param asset_path Non-zero for a packed-asset path (ignored).
/// @param data Optional source bytes (ignored).
/// @param size Source byte count (ignored).
/// @return `NULL`.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create_cstr(const char *path,
                                                           int8_t asset_path,
                                                           const uint8_t *data,
                                                           size_t size) {
    (void)path;
    (void)asset_path;
    (void)data;
    (void)size;
    return NULL;
}

/// @brief Ignore release of an absent glTF preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
void rt_gltf_preload_bundle_free(rt_gltf_preload_bundle *bundle) {
    (void)bundle;
}

/// @brief Return the decoded-image footprint of an absent preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_decoded_image_bytes(const rt_gltf_preload_bundle *bundle) {
    (void)bundle;
    return 0;
}

/// @brief Return the next decoded-image slice size for an absent preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
/// @param max_bytes Requested maximum slice size (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_next_decoded_image_slice_bytes(const rt_gltf_preload_bundle *bundle,
                                                             size_t max_bytes) {
    (void)bundle;
    (void)max_bytes;
    return 0;
}

/// @brief Report that no decoded-image slice was prepared.
/// @param bundle Preload-bundle pointer (ignored).
/// @param max_bytes Requested maximum slice size (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_prepare_decoded_image_slice(rt_gltf_preload_bundle *bundle,
                                                          size_t max_bytes) {
    (void)bundle;
    (void)max_bytes;
    return 0;
}

/// @brief Initialize camera-position outputs when no camera is available.
/// @param camera Camera3D handle (ignored).
/// @param x Optional X output receiving `0.0`.
/// @param y Optional Y output receiving `0.0`.
/// @param z Optional Z output receiving `0.0`.
/// @return `0`, indicating that no position was read.
int8_t rt_camera3d_get_position_components(void *camera, double *x, double *y, double *z) {
    (void)camera;
    if (x)
        *x = 0.0;
    if (y)
        *y = 0.0;
    if (z)
        *z = 0.0;
    return 0;
}

/// @brief Ignore a component-wise look-at update on a disabled camera.
/// @param camera Camera3D handle (ignored).
/// @param x Target X coordinate (ignored).
/// @param y Target Y coordinate (ignored).
/// @param z Target Z coordinate (ignored).
/// @param up_x Up-vector X component (ignored).
/// @param up_y Up-vector Y component (ignored).
/// @param up_z Up-vector Z component (ignored).
void rt_camera3d_look_at_components(
    void *camera, double x, double y, double z, double up_x, double up_y, double up_z) {
    (void)camera;
    (void)x;
    (void)y;
    (void)z;
    (void)up_x;
    (void)up_y;
    (void)up_z;
}

/// @brief Ignore a component-wise orbit update on a disabled camera.
/// @param camera Camera3D handle (ignored).
/// @param target_x Orbit-target X coordinate (ignored).
/// @param target_y Orbit-target Y coordinate (ignored).
/// @param target_z Orbit-target Z coordinate (ignored).
/// @param yaw Orbit yaw (ignored).
/// @param pitch Orbit pitch (ignored).
/// @param distance Orbit distance (ignored).
void rt_camera3d_orbit_components(void *camera,
                                  double target_x,
                                  double target_y,
                                  double target_z,
                                  double yaw,
                                  double pitch,
                                  double distance) {
    (void)camera;
    (void)target_x;
    (void)target_y;
    (void)target_z;
    (void)yaw;
    (void)pitch;
    (void)distance;
}

/// @brief Ignore a request to show the cursor without a graphics backend.
void vgfx_show_cursor(void) {}

/// @brief Ignore a request to hide the cursor without a graphics backend.
void vgfx_hide_cursor(void) {}
