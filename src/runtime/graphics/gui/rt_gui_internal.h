//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_internal.h
// Purpose: Share GUI application state, checked opaque-handle conversions, coordinate/string
// sanitizers, subhandle lifetime bridges, and cross-module helper declarations among the split
// runtime GUI implementation units.
//
// Key invariants:
//   - s_current_app identifies the construction context used by widget and font helpers.
//   - Public opaque handles are authenticated against live registries before they are dereferenced.
//   - Public logical geometry is converted to bounded framebuffer geometry exactly once.
//   - Default fonts and themes are installed per app and are lazily initialized on first use.
//   - This header is implementation-only; it is not part of the public runtime API.
//   - App, widget, font, and subhandle ownership remains with their defining GUI subsystem.
//
// Ownership/Lifetime:
//   - Internal module header; not included by user code or public headers.
//   - Returned application, widget, toolkit-item, font, and theme pointers are borrowed unless a
//     function contract explicitly transfers or allocates ownership.
//   - Heap strings returned by conversion helpers are caller-owned and released with free().
//
// Links: src/runtime/graphics/gui/rt_gui.h, src/runtime/core/rt_string.h,
//        src/runtime/oop/rt_object.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_gui.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"

#include "../lib/graphics/include/vgfx.h"
#include "../lib/gui/include/vg_event.h"
#include "../lib/gui/include/vg_font.h"
#include "../lib/gui/include/vg_ide_widgets.h"
#include "../lib/gui/include/vg_layout.h"
#include "../lib/gui/include/vg_theme.h"
#include "../lib/gui/include/vg_widget.h"
#include "../lib/gui/include/vg_widgets.h"

// Native file dialogs on macOS and Windows
#if RT_PLATFORM_MACOS || RT_PLATFORM_WINDOWS
#include "../lib/gui/src/dialogs/vg_filedialog_native.h"
#endif

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unix: strings.h provides strcasecmp.
#if !RT_PLATFORM_WINDOWS
#include <strings.h>
#endif

/// @brief Compare two GUI ASCII tokens case-insensitively without exporting a macro.
/// @details Windows exposes the comparison as `_stricmp`; POSIX exposes `strcasecmp`. Keeping the
///          choice behind a local helper avoids changing the namespace of every translation unit
///          that includes this internal GUI header.
/// @param a First NUL-terminated token.
/// @param b Second NUL-terminated token.
/// @return Less than, equal to, or greater than zero using the platform comparison semantics.
static inline int rt_gui_ascii_casecmp(const char *a, const char *b) {
#if RT_PLATFORM_WINDOWS
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

//=============================================================================
// App state (defined in rt_gui_app.c)
//=============================================================================

/// @brief One normalized keyboard stroke in a runtime shortcut or chord.
/// @details Modifier fields are Boolean flags and @c key stores the translated VG key code. A
///          zero key marks an unused stroke, including the second half of a single-stroke shortcut.
typedef struct {
    int ctrl;
    int shift;
    int alt;
    int super;
    int key;
} rt_gui_shortcut_stroke_t;

/// @brief App-owned registration record for one named keyboard shortcut.
/// @details The app owns the duplicated identifier, key specification, and description. @c enabled
///          controls matching, while @c triggered is transient per-poll state. The parsed stroke
///          pair avoids reparsing the textual key specification on every event.
typedef struct {
    char *id;
    char *keys;
    char *description;
    int enabled;
    int triggered;
    rt_gui_shortcut_stroke_t first;
    rt_gui_shortcut_stroke_t second; ///< Empty key for a single-stroke shortcut.
} rt_gui_shortcut_t;

/// @brief App-owned batch of filesystem paths delivered by the latest drop event.
/// @details Each path and the pointer array are heap-owned by the app. @c was_dropped distinguishes
///          a pending batch from retained storage that may be replaced by the next platform event.
typedef struct {
    char **files;
    int64_t file_count;
    int64_t was_dropped;
} rt_gui_file_drop_data_t;

/// @brief Logical theme source selected for one GUI application.
/// @details Built-in dark and light modes select immutable palettes, system mode follows the host
///          appearance, and custom mode uses the app-owned unscaled custom palette.
typedef enum {
    RT_GUI_THEME_DARK = 0,
    RT_GUI_THEME_LIGHT = 1,
    RT_GUI_THEME_SYSTEM = 2,
    RT_GUI_THEME_CUSTOM = 3,
} rt_gui_theme_kind_t;

/// @brief Complete private state of one runtime GUI application.
/// @details The record owns the native window, root widget tree, installed theme/font resources,
///          modal and shortcut registries, detached overlays, accessibility preferences, input
///          transients, and retained-rendering bookkeeping. It is shared only among runtime GUI
///          implementation units and must be accessed on the GUI main thread.
typedef struct {
    uint64_t magic;
    vgfx_window_t window;      ///< Underlying graphics window handle.
    vg_widget_t *root;         ///< Root widget container for the UI hierarchy.
    vg_font_t *default_font;   ///< Regular proportional default font (lazily loaded).
    float default_font_size;   ///< Default font size in logical points.
    int default_font_owned;    ///< Non-zero when default_font is owned by the app.
    vg_font_t *role_font_bold; ///< App-owned bold role fallback; may alias default_font.
    vg_font_t *role_font_mono; ///< App-owned monospace role fallback; may alias default_font.
    int role_font_bold_owned;  ///< Non-zero when role_font_bold has independent ownership.
    int role_font_mono_owned;  ///< Non-zero when role_font_mono has independent ownership.
    vg_font_t **retired_fonts; ///< Fonts awaiting an unused safe presentation generation.
    uint64_t *retired_font_generations; ///< Generation at which each matching font was retired.
    int retired_font_count;             ///< Number of valid parallel retirement entries.
    int retired_font_cap;               ///< Allocated capacity of both retirement arrays.
    int64_t should_close;               ///< Non-zero when the application should exit.
    int64_t close_requested;            ///< Non-zero when a close request arrived this frame.
    int32_t prevent_close;     ///< Non-zero when close requests should not set should_close.
    vg_widget_t *last_clicked; ///< Widget clicked during the current frame.
    vg_statusbar_item_t *last_statusbar_clicked;
    vg_toolbar_item_t *last_toolbar_clicked;
    int32_t mouse_x; ///< Current mouse X coordinate in window space.
    int32_t mouse_y; ///< Current mouse Y coordinate in window space.
    uint64_t last_event_time_ms;
    uint64_t last_render_time_ms;
    int32_t animations_active; ///< Non-zero while the last scheduler pass found unsettled motion.
    int32_t frame_delta_override_pending; ///< One-shot deterministic delta consumed by Render.
    double frame_delta_override_ms;       ///< Pending deterministic frame delta in milliseconds.
    double scheduler_elapsed_ms;          ///< Full caller-visible elapsed scheduler time.
    double scheduler_clock_ms; ///< Timer clock advanced by real or deterministic frame deltas.
    uint64_t frame_generation; ///< Non-zero generation advanced once per render scheduler pass.
    int32_t last_layout_width;
    int32_t last_layout_height;
    vg_theme_t *theme;
    const vg_theme_t *theme_base;
    vg_theme_t *custom_theme_base; ///< Owned unscaled custom palette, or NULL when unset.
    float theme_scale;
    uint64_t theme_revision; ///< Monotonic revision incremented after each installed theme copy.
    uint64_t theme_reported_revision; ///< Revision consumed only by Theme.WasChanged.
    float user_scale; ///< User UI zoom multiplier applied atop the HiDPI scale (1.0 = default).
    rt_gui_theme_kind_t theme_kind;
    int32_t system_prefers_dark; ///< Last normalized platform appearance used by System mode.
    int32_t accessibility_high_contrast;  ///< Explicit per-app high-contrast preference.
    int32_t accessibility_reduced_motion; ///< Explicit per-app reduced-motion preference.
    uint64_t accessibility_revision;      ///< Monotonic preference/announcement revision.
    char *title;            ///< Mutable window/document title (owned, heap-allocated).
    char *application_name; ///< Stable App.New product name (owned, heap-allocated).
    vg_dialog_t **dialog_stack;
    int dialog_count;
    int dialog_cap;
    vg_widget_t *drag_candidate;
    int32_t drag_start_x;
    int32_t drag_start_y;
    vg_widget_t *drag_source;
    vg_widget_t *drag_over_widget;
    rt_gui_shortcut_t *shortcuts;
    int shortcut_count;
    int shortcut_cap;
    int shortcuts_global_enabled;
    int shortcut_chord_pending;
    rt_gui_shortcut_stroke_t shortcut_chord_prefix;
    uint64_t shortcut_chord_started_ms;
    char *triggered_shortcut_id;
    char **triggered_shortcut_ids;
    int triggered_shortcut_count;
    int triggered_shortcut_cap;
    rt_gui_file_drop_data_t file_drop;
    vg_commandpalette_t **command_palettes;
    int command_palette_count;
    int command_palette_cap;
    vg_contextmenu_t *active_context_menu; ///< Standalone right-click menu overlay (not parented
                                           ///< into app->root); painted and routed input directly.
    vg_notification_manager_t *notification_manager;
    vg_tooltip_manager_t tooltip_manager_state;
    vg_tooltip_t *manual_tooltip;
    uint32_t manual_tooltip_delay_ms;
    vg_widget_runtime_state_t widget_runtime_state;

    // Damage-region (partial) rendering — plan 07. When partial_paint_enabled is
    // 0 (kill switch via ZANNA_GUI_FULL_REPAINT=1), every dirty frame full-clears
    // and repaints the whole tree, matching the pre-plan-07 behavior exactly.
    int32_t partial_paint_enabled; ///< 1 = damage-region path allowed; 0 = always full repaint.
    uint64_t frames_full;          ///< Dirty frames that took the full-window repaint path.
    uint64_t frames_partial;       ///< Dirty frames that took the damage-region repaint path.
    float last_damage_w;           ///< Width of the last partial-paint damage rect (0 if full).
    float last_damage_h;           ///< Height of the last partial-paint damage rect (0 if full).
    float overlay_last_x;          ///< Left edge of the last painted detached-overlay union.
    float overlay_last_y;          ///< Top edge of the last painted detached-overlay union.
    float overlay_last_w;          ///< Width of the last painted detached-overlay union.
    float overlay_last_h;          ///< Height of the last painted detached-overlay union.
    int32_t overlay_last_valid;    ///< Non-zero when the retained overlay union contains pixels.
} rt_gui_app_t;

#define RT_GUI_APP_MAGIC UINT64_C(0x5254475541505031)

/// @brief Global pointer to the current app for widget constructors to access the default font.
/// @details This is the construction context, normally synchronized with the active app by
///          @ref rt_gui_activate_app. It is borrowed and may be NULL while no app is active.
extern rt_gui_app_t *s_current_app;

/// @brief Return the GUI app whose toolkit state is currently installed.
/// @details The active app receives input and rendering and supplies process-global widget runtime
///          state. Calling the function does not activate, retain, or otherwise mutate the app.
/// @return Borrowed active app pointer, or NULL when no app is active.
rt_gui_app_t *rt_gui_get_active_app(void);
/// @brief Install @p app as the active GUI application.
/// @details Saves the outgoing app's process-global widget state, installs the incoming state,
///          synchronizes system appearance and theme scaling, and updates construction context.
///          Passing NULL deactivates the current app. Re-activating the same app still refreshes
///          scale-sensitive theme state.
/// @param app Borrowed live app to activate, or NULL to clear the active app.
void rt_gui_activate_app(rt_gui_app_t *app);
/// @brief Resolve the registered GUI app that owns @p widget.
/// @details Walks a live widget's parent chain to its root and authenticates the root user-data
///          pointer against the live-app registry. App handles are accepted directly; stale,
///          foreign, and detached widget pointers are rejected without transferring ownership.
/// @param widget Candidate app or live widget pointer.
/// @return Borrowed owning app, or NULL when the handle has no live registered owner.
rt_gui_app_t *rt_gui_app_from_widget(vg_widget_t *widget);

/// @brief Resolve a widget handle's owning app for isolated runtime modules.
/// @details The opaque return avoids exposing private app layout to modules that intentionally
///          include only the public GUI bridge. Invalid, destroyed, or unparented widgets return
///          NULL. The pointer is borrowed and must never be retained past app destruction.
/// @param handle Candidate live widget handle.
/// @return Borrowed owning app pointer, or NULL.
/// @internal
void *rt_gui_widget_owner_app(void *handle);

/// @brief Return one owning app's current scheduler frame generation.
/// @details The function validates the opaque app handle and is side-effect free. Generation zero
///          means the app has not rendered yet; overflow wraps directly to one so zero remains the
///          permanent pre-render sentinel.
/// @param app Candidate opaque app pointer.
/// @return Current generation, or zero for an invalid app.
/// @internal
uint64_t rt_gui_app_frame_generation_for_owner(void *app);
/// @brief Read the monotonic millisecond clock used for GUI timing and animation.
/// @details The platform clock is suitable for elapsed-time comparisons and is independent of
///          wall-clock adjustments; its absolute epoch is intentionally unspecified.
/// @return Current monotonic timestamp in milliseconds.
uint64_t rt_gui_now_ms(void);
/// @brief Return the app's effective logical-to-framebuffer scale.
/// @details Multiplies the platform window scale by the app's user UI zoom. Invalid, zero, or
///          non-finite factors are replaced by 1.0, and a non-finite product also falls back to
///          1.0. The function does not mutate or activate the app.
/// @param app App whose scale is requested; NULL represents the identity scale.
/// @return Positive finite effective scale, with 1.0 as the invalid-app fallback.
float rt_gui_app_effective_scale(const rt_gui_app_t *app);
/// @brief Convert the app's logical default font size to effective framebuffer pixels.
/// @details Uses @ref rt_gui_app_effective_scale exactly once. The stored font size remains in
///          logical points so subsequent monitor-scale or user-zoom changes can be reapplied
///          without cumulative rounding or multiplication.
/// @param app App whose default font size is requested; NULL uses 14 logical points at 1x.
/// @return Positive finite font size in framebuffer pixels.
float rt_gui_app_effective_font_size(const rt_gui_app_t *app);

/// @brief Atomically copy already-converted straight RGBA bytes into a GUI Image.
/// @details This internal bridge lets streaming media reuse a conversion buffer and bypass the
///          public Pixels-to-RGBA temporary allocation. The lower image owns its own reusable copy;
///          caller storage remains borrowed for the duration of the call only.
/// @param image Live GUI Image handle.
/// @param rgba Borrowed width*height*4 interleaved RGBA bytes.
/// @param width Positive pixel width fitting int32_t.
/// @param height Positive pixel height fitting int32_t.
/// @return 1 after a complete atomic upload, otherwise 0 with prior pixels preserved.
/// @internal
int rt_gui_image_try_set_rgba_bytes(void *image,
                                    const uint8_t *rgba,
                                    int64_t width,
                                    int64_t height);

/// @brief Synchronize every runtime Minimap owned by one GUI app.
/// @details Called before retained dirty-state inspection so editor content/layout/viewport changes
///          can invalidate the corresponding minimap in the same frame. Traversal is allocation-
///          free and detached/destroyed wrappers are ignored.
/// @param app Borrowed owning app pointer.
/// @internal
void rt_minimap_sync_app(void *app);
/// @brief Re-apply the current theme to @p app's widget tree after a change.
/// @details Rebuilds the per-app scaled theme only when its base theme or effective scale changed.
///          A successful replacement invalidates every app-owned layout/paint surface, reapplies
///          the logical default font at the new effective pixel size, and increments the app's
///          theme revision. Allocation failure leaves the previous theme and revision unchanged.
/// @param app App whose theme should be refreshed; NULL is a no-op.
void rt_gui_refresh_theme(rt_gui_app_t *app);
/// @brief Resolve the logical, unscaled base palette currently selected by one app.
/// @details Dark and light return immutable toolkit singletons. System resolves through the
///          app's last synchronized platform appearance, while custom returns the app-owned
///          palette when present and deterministically falls back to dark otherwise. The returned
///          pointer is borrowed and remains valid only while the app and its selected palette live.
/// @param app App whose logical palette is requested; NULL selects the built-in dark palette.
/// @return Borrowed non-NULL unscaled theme suitable for cloning; never destroy the result.
const vg_theme_t *rt_gui_app_theme_base(const rt_gui_app_t *app);
/// @brief Atomically replace one app's logical custom palette and select Custom mode.
/// @details Builds the DPI-scaled/accessibility-adjusted installed copy before publishing any
///          state. On success the app takes ownership of @p candidate, destroys its prior custom
///          base and installed copy, increments the theme revision, reapplies fonts, and
///          invalidates theme dependents. On failure every app field remains unchanged and the
///          caller retains ownership of @p candidate.
/// @param app Target app; must be non-NULL.
/// @param candidate Owned unscaled theme clone offered for transfer; must be non-NULL.
/// @return One when ownership transferred and Custom mode was installed, otherwise zero.
int rt_gui_install_custom_theme(rt_gui_app_t *app, vg_theme_t *candidate);
/// @brief Synchronize a System-mode app with the host desktop appearance.
/// @details Queries the platform adapter on demand. A changed light/dark preference rebuilds the
///          per-app scaled theme, increments its theme revision, and invalidates theme-dependent
///          surfaces. Non-System apps and NULL handles are unchanged.
/// @param app App to synchronize; may be NULL.
void rt_gui_sync_system_theme(rt_gui_app_t *app);
/// @brief Switch @p app's theme mode and refresh its installed theme.
/// @details Accepts dark, light, system, or custom. Custom is ignored until an app-owned custom
///          palette has been installed. Re-selecting the effective current mode is allocation-free
///          unless a scale or platform appearance changed.
/// @param app Target app; NULL is a no-op.
/// @param kind Requested validated theme mode.
void rt_gui_set_theme_kind(rt_gui_app_t *app, rt_gui_theme_kind_t kind);
/// @brief Recompute the toolkit modal root after dialogs open, close, or are destroyed.
/// @details Compacts invalid and closed entries from the app's dialog stack and installs the
///          last remaining open dialog as the input-routing modal root. Passing NULL clears the
///          process-global modal root.
/// @param app App whose dialog stack should be synchronized, or NULL to clear modal routing.
void rt_gui_sync_modal_root(rt_gui_app_t *app);
/// @brief Apply the owning app's effective default font to @p widget.
/// @details Resolves and activates the widget's owner, lazily loads its default font, and
///          propagates the font and effective pixel size through the supported widget surface.
///          NULL, detached widgets, and failed font initialization are no-ops.
/// @param widget Live widget receiving its owning app's borrowed default font.
void rt_gui_apply_default_font(vg_widget_t *widget);
/// @brief Re-apply @p app's current default font and size to all app-owned GUI surfaces.
/// @details Propagates the effective pixel size through the root tree, notifications, command
///          palettes, tooltip, detached context menu, and dialogs without transferring font
///          ownership. Invalid apps or apps without a default font are unchanged.
/// @param app App whose current default font should be propagated; may be NULL.
void rt_gui_reapply_default_font(rt_gui_app_t *app);
/// @brief Clear runtime references that point into a widget subtree about to be destroyed.
/// @details Removes app-level click and drag pointers, invalidates item/subobject wrappers,
///          disconnects FindBar and Minimap targets, and clears dialog bridges before the lower
///          widget tree can release their storage. A NULL app still performs subtree-wide cleanup.
/// @param app Owning app whose cached pointers should be cleared; may be NULL.
/// @param widget Root of the live subtree being retired; NULL is a no-op.
void rt_widget_forget_runtime_refs(rt_gui_app_t *app, vg_widget_t *widget);
/// @brief Test whether @p needle is @p root or one of its descendants.
/// @details Performs an allocation-free depth-first traversal using parent and sibling links.
///          NULL arguments never match.
/// @param root Root of the widget subtree to inspect.
/// @param needle Candidate widget sought within the subtree.
/// @return One when the candidate is reachable from the root, otherwise zero.
int rt_gui_widget_tree_contains(vg_widget_t *root, const vg_widget_t *needle);
/// @brief Disconnect FindBar wrappers targeting a CodeEditor inside @p subtree.
/// @details Prevents retained FindBar objects from dereferencing an editor that is being destroyed;
///          unrelated targets remain attached.
/// @param subtree Widget subtree being retired; NULL is a no-op.
void rt_findbar_forget_editor_subtree(vg_widget_t *subtree);
/// @brief Disconnect Minimap wrappers targeting a CodeEditor inside @p subtree.
/// @details Clears only editor references contained by the retiring subtree and preserves every
///          Minimap whose target remains live.
/// @param subtree Widget subtree being retired; NULL is a no-op.
void rt_minimap_forget_editor_subtree(vg_widget_t *subtree);
/// @brief Invalidate MessageBox wrapper references to a dialog before its destruction.
/// @details Detaches the runtime-facing MessageBox state while the lower dialog pointer is still
///          identifiable, preventing subsequent use of released toolkit storage.
/// @param dialog Dialog being retired; NULL is a no-op.
void rt_messagebox_invalidate_dialog(vg_dialog_t *dialog);
/// @brief Invalidate FileDialog wrapper references to a dialog before its destruction.
/// @details Clears matching runtime bridge state without destroying unrelated dialogs.
/// @param dialog Dialog being retired; NULL is a no-op.
void rt_filedialog_invalidate_dialog(vg_dialog_t *dialog);
/// @brief Invalidate Zanna-facing subobject handles owned by @p subtree.
/// @details Marks matching stable wrappers stale before their lower TreeView, TabBar, ListBox,
///          menu, status-bar, or toolbar payloads are released. Wrapper objects remain valid
///          runtime values but no longer resolve to toolkit pointers.
/// @param subtree Root of the owner subtree being retired; NULL is a no-op.
void rt_gui_invalidate_widget_subhandles(vg_widget_t *subtree);
/// @brief Reclaim retired subobject payloads with no remaining managed wrapper.
/// @details Walks @p subtree's owner widgets and drains individual ListBox, TreeView, TabBar,
///          MenuBar, ContextMenu, StatusBar, and Toolbar retirement records only when the indexed
///          stable-wrapper registry proves the corresponding item or subtree group unreferenced.
///          The traversal is allocation-free and leaves every still-referenced stale wrapper and
///          its tombstone intact. Expected wrapper lookup is O(1); TreeView group checking is
///          linear only in the retired subtree being considered.
/// @param subtree Live widget subtree whose owners should be collected; NULL/retired roots are
///                ignored.
void rt_gui_collect_retired_subhandles(vg_widget_t *subtree);
/// @brief Return the process-global number of allocated GUI subhandle wrappers.
/// @details Intended for lifecycle regression tests and diagnostics; reading it has no side effects
///          and does not retain wrappers.
/// @return Number of wrappers not yet finalized.
size_t rt_gui_subhandle_debug_live_count(void);
/// @brief Return the allocated capacity of the expected-O(1) target index.
/// @return Power-of-two slot count, or zero when the registry is empty.
size_t rt_gui_subhandle_debug_index_capacity(void);
/// @brief Return the largest bounded target-index probe count observed since reset.
/// @return Maximum slots inspected by a target lookup; never exceeds current lookup capacity.
size_t rt_gui_subhandle_debug_max_probes(void);
/// @brief Reset subhandle target-index probe counters without altering registry state.
void rt_gui_subhandle_debug_reset_probes(void);
/// @brief Invalidate wrappers for retired nodes before @p tree reclaims their tombstones.
/// @details The tree's retired list still owns valid tombstone storage when this function is
///          called. It clears only wrappers whose node is already retired, preserving wrappers for
///          live nodes in the same tree. After return, `vg_treeview_prune_retired_nodes` may free
///          the retired storage without leaving a runtime wrapper that can dereference it.
/// @param tree Tree whose retired node wrappers should be invalidated; NULL is a no-op.
void rt_gui_invalidate_retired_tree_node_subhandles(vg_treeview_t *tree);
/// @brief Invalidate wrappers for retired tabs before @p tabbar reclaims their tombstones.
/// @details The tab bar's retired list still owns valid tombstone storage when this function is
///          called. It clears only wrappers whose tab is already retired, preserving wrappers for
///          live tabs. After return, `vg_tabbar_prune_retired_tabs` may safely free the tombstones.
/// @param tabbar Tab bar whose retired tab wrappers should be invalidated; NULL is a no-op.
void rt_gui_invalidate_retired_tab_subhandles(vg_tabbar_t *tabbar);
/// @brief Invalidate Zanna-facing handles for @p context menu and its descendants.
/// @details Recursively retires item and submenu wrappers, then retires the wrapper for the menu
///          itself. The lower menu remains owned by its existing toolkit owner.
/// @param menu Root context menu whose wrapper tree should be invalidated; NULL is a no-op.
void rt_gui_invalidate_contextmenu_tree(vg_contextmenu_t *menu);
/// @brief Invalidate item and submenu handles contained by @p context menu.
/// @details Retires direct item wrappers and recursively invalidates submenu trees while leaving
///          the wrapper for @p menu itself intact.
/// @param menu Context menu whose contents are being replaced or destroyed; NULL is a no-op.
void rt_gui_invalidate_contextmenu_contents(vg_contextmenu_t *menu);
/// @brief Return the stable managed Zanna handle for a tree node.
/// @param node Live lower-toolkit node to wrap.
/// @return Managed wrapper for @p node, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_tree_node(vg_tree_node_t *node);
/// @brief Return the stable managed Zanna handle for a tab.
/// @param tab Live lower-toolkit tab to wrap.
/// @return Managed wrapper for @p tab, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_tab(vg_tab_t *tab);
/// @brief Return the stable managed Zanna handle for a list-box item.
/// @param item Live lower-toolkit item to wrap.
/// @return Managed wrapper for @p item, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_listbox_item(vg_listbox_item_t *item);
/// @brief Return the stable managed Zanna handle for a menu.
/// @param menu Live lower-toolkit menu to wrap.
/// @return Managed wrapper for @p menu, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_menu(vg_menu_t *menu);
/// @brief Return the stable managed Zanna handle for a menu item.
/// @param item Live lower-toolkit menu item to wrap.
/// @return Managed wrapper for @p item, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_menu_item(vg_menu_item_t *item);
/// @brief Return the stable managed Zanna handle for a context menu.
/// @param menu Live lower-toolkit context menu to wrap.
/// @return Managed wrapper for @p menu, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_contextmenu(vg_contextmenu_t *menu);
/// @brief Return the stable managed Zanna handle for a status-bar item.
/// @param item Live lower-toolkit status-bar item to wrap.
/// @return Managed wrapper for @p item, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_statusbar_item(vg_statusbar_item_t *item);
/// @brief Return the stable managed Zanna handle for a toolbar item.
/// @param item Live lower-toolkit toolbar item to wrap.
/// @return Managed wrapper for @p item, or NULL for invalid input or allocation failure.
void *rt_gui_wrap_toolbar_item(vg_toolbar_item_t *item);
/// @brief Resolve a managed tree-node handle to its live lower-toolkit node.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live node, or NULL for a wrong-kind, retired, or ownerless handle.
vg_tree_node_t *rt_gui_tree_node_from_handle(void *handle);
/// @brief Resolve a managed tab handle to its live lower-toolkit tab.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live tab, or NULL for a wrong-kind, retired, or ownerless handle.
vg_tab_t *rt_gui_tab_from_handle(void *handle);
/// @brief Resolve a managed list-box-item handle to its live lower-toolkit item.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for a wrong-kind, retired, or ownerless handle.
vg_listbox_item_t *rt_gui_listbox_item_from_handle(void *handle);
/// @brief Resolve a managed menu handle to its live lower-toolkit menu.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live menu, or NULL for a wrong-kind, retired, or ownerless handle.
vg_menu_t *rt_gui_menu_from_handle(void *handle);
/// @brief Resolve a managed menu-item handle to its live lower-toolkit item.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for a wrong-kind, retired, or ownerless handle.
vg_menu_item_t *rt_gui_menu_item_from_handle(void *handle);
/// @brief Resolve a managed context-menu handle to its live lower-toolkit menu.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live context menu, or NULL for a wrong-kind, retired, or invalid handle.
vg_contextmenu_t *rt_gui_contextmenu_from_handle(void *handle);
/// @brief Resolve a managed status-bar item handle to its live lower-toolkit item.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for a wrong-kind, retired, or ownerless handle.
vg_statusbar_item_t *rt_gui_statusbar_item_from_handle(void *handle);
/// @brief Resolve a managed toolbar item handle to its live lower-toolkit item.
/// @param handle Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for a wrong-kind, retired, or ownerless handle.
vg_toolbar_item_t *rt_gui_toolbar_item_from_handle(void *handle);
/// @brief Register a command palette for app-level input and overlay routing.
/// @details Duplicate registrations are ignored. The app borrows the palette pointer; allocation
///          failure leaves the registration set unchanged.
/// @param app App that should track the palette.
/// @param palette Live palette to register; ownership remains with its creator.
void rt_gui_register_command_palette(rt_gui_app_t *app, vg_commandpalette_t *palette);
/// @brief Remove a command palette from app-level input and overlay routing.
/// @details The operation is idempotent and does not destroy or otherwise change the palette.
/// @param app App whose registry should be searched.
/// @param palette Previously registered palette to remove.
void rt_gui_unregister_command_palette(rt_gui_app_t *app, vg_commandpalette_t *palette);
/// @brief Append a copied filesystem path to an app's current file-drop batch.
/// @details The first path of a new batch releases the preceding batch. Invalid arguments,
///          overflow, and allocation failure leave no dangling borrowed path; the enabled-runtime
///          implementation owns every accepted copy until cleanup or the next batch.
/// @param app App receiving the platform drop event.
/// @param path Borrowed NUL-terminated path copied during the call.
void rt_gui_file_drop_add(rt_gui_app_t *app, const char *path);
/// @brief Test whether @p handle identifies a currently registered live GUI app.
/// @details Checks the current, active, and registered-app sets without dereferencing an unknown
///          address. This operation must run on the GUI main thread.
/// @param handle Opaque candidate application address.
/// @return One for a known live app, otherwise zero.
int rt_gui_is_app_handle_known(const void *handle);
/// @brief Test whether @p handle is recorded as a destroyed GUI app address.
/// @details Consults the tombstone registry without dereferencing the candidate, allowing checked
///          entry points to reject stale runtime handles safely.
/// @param handle Opaque candidate application address.
/// @return One when the address is tombstoned, otherwise zero.
int rt_gui_is_destroyed_app_handle(const void *handle);
/// @brief Retire @p font from @p app at its current render generation.
/// @details The app reclaims it only after a later safe boundary and after every app/widget/theme
///          reference disappears. Queue growth failure leaves prior entries intact.
/// @param app App that will schedule reclamation.
/// @param font Font being replaced or explicitly destroyed.
/// @return Non-zero if the font was queued/refreshed.
int rt_gui_retire_font(rt_gui_app_t *app, vg_font_t *font);
/// @brief Queue @p font in each app that still has a render or theme reference.
/// @param font Font whose public owner is releasing it.
/// @return Non-zero if at least one app queued the font; zero means immediate destruction is safe.
int rt_gui_retire_font_if_in_use(vg_font_t *font);

/// @brief Test whether @p handle is a known live GUI app handle.
/// @details Delegates to the authenticated app registry and never dereferences an unknown address.
/// @param handle Opaque candidate runtime handle.
/// @return One for a registered live app, otherwise zero.
static inline int rt_gui_is_app_handle(const void *handle) {
    return rt_gui_is_app_handle_known(handle);
}

/// @brief Test whether @p handle is registered as a live lower-toolkit widget.
/// @details A NULL, destroyed, foreign, or already-retired address returns false.
/// @param handle Opaque candidate runtime handle.
/// @return One for a live widget, otherwise zero.
static inline int rt_gui_is_widget_handle(const void *handle) {
    return handle && vg_widget_is_live((const vg_widget_t *)handle);
}

/// @brief Safe-cast @p handle to a widget, rejecting App/destroyed handles.
/// @details Registry checks precede the cast, so callers may pass opaque runtime values without
///          dereferencing stale app storage.
/// @param handle Opaque candidate runtime handle.
/// @return Borrowed live widget, or NULL when @p handle is not a valid widget.
static inline vg_widget_t *rt_gui_widget_handle_checked(void *handle) {
    if (!handle || rt_gui_is_app_handle(handle) || rt_gui_is_destroyed_app_handle(handle))
        return NULL;
    return rt_gui_is_widget_handle(handle) ? (vg_widget_t *)handle : NULL;
}

/// @brief Safe-cast @p handle to a live widget of a specific @p type.
/// @details Performs the general widget validation before reading the type discriminator.
/// @param handle Opaque candidate runtime handle.
/// @param type Exact lower-toolkit widget kind required by the caller.
/// @return Borrowed matching widget, or NULL for an invalid handle or type mismatch.
static inline vg_widget_t *rt_gui_widget_handle_checked_type(void *handle, vg_widget_type_t type) {
    vg_widget_t *widget = rt_gui_widget_handle_checked(handle);
    return widget && widget->type == type ? widget : NULL;
}

/// @brief Private runtime-object class tag used by managed Zanna.GUI.Font wrappers.
/// @details This negative internal identifier cannot collide with generated public class IDs and
///          lets opaque-handle validation reject arbitrary heap objects before reading a wrapper.
#define RT_GUI_FONT_HANDLE_CLASS_ID INT64_C(-0x47554601)

/// @brief Resolve a public GUI font handle to its live lower-toolkit font.
/// @details Accepts both modern managed Font wrappers and legacy raw vg_font_t pointers. Wrapper
///          validation uses the runtime heap metadata before reading private fields; raw pointers
///          are authenticated against the lower live-font registry without dereferencing unknown
///          memory. Explicitly destroyed and stale handles return NULL.
/// @param handle Managed Font wrapper, legacy raw font, or unrelated/null value.
/// @return Borrowed live vg_font_t, or NULL when validation fails.
vg_font_t *rt_gui_font_handle_checked(void *handle);

/// @brief Determine whether a handle is a live managed Zanna.GUI.Font wrapper.
/// @details The underlying font may already have been explicitly destroyed; this operation checks
///          wrapper identity/liveness only and is used to balance runtime retains in palettes.
/// @param handle Candidate opaque runtime value.
/// @return Non-zero for a live managed wrapper, otherwise zero.
int rt_gui_font_handle_is_managed(void *handle);

/// @brief Safe-cast @p handle to a registered live GUI app.
/// @param handle Opaque candidate runtime handle.
/// @return Borrowed app pointer, or NULL when @p handle is not a live app.
static inline rt_gui_app_t *rt_gui_app_handle_checked(void *handle) {
    return rt_gui_is_app_handle(handle) ? (rt_gui_app_t *)handle : NULL;
}

/// @brief Resolve any handle (NULL/App/widget) to its owning App.
/// @details NULL yields the active app; an App handle returns itself; a live
///          widget returns its owning app; a destroyed App handle yields NULL.
/// @param handle NULL, a candidate app, or a candidate widget handle.
/// @return Borrowed resolved app, or NULL for detached, stale, or foreign handles.
static inline rt_gui_app_t *rt_gui_app_from_handle(void *handle) {
    if (!handle)
        return rt_gui_get_active_app();
    if (rt_gui_is_app_handle(handle))
        return (rt_gui_app_t *)handle;
    if (rt_gui_is_destroyed_app_handle(handle))
        return NULL;
    return rt_gui_is_widget_handle(handle) ? rt_gui_app_from_widget((vg_widget_t *)handle) : NULL;
}

/// @brief Resolve a handle to the widget that should act as a parent.
/// @details An App handle resolves to its root widget; a live widget resolves
///          to itself; NULL or a destroyed App handle yields NULL.
/// @param handle Candidate app or widget parent handle.
/// @return Borrowed root/widget parent, or NULL when no valid parent can be resolved.
static inline vg_widget_t *rt_gui_widget_parent_from_handle(void *handle) {
    if (!handle)
        return NULL;
    if (rt_gui_is_app_handle(handle)) {
        rt_gui_app_t *app = (rt_gui_app_t *)handle;
        return app->root;
    }
    if (rt_gui_is_destroyed_app_handle(handle))
        return NULL;
    return rt_gui_is_widget_handle(handle) ? (vg_widget_t *)handle : NULL;
}

/// @brief Test whether widgets of @p type can safely own arbitrary runtime children.
/// @details Only explicit container-like types are accepted; leaf and unknown future types fall
///          through to rejection so callers cannot graft children into incompatible layouts.
/// @param type Lower-toolkit widget discriminator to classify.
/// @return One for supported runtime containers, otherwise zero.
static inline int rt_gui_widget_type_accepts_runtime_children(vg_widget_type_t type) {
    switch (type) {
        case VG_WIDGET_CONTAINER:
        case VG_WIDGET_SCROLLVIEW:
        case VG_WIDGET_SPLITPANE:
        case VG_WIDGET_DIALOG:
        case VG_WIDGET_LISTVIEW:
        case VG_WIDGET_GROUPBOX:
        case VG_WIDGET_CUSTOM:
            return 1;
        case VG_WIDGET_LABEL:
        case VG_WIDGET_BUTTON:
        case VG_WIDGET_TEXTINPUT:
        case VG_WIDGET_CHECKBOX:
        case VG_WIDGET_RADIO:
        case VG_WIDGET_SLIDER:
        case VG_WIDGET_PROGRESS:
        case VG_WIDGET_LISTBOX:
        case VG_WIDGET_DROPDOWN:
        case VG_WIDGET_TREEVIEW:
        case VG_WIDGET_TABBAR:
        case VG_WIDGET_MENUBAR:
        case VG_WIDGET_MENU:
        case VG_WIDGET_MENUITEM:
        case VG_WIDGET_TOOLBAR:
        case VG_WIDGET_STATUSBAR:
        case VG_WIDGET_CODEEDITOR:
        case VG_WIDGET_OUTPUTPANE:
        case VG_WIDGET_IMAGE:
        case VG_WIDGET_SPINNER:
        case VG_WIDGET_COLORSWATCH:
        case VG_WIDGET_COLORPALETTE:
        case VG_WIDGET_COLORPICKER:
        default:
            return 0;
    }
}

/// @brief Resolve a parent handle only when it can own arbitrary runtime children.
/// @details NULL is valid for detached widget creation and returns NULL. Non-NULL
///          invalid handles return NULL. Only explicit runtime-container widget
///          types are accepted; leaf widgets are rejected even if their vtable
///          lacks a custom arrange callback.
/// @param handle Candidate app or widget parent handle.
/// @return Borrowed container widget, or NULL for detached creation or an invalid/leaf parent.
static inline vg_widget_t *rt_gui_widget_parent_container_from_handle(void *handle) {
    vg_widget_t *parent = rt_gui_widget_parent_from_handle(handle);
    if (!parent)
        return NULL;
    return rt_gui_widget_type_accepts_runtime_children(parent->type) ? parent : NULL;
}

#define RT_GUI_MAX_LAYOUT_VALUE 1000000.0
#define RT_GUI_STRING_DATA_MAGIC UINT64_C(0x5254475544535452)

#ifdef _MSC_VER
#define RT_GUI_FLEX_ARRAY_SIZE 1
#else
#define RT_GUI_FLEX_ARRAY_SIZE
#endif

typedef struct {
    uint64_t magic;
    size_t len;
    char bytes[RT_GUI_FLEX_ARRAY_SIZE];
} rt_gui_string_data_t;

/// @brief Test whether a floating-point value is neither NaN nor infinity.
/// @param value Double-precision value to classify.
/// @return One for a finite value, otherwise zero.
static inline int rt_gui_double_is_finite(double value) {
    return isfinite(value) ? 1 : 0;
}

/// @brief Clamp @p value to the inclusive [@p min_value, @p max_value] range.
/// @details The caller must supply an ordered range and handle NaN before calling; comparisons with
///          NaN deliberately leave the value unchanged.
/// @param value Value to clamp.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return The nearest value inside the supplied range.
static inline double rt_gui_clamp_f64(double value, double min_value, double max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Sanitize a double into a non-negative bounded float.
/// @param value Candidate public numeric input.
/// @param max_value Inclusive finite upper bound supplied by the caller.
/// @return Zero for non-finite or negative input, @p max_value above the bound, otherwise @p value.
static inline float rt_gui_sanitize_nonnegative_float(double value, double max_value) {
    if (!rt_gui_double_is_finite(value))
        return 0.0f;
    return (float)rt_gui_clamp_f64(value, 0.0, max_value);
}

/// @brief Sanitize a double into a symmetrically bounded float.
/// @param value Candidate public numeric input.
/// @param max_abs_value Non-negative magnitude used for both inclusive bounds.
/// @return Zero for non-finite input, otherwise @p value clamped to the symmetric range.
static inline float rt_gui_sanitize_signed_float(double value, double max_abs_value) {
    if (!rt_gui_double_is_finite(value))
        return 0.0f;
    return (float)rt_gui_clamp_f64(value, -max_abs_value, max_abs_value);
}

/// @brief Return the logical-to-framebuffer scale associated with a widget.
/// @details Attached widgets inherit the effective scale of their owning app. Detached widgets
///          deliberately use the identity scale so public geometry is deterministic before
///          attachment. @ref rt_gui_app_effective_scale guarantees a positive finite result.
/// @param widget Live widget whose coordinate boundary is being evaluated; may be NULL.
/// @return Positive finite effective scale, or 1 for a detached/NULL widget.
static inline float rt_gui_widget_effective_scale(vg_widget_t *widget) {
    return rt_gui_app_effective_scale(rt_gui_app_from_widget(widget));
}

/// @brief Convert a public non-negative logical metric to toolkit framebuffer units.
/// @details Invalid values become zero. Multiplication is performed in double precision and the
///          final result is clamped to @ref RT_GUI_MAX_LAYOUT_VALUE, preventing a large logical
///          value and scale from overflowing float storage.
/// @param widget Widget supplying the effective app scale.
/// @param value Public logical length.
/// @return Sanitized non-negative framebuffer length.
static inline float rt_gui_logical_length_to_physical(vg_widget_t *widget, double value) {
    if (!rt_gui_double_is_finite(value))
        return 0.0f;
    double physical = value * (double)rt_gui_widget_effective_scale(widget);
    return rt_gui_sanitize_nonnegative_float(physical, RT_GUI_MAX_LAYOUT_VALUE);
}

/// @brief Convert a signed public logical coordinate to toolkit framebuffer units.
/// @details Invalid values become zero and the scaled result is clamped symmetrically to the
///          supported layout range. Legitimate negative overlay positions remain negative.
/// @param widget Widget supplying the effective app scale.
/// @param value Public logical coordinate.
/// @return Sanitized signed framebuffer coordinate.
static inline float rt_gui_logical_coordinate_to_physical(vg_widget_t *widget, double value) {
    if (!rt_gui_double_is_finite(value))
        return 0.0f;
    double physical = value * (double)rt_gui_widget_effective_scale(widget);
    return rt_gui_sanitize_signed_float(physical, RT_GUI_MAX_LAYOUT_VALUE);
}

/// @brief Convert one toolkit framebuffer metric to a public logical value.
/// @param widget Widget supplying the effective app scale.
/// @param value Stored framebuffer coordinate or length.
/// @return Logical value after exactly one division by the positive effective scale.
static inline double rt_gui_physical_to_logical(vg_widget_t *widget, float value) {
    return (double)value / (double)rt_gui_widget_effective_scale(widget);
}

/// @brief Clamp a signed 64-bit value to caller-supplied 32-bit bounds.
/// @param value Wide integer to narrow.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return Safely narrowed value within [@p min_value, @p max_value].
static inline int32_t rt_gui_clamp_i64_to_i32(int64_t value, int32_t min_value, int32_t max_value) {
    if (value < (int64_t)min_value)
        return min_value;
    if (value > (int64_t)max_value)
        return max_value;
    return (int32_t)value;
}

/// @brief Validate a font size, returning @p fallback for non-finite input and
///        clamping otherwise to the supported [1, 256] point range.
/// @param size Candidate logical font size in points.
/// @param fallback Value returned verbatim when @p size is NaN or infinite.
/// @return @p fallback for non-finite input, otherwise the candidate clamped to [1, 256].
static inline double rt_gui_sanitize_font_size(double size, double fallback) {
    if (!rt_gui_double_is_finite(size))
        return fallback;
    return rt_gui_clamp_f64(size, 1.0, 256.0);
}

/// @brief Allocate an owned, magic-tagged copy of a runtime string's bytes.
/// @details Used to give widgets a stable NUL-terminated C buffer they own.
///          The leading magic is an internal validation guard for fields whose
///          owns_user_data flag already says they contain this wrapper type.
///          Embedded NUL bytes are preserved because the authoritative length is stored separately.
/// @param value Runtime string whose complete byte sequence should be copied.
/// @return New owned block released by @ref rt_gui_string_data_free_if_owned, or NULL on invalid
///         length, inaccessible bytes, overflow, or allocation failure.
static inline rt_gui_string_data_t *rt_gui_string_data_new(rt_string value) {
    int64_t len64 = rt_str_len(value);
    if (len64 < 0)
        return NULL;
    size_t len = (size_t)len64;
    const size_t header_size = offsetof(rt_gui_string_data_t, bytes);
    if (len > SIZE_MAX - header_size - 1)
        return NULL;
    const char *bytes = len ? rt_string_cstr(value) : "";
    if (len && !bytes)
        return NULL;
    rt_gui_string_data_t *data = (rt_gui_string_data_t *)malloc(header_size + len + 1);
    if (!data)
        return NULL;
    data->magic = RT_GUI_STRING_DATA_MAGIC;
    data->len = len;
    if (len)
        memcpy(data->bytes, bytes, len);
    data->bytes[len] = '\0';
    return data;
}

/// @brief Allocate an owned runtime string-data block from raw bytes.
/// @details Copies exactly @p len bytes, preserves embedded NULs, stores the authoritative length,
///          and appends an extra terminator for lower APIs that consume ordinary C strings.
/// @param bytes Borrowed byte span; may be NULL only when @p len is zero.
/// @param len Number of bytes to copy, excluding the appended terminator.
/// @return New owned block released by @ref rt_gui_string_data_free_if_owned, or NULL on invalid
///         input, size overflow, or allocation failure.
static inline rt_gui_string_data_t *rt_gui_string_data_new_bytes(const char *bytes, size_t len) {
    if (len > 0 && !bytes)
        return NULL;
    const size_t header_size = offsetof(rt_gui_string_data_t, bytes);
    if (len > SIZE_MAX - header_size - 1)
        return NULL;
    rt_gui_string_data_t *data = (rt_gui_string_data_t *)malloc(header_size + len + 1);
    if (!data)
        return NULL;
    data->magic = RT_GUI_STRING_DATA_MAGIC;
    data->len = len;
    if (len)
        memcpy(data->bytes, bytes, len);
    data->bytes[len] = '\0';
    return data;
}

/// @brief True if @p ptr is an rt_gui_string_data_t block (magic matches).
/// @details Only call this for pointers that an out-of-band ownership flag says
///          may be an rt_gui_string_data_t. It is not a safe generic borrowed
///          C-string discriminator.
/// @param ptr Candidate block already authorized by its separate ownership flag.
/// @return One when @p ptr is non-NULL and carries the expected magic, otherwise zero.
static inline int rt_gui_string_data_is_owned(const void *ptr) {
    if (!ptr)
        return 0;
    const rt_gui_string_data_t *data = (const rt_gui_string_data_t *)ptr;
    return data->magic == RT_GUI_STRING_DATA_MAGIC;
}

/// @brief free() @p ptr only if it is a GUI-owned string-data block.
/// @details Only use this when an ownership flag says @p ptr may hold runtime
///          string data; plain borrowed strings are not self-describing. NULL and blocks without
///          the expected magic are left untouched.
/// @param ptr Candidate owned block to release.
static inline void rt_gui_string_data_free_if_owned(void *ptr) {
    if (rt_gui_string_data_is_owned(ptr))
        free(ptr);
}

/// @brief Convert a GUI string handle back to a runtime string.
/// @details Handles owned string-data blocks using their stored byte length.
///          Embedded NULs therefore survive the round trip. NULL or an unexpected block yields
///          the canonical empty string; ownership of @p ptr is unchanged.
/// @param ptr Candidate GUI-owned string-data block.
/// @return Newly constructed runtime string value, or the canonical empty string for invalid input.
static inline rt_string rt_gui_string_data_to_rt_string(const void *ptr) {
    if (!ptr)
        return rt_str_empty();
    if (rt_gui_string_data_is_owned(ptr)) {
        const rt_gui_string_data_t *data = (const rt_gui_string_data_t *)ptr;
        return rt_string_from_bytes(data->bytes, data->len);
    }
    return rt_str_empty();
}

//=============================================================================
// Shared helpers
//=============================================================================

/// @brief Convert a runtime string to a heap-allocated NUL-terminated C string.
/// @details Allocates a new buffer via malloc, copies the string contents, and
///          appends a NUL terminator. The caller is responsible for freeing the
///          returned buffer. Embedded NUL bytes are copied unchanged, so callers requiring a
///          conventional path or identifier must use @ref rt_string_to_cstr_no_nul.
/// @param str Runtime string to copy; may be NULL.
/// @return Caller-owned C buffer, or NULL for a null/invalid string, overflow, or allocation
///         failure.
static inline char *rt_string_to_cstr(rt_string str) {
    if (!str)
        return NULL;
    int64_t len64 = rt_str_len(str);
    if (len64 < 0)
        return NULL;
    if ((uint64_t)len64 > (uint64_t)SIZE_MAX)
        return NULL;
    size_t len = (size_t)len64;
    if (len > SIZE_MAX - 1)
        return NULL;
    const char *bytes = len ? rt_string_cstr(str) : "";
    if (len && !bytes)
        return NULL;
    char *result = malloc(len + 1);
    if (!result)
        return NULL;
    if (len)
        memcpy(result, bytes, len);
    result[len] = '\0';
    return result;
}

/// @brief Test whether a runtime string contains an embedded NUL byte.
/// @details A NULL, empty, invalid-length, or inaccessible runtime string is treated as containing
///          no embedded NUL; callers that need to distinguish invalid input must validate first.
/// @param str Runtime string whose authoritative byte span should be inspected.
/// @return One when a NUL occurs before the stored length, otherwise zero.
static inline int rt_string_contains_nul(rt_string str) {
    if (!str)
        return 0;
    int64_t len64 = rt_str_len(str);
    if (len64 <= 0)
        return 0;
    const char *bytes = rt_string_cstr(str);
    if (!bytes)
        return 0;
    return memchr(bytes, '\0', (size_t)len64) != NULL;
}

/// @brief Convert a runtime string to a C string only when it contains no embedded NUL.
/// @details Use for identifiers, filesystem paths, filters, shortcuts, and other
///          non-display values where silent C-string truncation would be incorrect.
/// @param str Runtime string to validate and copy.
/// @return Caller-owned NUL-terminated copy, or NULL for embedded NUL, invalid input, overflow, or
///         allocation failure.
static inline char *rt_string_to_cstr_no_nul(rt_string str) {
    if (rt_string_contains_nul(str))
        return NULL;
    return rt_string_to_cstr(str);
}

/// @brief Convert a runtime string to GUI-visible UTF-8 text.
/// @details GUI widgets store and render NUL-terminated text, so embedded NUL
///          bytes are replaced with U+FFFD instead of truncating the suffix.
///          NULL runtime strings become an allocated empty string. All successful returns are
///          caller-owned and use malloc-compatible storage.
/// @param str Runtime display string to convert; may be NULL.
/// @return Caller-owned UTF-8 C string, or NULL on invalid length, inaccessible bytes, overflow,
///         or allocation failure.
static inline char *rt_string_to_gui_cstr(rt_string str) {
    if (!str) {
        char *empty = malloc(1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }

    int64_t len64 = rt_str_len(str);
    if (len64 < 0)
        return NULL;
    size_t len = (size_t)len64;
    const char *bytes = len ? rt_string_cstr(str) : "";
    if (len && !bytes)
        return NULL;

    size_t nul_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\0')
            nul_count++;
    }
    if (len > SIZE_MAX - 1 || nul_count > (SIZE_MAX - len - 1) / 2)
        return NULL;

    char *result = malloc(len + nul_count * 2 + 1);
    if (!result)
        return NULL;

    char *out = result;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\0') {
            memcpy(out, "\xEF\xBF\xBD", 3);
            out += 3;
        } else {
            *out++ = bytes[i];
        }
    }
    *out = '\0';
    return result;
}

/// @brief Return @p text when non-NULL, otherwise the immutable empty C string.
/// @details GUI wrapper functions often receive converted runtime strings that can
///          be NULL only after allocation or validation failure. Passing this helper's
///          result to VG labels/placeholders preserves the established empty-string
///          fallback without forcing every caller to duplicate the same ternary.
/// @param text Borrowed candidate C string.
/// @return @p text when non-NULL, otherwise a borrowed immutable empty string.
static inline const char *rt_gui_cstr_or_empty(const char *text) {
    return text ? text : "";
}

/// @brief Ensure the default font is loaded (lazy init on first use).
/// @details Loads the default font from the embedded font data if it has not
///          been loaded yet. Defined in rt_gui_app.c.
void rt_gui_ensure_default_font(void);

/// @brief Track the last clicked widget (set by GUI.App.Poll).
/// @details Records the widget that was clicked during the current event poll
///          cycle so that click handlers can query it. Defined in
///          rt_gui_widgets_complex.c.
/// @param widget Pointer to the clicked widget (may be NULL to clear).
void rt_gui_set_last_clicked(void *widget);

/// @brief Push @p dlg onto @p app's modal-dialog stack.
/// @details Ignores duplicates, grows the app-owned pointer array if necessary, associates the
///          dialog with the app, and makes it the toolkit's topmost modal root. Allocation failure
///          leaves both the stack and modal routing unchanged.
/// @param app App that will borrow and route the dialog.
/// @param dlg Live dialog to register; ownership is not transferred.
void rt_gui_push_dialog(rt_gui_app_t *app, vg_dialog_t *dlg);
/// @brief Remove @p dlg from @p app's modal-dialog stack.
/// @details Compacts the pointer array and resynchronizes modal routing when the dialog is found.
///          The operation is idempotent and does not destroy the dialog.
/// @param app App whose dialog stack should be searched.
/// @param dlg Dialog identity to remove.
void rt_gui_remove_dialog(rt_gui_app_t *app, vg_dialog_t *dlg);
/// @brief Return the topmost open dialog on an app's modal stack.
/// @details First removes closed or invalid entries and updates the toolkit modal root, so a
///          non-NULL result is always the current input-routing dialog.
/// @param app App whose modal stack should be queried; may be NULL.
/// @return Borrowed topmost open dialog, or NULL when none remains.
vg_dialog_t *rt_gui_top_dialog(rt_gui_app_t *app);

/// @brief Free per-app feature resources owned by rt_gui_features.c.
/// @details Called during app destruction to release file-drop, shortcut, overlay, tooltip,
///          notification, and other optional feature state. Graphics-disabled builds provide a
///          no-op implementation.
/// @param app App whose feature-owned allocations should be released; may be NULL.
void rt_gui_features_cleanup(rt_gui_app_t *app);

/// @brief Re-apply effective HiDPI scaling to the active app's installed theme.
/// @details Delegates to @ref rt_gui_refresh_theme so current window scale, user zoom, font size,
///          revision tracking, and dependent invalidation remain synchronized. No active app is a
///          no-op.
void rt_theme_apply_hidpi_scale(void);

//=============================================================================
// macOS native app menubar bridge
//=============================================================================

#if RT_PLATFORM_MACOS
/// @brief Register @p menubar as the app's native macOS menubar.
/// @details Replaces the native menu representation associated with the menubar's owning app
///          without transferring ownership of the lower-toolkit widget.
/// @param menubar Live runtime menubar to expose through the platform menu bar.
/// @return true if it became the active native menubar. Defined in
///         rt_gui_macos_menu.m.
bool rt_gui_macos_menu_register_menubar(vg_menubar_t *menubar);
/// @brief Unregister a previously registered native macOS menubar.
/// @param menubar Borrowed menubar being detached; unknown or NULL pointers are ignored.
void rt_gui_macos_menu_unregister_menubar(vg_menubar_t *menubar);
/// @brief Rebuild the native macOS menu from @p menubar's current contents.
/// @param menubar Borrowed registered menubar whose item hierarchy should be mirrored.
void rt_gui_macos_menu_sync_for_menubar(vg_menubar_t *menubar);
/// @brief Rebuild the native macOS menu for @p app's current menubar.
/// @param app App whose registered menubar should be synchronized; may be NULL.
void rt_gui_macos_menu_sync_app(rt_gui_app_t *app);
/// @brief Tear down native macOS menu state owned by @p app.
/// @param app App being destroyed; may be NULL.
void rt_gui_macos_menu_app_destroy(rt_gui_app_t *app);
#else
/// @brief Non-Apple no-op stub for the native macOS menubar bridge.
/// @param menubar Ignored lower-toolkit menubar.
/// @return Always false (no native menubar on this platform).
static inline bool rt_gui_macos_menu_register_menubar(vg_menubar_t *menubar) {
    (void)menubar;
    return false;
}

/// @brief Non-Apple no-op stub: unregister a native macOS menubar.
/// @param menubar Ignored lower-toolkit menubar.
static inline void rt_gui_macos_menu_unregister_menubar(vg_menubar_t *menubar) {
    (void)menubar;
}

/// @brief Non-Apple no-op stub: sync a menubar to the native macOS menu.
/// @param menubar Ignored lower-toolkit menubar.
static inline void rt_gui_macos_menu_sync_for_menubar(vg_menubar_t *menubar) {
    (void)menubar;
}

/// @brief Non-Apple no-op stub: sync an app's menus to the native macOS menu.
/// @param app Ignored GUI app.
static inline void rt_gui_macos_menu_sync_app(rt_gui_app_t *app) {
    (void)app;
}

/// @brief Non-Apple no-op stub: tear down native macOS menus for an app.
/// @param app Ignored GUI app.
static inline void rt_gui_macos_menu_app_destroy(rt_gui_app_t *app) {
    (void)app;
}
#endif

/// @brief Clear all triggered shortcut flags for the current frame.
/// @details Called at the start of each poll cycle so per-shortcut flags and the ordered trigger
///          queue describe only events from the new frame. Pending chord-prefix state is retained.
/// @param app App whose shortcut trigger state should be reset; NULL is a no-op.
void rt_shortcuts_clear_triggered(rt_gui_app_t *app);

/// @brief Check whether a key/modifier combination matches any registered shortcut.
/// @details Normalizes ASCII letter case, advances or expires any pending two-stroke chord, and
///          records enabled matches in both the shortcut flag and ordered trigger queue. Disabled
///          global shortcut routing consumes nothing.
/// @param app App owning the shortcut table and chord state.
/// @param key Translated @c VG_KEY_* code that was pressed.
/// @param mods Active translated @c VG_MOD_* bit flags.
/// @return One if a shortcut fired or a chord prefix was consumed, otherwise zero.
int8_t rt_shortcuts_check_key(rt_gui_app_t *app, int key, int mods);

//=============================================================================
// Shared status-bar / tool-bar helpers (defined in rt_gui_menus.c, consumed by
// the status-bar/tool-bar widgets in rt_gui_bars.c). Icon helpers are also used
// by the menu widgets that remain in rt_gui_menus.c.
//=============================================================================

/// @brief Resolve a runtime StatusBarItem wrapper to its live toolkit item.
/// @param item Candidate managed subhandle.
/// @return Borrowed live status-bar item, or NULL for invalid, stale, or wrong-kind input.
vg_statusbar_item_t *rt_statusbaritem_checked(void *item);
/// @brief Resolve a runtime ToolbarItem wrapper to its live toolkit item.
/// @param item Candidate managed subhandle.
/// @return Borrowed live toolbar item, or NULL for invalid, stale, or wrong-kind input.
vg_toolbar_item_t *rt_toolbaritem_checked(void *item);
/// @brief Validate an opaque handle as a live StatusBar widget.
/// @param bar Candidate widget handle.
/// @return Borrowed status bar, or NULL for an invalid handle or type mismatch.
vg_statusbar_t *rt_statusbar_checked(void *bar);
/// @brief Validate an opaque handle as a live Toolbar widget.
/// @param toolbar Candidate widget handle.
/// @return Borrowed toolbar, or NULL for an invalid handle or type mismatch.
vg_toolbar_t *rt_toolbar_checked(void *toolbar);
/// @brief Validate and narrow a public status-bar zone value.
/// @param zone Caller-supplied integer expected to lie from left through right zone.
/// @param out_zone Optional destination written only when @p zone is valid.
/// @return One for a valid zone, otherwise zero with @p out_zone unchanged.
int rt_statusbar_zone_checked(int64_t zone, vg_statusbar_zone_t *out_zone);
/// @brief Convert a runtime Pixels object from packed RRGGBBAA into a toolkit icon.
/// @details Validates dimensions and multiplication before repacking the borrowed pixel buffer into
///          RGBA bytes. The returned toolkit icon owns its internal copy; decorative-icon failures
///          are represented by @c VG_ICON_NONE rather than a runtime trap.
/// @param pixels Borrowed runtime Pixels object.
/// @return Populated owned icon on success, otherwise a zero-initialized icon.
vg_icon_t rt_gui_icon_from_pixels(void *pixels);
/// @brief Load an image path and convert it into an independently owned toolkit icon.
/// @details Releases the temporary runtime Pixels object after conversion. NULL, empty, unreadable,
///          malformed, or unrepresentable images produce the normal no-icon sentinel.
/// @param path Borrowed NUL-terminated filesystem path.
/// @return Populated owned icon on success, otherwise a zero-initialized icon.
vg_icon_t rt_gui_icon_from_path_cstr(const char *path);
