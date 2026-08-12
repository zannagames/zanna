//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_app.c
/// @brief Implements GUI application lifecycle, input dispatch, layout, and rendering.
///
/// @details
/// This module owns per-application windows and retained widget roots, switches
/// global toolkit state between applications, routes platform events through
/// overlays and the widget tree, schedules animation work, and performs full or
/// damage-region painting. Graphics-disabled definitions preserve the same C ABI
/// with deterministic inert behavior.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_app.c
// Purpose: GUI application lifecycle management for Zanna's GUI runtime layer.
//   Creates and owns the ZannaGFX window, the root vg_widget container, and the
//   default font. Provides the main loop entry points: rt_gui_app_poll (event
//   dispatch), rt_gui_app_render (layout + paint + present), and
//   rt_gui_app_destroy. Also manages the active modal dialog and a resize
//   callback so the window repaints during macOS live-resize.
//
// Key invariants:
//   - s_current_app is a global pointer valid between app_new and app_destroy;
//     widget constructors use it to inherit the default font.
//   - The root widget must NOT have a fixed size set; layout is driven by the
//     physical window dimensions on every render call.
//   - g_active_dialog is at most one modal dialog; nested dialogs are rejected.
//   - The default font is loaded lazily via rt_gui_ensure_default_font() and
//     uses the embedded font if no file path is configured.
//   - HiDPI scale is applied immediately after window creation; all widget
//     sizes and font sizes are in physical pixels.
//   - Dark theme is applied by default at app creation.
//
// Ownership/Lifetime:
//   - rt_gui_app_t is allocated via rt_obj_new_i64 (GC heap) and zeroed;
//     rt_gui_app_destroy releases the window/widget tree and is also installed
//     as a GC finalizer fallback for leaked app handles.
//   - The root widget and all its children are owned by the vg widget tree;
//     vg_widget_destroy(root) frees the entire subtree.
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/runtime/graphics/rt_gui_widgets.c (basic widget implementations),
//        src/lib/gui/include/vg.h (ZannaGUI C API),
//        src/lib/graphics/include/vgfx.h (window/event layer)
//
//===----------------------------------------------------------------------===//

#include "fonts/embedded_font.h"
#include "rt_gui_accessibility_platform.h"
#include "rt_gui_app_internal.h"
#include "rt_gui_automation_bridge.h"
#include "rt_gui_font_platform.h"
#include "rt_gui_internal.h"
#include "rt_input.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_time.h"
#include "rt_videowidget.h"

#ifdef ZANNA_ENABLE_GRAPHICS

// Global pointer to the app currently bound to the runtime-facing constructors.
rt_gui_app_t *s_current_app = NULL;
rt_gui_app_t *s_active_app = NULL;
rt_gui_app_t **s_registered_apps = NULL;
int s_registered_app_count = 0;
static int s_registered_app_cap = 0;
static const void **s_destroyed_app_handles = NULL;
static int s_destroyed_app_count = 0;
static int s_destroyed_app_cap = 0;

#define RT_GUI_DESTROYED_APP_TOMBSTONE_LIMIT 1024

extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

static void rt_gui_app_finalizer(void *app_ptr);

/// @brief Duplicate a NUL-terminated GUI app string with malloc ownership.
/// @details GUI app titles and drag/drop payload snapshots are released with
///          `free`, so this helper avoids depending on platform-specific
///          `strdup` declarations while preserving the existing ownership model.
/// @param text Source string to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_gui_app_strdup(const char *text) {
    return rt_gui_strdup_bounded(text);
}

/// @brief Return the index of `app` in `s_registered_apps`, or -1 if not found.
/// @details Linear search over the live-app registry. The registry is small
///          (typically 1-4 apps) so O(n) is fine.
/// @param app Borrowed application pointer to locate.
/// @return Zero-based registry index, or -1 when absent.
static int rt_gui_app_index(const rt_gui_app_t *app) {
    RT_ASSERT_MAIN_THREAD();
    if (!app)
        return -1;
    for (int i = 0; i < s_registered_app_count; i++) {
        if (s_registered_apps[i] == app)
            return i;
    }
    return -1;
}

/// @brief Return the index of `handle` in `s_destroyed_app_handles`, or -1 if absent.
/// @details The destroyed-app set lets the runtime reject stale handles (Zia
///          code that holds a reference past app_destroy) without dereferencing
///          a freed pointer.
/// @param handle Opaque application address to locate.
/// @return Zero-based tombstone index, or -1 when absent.
static int rt_gui_destroyed_app_index(const void *handle) {
    RT_ASSERT_MAIN_THREAD();
    if (!handle)
        return -1;
    for (int i = 0; i < s_destroyed_app_count; i++) {
        if (s_destroyed_app_handles[i] == handle)
            return i;
    }
    return -1;
}

/// @brief Return non-zero if `handle` points to an app that has already been destroyed.
/// @details Used by checked entry points to guard against use-after-destroy
///          without dereferencing the potentially freed pointer.
/// @param handle Opaque candidate application handle.
/// @return 1 when the address is tombstoned, otherwise 0.
int rt_gui_is_destroyed_app_handle(const void *handle) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_destroyed_app_index(handle) >= 0;
}

/// @brief Return non-zero if `handle` is a currently-live (not destroyed) app handle.
/// @details Checks the current, active, and registered-app lists. Used by
///          `rt_gui_app_from_widget` to confirm that a widget's user_data
///          really is an app pointer before casting it.
/// @param handle Opaque candidate application handle.
/// @return 1 when the handle identifies a registered live app, otherwise 0.
int rt_gui_is_app_handle_known(const void *handle) {
    RT_ASSERT_MAIN_THREAD();
    if (!handle)
        return 0;
    if (handle == s_current_app || handle == s_active_app)
        return 1;
    return rt_gui_app_index((const rt_gui_app_t *)handle) >= 0;
}

/// @brief Remove `handle` from the destroyed-app tombstone list.
/// @details Called when an app handle is about to be re-registered (e.g., a
///          new rt_gui_app_new called with the same address from the GC heap).
///          Without this, the recycled address would be permanently rejected.
/// @param handle Opaque application address to remove from the tombstone list.
static void rt_gui_forget_destroyed_app_handle(const void *handle) {
    RT_ASSERT_MAIN_THREAD();
    int idx = rt_gui_destroyed_app_index(handle);
    if (idx < 0)
        return;
    memmove(&s_destroyed_app_handles[idx],
            &s_destroyed_app_handles[idx + 1],
            (size_t)(s_destroyed_app_count - idx - 1) * sizeof(*s_destroyed_app_handles));
    s_destroyed_app_count--;
}

/// @brief Record `handle` in the destroyed-app tombstone list so future lookups reject it.
/// @details The list is capped and drops the oldest tombstone when full. Duplicate
///          entries are skipped. A handle that wasn't found in the live registry
///          should still be noted (e.g., if the GC freed it before unregister ran).
/// @param handle Opaque destroyed application address to record.
static void rt_gui_note_destroyed_app_handle(const void *handle) {
    RT_ASSERT_MAIN_THREAD();
    if (!handle || rt_gui_destroyed_app_index(handle) >= 0)
        return;
    if (s_destroyed_app_count >= RT_GUI_DESTROYED_APP_TOMBSTONE_LIMIT) {
        memmove(&s_destroyed_app_handles[0],
                &s_destroyed_app_handles[1],
                (size_t)(s_destroyed_app_count - 1) * sizeof(*s_destroyed_app_handles));
        s_destroyed_app_count--;
    }
    if (s_destroyed_app_count >= s_destroyed_app_cap) {
        if (s_destroyed_app_cap > INT_MAX / 2)
            return;
        int new_cap = s_destroyed_app_cap ? s_destroyed_app_cap * 2 : 4;
        if (new_cap > RT_GUI_DESTROYED_APP_TOMBSTONE_LIMIT)
            new_cap = RT_GUI_DESTROYED_APP_TOMBSTONE_LIMIT;
        void *p =
            realloc(s_destroyed_app_handles, (size_t)new_cap * sizeof(*s_destroyed_app_handles));
        if (!p)
            return;
        s_destroyed_app_handles = (const void **)p;
        s_destroyed_app_cap = new_cap;
    }
    s_destroyed_app_handles[s_destroyed_app_count++] = handle;
}

/// @brief Add `app` to the global live-app registry.
/// @details Removes the handle from the destroyed-tombstone list first (in case
///          the GC recycled the address). Duplicate registrations are silently
///          accepted. The registry uses geometric-growth realloc.
/// @param app Live application to register.
/// @return 1 on success, 0 on OOM.
int rt_gui_register_app(rt_gui_app_t *app) {
    RT_ASSERT_MAIN_THREAD();
    if (!app)
        return 0;
    rt_gui_forget_destroyed_app_handle(app);
    if (rt_gui_app_index(app) >= 0)
        return 1;
    if (s_registered_app_count >= s_registered_app_cap) {
        int new_cap = 0;
        if (!rt_gui_next_collection_capacity_i32(s_registered_app_cap,
                                                 s_registered_app_count,
                                                 4,
                                                 sizeof(*s_registered_apps),
                                                 &new_cap))
            return 0;
        void *p = realloc(s_registered_apps, (size_t)new_cap * sizeof(*s_registered_apps));
        if (!p)
            return 0;
        s_registered_apps = (rt_gui_app_t **)p;
        s_registered_app_cap = new_cap;
    }
    s_registered_apps[s_registered_app_count++] = app;
    return 1;
}

/// @brief Remove `app` from the live-app registry and add it to the tombstone list.
/// @details After removal the handle is considered destroyed — subsequent calls to
///          `rt_gui_is_app_handle_known` with this address will return false.
/// @param app Registered application to remove.
static void rt_gui_unregister_app(rt_gui_app_t *app) {
    RT_ASSERT_MAIN_THREAD();
    int idx = rt_gui_app_index(app);
    if (idx < 0)
        return;
    memmove(&s_registered_apps[idx],
            &s_registered_apps[idx + 1],
            (size_t)(s_registered_app_count - idx - 1) * sizeof(*s_registered_apps));
    s_registered_app_count--;
    rt_gui_note_destroyed_app_handle(app);
}

/// @brief Return the current wall-clock time in milliseconds.
/// @details Converts the microsecond-precision platform clock to milliseconds.
///          Used throughout the GUI subsystem for event timestamps, tooltip
///          delays, toast durations, and animation timing.
/// @return Monotonic time in milliseconds (wraps after ~585 million years).
uint64_t rt_gui_now_ms(void) {
    return (uint64_t)(rt_clock_ticks_us() / 1000);
}

/// @brief Capture the narrow validated App view consumed by C++ GUI automation.
/// @details Clearing the destination first gives failure atomicity. The deterministic scheduler
///          time is preferred after initialization so authored input shares the same temporal
///          domain as tooltip, notification, and animation work driven by RenderFrame. Opaque
///          native pointers remain borrowed and are never exposed beyond the current operation.
/// @param app_ptr Candidate managed App handle.
/// @param out_view Destination for the validated borrowed view.
/// @return 1 for a live app with a platform window, otherwise 0.
int8_t rt_gui_automation_snapshot_app(void *app_ptr, rt_gui_automation_app_view_t *out_view) {
    RT_ASSERT_MAIN_THREAD();
    if (!out_view)
        return 0;
    memset(out_view, 0, sizeof(*out_view));
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app || !app->window)
        return 0;
    double clock_ms = app->scheduler_clock_ms;
    uint64_t timestamp = !isfinite(clock_ms) || clock_ms <= 0.0 ? rt_gui_now_ms()
                         : clock_ms >= (double)UINT64_MAX       ? UINT64_MAX
                                                                : (uint64_t)clock_ms;
    out_view->window = app->window;
    out_view->root = app->root;
    out_view->event_time_ms = rt_gui_saturating_u64_to_i64(timestamp);
    return 1;
}

/// @brief Reset all global widget runtime state to defaults.
/// @details Zeroes the shared widget runtime state (focus, capture, hover
///          tracking), clears the tooltip manager, and restores the dark theme
///          as the current theme. Called when no app is active (e.g., after the
///          last app is destroyed) so stale pointers from a previous app don't
///          linger in global state.
static void rt_gui_clear_widget_runtime_state(void) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);
    *vg_tooltip_manager_get() = (vg_tooltip_manager_t){0};
    vg_theme_set_current(vg_theme_dark());
}

/// @brief Snapshot the current global widget state into an app struct.
/// @details The vg widget system uses global state for focus, keyboard capture,
///          and tooltip tracking. When switching between multiple GUI apps, we
///          must save this state so each app gets its own independent focus and
///          tooltip context. This is the "save" half of a save/restore pair.
/// @param app App whose state fields will be overwritten with current globals.
static void rt_gui_save_app_runtime_state(rt_gui_app_t *app) {
    if (!app)
        return;
    vg_widget_get_runtime_state(&app->widget_runtime_state);
    app->tooltip_manager_state = *vg_tooltip_manager_get();
}

/// @brief Restore previously-saved widget state from an app struct.
/// @details The "restore" half of the save/restore pair. Pushes the app's
///          saved focus, capture, and tooltip state back into the global vg
///          widget system. If app is NULL, clears to defaults instead.
/// @param app App whose saved state will become the active global state.
static void rt_gui_restore_app_runtime_state(rt_gui_app_t *app) {
    if (!app) {
        rt_gui_clear_widget_runtime_state();
        return;
    }
    vg_widget_set_runtime_state(&app->widget_runtime_state);
    *vg_tooltip_manager_get() = app->tooltip_manager_state;
}

#if RT_PLATFORM_MACOS
/// @brief Return non-zero if the macOS native menu bar should be re-synced during an app switch.
/// @details Syncing is needed whenever a window is involved in the transition:
///          when activating an app that has a window, or when deactivating an
///          app that had one. Avoids unnecessary native menu work when both
///          parties are windowless (e.g., headless test apps).
/// @param incoming Application being activated, or NULL.
/// @param outgoing Application being deactivated, or NULL.
/// @return Non-zero when native menu synchronization is required.
static int rt_gui_should_sync_macos_menu(rt_gui_app_t *incoming, rt_gui_app_t *outgoing) {
    return (incoming && incoming->window) || (!incoming && outgoing && outgoing->window);
}
#endif

/// @brief Resolve an app's selected logical theme before DPI scaling and accessibility overlays.
/// @details System mode maps the app's last synchronized desktop appearance to a built-in
///          singleton. Custom mode returns the app-owned clone when present; a malformed custom
///          state safely falls back to dark. NULL also maps to dark. The result is borrowed.
/// @param app App to inspect; may be NULL.
/// @return Borrowed non-NULL logical base theme; never destroy this pointer directly.
const vg_theme_t *rt_gui_app_theme_base(const rt_gui_app_t *app) {
    if (!app)
        return vg_theme_dark();
    switch (app->theme_kind) {
        case RT_GUI_THEME_LIGHT:
            return vg_theme_light();
        case RT_GUI_THEME_SYSTEM:
            return app->system_prefers_dark ? vg_theme_dark() : vg_theme_light();
        case RT_GUI_THEME_CUSTOM:
            return app->custom_theme_base ? app->custom_theme_base : vg_theme_dark();
        case RT_GUI_THEME_DARK:
        default:
            return vg_theme_dark();
    }
}

/// @brief Pre-order traversal step over the visible widget tree rooted at @p root.
/// @details Returns the next widget after @p node: its first child when @p node is
///          visible and has children, otherwise the nearest ancestor's next sibling,
///          stopping (NULL) at @p root. Gating descent on visibility lets a hidden
///          container's entire subtree be skipped in a single step.
/// @param root Traversal root that bounds ancestor climbing.
/// @param node Current traversal node.
/// @return Borrowed next traversal node, or NULL at the end.
static vg_widget_t *rt_gui_next_visible_widget(vg_widget_t *root, vg_widget_t *node) {
    if (!root || !node)
        return NULL;
    if (node->visible && node->first_child)
        return node->first_child;
    while (node && node != root && !node->next_sibling)
        node = node->parent;
    if (!node || node == root)
        return NULL;
    return node->next_sibling;
}

/// @brief Iteratively advance scheduled animation state across a widget subtree.
/// @details Called before paint with an explicit elapsed time. Every visible widget advances its
///          generic hover/press/focus motion independently of whether it will paint. Specialized
///          controls additionally advance cursor, indeterminate, and scrolling animation using
///          seconds. Invalid deltas become zero and large deltas are capped to 250 ms.
/// @param widget Root of the retained surface to tick; NULL or hidden roots are skipped.
/// @param dt Elapsed time in seconds.
/// @return True if generic state motion remains unsettled and needs another frame.
static bool rt_gui_tick_widget_tree(vg_widget_t *widget, float dt) {
    if (!widget || !widget->visible)
        return false;
    if (!isfinite(dt) || dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.25f)
        dt = 0.25f;

    bool animation_active = false;
    for (vg_widget_t *node = widget; node; node = rt_gui_next_visible_widget(widget, node)) {
        if (!node->visible)
            continue;
        animation_active |= vg_widget_anim_advance(node, dt * 1000.0f);
        if (dt <= 0.0f)
            continue;
        switch (node->type) {
            case VG_WIDGET_TEXTINPUT:
                vg_textinput_tick((vg_textinput_t *)node, dt);
                break;
            case VG_WIDGET_PROGRESS:
                vg_progressbar_tick((vg_progressbar_t *)node, dt);
                break;
            case VG_WIDGET_CODEEDITOR:
                vg_codeeditor_tick((vg_codeeditor_t *)node, dt);
                animation_active |=
                    vg_codeeditor_smooth_tick((vg_codeeditor_t *)node, dt * 1000.0f);
                break;
            case VG_WIDGET_SCROLLVIEW:
                animation_active |= vg_scrollview_tick((vg_scrollview_t *)node, dt * 1000.0f);
                break;
            case VG_WIDGET_OUTPUTPANE:
                vg_outputpane_tick((vg_outputpane_t *)node, dt);
                break;
            default:
                break;
        }
    }
    return animation_active;
}

/// @brief Test whether any widget in a visible subtree has its layout-dirty flag set.
/// @details Drives the "skip layout pass entirely if nothing changed" fast path
///          in the render loop. Hidden subtrees are skipped because their
///          dirty flags don't affect the visible output until they're shown
///          again (at which point the show transition will re-flag layout).
///          The walk short-circuits on the first dirty descendant — a typical
///          frame finds nothing dirty in O(visible widget count) and the
///          render loop can go straight to paint.
/// @param widget Borrowed subtree root.
/// @return True when a visible widget requires layout, otherwise false.
static bool rt_gui_widget_tree_needs_layout(const vg_widget_t *widget) {
    if (!widget || !widget->visible)
        return false;
    for (const vg_widget_t *node = widget; node;) {
        if (node->visible && node->needs_layout)
            return true;
        if (node->visible && node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != widget && !node->next_sibling)
            node = node->parent;
        if (!node || node == widget)
            break;
        node = node->next_sibling;
    }
    return false;
}

/// @brief Test whether any widget in a subtree has its paint-dirty flag set.
/// @details Hidden overlay roots can be paint-dirty after being dismissed; that
///          still requires one full repaint to erase their previous pixels.
/// @param widget Borrowed subtree root.
/// @return True when any relevant widget requires painting, otherwise false.
static bool rt_gui_widget_tree_needs_paint(const vg_widget_t *widget) {
    if (!widget)
        return false;
    if (widget->needs_paint)
        return true;
    if (!widget->visible)
        return false;
    for (const vg_widget_t *node = widget; node;) {
        if (node != widget && node->needs_paint)
            return true;
        if (node->visible && node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != widget && !node->next_sibling)
            node = node->parent;
        if (!node || node == widget)
            break;
        node = node->next_sibling;
    }
    return false;
}

/// @brief Clear paint-dirty flags after a complete full-window repaint.
/// @param widget Borrowed subtree root; NULL is ignored.
static void rt_gui_widget_tree_clear_paint(vg_widget_t *widget) {
    if (!widget)
        return;
    widget->needs_paint = false;
    for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling)
        rt_gui_widget_tree_clear_paint(child);
}

/// @brief Test whether any detached application overlay requires repainting.
/// @param app Borrowed application whose palettes, dialogs, notifications, tooltips, and context
/// menu are inspected.
/// @return True when at least one overlay subtree is paint-dirty.
static bool rt_gui_app_overlays_need_paint(const rt_gui_app_t *app) {
    if (!app)
        return false;
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i] &&
            rt_gui_widget_tree_needs_paint(&app->command_palettes[i]->base)) {
            return true;
        }
    }
    for (int i = 0; i < app->dialog_count; i++) {
        if (app->dialog_stack[i] && rt_gui_widget_tree_needs_paint(&app->dialog_stack[i]->base))
            return true;
    }
    if (app->notification_manager &&
        rt_gui_widget_tree_needs_paint(&app->notification_manager->base))
        return true;
    vg_tooltip_manager_t *tooltip_mgr = vg_tooltip_manager_get();
    if (tooltip_mgr && tooltip_mgr->active_tooltip &&
        rt_gui_widget_tree_needs_paint(&tooltip_mgr->active_tooltip->base)) {
        return true;
    }
    if (app->manual_tooltip && rt_gui_widget_tree_needs_paint(&app->manual_tooltip->base))
        return true;
    if (app->active_context_menu && rt_gui_widget_tree_needs_paint(&app->active_context_menu->base))
        return true;
    return false;
}

/// @brief Screen-space damage-rectangle accumulator for partial repaint (plan 07).
typedef struct rt_gui_damage {
    float x0, y0, x1, y1; ///< Union bounds in screen pixels (x1/y1 exclusive).
    bool any;             ///< True once at least one dirty widget contributed.
} rt_gui_damage_t;

// Forward declaration: rt_gui_collect_damage prunes the same internally-painting
// subtrees (e.g. ScrollView) that render_widget_tree does, so damage detection
// only compares widgets whose last-paint bounds are actually recorded.
static int rt_gui_widget_paints_children_internally(vg_widget_t *widget);

/// @brief Union a screen rectangle into the damage accumulator (ignores empties).
/// @param dmg Damage accumulator to update.
/// @param x Rectangle left coordinate in screen pixels.
/// @param y Rectangle top coordinate in screen pixels.
/// @param w Rectangle width in screen pixels.
/// @param h Rectangle height in screen pixels.
static void rt_gui_damage_add(rt_gui_damage_t *dmg, float x, float y, float w, float h) {
    if (w <= 0.0f || h <= 0.0f)
        return;
    float x1 = x + w;
    float y1 = y + h;
    if (!dmg->any) {
        dmg->x0 = x;
        dmg->y0 = y;
        dmg->x1 = x1;
        dmg->y1 = y1;
        dmg->any = true;
        return;
    }
    if (x < dmg->x0)
        dmg->x0 = x;
    if (y < dmg->y0)
        dmg->y0 = y;
    if (x1 > dmg->x1)
        dmg->x1 = x1;
    if (y1 > dmg->y1)
        dmg->y1 = y1;
}

/// @brief Add one live widget's conservative visual bounds to a damage union.
/// @details The toolkit query includes explicit overflow, popup callbacks, theme shadows, focus
///          glow, and anti-aliased effects. Invalid or empty widgets contribute nothing.
/// @param dmg Destination union; NULL is ignored.
/// @param widget Candidate live widget; NULL or retired handles are ignored.
static void rt_gui_damage_add_widget(rt_gui_damage_t *dmg, vg_widget_t *widget) {
    if (!dmg || !vg_widget_is_live(widget))
        return;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    vg_widget_get_visual_bounds(widget, &x, &y, &w, &h);
    rt_gui_damage_add(dmg, x, y, w, h);
}

/// @brief Clamp a detached context menu before querying or painting its visual rectangle.
/// @details Context-menu construction has no window handle, so the lower widget historically
///          clamped during paint. Retained damage must know the same final rectangle before paint;
///          applying the idempotent clamp here keeps both paths in agreement.
/// @param menu Visible detached context menu; NULL is ignored.
/// @param win_w Current framebuffer width in physical pixels.
/// @param win_h Current framebuffer height in physical pixels.
static void rt_gui_clamp_context_menu(vg_contextmenu_t *menu, int32_t win_w, int32_t win_h) {
    if (!menu || !menu->is_visible)
        return;
    float max_x = (float)win_w - menu->base.width;
    float max_y = (float)win_h - menu->base.height;
    if (menu->base.x > max_x)
        menu->base.x = max_x;
    if (menu->base.y > max_y)
        menu->base.y = max_y;
    if (menu->base.x < 0.0f)
        menu->base.x = 0.0f;
    if (menu->base.y < 0.0f)
        menu->base.y = 0.0f;
}

/// @brief Measure and arrange one detached tooltip into the geometry that its paint callback uses.
/// @details Tooltip paint consumes `measured_width/height`, while generic visual-bounds queries use
///          arranged geometry. Synchronizing the two before damage collection prevents a visible
///          tooltip from reporting an empty rectangle after its font or text changes.
/// @param tooltip Tooltip to prepare; NULL or hidden tooltips are ignored.
/// @param win_w Available framebuffer width in physical pixels.
/// @param win_h Available framebuffer height in physical pixels.
static void rt_gui_prepare_tooltip(vg_tooltip_t *tooltip, int32_t win_w, int32_t win_h) {
    if (!tooltip || !tooltip->is_visible)
        return;
    vg_widget_measure(&tooltip->base, (float)win_w, (float)win_h);
    vg_widget_arrange(&tooltip->base,
                      tooltip->base.x,
                      tooltip->base.y,
                      tooltip->base.measured_width,
                      tooltip->base.measured_height);
}

/// @brief Collect the current detached-overlay layer's conservative screen-space union.
/// @details Detached command palettes, dialogs, context menus, notifications, and tooltips are not
///          descendants of the normal root tree. Modal dialog backdrops contribute the complete
///          framebuffer; notification managers contribute only their visible toast cards rather
///          than their full-window positioning surface.
/// @param app App whose overlay layer is inspected; NULL produces an empty union.
/// @param tooltip_mgr App-activated tooltip manager; may be NULL.
/// @param win_w Current framebuffer width in physical pixels.
/// @param win_h Current framebuffer height in physical pixels.
/// @param dmg Destination union, initialized or pre-populated by the caller.
static void rt_gui_collect_detached_overlay_bounds(rt_gui_app_t *app,
                                                   vg_tooltip_manager_t *tooltip_mgr,
                                                   int32_t win_w,
                                                   int32_t win_h,
                                                   rt_gui_damage_t *dmg) {
    if (!app || !dmg)
        return;
    for (int i = 0; i < app->command_palette_count; ++i) {
        vg_commandpalette_t *palette = app->command_palettes[i];
        if (palette && palette->is_visible)
            rt_gui_damage_add_widget(dmg, &palette->base);
    }
    for (int i = 0; i < app->dialog_count; ++i) {
        vg_dialog_t *dialog = app->dialog_stack[i];
        if (!dialog || !dialog->is_open)
            continue;
        if (dialog->modal && dialog->overlay_color)
            rt_gui_damage_add(dmg, 0.0f, 0.0f, (float)win_w, (float)win_h);
        else
            rt_gui_damage_add_widget(dmg, &dialog->base);
    }
    if (app->active_context_menu && app->active_context_menu->is_visible)
        rt_gui_damage_add_widget(dmg, &app->active_context_menu->base);
    if (app->notification_manager) {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        if (vg_notification_manager_get_visual_bounds(app->notification_manager, &x, &y, &w, &h)) {
            rt_gui_damage_add(dmg, x, y, w, h);
        }
    }
    if (tooltip_mgr && tooltip_mgr->active_tooltip && tooltip_mgr->active_tooltip->is_visible) {
        rt_gui_damage_add_widget(dmg, &tooltip_mgr->active_tooltip->base);
    }
    if (app->manual_tooltip && app->manual_tooltip->is_visible)
        rt_gui_damage_add_widget(dmg, &app->manual_tooltip->base);
}

/// @brief Test whether the current overlay union differs from the last presented union.
/// @details A validity transition captures show/hide/removal even after the detached widget pointer
///          has been cleared. Edge comparisons use half-pixel tolerance to avoid insignificant
///          floating-point layout noise.
/// @param app App containing retained overlay state.
/// @param current Current overlay union.
/// @return True when a previous or current overlay rectangle must be damaged.
static bool rt_gui_overlay_bounds_changed(const rt_gui_app_t *app, const rt_gui_damage_t *current) {
    if (!app || !current)
        return false;
    bool previous_valid = app->overlay_last_valid != 0;
    if (previous_valid != current->any)
        return true;
    if (!previous_valid)
        return false;
    return fabsf(app->overlay_last_x - current->x0) > 0.5f ||
           fabsf(app->overlay_last_y - current->y0) > 0.5f ||
           fabsf(app->overlay_last_w - (current->x1 - current->x0)) > 0.5f ||
           fabsf(app->overlay_last_h - (current->y1 - current->y0)) > 0.5f;
}

/// @brief Commit the overlay union associated with a successfully presented retained frame.
/// @param app App whose retained overlay state is replaced; NULL is ignored.
/// @param current Current detached-overlay union.
static void rt_gui_commit_overlay_bounds(rt_gui_app_t *app, const rt_gui_damage_t *current) {
    if (!app || !current)
        return;
    app->overlay_last_valid = current->any ? 1 : 0;
    if (current->any) {
        app->overlay_last_x = current->x0;
        app->overlay_last_y = current->y0;
        app->overlay_last_w = current->x1 - current->x0;
        app->overlay_last_h = current->y1 - current->y0;
    } else {
        app->overlay_last_x = 0.0f;
        app->overlay_last_y = 0.0f;
        app->overlay_last_w = 0.0f;
        app->overlay_last_h = 0.0f;
    }
}

/// @brief Whether a widget's complete visual rectangle differs from its last paint.
/// @param w Widget whose retained paint record is compared.
/// @param x Current visual-bounds X in screen pixels.
/// @param y Current visual-bounds Y in screen pixels.
/// @param width Current visual-bounds width in screen pixels.
/// @param height Current visual-bounds height in screen pixels.
/// @return True when no prior rectangle exists or any edge changed materially.
static bool rt_gui_widget_bounds_moved(
    const vg_widget_t *w, float x, float y, float width, float height) {
    return !w->last_paint_valid || fabsf(w->last_paint_x - x) > 0.5f ||
           fabsf(w->last_paint_y - y) > 0.5f || fabsf(w->last_paint_w - width) > 0.5f ||
           fabsf(w->last_paint_h - height) > 0.5f;
}

/// @brief Accumulate the damage region for @p widget's subtree into @p dmg (plan 07).
/// @details Walks the same widgets render_widget_tree paints (pruning subtrees that
///          paint their own children, e.g. ScrollView, whose last-paint bounds are
///          never recorded). A widget contributes damage when it is paint-dirty
///          (content changed) OR its current screen bounds differ from its last-paint
///          bounds (it moved/resized) — the latter damages both the new AND vacated
///          rectangles. A widget hidden since its last paint damages its old bounds
///          once so its pixels are erased. This is what lets a partial repaint fire
///          even though the editor marks needs_layout on every keystroke: the layout
///          re-runs but is idempotent, so nothing moves and only the editor is dirty.
///          Over-coverage is always safe; only under-coverage could leave stale pixels.
/// @param widget Borrowed subtree root.
/// @param dmg Damage accumulator receiving current and vacated bounds.
static void rt_gui_collect_damage(vg_widget_t *widget, rt_gui_damage_t *dmg) {
    if (!widget || !dmg)
        return;
    vg_widget_t *node = widget;
    while (node) {
        bool descend = false;
        if (!node->visible) {
            // Hidden since its last paint → erase the vacated pixels once.
            if (node->last_paint_valid) {
                rt_gui_damage_add(dmg,
                                  node->last_paint_x,
                                  node->last_paint_y,
                                  node->last_paint_w,
                                  node->last_paint_h);
                node->last_paint_valid = false;
            }
        } else {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            vg_widget_get_visual_bounds(node, &x, &y, &w, &h);
            bool moved = rt_gui_widget_bounds_moved(node, x, y, w, h);
            if (node->needs_paint || moved) {
                rt_gui_damage_add(dmg, x, y, w, h); // current bounds
                if (moved && node->last_paint_valid)
                    rt_gui_damage_add(dmg,
                                      node->last_paint_x,
                                      node->last_paint_y,
                                      node->last_paint_w,
                                      node->last_paint_h); // vacated bounds
            }
            descend = node->first_child && !rt_gui_widget_paints_children_internally(node);
        }

        if (descend) {
            node = node->first_child;
        } else {
            while (node && node != widget && !node->next_sibling)
                node = node->parent;
            if (!node || node == widget)
                break;
            node = node->next_sibling;
        }
    }
}

/// @brief Clear paint-dirty flags across the root tree and every detached overlay.
/// @param app Borrowed application whose retained paint state is consumed.
static void rt_gui_app_clear_paint_flags(rt_gui_app_t *app) {
    if (!app)
        return;
    rt_gui_widget_tree_clear_paint(app->root);
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i])
            rt_gui_widget_tree_clear_paint(&app->command_palettes[i]->base);
    }
    for (int i = 0; i < app->dialog_count; i++) {
        if (app->dialog_stack[i])
            rt_gui_widget_tree_clear_paint(&app->dialog_stack[i]->base);
    }
    if (app->notification_manager)
        rt_gui_widget_tree_clear_paint(&app->notification_manager->base);
    vg_tooltip_manager_t *tooltip_mgr = vg_tooltip_manager_get();
    if (tooltip_mgr && tooltip_mgr->active_tooltip)
        rt_gui_widget_tree_clear_paint(&tooltip_mgr->active_tooltip->base);
    if (app->manual_tooltip)
        rt_gui_widget_tree_clear_paint(&app->manual_tooltip->base);
    if (app->active_context_menu)
        rt_gui_widget_tree_clear_paint(&app->active_context_menu->base);
}

/// @brief Scale a signed integer theme offset without overflowing its storage type.
/// @details Elevation offsets are integer pixels even though the remaining spatial tokens are
///          floats. This helper performs the multiply in double precision, clamps before the
///          conversion, and rounds to the nearest pixel. Invalid scales use the identity factor.
/// @param value Logical integer offset from the base theme.
/// @param scale Effective logical-to-framebuffer scale.
/// @return Scaled offset clamped to the representable `int` range.
static int rt_gui_scale_theme_offset(int value, float scale) {
    if (!isfinite(scale) || scale <= 0.0f)
        scale = 1.0f;
    double result = (double)value * (double)scale;
    if (result >= (double)INT_MAX)
        return INT_MAX;
    if (result <= (double)INT_MIN)
        return INT_MIN;
    return (int)lround(result);
}

/// @brief Scale the spatial members of one elevation token in-place.
/// @details Blur and offsets are distances and therefore follow DPI/user zoom. Alpha is an
///          opacity and deliberately remains unchanged.
/// @param elevation Mutable elevation token copied from an unscaled base theme.
/// @param scale Effective logical-to-framebuffer scale.
static void rt_gui_scale_elevation(vg_elevation_t *elevation, float scale) {
    if (!elevation)
        return;
    elevation->blur *= scale;
    elevation->dx = rt_gui_scale_theme_offset(elevation->dx, scale);
    elevation->dy = rt_gui_scale_theme_offset(elevation->dy, scale);
}

/// @brief Apply a HiDPI scale factor to all size-sensitive theme fields.
/// @details Multiplies typography, spacing, control metrics, named radii, elevation blur/offset,
///          and focus-glow width by the given scale. Colors, opacities, gradient strength, and
///          motion durations are dimensionless or time-based and remain unchanged. This keeps
///          base themes in logical units while producing a complete per-app framebuffer theme.
///          A non-finite or non-positive scale is replaced by 1.0.
/// @param theme Mutable theme to scale in-place.
/// @param scale Effective window-scale times user-zoom multiplier.
static void rt_gui_scale_theme(vg_theme_t *theme, float scale) {
    if (!theme)
        return;
    if (!isfinite(scale) || scale <= 0.0f)
        scale = 1.0f;
    theme->ui_scale = scale;
    theme->typography.size_small *= scale;
    theme->typography.size_normal *= scale;
    theme->typography.size_large *= scale;
    theme->typography.size_heading *= scale;
    theme->spacing.xs *= scale;
    theme->spacing.sm *= scale;
    theme->spacing.md *= scale;
    theme->spacing.lg *= scale;
    theme->spacing.xl *= scale;
    theme->button.height *= scale;
    theme->button.padding_h *= scale;
    theme->button.border_radius *= scale;
    theme->button.border_width *= scale;
    theme->input.height *= scale;
    theme->input.padding_h *= scale;
    theme->input.border_radius *= scale;
    theme->input.border_width *= scale;
    theme->scrollbar.width *= scale;
    theme->scrollbar.min_thumb_size *= scale;
    theme->scrollbar.border_radius *= scale;
    theme->radius.none *= scale;
    theme->radius.sm *= scale;
    theme->radius.md *= scale;
    theme->radius.lg *= scale;
    theme->radius.xl *= scale;
    theme->radius.pill *= scale;
    rt_gui_scale_elevation(&theme->elevation.level0, scale);
    rt_gui_scale_elevation(&theme->elevation.level1, scale);
    rt_gui_scale_elevation(&theme->elevation.level2, scale);
    rt_gui_scale_elevation(&theme->elevation.level3, scale);
    theme->focus.glow_width *= scale;
}

/// @brief Apply per-app accessibility appearance preferences to a mutable theme copy.
/// @details High contrast replaces every commonly paired surface/text/state token with a
///          deterministic palette whose normal text pairs exceed WCAG AA. Reduced motion disables
///          tweening without altering final states or DPI-scaled spatial metrics.
/// @param app App supplying accessibility preferences; may be NULL.
/// @param theme Mutable per-app theme copy to update; may be NULL.
static void rt_gui_apply_accessibility_theme(const rt_gui_app_t *app, vg_theme_t *theme) {
    if (!app || !theme)
        return;
    if (app->accessibility_high_contrast) {
        theme->colors.bg_primary = 0x000000;
        theme->colors.bg_secondary = 0x080808;
        theme->colors.bg_tertiary = 0x121212;
        theme->colors.bg_hover = 0x1F1F1F;
        theme->colors.bg_active = 0x003D4D;
        theme->colors.bg_selected = 0x005066;
        theme->colors.bg_disabled = 0x181818;
        theme->colors.fg_primary = 0xFFFFFF;
        theme->colors.fg_secondary = 0xE6E6E6;
        theme->colors.fg_tertiary = 0xCCCCCC;
        theme->colors.fg_disabled = 0xB8B8B8;
        theme->colors.fg_placeholder = 0xB8B8B8;
        theme->colors.fg_link = 0x5FE7FF;
        theme->colors.accent_primary = 0x00D7FF;
        theme->colors.accent_secondary = 0x7CEBFF;
        theme->colors.accent_danger = 0xFF6B6B;
        theme->colors.accent_warning = 0xFFD75F;
        theme->colors.accent_success = 0x7DFF9B;
        theme->colors.accent_info = 0x5FE7FF;
        theme->colors.border_primary = 0xFFFFFF;
        theme->colors.border_secondary = 0xB8B8B8;
        theme->colors.border_focus = 0x00D7FF;
        theme->colors.syntax_keyword = 0x7CEBFF;
        theme->colors.syntax_type = 0x7DFFDA;
        theme->colors.syntax_function = 0xFFF08A;
        theme->colors.syntax_variable = 0xFFFFFF;
        theme->colors.syntax_string = 0xFFB38A;
        theme->colors.syntax_number = 0xBFFF9B;
        theme->colors.syntax_comment = 0xB8B8B8;
        theme->colors.syntax_operator = 0xFFFFFF;
        theme->colors.syntax_error = 0xFF6B6B;
        theme->focus.glow_color = theme->colors.border_focus;
        theme->gradient.enabled = false;
    }
    if (app->accessibility_reduced_motion)
        theme->motion.enabled = false;
}

/// @brief Mark every app-owned surface dirty after theme metrics or colors change.
/// @details Root, detached overlays, dialogs, notifications, tooltips, and the standalone context
///          menu do not all share one parent tree. Invalidating each surface prevents the idle
///          render fast path from hiding a theme/scale change and forces measurement wherever
///          theme-dependent geometry may have changed.
/// @param app App whose retained GUI surfaces should be invalidated; NULL is a no-op.
static void rt_gui_invalidate_theme_dependents(rt_gui_app_t *app) {
    if (!app)
        return;
    vg_widget_invalidate_layout(app->root);
    for (int i = 0; i < app->command_palette_count; ++i) {
        if (app->command_palettes[i])
            vg_widget_invalidate_layout(&app->command_palettes[i]->base);
    }
    for (int i = 0; i < app->dialog_count; ++i) {
        if (app->dialog_stack[i])
            vg_widget_invalidate_layout(&app->dialog_stack[i]->base);
    }
    if (app->notification_manager)
        vg_widget_invalidate_layout(&app->notification_manager->base);
    if (app->tooltip_manager_state.active_tooltip)
        vg_widget_invalidate_layout(&app->tooltip_manager_state.active_tooltip->base);
    if (s_active_app == app) {
        vg_tooltip_manager_t *tooltip_manager = vg_tooltip_manager_get();
        if (tooltip_manager && tooltip_manager->active_tooltip)
            vg_widget_invalidate_layout(&tooltip_manager->active_tooltip->base);
    }
    if (app->manual_tooltip)
        vg_widget_invalidate_layout(&app->manual_tooltip->base);
    if (app->active_context_menu)
        vg_widget_invalidate_layout(&app->active_context_menu->base);
}

/// @brief Return the app's effective logical-to-framebuffer scale.
/// @details Window scale and user zoom are sanitized independently so an invalid factor cannot
///          poison the product. The result is also checked for overflow/non-finite values.
/// @param app App to inspect; NULL returns the identity scale.
/// @return Positive finite scale suitable for theme and font conversion.
float rt_gui_app_effective_scale(const rt_gui_app_t *app) {
    if (!app)
        return 1.0f;
    float window_scale = app->window ? vgfx_window_get_scale(app->window) : 1.0f;
    if (!isfinite(window_scale) || window_scale <= 0.0f)
        window_scale = 1.0f;
    float user_scale = app->user_scale;
    if (!isfinite(user_scale) || user_scale <= 0.0f)
        user_scale = 1.0f;
    float effective = window_scale * user_scale;
    return isfinite(effective) && effective > 0.0f ? effective : 1.0f;
}

/// @brief Convert the app's logical default font size to framebuffer pixels.
/// @details The logical value is sanitized before multiplication and the result is checked so a
///          corrupt app cannot propagate NaN or infinity into text measurement.
/// @param app App to inspect; NULL returns the 14-pixel compatibility default.
/// @return Positive finite effective font size.
float rt_gui_app_effective_font_size(const rt_gui_app_t *app) {
    float logical_size = app ? app->default_font_size : 14.0f;
    if (!isfinite(logical_size) || logical_size <= 0.0f)
        logical_size = 14.0f;
    float effective_size = logical_size * rt_gui_app_effective_scale(app);
    return isfinite(effective_size) && effective_size > 0.0f ? effective_size : logical_size;
}

/// @brief Build one effective per-app theme without publishing it.
/// @details Clones @p base, applies the effective scale exactly once, then composes high-contrast
///          and reduced-motion preferences. This isolated allocation step enables atomic custom
///          palette replacement.
/// @param app App supplying accessibility preferences; must be non-NULL.
/// @param base Borrowed logical unscaled palette; must be non-NULL.
/// @param scale Sanitized positive effective scale.
/// @return Owned effective theme, or NULL on allocation failure.
static vg_theme_t *rt_gui_build_effective_theme(const rt_gui_app_t *app,
                                                const vg_theme_t *base,
                                                float scale) {
    if (!app || !base)
        return NULL;
    vg_theme_t *theme = vg_theme_create(base->name, base);
    if (!theme)
        return NULL;
    if (!theme->typography.font_regular && vg_font_is_live(app->default_font))
        theme->typography.font_regular = app->default_font;
    vg_font_t *bold_fallback =
        vg_font_is_live(app->role_font_bold) ? app->role_font_bold : app->default_font;
    vg_font_t *mono_fallback =
        vg_font_is_live(app->role_font_mono) ? app->role_font_mono : app->default_font;
    if (!theme->typography.font_bold && vg_font_is_live(bold_fallback))
        theme->typography.font_bold = bold_fallback;
    if (!theme->typography.font_mono && vg_font_is_live(mono_fallback))
        theme->typography.font_mono = mono_fallback;
    rt_gui_scale_theme(theme, scale);
    rt_gui_apply_accessibility_theme(app, theme);
    return theme;
}

/// @brief Publish a fully built effective theme and retire the previous installed copy.
/// @details This function performs no allocation. The revision saturates at UINT64_MAX, and all
///          theme-dependent surfaces are invalidated only after every app cache field is coherent.
/// @param app App receiving the new theme; must be non-NULL.
/// @param theme Owned effective theme transferred to the app; must be non-NULL.
/// @param base Borrowed logical base whose lifetime exceeds this installed cache entry.
/// @param scale Effective scale used to build @p theme.
static void rt_gui_commit_effective_theme(rt_gui_app_t *app,
                                          vg_theme_t *theme,
                                          const vg_theme_t *base,
                                          float scale) {
    vg_theme_t *old_theme = app->theme;
    app->theme = theme;
    app->theme_base = base;
    app->theme_scale = scale;
    if (app->theme_revision < UINT64_MAX)
        ++app->theme_revision;
    if (s_active_app == app)
        vg_theme_set_current(app->theme);
    rt_gui_reapply_default_font(app);
    rt_gui_invalidate_theme_dependents(app);
    vg_theme_destroy(old_theme);
}

/// @brief Rebuild and activate the app's scaled theme if the base or scale changed.
/// @details Creates a fresh mutable copy of the logical base, scales it to the current window's
///          effective HiDPI factor, and applies accessibility preferences. The cached path is
///          allocation-free. Allocation failure leaves every installed theme/cache field intact.
/// @param app App whose theme to refresh (no-op if NULL).
void rt_gui_refresh_theme(rt_gui_app_t *app) {
    if (!app)
        return;

    const vg_theme_t *base = rt_gui_app_theme_base(app);
    float scale = rt_gui_app_effective_scale(app);

    if (app->theme && app->theme_base == base && app->theme_scale == scale) {
        if (s_active_app == app)
            vg_theme_set_current(app->theme);
        return;
    }

    vg_theme_t *theme = rt_gui_build_effective_theme(app, base, scale);
    if (!theme)
        return;
    rt_gui_commit_effective_theme(app, theme, base, scale);
}

/// @brief Atomically install an owned logical custom palette into one app.
/// @details The effective clone is built before any app field changes. Success transfers candidate
///          ownership, publishes Custom mode and its cache in one main-thread operation, then
///          destroys the displaced logical palette. Failure leaves candidate caller-owned.
/// @param app Target app.
/// @param candidate Owned unscaled custom palette offered for transfer.
/// @return One on complete installation, otherwise zero.
int rt_gui_install_custom_theme(rt_gui_app_t *app, vg_theme_t *candidate) {
    if (!app || !candidate)
        return 0;
    float scale = rt_gui_app_effective_scale(app);
    vg_theme_t *effective = rt_gui_build_effective_theme(app, candidate, scale);
    if (!effective)
        return 0;
    vg_theme_t *old_custom = app->custom_theme_base;
    app->custom_theme_base = candidate;
    app->theme_kind = RT_GUI_THEME_CUSTOM;
    rt_gui_commit_effective_theme(app, effective, candidate, scale);
    vg_theme_destroy(old_custom);
    return 1;
}

/// @brief Synchronize a System-mode app with the platform's current appearance preference.
/// @details The adapter query is allocation-free. A changed preference invalidates the cached
///          logical base and refreshes immediately; unchanged or non-System apps are no-ops.
/// @param app App to synchronize; NULL is accepted.
void rt_gui_sync_system_theme(rt_gui_app_t *app) {
    if (!app || app->theme_kind != RT_GUI_THEME_SYSTEM)
        return;
    int32_t prefers_dark = rt_gui_accessibility_platform_prefers_dark(app->window) ? 1 : 0;
    if (prefers_dark == app->system_prefers_dark)
        return;
    app->system_prefers_dark = prefers_dark;
    app->theme_base = NULL;
    app->theme_scale = 0.0f;
    rt_gui_refresh_theme(app);
}

/// @brief Switch the app among dark, light, system, and custom theme modes.
/// @details Invalid enum values and custom mode without an installed palette are ignored. System
///          mode samples the platform immediately. A repeated selection still refreshes scale but
///          avoids forced allocation when the effective base is unchanged.
/// @param app Target app; NULL is a no-op.
/// @param kind Requested theme mode.
void rt_gui_set_theme_kind(rt_gui_app_t *app, rt_gui_theme_kind_t kind) {
    if (!app)
        return;
    if (kind < RT_GUI_THEME_DARK || kind > RT_GUI_THEME_CUSTOM)
        return;
    if (kind == RT_GUI_THEME_CUSTOM && !app->custom_theme_base)
        return;
    if (kind == RT_GUI_THEME_SYSTEM)
        app->system_prefers_dark = rt_gui_accessibility_platform_prefers_dark(app->window) ? 1 : 0;
    if (app->theme_kind == kind) {
        rt_gui_refresh_theme(app);
        return;
    }
    app->theme_kind = kind;
    app->theme_base = NULL;
    app->theme_scale = 0.0f;
    rt_gui_refresh_theme(app);
}

/// @brief Return the currently active GUI app, or NULL if none is active.
/// @details The active app is the one whose widget tree, theme, and runtime
///          state are installed in the global vg widget system. There is at
///          most one active app at a time.
/// @return Pointer to the active app, or NULL.
rt_gui_app_t *rt_gui_get_active_app(void) {
    return s_active_app;
}

/// @brief Make the given app the active GUI app.
/// @details Saves the outgoing app's widget runtime state (focus, capture,
///          tooltips), installs the incoming app's state, refreshes its theme,
///          and syncs the macOS native menu bar. If the app is already active,
///          just refreshes the theme (handles window scale changes). Both
///          s_active_app and s_current_app are updated so widget constructors
///          and font lookups use the correct app context.
/// @param app App to activate. May be NULL to deactivate.
void rt_gui_activate_app(rt_gui_app_t *app) {
    RT_ASSERT_MAIN_THREAD();
#if RT_PLATFORM_MACOS
    rt_gui_app_t *previous_active = s_active_app;
#endif
    if (app == s_active_app) {
        s_current_app = app;
        rt_gui_sync_system_theme(app);
        rt_gui_refresh_theme(app);
        return;
    }

    if (s_active_app) {
        rt_gui_save_app_runtime_state(s_active_app);
    }

    s_active_app = app;
    s_current_app = app;
    rt_gui_sync_system_theme(app);
    rt_gui_refresh_theme(app);
    rt_gui_restore_app_runtime_state(app);
#if RT_PLATFORM_MACOS
    if (rt_gui_should_sync_macos_menu(app, previous_active))
        rt_gui_macos_menu_sync_app(app);
#else
    rt_gui_macos_menu_sync_app(app);
#endif
}

/// @brief Resolve the owning app for a given widget by walking the parent chain.
/// @details Widgets don't store a direct back-pointer to their app. Instead,
///          the root widget's user_data is set to the app pointer at creation.
///          This function walks up the parent chain until it finds a root
///          (parentless) widget and returns its user_data. If the pointer is
///          actually an app handle (registry check), it's returned directly.
/// @param widget Any widget in the tree.
/// @return The owning rt_gui_app_t, or NULL if the widget is detached.
rt_gui_app_t *rt_gui_app_from_widget(vg_widget_t *widget) {
    if (rt_gui_is_destroyed_app_handle(widget))
        return NULL;
    if (rt_gui_is_app_handle(widget))
        return (rt_gui_app_t *)widget;
    if (!vg_widget_is_live(widget))
        return NULL;
    for (vg_widget_t *w = widget; w; w = w->parent) {
        if (!w->parent && w->user_data) {
            rt_gui_app_t *candidate = (rt_gui_app_t *)w->user_data;
            if (rt_gui_is_app_handle(candidate))
                return candidate;
        }
    }
    return NULL;
}

/// @brief Return a validated GUI app's current scheduler generation.
/// @details This isolated-module bridge is side-effect free and returns zero for stale or foreign
///          pointers. The render scheduler is the sole writer of the generation field.
/// @param app_ptr Candidate managed application handle.
/// @return Current frame generation, or 0 for an invalid handle.
uint64_t rt_gui_app_frame_generation_for_owner(void *app_ptr) {
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    return app ? app->frame_generation : 0u;
}

/// @brief Double the dialog stack capacity when full (amortized O(1) growth).
/// @details The dialog stack uses a dynamic array with geometric growth. This
///          avoids per-push allocation while keeping memory usage reasonable
///          for the typical case (1-3 nested dialogs).
/// @param app App whose dialog stack to grow.
/// @return 1 when capacity is available, otherwise 0.
static int rt_gui_grow_dialog_stack(rt_gui_app_t *app) {
    if (!app || app->dialog_count < 0 || app->dialog_cap < 0 || app->dialog_count > app->dialog_cap)
        return 0;
    if (app->dialog_count < app->dialog_cap)
        return 1;
    int new_cap = 0;
    if (!rt_gui_next_collection_capacity_i32(
            app->dialog_cap, app->dialog_count, 4, sizeof(*app->dialog_stack), &new_cap))
        return 0;
    void *p = realloc(app->dialog_stack, (size_t)new_cap * sizeof(*app->dialog_stack));
    if (!p)
        return 0;
    app->dialog_stack = p;
    app->dialog_cap = new_cap;
    return 1;
}

/// @brief Compact the dialog stack and set the topmost open dialog as modal root.
/// @details Removes closed dialogs from the stack (compacting in-place), then
///          tells the vg widget system which widget is the modal root. When a
///          modal root is set, all input events outside that widget's bounds are
///          blocked — this implements the modal dialog interaction pattern where
///          the user must dismiss the dialog before interacting with the rest
///          of the UI.
/// @param app App whose dialog stack to synchronize.
void rt_gui_sync_modal_root(rt_gui_app_t *app) {
    if (!app) {
        vg_widget_set_modal_root(NULL);
        return;
    }

    vg_dialog_t *top = NULL;
    int write = 0;
    for (int i = 0; i < app->dialog_count; i++) {
        vg_dialog_t *dlg = app->dialog_stack[i];
        if (!dlg || !vg_widget_is_live(&dlg->base) || !dlg->is_open)
            continue;
        app->dialog_stack[write++] = dlg;
        top = dlg;
    }
    app->dialog_count = write;
    vg_widget_set_modal_root(top ? &top->base : NULL);
}

/// @brief Push a dialog onto the app's modal dialog stack.
/// @details Adds the dialog if it isn't already present (dedup check), grows
///          the stack if needed, and syncs the modal root so the new dialog
///          captures input. The dialog's user_data is set to the app pointer
///          so the dialog can find its owning app when needed.
/// @param app Target app.
/// @param dlg Dialog to push (ignored if already on the stack).
void rt_gui_push_dialog(rt_gui_app_t *app, vg_dialog_t *dlg) {
    if (!app || !dlg)
        return;
    for (int i = 0; i < app->dialog_count; i++) {
        if (app->dialog_stack[i] == dlg)
            return;
    }
    if (!rt_gui_grow_dialog_stack(app))
        return;
    if (app->dialog_count < app->dialog_cap) {
        dlg->base.user_data = app;
        app->dialog_stack[app->dialog_count++] = dlg;
        rt_gui_sync_modal_root(app);
    }
}

/// @brief Remove a dialog from the app's modal dialog stack.
/// @details Finds the dialog by pointer identity, shifts subsequent entries
///          down via memmove, and re-syncs the modal root. If the removed
///          dialog was the topmost modal, the next dialog (or NULL) becomes
///          the new modal root.
/// @param app Target app.
/// @param dlg Dialog to remove.
void rt_gui_remove_dialog(rt_gui_app_t *app, vg_dialog_t *dlg) {
    if (!app || !dlg)
        return;
    for (int i = 0; i < app->dialog_count; i++) {
        if (app->dialog_stack[i] != dlg)
            continue;
        memmove(&app->dialog_stack[i],
                &app->dialog_stack[i + 1],
                (size_t)(app->dialog_count - i - 1) * sizeof(*app->dialog_stack));
        app->dialog_count--;
        rt_gui_sync_modal_root(app);
        break;
    }
}

/// @brief Return the topmost open dialog on the stack, or NULL if none.
/// @details Compacts closed dialogs before returning, so the result is always
///          an open dialog or NULL. Used by the poll/render loops to determine
///          the current modal root and event routing target.
/// @param app App to query.
/// @return Topmost open vg_dialog_t, or NULL.
vg_dialog_t *rt_gui_top_dialog(rt_gui_app_t *app) {
    if (!app)
        return NULL;
    rt_gui_sync_modal_root(app);
    return app->dialog_count > 0 ? app->dialog_stack[app->dialog_count - 1] : NULL;
}

/// @brief Double the command palette array capacity when full.
/// @details Same geometric-growth pattern as the dialog stack. Apps rarely have
///          more than 1-2 command palettes, but the dynamic array handles the
///          general case safely.
/// @param app App whose command palette array to grow.
/// @return 1 when capacity is available, otherwise 0.
static int rt_gui_grow_command_palette_array(rt_gui_app_t *app) {
    if (!app || app->command_palette_count < 0 || app->command_palette_cap < 0 ||
        app->command_palette_count > app->command_palette_cap)
        return 0;
    if (app->command_palette_count < app->command_palette_cap)
        return 1;
    int new_cap = 0;
    if (!rt_gui_next_collection_capacity_i32(app->command_palette_cap,
                                             app->command_palette_count,
                                             4,
                                             sizeof(*app->command_palettes),
                                             &new_cap))
        return 0;
    void *p = realloc(app->command_palettes, (size_t)new_cap * sizeof(*app->command_palettes));
    if (!p)
        return 0;
    app->command_palettes = p;
    app->command_palette_cap = new_cap;
    return 1;
}

/// @brief Register a command palette with the app for event routing and rendering.
/// @details Command palettes are rendered as overlays above all other content
///          and receive keyboard/mouse events before the widget tree. The app
///          tracks all registered palettes so the poll loop can route events to
///          whichever one is visible. Duplicate registrations are silently ignored.
/// @param app Target app.
/// @param palette Command palette to register.
void rt_gui_register_command_palette(rt_gui_app_t *app, vg_commandpalette_t *palette) {
    if (!app || !palette)
        return;
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i] == palette)
            return;
    }
    if (!rt_gui_grow_command_palette_array(app))
        return;
    if (app->command_palette_count < app->command_palette_cap) {
        app->command_palettes[app->command_palette_count++] = palette;
    }
}

/// @brief Unregister a command palette from the app.
/// @details Removes the palette from the app's tracking array so it is no
///          longer rendered or receives events. Called during palette destruction.
/// @param app Target app.
/// @param palette Command palette to unregister.
void rt_gui_unregister_command_palette(rt_gui_app_t *app, vg_commandpalette_t *palette) {
    if (!app || !palette)
        return;
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i] != palette)
            continue;
        memmove(&app->command_palettes[i],
                &app->command_palettes[i + 1],
                (size_t)(app->command_palette_count - i - 1) * sizeof(*app->command_palettes));
        app->command_palette_count--;
        break;
    }
}

/// @brief Find the topmost visible command palette, if any.
/// @details Scans the palette array in reverse (most-recently-registered first)
///          to find one that is both programmatically visible (is_visible) and
///          widget-visible (base.visible). The poll loop uses this to intercept
///          keyboard events before they reach the widget tree.
/// @param app App to search.
/// @return Topmost visible palette, or NULL if none are open.
static vg_commandpalette_t *rt_gui_top_visible_command_palette(rt_gui_app_t *app) {
    if (!app)
        return NULL;
    for (int i = app->command_palette_count - 1; i >= 0; i--) {
        vg_commandpalette_t *palette = app->command_palettes[i];
        if (palette && palette->is_visible && palette->base.visible)
            return palette;
    }
    return NULL;
}

/// @brief Push or pop the active modal dialog.
/// @details When dlg is non-NULL, pushes it onto the dialog stack so it becomes
///          the modal root. When dlg is NULL, pops the topmost dialog. This is
///          the Zia-facing entry point for modal dialog management.
/// @param dlg Dialog to push, or NULL to pop the topmost dialog.
void rt_gui_set_active_dialog(void *dlg) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = s_current_app ? s_current_app : s_active_app;
    if (!app)
        return;
    if (dlg) {
        rt_gui_push_dialog(app, (vg_dialog_t *)dlg);
    } else {
        vg_dialog_t *top = rt_gui_top_dialog(app);
        if (top)
            rt_gui_remove_dialog(app, top);
    }
}

/// @brief Resize callback invoked by the platform during live window resizing.
/// @details On macOS, the Cocoa run loop enters a modal tracking mode during
///          window resize, blocking our main thread. Without this callback, the
///          framebuffer stays black until the drag ends. By registering this as
///          the vgfx resize callback, we re-render the full UI on every resize
///          event, giving smooth live feedback.
/// @param userdata Pointer to the rt_gui_app_t (set at registration time).
/// @param w New window width in physical pixels (unused; render reads it).
/// @param h New window height in physical pixels (unused; render reads it).
static void rt_gui_app_resize_render(void *userdata, int32_t w, int32_t h) {
    (void)w;
    (void)h;
    rt_gui_app_render(userdata);
}

/// @brief Internal reason for an expected GUI application construction failure.
/// @details The enum stays private to the shared constructor so the compatibility constructor and
///          `App.TryNew` execute identical cleanup paths while exposing different error channels.
typedef enum {
    RT_GUI_APP_CREATE_OK = 0,     ///< Construction completed successfully.
    RT_GUI_APP_CREATE_STATE = 1,  ///< Managed/native application state could not be allocated.
    RT_GUI_APP_CREATE_WINDOW = 2, ///< The graphics backend could not create a native window.
    RT_GUI_APP_CREATE_ROOT = 3,   ///< The root widget container could not be allocated.
} rt_gui_app_create_error_t;

/// @brief Store a constructor failure reason when the caller requested diagnostics.
/// @param out_error Optional destination owned by the caller.
/// @param error Reason to store.
static void rt_gui_app_set_create_error(rt_gui_app_create_error_t *out_error,
                                        rt_gui_app_create_error_t error) {
    if (out_error)
        *out_error = error;
}

/// @brief Return the stable public message for one app-construction failure reason.
/// @param error Failure reason produced by @ref rt_gui_app_create.
/// @return Process-lifetime NUL-terminated error text.
static const char *rt_gui_app_create_error_message(rt_gui_app_create_error_t error) {
    switch (error) {
        case RT_GUI_APP_CREATE_WINDOW:
            return "GUI application window could not be created";
        case RT_GUI_APP_CREATE_ROOT:
            return "GUI root widget could not be allocated";
        case RT_GUI_APP_CREATE_STATE:
        case RT_GUI_APP_CREATE_OK:
        default:
            return "GUI application state could not be allocated";
    }
}

/// @brief Shared implementation for compatibility and Result-returning app constructors.
/// @details Allocates the app struct on the GC heap (rt_obj_new_i64), creates a
///          platform window via vgfx, sets up a root container widget, registers
///          the live-resize callback, applies dark theme by default, and activates
///          the app as current. The root widget is NOT given a fixed size — it is
///          resized dynamically every frame from the physical window dimensions. Every failure
///          path unwinds native resources before publishing the corresponding error reason.
/// @param title  Window title (runtime string).
/// @param width  Initial window width in logical pixels (clamped to [1, INT32_MAX]).
/// @param height Initial window height in logical pixels.
/// @param out_error Optional destination for an exact construction failure reason.
/// @return Pointer to the new app, or NULL on failure.
static void *rt_gui_app_create(rt_string title,
                               int64_t width,
                               int64_t height,
                               rt_gui_app_create_error_t *out_error) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_OK);
    rt_gui_app_t *app = (rt_gui_app_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_gui_app_t));
    if (!app) {
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_STATE);
        return NULL;
    }
    memset(app, 0, sizeof(rt_gui_app_t));
    app->magic = RT_GUI_APP_MAGIC;
    rt_obj_set_finalizer(app, rt_gui_app_finalizer);

    // Create window
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = (int32_t)(width < 1 ? 1 : width > INT32_MAX ? INT32_MAX : width);
    params.height = (int32_t)(height < 1 ? 1 : height > INT32_MAX ? INT32_MAX : height);
    char *ctitle = rt_string_to_gui_cstr(title);
    if (!ctitle) {
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_STATE);
        return NULL;
    }
    const char *window_title = ctitle;
    params.title = window_title;
    params.resizable = 1;

    app->window = vgfx_create_window(&params);
    app->title = rt_gui_app_strdup(window_title);
    app->application_name = rt_gui_app_strdup(window_title);
    free(ctitle);

    if (!app->window) {
        // app is GC-allocated (rt_obj_new_i64) so it will be reclaimed by the
        // collector. Zero the struct so the GC finalizer (if any) sees clean state.
        free(app->title);
        app->title = NULL;
        free(app->application_name);
        app->application_name = NULL;
        memset(app, 0, sizeof(rt_gui_app_t));
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_WINDOW);
        return NULL;
    }
    if (!app->title || !app->application_name) {
        vgfx_destroy_window(app->window);
        app->window = NULL;
        free(app->title);
        app->title = NULL;
        free(app->application_name);
        app->application_name = NULL;
        memset(app, 0, sizeof(rt_gui_app_t));
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_STATE);
        return NULL;
    }

    // Register resize callback so the window repaints during macOS live-resize.
    // Without this, the Cocoa modal resize loop blocks our main thread and
    // the framebuffer stays black until the drag ends.
    vgfx_set_resize_callback(app->window, rt_gui_app_resize_render, app);

    // Create root container. The root is sized dynamically every frame by
    // vg_widget_layout(root, win_w, win_h) in rt_gui_app_render, which reads
    // the current physical window dimensions from vgfx_get_size. Do NOT pin it
    // with vg_widget_set_fixed_size — that creates hard min=max constraints that
    // prevent the layout engine from resizing the root on window resize.
    app->root = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!app->root) {
        vgfx_destroy_window(app->window);
        free(app->title);
        app->title = NULL;
        free(app->application_name);
        app->application_name = NULL;
        memset(app, 0, sizeof(rt_gui_app_t));
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_ROOT);
        return NULL;
    }

    app->theme_kind = RT_GUI_THEME_DARK;
    app->system_prefers_dark = rt_gui_accessibility_platform_prefers_dark(app->window) ? 1 : 0;
    app->user_scale = 1.0f;
    app->accessibility_high_contrast = rt_gui_accessibility_platform_high_contrast(app->window);
    app->accessibility_reduced_motion = rt_gui_accessibility_platform_reduced_motion(app->window);
    app->root->user_data = app;
    vg_widget_set_accessible_role(app->root, VG_ACCESSIBLE_ROLE_APPLICATION);
    vg_widget_set_accessible_name(app->root, app->title);
    app->shortcuts_global_enabled = 1;
    app->manual_tooltip_delay_ms = 500;

    // Damage-region rendering is on by default; ZANNA_GUI_FULL_REPAINT=1 is the
    // one-line kill switch that restores unconditional full-window repaints.
    {
        const char *full = getenv("ZANNA_GUI_FULL_REPAINT");
        app->partial_paint_enabled = (full && full[0] == '1' && full[1] == '\0') ? 0 : 1;
    }

    if (!rt_gui_register_app(app)) {
        vg_widget_destroy(app->root);
        vgfx_destroy_window(app->window);
        free(app->title);
        app->title = NULL;
        free(app->application_name);
        app->application_name = NULL;
        memset(app, 0, sizeof(rt_gui_app_t));
        rt_gui_app_set_create_error(out_error, RT_GUI_APP_CREATE_STATE);
        return NULL;
    }

    rt_gui_accessibility_platform_attach(app->window, app->root);
    rt_gui_activate_app(app);
    app->theme_reported_revision = app->theme_revision;
    return app;
}

/// @brief Report whether this runtime contains the GUI backend.
/// @details Graphics-enabled builds return true without touching the window system. This keeps
///          startup capability checks deterministic even on headless hosts.
/// @return Always 1 in this implementation branch.
int64_t rt_gui_system_is_available(void) {
    return 1;
}

/// @brief Return the GUI capability failure reason for a graphics-enabled runtime.
/// @details Backend initialization is attempted only by app construction, so compilation-level
///          availability has no explanatory error text.
/// @return Caller-owned empty runtime string.
rt_string rt_gui_system_get_unavailable_reason(void) {
    return rt_str_empty();
}

/// @brief Create a GUI app through the compatibility nullable constructor.
/// @details Expected construction failures retain the historical NULL result. New code can use
///          @ref rt_gui_app_try_new to receive a stable error value.
/// @param title Window title.
/// @param width Initial logical width.
/// @param height Initial logical height.
/// @return New application handle, or NULL on failure.
void *rt_gui_app_new(rt_string title, int64_t width, int64_t height) {
    return rt_gui_app_create(title, width, height, NULL);
}

/// @brief Create a GUI app and encode expected construction failure in `Zanna.Result`.
/// @details The Result retains a successful app handle. The constructor's temporary reference is
///          released after wrapping so ownership is transferred exactly once to the Result.
/// @param title Window title.
/// @param width Initial logical width.
/// @param height Initial logical height.
/// @return Caller-owned Result containing the app or an exact error string.
void *rt_gui_app_try_new(rt_string title, int64_t width, int64_t height) {
    rt_gui_app_create_error_t error = RT_GUI_APP_CREATE_OK;
    void *app = rt_gui_app_create(title, width, height, &error);
    if (!app)
        return rt_result_err_str(rt_const_cstr(rt_gui_app_create_error_message(error)));

    void *result = rt_result_ok(app);
    if (rt_obj_release_check0(app))
        rt_obj_free(app);
    return result;
}

/// @brief GC finalizer for GUI apps. Mirrors explicit Destroy so native windows
///        and widget trees do not leak when user code drops the app handle.
/// @param app_ptr Managed application object selected for finalization.
static void rt_gui_app_finalizer(void *app_ptr) {
    rt_gui_app_t *app = (rt_gui_app_t *)app_ptr;
    if (app && app->magic == RT_GUI_APP_MAGIC)
        rt_gui_app_destroy(app_ptr);
}

/// @brief Install app-owned regular, bold, and monospace typography fallbacks lazily.
/// @details Proportional regular/bold roles use the zero-dependency host adapter first. Regular
///          and mono fall back to independent copies of embedded JetBrains Mono; bold falls back
///          to an independent embedded copy and finally aliases regular only if allocation/parsing
///          fails. All role metadata stores the app's logical point size. Missing roles in the
///          current effective theme are populated without overriding explicit custom roles.
///          Ownership flags prevent double destruction when a fallback aliases regular.
void rt_gui_ensure_default_font(void) {
    RT_ASSERT_MAIN_THREAD();
    if (!s_current_app || s_current_app->default_font)
        return;
    rt_gui_app_t *app = s_current_app;
    app->default_font_size = 14.0f;
    app->default_font = rt_gui_font_platform_load_system_ui(false);
    if (!app->default_font)
        app->default_font = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    if (!app->default_font)
        return;
    app->default_font_owned = 1;
    vg_font_set_logical_size(app->default_font, app->default_font_size);

    app->role_font_bold = rt_gui_font_platform_load_system_ui(true);
    if (!app->role_font_bold)
        app->role_font_bold = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    if (app->role_font_bold) {
        app->role_font_bold_owned = 1;
        vg_font_set_logical_size(app->role_font_bold, app->default_font_size);
    } else {
        app->role_font_bold = app->default_font;
        app->role_font_bold_owned = 0;
    }

    app->role_font_mono = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    if (app->role_font_mono) {
        app->role_font_mono_owned = 1;
        vg_font_set_logical_size(app->role_font_mono, app->default_font_size);
    } else {
        app->role_font_mono = app->default_font;
        app->role_font_mono_owned = 0;
    }

    if (app->theme) {
        if (!app->theme->typography.font_regular)
            app->theme->typography.font_regular = app->default_font;
        if (!app->theme->typography.font_bold)
            app->theme->typography.font_bold = app->role_font_bold;
        if (!app->theme->typography.font_mono)
            app->theme->typography.font_mono = app->role_font_mono;
    }
}

/// @brief Tear down the GUI application, releasing all owned resources.
/// @details Destruction order is critical to avoid use-after-free:
///          1. Activate the app so cleanup operates on the right global state.
///          2. Clean up feature resources (command palettes, notifications, etc.).
///          3. Destroy all dialogs on the stack and free the stack array.
///          4. Free keyboard shortcuts and their string data.
///          5. Clear global app pointers if this was the active/current app.
///          6. Destroy the theme, default font, retired fonts, root widget tree,
///             and finally the platform window.
///          The app struct itself is GC-allocated, so it will be reclaimed by the
///          collector — we just zero the magic so stale pointers are detected.
/// @param app_ptr Pointer to the app (opaque void* from the Zia layer).
void rt_gui_app_destroy(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    if (!app_ptr)
        return;
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return;
    rt_gui_app_t *previous_active = s_active_app;
    rt_gui_activate_app(app);
    if (app->window)
        vgfx_set_resize_callback(app->window, NULL, NULL);
    rt_gui_features_cleanup(app);
    rt_videowidget_forget_app(app);

    for (int i = 0; i < app->dialog_count; i++) {
        if (app->dialog_stack[i]) {
            rt_messagebox_invalidate_dialog(app->dialog_stack[i]);
            rt_filedialog_invalidate_dialog(app->dialog_stack[i]);
            vg_widget_destroy(&app->dialog_stack[i]->base);
        }
    }
    free(app->dialog_stack);
    app->dialog_stack = NULL;
    app->dialog_count = 0;
    app->dialog_cap = 0;

    free(app->command_palettes);
    app->command_palettes = NULL;
    app->command_palette_count = 0;
    app->command_palette_cap = 0;

    for (int i = 0; i < app->shortcut_count; i++) {
        free(app->shortcuts[i].id);
        free(app->shortcuts[i].keys);
        free(app->shortcuts[i].description);
    }
    free(app->shortcuts);
    app->shortcuts = NULL;
    app->shortcut_count = 0;
    app->shortcut_cap = 0;
    free(app->triggered_shortcut_id);
    app->triggered_shortcut_id = NULL;
    for (int i = 0; i < app->triggered_shortcut_count; i++)
        free(app->triggered_shortcut_ids[i]);
    free(app->triggered_shortcut_ids);
    app->triggered_shortcut_ids = NULL;
    app->triggered_shortcut_count = 0;
    app->triggered_shortcut_cap = 0;

    if (s_current_app == app || s_active_app == app) {
        rt_gui_macos_menu_app_destroy(app);
        s_current_app = NULL;
        s_active_app = NULL;
        rt_gui_clear_widget_runtime_state();
    }

    free(app->title);
    app->title = NULL;
    free(app->application_name);
    app->application_name = NULL;
    rt_gui_accessibility_platform_detach(app->window);
    if (app->root) {
        rt_widget_forget_runtime_refs(app, app->root);
        vg_widget_destroy(app->root);
        app->root = NULL;
    }
    if (app->theme) {
        if (vg_theme_get_current() == app->theme)
            vg_theme_set_current(vg_theme_dark());
        vg_theme_destroy(app->theme);
        app->theme = NULL;
    }
    if (app->custom_theme_base) {
        vg_theme_destroy(app->custom_theme_base);
        app->custom_theme_base = NULL;
    }
    vg_font_t *owned_role_fonts[3] = {
        app->default_font_owned ? app->default_font : NULL,
        app->role_font_bold_owned ? app->role_font_bold : NULL,
        app->role_font_mono_owned ? app->role_font_mono : NULL,
    };
    for (size_t role = 0; role < sizeof(owned_role_fonts) / sizeof(owned_role_fonts[0]); ++role) {
        vg_font_t *font = owned_role_fonts[role];
        if (!font)
            continue;
        int duplicate = 0;
        for (size_t earlier = 0; earlier < role; ++earlier) {
            if (owned_role_fonts[earlier] == font) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;
        int already_retired = 0;
        for (int i = 0; i < app->retired_font_count; ++i) {
            if (app->retired_fonts[i] == font) {
                already_retired = 1;
                break;
            }
        }
        if (!already_retired && vg_font_is_live(font) &&
            !rt_gui_retire_font_in_other_apps(app, font)) {
            vg_font_destroy(font);
        }
    }
    app->default_font = NULL;
    app->role_font_bold = NULL;
    app->role_font_mono = NULL;
    app->default_font_owned = 0;
    app->role_font_bold_owned = 0;
    app->role_font_mono_owned = 0;
    for (int i = 0; i < app->retired_font_count; i++) {
        if (app->retired_fonts[i] && vg_font_is_live(app->retired_fonts[i]) &&
            !rt_gui_retire_font_in_other_apps(app, app->retired_fonts[i]))
            vg_font_destroy(app->retired_fonts[i]);
    }
    free(app->retired_fonts);
    free(app->retired_font_generations);
    app->retired_fonts = NULL;
    app->retired_font_generations = NULL;
    app->retired_font_count = 0;
    app->retired_font_cap = 0;
    if (app->window) {
        vgfx_destroy_window(app->window);
        app->window = NULL;
    }
    rt_gui_unregister_app(app);
    app->magic = 0;
    if (previous_active && previous_active != app && rt_gui_is_app_handle(previous_active)) {
        rt_gui_activate_app(previous_active);
    }
}

/// @brief Query whether the application's window has been closed.
/// @details Returns non-zero once the platform window receives a close event
///          (e.g., user clicked the X button, Alt+F4, or Cmd+Q). Zia code
///          polls this in the main loop to decide when to exit:
///          `while not app.ShouldClose() { ... }`
/// @param app_ptr Pointer to the app.
/// @return 1 if the window should close, 0 otherwise. Returns 1 for NULL.
int64_t rt_gui_app_should_close(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return 1;
    return app->should_close;
}

// Forward declarations for the recursive paint walker.
/// @brief Recursive widget-tree paint pass — emits draw calls for `widget` and its descendants.
static void render_widget_tree(vgfx_window_t window,
                               vg_widget_t *widget,
                               float parent_abs_x,
                               float parent_abs_y);
/// @brief Second pass that draws focus rings, drag previews, and tooltips above the main tree.
static void render_widget_overlay_tree(vgfx_window_t window,
                                       vg_widget_t *widget,
                                       float parent_abs_x,
                                       float parent_abs_y);
/// @brief Forward declaration; defined below.
static int rt_gui_widget_accepts_drop_type(vg_widget_t *widget, const char *type);
/// @brief Forward declaration; defined below.
static int rt_gui_send_event_to_widget(vg_widget_t *widget, vg_event_t *event);

/// @brief True if `widget` paints its own descendants (e.g. ScrollViews clip their own children).
///
/// Such widgets are skipped by the recursive painter — they are
/// expected to call into their custom paint vtable to handle
/// child rendering with whatever clipping / scrolling the widget needs.
/// @param widget Borrowed widget to classify.
/// @return 1 when the widget owns descendant painting, otherwise 0.
static int rt_gui_widget_paints_children_internally(vg_widget_t *widget) {
    if (!widget)
        return 0;
    if (widget->type == VG_WIDGET_SCROLLVIEW)
        return 1;
    return widget->type == VG_WIDGET_CUSTOM && widget->vtable && widget->vtable->paint_overlay;
}

/// @brief Check if a widget accepts a given drag-and-drop data type.
/// @details Parses the widget's comma-separated accepted_drop_types string and
///          compares each entry (case-insensitive) against the given type. If
///          the widget has no type filter or the filter is empty, it accepts all
///          types. Non-drop-target widgets always return 0.
/// @param widget Widget to check.
/// @param type   MIME-like type string from the drag source.
/// @return Non-zero if the widget accepts this type.
static int rt_gui_widget_accepts_drop_type(vg_widget_t *widget, const char *type) {
    if (!widget || !widget->is_drop_target)
        return 0;
    if (!widget->accepted_drop_types || !widget->accepted_drop_types[0])
        return 1;
    if (!type || !type[0])
        return 0;

    const char *p = widget->accepted_drop_types;
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && end[-1] == ' ')
            end--;
        size_t len = (size_t)(end - start);
        if (len == strlen(type) && strncasecmp(start, type, len) == 0)
            return 1;
    }
    return 0;
}

/// @brief Find the drag-drop target for a drop of @p type at widget @p hit.
/// @details Walks from @p hit up through its ancestors and returns the first widget
///          (other than the drag @p source) that accepts the payload type, so a drop
///          on a child bubbles to an accepting container. NULL if none qualifies.
/// @param hit Deepest borrowed widget under the pointer.
/// @param source Borrowed drag-source widget to exclude.
/// @param type Borrowed MIME-like drag payload type.
/// @return Borrowed accepting widget, or NULL when no candidate qualifies.
static vg_widget_t *rt_gui_find_drop_target(vg_widget_t *hit,
                                            vg_widget_t *source,
                                            const char *type) {
    for (vg_widget_t *candidate = hit; candidate; candidate = candidate->parent) {
        if (candidate == source)
            continue;
        if (rt_gui_widget_accepts_drop_type(candidate, type))
            return candidate;
    }
    return NULL;
}

/// @brief Send a GUI event directly to a specific widget (bypassing tree dispatch).
/// @details Sets the event target to the widget and, for mouse events, converts
///          screen-space coordinates to widget-local coordinates using the
///          widget's screen bounds. Returns whether the event was consumed.
///          Used for command palette and notification manager event routing.
/// @param widget Target widget.
/// @param event  GUI event to deliver.
/// @return 1 if the event was consumed, 0 otherwise.
static int rt_gui_send_event_to_widget(vg_widget_t *widget, vg_event_t *event) {
    if (!widget || !event)
        return 0;
    event->target = widget;
    // NOTE: VG_EVENT_MOUSE_WHEEL intentionally omitted. event.mouse and
    // event.wheel share a union, so writing mouse.x/y would overwrite
    // wheel.delta_x/y and silently destroy the scroll delta before the
    // widget's wheel handler could consume it.
    if (event->type == VG_EVENT_MOUSE_MOVE || event->type == VG_EVENT_MOUSE_DOWN ||
        event->type == VG_EVENT_MOUSE_UP || event->type == VG_EVENT_CLICK ||
        event->type == VG_EVENT_DOUBLE_CLICK) {
        float sx = 0.0f, sy = 0.0f;
        vg_widget_get_screen_bounds(widget, &sx, &sy, NULL, NULL);
        event->mouse.x = event->mouse.screen_x - sx;
        event->mouse.y = event->mouse.screen_y - sy;
    }
    return vg_event_send(widget, event) ? 1 : 0;
}

/// @brief Extract the screen-space X coordinate from any pointer event.
/// @details Mouse and wheel events store their screen coordinates in different
///          union members, so this helper centralizes the disambiguation. Wheel
///          events use `event->wheel.screen_x`; all other pointer events use
///          `event->mouse.screen_x`.
/// @param event Borrowed pointer event.
/// @return Screen-space X coordinate, or 0 for NULL.
static float rt_gui_event_screen_x(const vg_event_t *event) {
    if (!event)
        return 0.0f;
    return event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_x : event->mouse.screen_x;
}

/// @brief Extract the screen-space Y coordinate from any pointer event.
/// @details Mirrors `rt_gui_event_screen_x`; dispatches on wheel vs. mouse
///          union to read the correct Y field.
/// @param event Borrowed pointer event.
/// @return Screen-space Y coordinate, or 0 for NULL.
static float rt_gui_event_screen_y(const vg_event_t *event) {
    if (!event)
        return 0.0f;
    return event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_y : event->mouse.screen_y;
}

/// @brief Snapshot the widget that was clicked this frame into `app->last_clicked`.
/// @details The vg widget system records the last-clicked widget and its timestamp
///          in a shared runtime state. After each dispatched event, we copy that
///          state to the app so Zia code can call `App.LastClicked()` without
///          needing direct access to the vg internals. The timestamp guard
///          prevents stale clicks from a previous frame from being re-captured.
/// @param app Application receiving the click snapshot.
/// @param event Borrowed dispatched event used to reject stale click timestamps; may be NULL.
static void rt_gui_capture_reported_click(rt_gui_app_t *app, const vg_event_t *event) {
    if (!app)
        return;
    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    if (!state.reported_click_widget)
        return;
    if (event && state.reported_click_time_ms != 0 &&
        event->timestamp != state.reported_click_time_ms)
        return;
    app->last_clicked = state.reported_click_widget;
}

/// @brief Recompute which widget the in-progress drag is currently hovering over.
/// @details Hit-tests the current mouse position against the event root to find
///          the drop-target candidate; skips the drag source itself and widgets
///          that reject the source's data type via `rt_gui_widget_accepts_drop_type`.
///          The `_is_drag_over` flag is cleared on the previous target and set
///          on the new one so painters can highlight valid drop zones.
/// @param app Application whose drag state is updated.
/// @param event_root Borrowed widget root used for hit testing.
static void rt_gui_update_drag_over_target(rt_gui_app_t *app, vg_widget_t *event_root) {
    if (!app)
        return;

    vg_widget_t *next = NULL;
    if (app->drag_source && !vg_widget_is_live(app->drag_source))
        app->drag_source = NULL;
    if (app->drag_over_widget && !vg_widget_is_live(app->drag_over_widget))
        app->drag_over_widget = NULL;

    if (app->drag_source && event_root) {
        vg_widget_t *hit = vg_widget_hit_test(event_root, (float)app->mouse_x, (float)app->mouse_y);
        next = rt_gui_find_drop_target(hit, app->drag_source, app->drag_source->drag_type);
    }

    if (app->drag_over_widget && app->drag_over_widget != next) {
        app->drag_over_widget->_is_drag_over = false;
    }
    if (next) {
        next->_is_drag_over = true;
    }
    app->drag_over_widget = next;
}

/// @brief Clear the pending drag candidate and its recorded start position.
/// @details Called when a press is released without crossing the drag
///          threshold, or when the candidate widget is no longer live.
/// @param app Application whose candidate state is cleared.
static void rt_gui_cancel_drag_candidate(rt_gui_app_t *app) {
    if (!app)
        return;
    app->drag_candidate = NULL;
    app->drag_start_x = 0;
    app->drag_start_y = 0;
}

/// @brief Promote a pending press to an active drag once the pointer moves far
///        enough from the press point.
/// @details Uses a squared-distance dead-zone (`dx*dx + dy*dy < 16`, i.e. a
///          4-pixel radius) so small jitters during a click do not start a
///          drag. If the candidate widget died meanwhile the candidate is
///          cancelled instead.
/// @param app Application whose pending drag is evaluated.
/// @param event_root Borrowed event root used to resolve the current drop target.
static void rt_gui_maybe_start_drag(rt_gui_app_t *app, vg_widget_t *event_root) {
    if (!app || app->drag_source || !app->drag_candidate)
        return;
    double dx = (double)app->mouse_x - (double)app->drag_start_x;
    double dy = (double)app->mouse_y - (double)app->drag_start_y;
    if (dx * dx + dy * dy < 16)
        return;
    if (!event_root || !vg_widget_is_live(app->drag_candidate)) {
        rt_gui_cancel_drag_candidate(app);
        return;
    }
    app->drag_source = app->drag_candidate;
    app->drag_candidate = NULL;
    app->drag_source->_is_being_dragged = true;
    rt_gui_update_drag_over_target(app, event_root);
}

/// @brief Finish an in-progress drag: hit-test the drop target and deliver it.
/// @details Always clears the drag candidate first. If a drag was active, the
///          widget under the pointer is hit-tested and, when it accepts the
///          source's drag type and is not the source itself, receives the drop.
/// @param app Application whose active drag is completed and cleared.
/// @param event_root Borrowed event root used to hit-test the drop point.
static void rt_gui_complete_drag_drop(rt_gui_app_t *app, vg_widget_t *event_root) {
    if (!app)
        return;
    rt_gui_cancel_drag_candidate(app);
    if (!app->drag_source)
        return;
    if (!vg_widget_is_live(app->drag_source)) {
        app->drag_source = NULL;
        rt_gui_update_drag_over_target(app, event_root);
        return;
    }

    vg_widget_t *source = app->drag_source;
    source->_is_being_dragged = false;
    vg_widget_t *hit =
        event_root ? vg_widget_hit_test(event_root, (float)app->mouse_x, (float)app->mouse_y)
                   : NULL;
    vg_widget_t *target = rt_gui_find_drop_target(hit, source, source->drag_type);
    if (target) {
        char *new_type = source->drag_type ? rt_gui_app_strdup(source->drag_type) : NULL;
        char *new_data = source->drag_data ? rt_gui_app_strdup(source->drag_data) : NULL;
        if ((source->drag_type && !new_type) || (source->drag_data && !new_data)) {
            free(new_type);
            free(new_data);
        } else {
            free(target->_drop_received_type);
            free(target->_drop_received_data);
            target->_drop_received_type = new_type;
            target->_drop_received_data = new_data;
            // Record the landing point: consumers must not depend on the
            // live pointer, which can move (or reset) before they poll.
            target->_drop_received_x = (float)app->mouse_x;
            target->_drop_received_y = (float)app->mouse_y;
            target->_was_dropped = true;
            target->_is_drag_over = false;
        }
    }
    app->drag_source = NULL;
    rt_gui_update_drag_over_target(app, event_root);
}

/// @brief Return the effective event-dispatch root for hit testing.
/// @details When a modal dialog is open, all hit testing and event dispatch is
///          scoped to the dialog's widget subtree (not the full app root). This
///          prevents clicks from "leaking through" to background widgets.
/// @param app Active app.
/// @return The topmost dialog widget, or the app root if no dialog is open.
static vg_widget_t *rt_gui_hit_root(rt_gui_app_t *app) {
    vg_dialog_t *top_dialog = rt_gui_top_dialog(app);
    return top_dialog ? &top_dialog->base : app->root;
}

/// @brief Measure and position a command palette centered near the top of the window.
/// @details Palettes are positioned at horizontal center and 15% down from the
///          top — mimicking VS Code's Ctrl+Shift+P behavior.
/// @param app     Active app.
/// @param palette Palette to layout.
/// @param win_w   Window width in physical pixels.
/// @param win_h   Window height in physical pixels.
static void rt_gui_layout_command_palette(rt_gui_app_t *app,
                                          vg_commandpalette_t *palette,
                                          int32_t win_w,
                                          int32_t win_h) {
    if (!app || !palette)
        return;
    vg_widget_measure(&palette->base, (float)win_w, (float)win_h);
    float pw = palette->base.measured_width;
    float ph = palette->base.measured_height;
    vg_widget_arrange(&palette->base, (win_w - pw) / 2.0f, win_h * 0.15f, pw, ph);
}

/// @brief Synchronize a detached context menu's style, measured size, and clamped position.
/// @details Runtime-owned theme/font inheritance is applied before damage collection so the
///          retained rectangle exactly matches the subsequent paint. Unchanged theme and font
///          values are idempotent and do not create perpetual dirty frames.
/// @param app Owning app; NULL is ignored.
/// @param win_w Current framebuffer width in physical pixels.
/// @param win_h Current framebuffer height in physical pixels.
/// @param effective_font_size Effective inherited font size in physical pixels.
static void rt_gui_prepare_context_menu(rt_gui_app_t *app,
                                        int32_t win_w,
                                        int32_t win_h,
                                        float effective_font_size) {
    if (!app || !app->active_context_menu || !app->active_context_menu->is_visible)
        return;
    vg_contextmenu_t *menu = app->active_context_menu;
    vg_contextmenu_apply_theme(menu, app->theme);
    vg_font_t *font = rt_gui_font_handle_checked(app->default_font);
    if (font)
        vg_contextmenu_set_font(menu, font, effective_font_size);
    if (menu->base.needs_layout || menu->base.width <= 0.0f || menu->base.height <= 0.0f) {
        float x = menu->base.x;
        float y = menu->base.y;
        vg_widget_measure(&menu->base, (float)win_w, (float)win_h);
        vg_widget_arrange(&menu->base, x, y, menu->base.measured_width, menu->base.measured_height);
    }
    rt_gui_clamp_context_menu(menu, win_w, win_h);
}

/// @brief Paint every visible detached overlay in stable compositor Z order.
/// @details The caller owns clearing and any active clip limit. Painting every overlay is required
///          even when only the normal root is dirty: a root repaint beneath a static overlay must
///          restore the overlay pixels intersecting that damage rectangle. The function performs
///          no allocation beyond allocations intrinsic to individual legacy paint callbacks.
/// @param app App whose detached overlay layer is painted; NULL is ignored.
/// @param tooltip_mgr Activated tooltip manager; may be NULL.
static void rt_gui_paint_detached_overlays(rt_gui_app_t *app, vg_tooltip_manager_t *tooltip_mgr) {
    if (!app)
        return;
    for (int i = 0; i < app->command_palette_count; ++i) {
        vg_commandpalette_t *palette = app->command_palettes[i];
        if (palette && palette->is_visible && palette->base.vtable && palette->base.vtable->paint)
            palette->base.vtable->paint(&palette->base, (void *)app->window);
    }
    for (int i = 0; i < app->dialog_count; ++i) {
        vg_dialog_t *dialog = app->dialog_stack[i];
        if (dialog && dialog->is_open && dialog->base.vtable && dialog->base.vtable->paint)
            dialog->base.vtable->paint(&dialog->base, (void *)app->window);
    }
    if (app->active_context_menu && app->active_context_menu->is_visible) {
        vg_widget_t *menu = &app->active_context_menu->base;
        if (menu->vtable && menu->vtable->paint)
            menu->vtable->paint(menu, (void *)app->window);
    }
    if (app->notification_manager && app->notification_manager->base.vtable &&
        app->notification_manager->base.vtable->paint) {
        app->notification_manager->base.vtable->paint(&app->notification_manager->base,
                                                      (void *)app->window);
    }
    if (tooltip_mgr && tooltip_mgr->active_tooltip && tooltip_mgr->active_tooltip->is_visible) {
        vg_widget_t *tooltip = &tooltip_mgr->active_tooltip->base;
        if (tooltip->vtable && tooltip->vtable->paint)
            tooltip->vtable->paint(tooltip, (void *)app->window);
    }
    if (app->manual_tooltip && app->manual_tooltip->is_visible) {
        vg_widget_t *tooltip = &app->manual_tooltip->base;
        if (tooltip->vtable && tooltip->vtable->paint)
            tooltip->vtable->paint(tooltip, (void *)app->window);
    }
}

/// @brief Test if a screen-space point falls within the command palette bounds.
/// @details Used to determine whether mouse events should be routed to the
///          palette or dismissed (clicks outside close the palette).
/// @param palette Borrowed command palette.
/// @param x Screen-space X coordinate.
/// @param y Screen-space Y coordinate.
/// @return Non-zero if (x, y) is inside the palette's layout rectangle.
static int rt_gui_palette_contains_point(vg_commandpalette_t *palette, float x, float y) {
    if (!palette)
        return 0;
    return x >= palette->base.x && x < palette->base.x + palette->base.width &&
           y >= palette->base.y && y < palette->base.y + palette->base.height;
}

/// @brief Process all pending platform events and dispatch them to the widget tree.
/// @details This is one half of the main loop (poll + render). It:
///          1. Clears per-frame state (last_clicked, drag-over, shortcut triggers).
///          2. Polls the platform event queue via vgfx_poll_event.
///          3. Handles close events, file drops, keyboard shortcuts.
///          4. Routes mouse/keyboard events through modal dialogs, command
///             palettes, notification manager, and finally the widget tree.
///          5. Manages drag-and-drop state transitions (start → over → drop).
///          Events are converted from platform format (vgfx_event_t) to GUI
///          format (vg_event_t) and dispatched via vg_event_dispatch. The
///          command palette intercepts all keyboard events when visible, and
///          mouse events inside its bounds. Clicks outside dismiss it.
/// @param app_ptr Pointer to the app.
void rt_gui_app_poll(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return;
    if (!app->window)
        return;
    rt_gui_activate_app(app);
    rt_gui_sync_modal_root(app);

    // Clear last clicked
    app->last_clicked = NULL;
    app->last_statusbar_clicked = NULL;
    app->last_toolbar_clicked = NULL;
    rt_gui_set_last_clicked(NULL);
    vg_widget_clear_reported_click();

    // Clear shortcut triggered flags from previous frame
    rt_shortcuts_clear_triggered(app);
    app->close_requested = 0;

    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();

    // Get mouse position
    vgfx_mouse_pos(app->window, &app->mouse_x, &app->mouse_y);
    rt_mouse_update_pos(app->mouse_x, app->mouse_y);

    // Poll events
    vgfx_event_t event;
    bool processed_event = false;
    while (vgfx_poll_event(app->window, &event)) {
        processed_event = true;
        app->last_event_time_ms = (uint64_t)event.time_ms;
        if (event.type == VGFX_EVENT_KEY_DOWN) {
            rt_keyboard_on_vgfx_key_down((int64_t)event.data.key.key);
        } else if (event.type == VGFX_EVENT_KEY_UP) {
            rt_keyboard_on_vgfx_key_up((int64_t)event.data.key.key);
        } else if (event.type == VGFX_EVENT_TEXT_INPUT) {
            rt_keyboard_text_input((int32_t)event.data.text.codepoint);
        } else if (event.type == VGFX_EVENT_MOUSE_MOVE) {
            rt_mouse_update_pos((int64_t)event.data.mouse_move.x, (int64_t)event.data.mouse_move.y);
        } else if (event.type == VGFX_EVENT_MOUSE_DOWN) {
            rt_mouse_update_pos((int64_t)event.data.mouse_button.x,
                                (int64_t)event.data.mouse_button.y);
            rt_mouse_button_down((int64_t)event.data.mouse_button.button);
        } else if (event.type == VGFX_EVENT_MOUSE_UP) {
            rt_mouse_update_pos((int64_t)event.data.mouse_button.x,
                                (int64_t)event.data.mouse_button.y);
            rt_mouse_button_up((int64_t)event.data.mouse_button.button);
        } else if (event.type == VGFX_EVENT_SCROLL) {
            rt_mouse_update_pos((int64_t)event.data.scroll.x, (int64_t)event.data.scroll.y);
            rt_mouse_update_wheel((double)event.data.scroll.delta_x,
                                  (double)event.data.scroll.delta_y);
        }

        if (event.type == VGFX_EVENT_CLOSE) {
            app->close_requested = 1;
            if (!app->prevent_close)
                app->should_close = 1;
            continue;
        }

        // File drop events — collect per-app state.
        if (event.type == VGFX_EVENT_FILE_DROP) {
            rt_gui_file_drop_add(app, event.data.file_drop.path);
            continue;
        }

        // Convert platform event to GUI event and dispatch to widget tree
        if (app->root) {
            vg_event_t gui_event = vg_event_from_platform(&event);
            if (gui_event.type == VG_EVENT_NONE)
                continue;
            vg_commandpalette_t *top_palette = rt_gui_top_visible_command_palette(app);
            vg_widget_t *event_root = rt_gui_hit_root(app);
            int32_t win_w = 0, win_h = 0;
            if (top_palette) {
                vgfx_get_size(app->window, &win_w, &win_h);
                rt_gui_layout_command_palette(app, top_palette, win_w, win_h);
            }

            // Track mouse position from events
            if (event.type == VGFX_EVENT_MOUSE_DOWN || event.type == VGFX_EVENT_MOUSE_UP) {
                app->mouse_x = event.data.mouse_button.x;
                app->mouse_y = event.data.mouse_button.y;
            }
            if (event.type == VGFX_EVENT_MOUSE_MOVE) {
                app->mouse_x = event.data.mouse_move.x;
                app->mouse_y = event.data.mouse_move.y;
            }
            rt_gui_update_drag_over_target(app, event_root);
            if (event.type == VGFX_EVENT_MOUSE_MOVE) {
                rt_gui_maybe_start_drag(app, event_root);

                // Drag-and-drop: update drag-over state during drag
                rt_gui_update_drag_over_target(app, event_root);

                if (top_palette) {
                    vg_tooltip_manager_on_leave(vg_tooltip_manager_get());
                } else {
                    vg_widget_t *hovered =
                        vg_widget_hit_test(event_root, (float)app->mouse_x, (float)app->mouse_y);
                    if (hovered) {
                        vg_tooltip_manager_on_hover(
                            vg_tooltip_manager_get(), hovered, app->mouse_x, app->mouse_y);
                    } else {
                        vg_tooltip_manager_on_leave(vg_tooltip_manager_get());
                    }
                }
            }

            // Active context menu (standalone right-click overlay). Route input to it
            // first and let contextmenu_handle_event manage hover, item clicks (which set
            // item->was_clicked for the poll-based WasClicked API), outside-click
            // dismissal, and Escape. It is not in app->root's tree, so the normal
            // dispatch below can never reach it.
            if (app->active_context_menu && app->active_context_menu->is_visible) {
                int menu_mouse =
                    gui_event.type == VG_EVENT_MOUSE_MOVE ||
                    gui_event.type == VG_EVENT_MOUSE_DOWN || gui_event.type == VG_EVENT_MOUSE_UP ||
                    gui_event.type == VG_EVENT_CLICK || gui_event.type == VG_EVENT_DOUBLE_CLICK;
                int menu_key = gui_event.type == VG_EVENT_KEY_DOWN ||
                               gui_event.type == VG_EVENT_KEY_UP ||
                               gui_event.type == VG_EVENT_KEY_CHAR;
                if (menu_mouse || menu_key) {
                    rt_gui_send_event_to_widget(&app->active_context_menu->base, &gui_event);
                    rt_gui_capture_reported_click(app, &gui_event);
                    if (!app->active_context_menu->is_visible)
                        app->active_context_menu = NULL;
                    continue;
                }
            }

            if (top_palette && top_palette->is_visible) {
                int is_mouse_event =
                    gui_event.type == VG_EVENT_MOUSE_MOVE ||
                    gui_event.type == VG_EVENT_MOUSE_DOWN || gui_event.type == VG_EVENT_MOUSE_UP ||
                    gui_event.type == VG_EVENT_CLICK || gui_event.type == VG_EVENT_DOUBLE_CLICK ||
                    gui_event.type == VG_EVENT_MOUSE_WHEEL;
                int inside_palette =
                    rt_gui_palette_contains_point(top_palette,
                                                  rt_gui_event_screen_x(&gui_event),
                                                  rt_gui_event_screen_y(&gui_event));

                if (is_mouse_event && inside_palette) {
                    rt_gui_send_event_to_widget(&top_palette->base, &gui_event);
                    rt_gui_capture_reported_click(app, &gui_event);
                    if (event.type == VGFX_EVENT_MOUSE_UP)
                        rt_gui_complete_drag_drop(app, rt_gui_hit_root(app));
                    continue;
                }
                if (is_mouse_event && !inside_palette) {
                    if (gui_event.type == VG_EVENT_MOUSE_DOWN || gui_event.type == VG_EVENT_CLICK) {
                        vg_commandpalette_hide(top_palette);
                    }
                    if (event.type == VGFX_EVENT_MOUSE_UP)
                        rt_gui_complete_drag_drop(app, rt_gui_hit_root(app));
                    continue;
                }
                if (gui_event.type == VG_EVENT_KEY_DOWN || gui_event.type == VG_EVENT_KEY_UP ||
                    gui_event.type == VG_EVENT_KEY_CHAR) {
                    rt_gui_send_event_to_widget(&top_palette->base, &gui_event);
                    rt_gui_capture_reported_click(app, &gui_event);
                    continue;
                }
            }

            if (app->notification_manager &&
                rt_gui_send_event_to_widget(&app->notification_manager->base, &gui_event)) {
                rt_gui_capture_reported_click(app, &gui_event);
                if (event.type == VGFX_EVENT_MOUSE_UP)
                    rt_gui_complete_drag_drop(app, rt_gui_hit_root(app));
                continue;
            }

            // Drag-and-drop: start a drag candidate on mouse-down only after
            // overlays have had first chance to route or consume the event.
            if (event.type == VGFX_EVENT_MOUSE_DOWN && !app->drag_source &&
                event.data.mouse_button.button == VGFX_MOUSE_LEFT) {
                vg_widget_t *hit =
                    vg_widget_hit_test(event_root, (float)app->mouse_x, (float)app->mouse_y);
                if (hit && hit->draggable) {
                    app->drag_candidate = hit;
                    app->drag_start_x = app->mouse_x;
                    app->drag_start_y = app->mouse_y;
                }
            }

            if (gui_event.type == VG_EVENT_KEY_DOWN) {
                bool handled = vg_event_dispatch(app->root, &gui_event);
                rt_gui_capture_reported_click(app, &gui_event);
                rt_gui_sync_modal_root(app);
                if (handled || rt_gui_top_dialog(app))
                    continue;

                // Let the focused widget/root see the key first; only unhandled
                // non-modal key-downs become global shortcuts.
                if (rt_shortcuts_check_key(app, gui_event.key.key, (int)gui_event.modifiers))
                    continue;
                continue;
            }

            // Dispatch all events to widget tree (handles focus, keyboard, etc.)
            vg_event_dispatch(app->root, &gui_event);
            rt_gui_capture_reported_click(app, &gui_event);
            rt_gui_sync_modal_root(app);
            if (event.type == VGFX_EVENT_MOUSE_UP)
                rt_gui_complete_drag_drop(app, rt_gui_hit_root(app));
        }
    }

    // User-driven close/remove handlers can retire lightweight tab or item records without
    // entering a public removal wrapper. Collect only after an actual dispatch batch so idle polls
    // retain their zero-traversal fast path.
    if (processed_event && app->root)
        rt_gui_collect_retired_subhandles(app->root);

    // Recompute the absolute delta after this frame's events so Mouse.DeltaX/Y
    // describe the same frame as Mouse.X/Y instead of lagging one poll behind.
    rt_mouse_finalize_frame();
}

/// @brief Layout, paint, and present the entire UI to the window.
/// @details This is the render half of the main loop. It:
///          1. Ensures the default font is loaded and theme is up-to-date.
///          2. Runs the vg layout engine (vg_widget_layout) with current window
///             dimensions, computing sizes and positions for all widgets.
///          3. Clears the framebuffer with the theme's background color.
///          4. Walks the widget tree via render_widget_tree, converting relative
///             coordinates to absolute for painting, then restoring them.
///          5. Paints overlays: popup dropdowns, command palettes, dialogs,
///             notifications, and tooltips — each above the previous layer.
///          6. Presents the framebuffer via vgfx_update.
///          Widget coordinates remain relative after this call, so hit testing
///          during the next poll() uses parent-chain walks correctly.
/// @param app_ptr Pointer to the app.
void rt_gui_app_render(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return;
    if (!app->window)
        return;
    rt_gui_activate_app(app);

    // Ensure a default font is available for widget rendering.
    rt_gui_ensure_default_font();
    rt_gui_sync_system_theme(app);
    rt_gui_refresh_theme(app);
    float effective_font_size = rt_gui_app_effective_font_size(app);

    if (app->notification_manager && app->default_font) {
        vg_notification_manager_set_font(
            app->notification_manager, app->default_font, effective_font_size);
    }
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i] && app->default_font) {
            vg_commandpalette_set_font(
                app->command_palettes[i], app->default_font, effective_font_size);
        }
    }
    if (app->manual_tooltip && app->default_font) {
        app->manual_tooltip->font = app->default_font;
        app->manual_tooltip->font_size = effective_font_size;
    }

    // Cache window dimensions once — reused for layout and dialog centering.
    int32_t win_w = 0, win_h = 0;
    vgfx_get_size(app->window, &win_w, &win_h);
    uint64_t now_ms = rt_gui_now_ms();
    float dt = 0.0f;
    double scheduler_delta_ms = 0.0;
    if (app->frame_delta_override_pending) {
        double delta_ms = app->frame_delta_override_ms;
        app->frame_delta_override_pending = 0;
        app->frame_delta_override_ms = 0.0;
        if (!isfinite(delta_ms) || delta_ms < 0.0)
            delta_ms = 0.0;
        app->scheduler_elapsed_ms += delta_ms;
        scheduler_delta_ms = delta_ms;
        double animation_delta_ms = delta_ms > 250.0 ? 250.0 : delta_ms;
        dt = (float)(animation_delta_ms / 1000.0);
    } else if (app->last_render_time_ms > 0 && now_ms > app->last_render_time_ms) {
        scheduler_delta_ms = (double)(now_ms - app->last_render_time_ms);
        dt = (float)scheduler_delta_ms / 1000.0f;
        if (dt > 0.25f)
            dt = 0.25f;
        app->scheduler_elapsed_ms += scheduler_delta_ms;
    }
    if (!isfinite(app->scheduler_clock_ms) || app->scheduler_clock_ms <= 0.0)
        app->scheduler_clock_ms = (double)now_ms;
    if (scheduler_delta_ms > (double)UINT64_MAX - app->scheduler_clock_ms)
        app->scheduler_clock_ms = (double)UINT64_MAX;
    else
        app->scheduler_clock_ms += scheduler_delta_ms;
    uint64_t scheduler_now_ms = app->scheduler_clock_ms >= (double)UINT64_MAX
                                    ? UINT64_MAX
                                    : (uint64_t)app->scheduler_clock_ms;
    app->last_render_time_ms = now_ms;
    app->animations_active = 0;
    app->frame_generation = app->frame_generation == UINT64_MAX ? 1u : app->frame_generation + 1u;
    rt_videowidget_update_app(app, (double)dt, app->frame_generation);
    rt_minimap_sync_app(app);

    bool did_layout = false;
    bool size_changed = false;
    if (app->root) {
        size_changed = app->last_layout_width != win_w || app->last_layout_height != win_h;
        if (size_changed || rt_gui_widget_tree_needs_layout(app->root)) {
            vg_widget_layout(app->root, (float)win_w, (float)win_h);
            app->last_layout_width = win_w;
            app->last_layout_height = win_h;
            did_layout = true;
        }
        app->animations_active |= rt_gui_tick_widget_tree(app->root, dt);
    }

    for (int i = 0; i < app->command_palette_count; i++) {
        vg_commandpalette_t *palette = app->command_palettes[i];
        if (!palette || !palette->is_visible)
            continue;
        app->animations_active |= rt_gui_tick_widget_tree(&palette->base, dt);
        rt_gui_layout_command_palette(app, palette, win_w, win_h);
    }

    for (int i = 0; i < app->dialog_count; i++) {
        vg_dialog_t *dlg = app->dialog_stack[i];
        if (!dlg || !dlg->is_open)
            continue;
        app->animations_active |= rt_gui_tick_widget_tree(&dlg->base, dt);
        if (app->default_font) {
            vg_dialog_set_font(dlg, app->default_font, effective_font_size);
        }
        if (dlg->base.measured_width < 1.0f || dlg->base.needs_layout) {
            vg_widget_measure(&dlg->base, (float)win_w, (float)win_h);
        }
        float ref_x = 0.0f;
        float ref_y = 0.0f;
        float ref_w = (float)win_w;
        float ref_h = (float)win_h;
        if (dlg->modal_parent) {
            vg_widget_get_screen_bounds(dlg->modal_parent, &ref_x, &ref_y, &ref_w, &ref_h);
        }
        float dw = dlg->base.measured_width;
        float dh = dlg->base.measured_height;
        vg_widget_arrange(
            &dlg->base, ref_x + (ref_w - dw) / 2.0f, ref_y + (ref_h - dh) / 2.0f, dw, dh);
    }

    if (app->notification_manager) {
        app->notification_manager->base.x = 0.0f;
        app->notification_manager->base.y = 0.0f;
        app->notification_manager->base.width = (float)win_w;
        app->notification_manager->base.height = (float)win_h;
        vg_notification_manager_update(app->notification_manager, scheduler_now_ms);
        app->animations_active |= rt_gui_tick_widget_tree(&app->notification_manager->base, dt);
    }

    vg_tooltip_manager_t *tooltip_mgr = vg_tooltip_manager_get();
    vg_tooltip_manager_update(tooltip_mgr, scheduler_now_ms);
    if (tooltip_mgr->active_tooltip)
        app->animations_active |= rt_gui_tick_widget_tree(&tooltip_mgr->active_tooltip->base, dt);
    if (tooltip_mgr->active_tooltip && tooltip_mgr->active_tooltip->is_visible &&
        app->default_font) {
        tooltip_mgr->active_tooltip->font = app->default_font;
        tooltip_mgr->active_tooltip->font_size = effective_font_size;
    }
    if (app->manual_tooltip && app->manual_tooltip->is_visible && app->default_font) {
        app->manual_tooltip->font = app->default_font;
        app->manual_tooltip->font_size = effective_font_size;
    }
    rt_gui_prepare_tooltip(tooltip_mgr ? tooltip_mgr->active_tooltip : NULL, win_w, win_h);
    rt_gui_prepare_tooltip(app->manual_tooltip, win_w, win_h);
    rt_gui_prepare_context_menu(app, win_w, win_h, effective_font_size);
    if (app->manual_tooltip)
        app->animations_active |= rt_gui_tick_widget_tree(&app->manual_tooltip->base, dt);
    if (app->active_context_menu)
        app->animations_active |= rt_gui_tick_widget_tree(&app->active_context_menu->base, dt);

    rt_gui_accessibility_platform_sync(app->window, app->root);

    bool root_needs_paint = rt_gui_widget_tree_needs_paint(app->root);
    bool overlays_need_paint = rt_gui_app_overlays_need_paint(app);
    rt_gui_damage_t overlay_current = {0};
    rt_gui_collect_detached_overlay_bounds(app, tooltip_mgr, win_w, win_h, &overlay_current);
    bool overlay_bounds_changed = rt_gui_overlay_bounds_changed(app, &overlay_current);
    bool overlay_layer_needs_damage = overlays_need_paint || overlay_bounds_changed;
    if (!did_layout && !size_changed && !root_needs_paint && !overlay_layer_needs_damage) {
        vgfx_pump_events(app->window);
        // Nothing to repaint this frame. Pace the frame anyway so an idle GUI
        // does not busy-loop a CPU core: with an FPS cap we sleep to the frame
        // deadline; uncapped, we take a 1ms anti-spin floor. Presentation is
        // skipped because the framebuffer is unchanged.
        vgfx_frame_pace(app->window, 1);
        rt_gui_collect_retired_fonts(app);
        return;
    }

    vg_theme_t *theme = vg_theme_get_current();
    vgfx_color_t theme_bg = theme ? theme->colors.bg_secondary : 0xFF1E1E1E;

    // Choose full-window vs. damage-region (partial) repaint. A framebuffer resize
    // still requires a full frame. Layout and detached overlays are safe on the
    // partial path because current/previous visual rectangles are retained below and
    // all paint callbacks execute beneath a compositor-owned clip limit.
    // did_layout is intentionally NOT a veto: the editor marks needs_layout on
    // every keystroke to recompute content metrics, but that layout is idempotent
    // (no widget moves), so rt_gui_collect_damage — which diffs each widget's
    // current bounds against its last-paint bounds — reports only the truly changed
    // region. A real reflow that moves widgets shows up as large damage and
    // collapses to a full repaint below. Window resize (framebuffer realloc) remains
    // the only structural veto.
    bool use_partial = app->partial_paint_enabled && !size_changed;
    int32_t dmg_x = 0, dmg_y = 0, dmg_w = 0, dmg_h = 0;
    if (use_partial) {
        rt_gui_damage_t dmg = {0};
        rt_gui_collect_damage(app->root, &dmg);
        if (overlay_layer_needs_damage) {
            if (app->overlay_last_valid) {
                rt_gui_damage_add(&dmg,
                                  app->overlay_last_x,
                                  app->overlay_last_y,
                                  app->overlay_last_w,
                                  app->overlay_last_h);
            }
            if (overlay_current.any) {
                rt_gui_damage_add(&dmg,
                                  overlay_current.x0,
                                  overlay_current.y0,
                                  overlay_current.x1 - overlay_current.x0,
                                  overlay_current.y1 - overlay_current.y0);
            }
        }
        if (!dmg.any) {
            // A dirty flag can legitimately collapse to no pixels (for example a newly
            // created notification at opacity zero). Avoid a full repaint while still
            // consuming the retained state so the next animation tick starts cleanly.
            rt_gui_app_clear_paint_flags(app);
            rt_gui_commit_overlay_bounds(app, &overlay_current);
            vgfx_pump_events(app->window);
            vgfx_frame_pace(app->window, 1);
            rt_gui_collect_retired_fonts(app);
            return;
        } else {
            float fx0 = dmg.x0 - 1.0f, fy0 = dmg.y0 - 1.0f; // 1px slack for AA edge rims
            float fx1 = dmg.x1 + 1.0f, fy1 = dmg.y1 + 1.0f;
            if (fx0 < 0.0f)
                fx0 = 0.0f;
            if (fy0 < 0.0f)
                fy0 = 0.0f;
            if (fx1 > (float)win_w)
                fx1 = (float)win_w;
            if (fy1 > (float)win_h)
                fy1 = (float)win_h;
            dmg_x = (int32_t)fx0;
            dmg_y = (int32_t)fy0;
            dmg_w = (int32_t)(fx1 - fx0 + 0.999f); // ceil so the fractional edge is covered
            dmg_h = (int32_t)(fy1 - fy0 + 0.999f);
            double win_area = (double)win_w * (double)win_h;
            double dmg_area = (double)dmg_w * (double)dmg_h;
            // Only fall back to full when the damage bounds are nearly the whole
            // window — the partial path is always correct, so the collapse is just
            // to skip clip setup when it would save almost nothing. At 90% the
            // common "typing dirties the whole editor" case (~2/3 of the window)
            // still repaints partially, skipping the sidebar/tabs/panels/bars.
            if (dmg_w <= 0 || dmg_h <= 0 || (win_area > 0.0 && dmg_area >= win_area * 0.9))
                use_partial = false;
        }
    }

    if (use_partial && !vgfx_push_clip_limit(app->window, dmg_x, dmg_y, dmg_w, dmg_h))
        use_partial = false;

    if (use_partial) {
        // Clear only the damage rectangle and repaint every retained layer. The clip
        // limit survives widget-local vgfx_set_clip/vgfx_clear_clip pairs, so no paint
        // callback can escape the compositor damage region. Static detached overlays
        // are repainted under the same limit to restore portions intersecting dirty
        // normal content.
        vgfx_cls(app->window, theme_bg);
        if (app->root) {
            render_widget_tree(app->window, app->root, 0.0f, 0.0f);
            render_widget_overlay_tree(app->window, app->root, 0.0f, 0.0f);
        }
        rt_gui_paint_detached_overlays(app, tooltip_mgr);
        vgfx_pop_clip_limit(app->window);
        app->frames_partial++;
        app->last_damage_w = (float)dmg_w;
        app->last_damage_h = (float)dmg_h;
        rt_gui_app_clear_paint_flags(app);
        rt_gui_commit_overlay_bounds(app, &overlay_current);
        vgfx_update(app->window);
        rt_gui_collect_retired_fonts(app);
        return;
    }

    // Full-window repaint (the pre-plan-07 path). Absolute offsets are accumulated
    // during traversal so widget->x/y stay relative. This is critical: hit testing in
    // poll() uses vg_widget_get_screen_bounds() which walks the parent chain from
    // relative coords. If we converted to absolute here, hit testing would
    // double-count parent offsets and fail.
    app->frames_full++;
    app->last_damage_w = 0.0f;
    app->last_damage_h = 0.0f;
    vgfx_cls(app->window, theme_bg);
    if (app->root) {
        render_widget_tree(app->window, app->root, 0.0f, 0.0f);
    }

    // Paint widget overlays (dropdowns, menubar popups, floating panels) in a
    // second pass after the normal tree walk.
    if (app->root) {
        render_widget_overlay_tree(app->window, app->root, 0.0f, 0.0f);
    }

    rt_gui_paint_detached_overlays(app, tooltip_mgr);

    // Present
    rt_gui_app_clear_paint_flags(app);
    rt_gui_commit_overlay_bounds(app, &overlay_current);
    vgfx_update(app->window);
    rt_gui_collect_retired_fonts(app);
}

/// @brief Merge a candidate delay into an optional nearest-deadline value.
/// @details Negative candidates represent no deadline and are ignored. Non-negative values replace
///          an absent deadline or a later existing value.
/// @param current Current deadline, using -1 for none.
/// @param candidate Candidate deadline, using -1 for none.
/// @return Earliest non-negative deadline, or -1 when both inputs are absent.
static int64_t rt_gui_min_deadline_ms(int64_t current, int64_t candidate) {
    if (candidate < 0)
        return current;
    return current < 0 || candidate < current ? candidate : current;
}

/// @brief Convert a remaining fractional-second timer to a non-negative millisecond deadline.
/// @details Uses ceiling so the scheduler never wakes early and spins on a timer that has not yet
///          crossed its threshold. Expired/non-finite values request immediate work.
/// @param remaining_seconds Time remaining until a widget timer fires.
/// @return Non-negative millisecond delay.
static int64_t rt_gui_seconds_to_deadline_ms(float remaining_seconds) {
    if (!isfinite(remaining_seconds) || remaining_seconds <= 0.0f)
        return 0;
    double milliseconds = ceil((double)remaining_seconds * 1000.0);
    return milliseconds >= (double)INT64_MAX ? INT64_MAX : (int64_t)milliseconds;
}

/// @brief Find the next specialized timer deadline in a visible widget subtree.
/// @details Covers caret blink, indeterminate progress, and editor drag-autoscroll sources. Generic
///          hover/press/focus motion is represented by `app->animations_active`. The traversal is
///          allocation-free and short-circuits when immediate work is discovered.
/// @param root Retained surface root to inspect; NULL or hidden roots have no deadline.
/// @return Delay in milliseconds, or -1 when the subtree has no scheduled timer.
static int64_t rt_gui_widget_tree_next_deadline_ms(vg_widget_t *root) {
    if (!root || !root->visible)
        return -1;
    int64_t deadline_ms = -1;
    for (vg_widget_t *node = root; node; node = rt_gui_next_visible_widget(root, node)) {
        if (!node->visible)
            continue;
        int64_t candidate = -1;
        switch (node->type) {
            case VG_WIDGET_TEXTINPUT: {
                vg_textinput_t *input = (vg_textinput_t *)node;
                if (node->state & VG_STATE_FOCUSED)
                    candidate = rt_gui_seconds_to_deadline_ms(0.5f - input->cursor_blink_time);
                break;
            }
            case VG_WIDGET_PROGRESS: {
                vg_progressbar_t *progress = (vg_progressbar_t *)node;
                if (progress->style == VG_PROGRESS_INDETERMINATE)
                    candidate = 16;
                break;
            }
            case VG_WIDGET_CODEEDITOR: {
                vg_codeeditor_t *editor = (vg_codeeditor_t *)node;
                if (editor->selection_dragging)
                    candidate = 16;
                else if (node->state & VG_STATE_FOCUSED)
                    candidate = rt_gui_seconds_to_deadline_ms(0.5f - editor->cursor_blink_time);
                break;
            }
            case VG_WIDGET_OUTPUTPANE: {
                vg_outputpane_t *pane = (vg_outputpane_t *)node;
                if (pane->terminal_mode && pane->has_focus)
                    candidate = rt_gui_seconds_to_deadline_ms(0.5f - pane->caret_blink_time);
                break;
            }
            default:
                break;
        }
        deadline_ms = rt_gui_min_deadline_ms(deadline_ms, candidate);
        if (deadline_ms == 0)
            return 0;
    }
    return deadline_ms;
}

/// @brief Compute the nearest currently-known app scheduler deadline.
/// @details Dirty retained state and unsettled generic motion require an immediate frame. Clean
///          surfaces are scanned for specialized timer sources so `PollWait` cannot sleep past a
///          caret blink, drag-autoscroll, or indeterminate-progress update.
/// @param app Valid live GUI app.
/// @return Milliseconds until work, 0 for immediate work, or -1 when none is known.
static int64_t rt_gui_app_next_deadline_ms(const rt_gui_app_t *app) {
    if (!app)
        return -1;
    if (app->animations_active || rt_gui_widget_tree_needs_layout(app->root) ||
        rt_gui_widget_tree_needs_paint(app->root) || rt_gui_app_overlays_need_paint(app)) {
        return 0;
    }
    uint64_t scheduler_now_ms =
        isfinite(app->scheduler_clock_ms) && app->scheduler_clock_ms > 0.0
            ? (app->scheduler_clock_ms >= (double)UINT64_MAX ? UINT64_MAX
                                                             : (uint64_t)app->scheduler_clock_ms)
            : rt_gui_now_ms();
    int64_t deadline_ms = rt_gui_widget_tree_next_deadline_ms(app->root);
    for (int i = 0; i < app->command_palette_count && deadline_ms != 0; ++i) {
        if (app->command_palettes[i])
            deadline_ms = rt_gui_min_deadline_ms(
                deadline_ms, rt_gui_widget_tree_next_deadline_ms(&app->command_palettes[i]->base));
    }
    for (int i = 0; i < app->dialog_count && deadline_ms != 0; ++i) {
        if (app->dialog_stack[i])
            deadline_ms = rt_gui_min_deadline_ms(
                deadline_ms, rt_gui_widget_tree_next_deadline_ms(&app->dialog_stack[i]->base));
    }
    if (app->notification_manager)
        deadline_ms = rt_gui_min_deadline_ms(
            deadline_ms, rt_gui_widget_tree_next_deadline_ms(&app->notification_manager->base));
    deadline_ms = rt_gui_min_deadline_ms(
        deadline_ms,
        vg_notification_manager_next_deadline_ms(app->notification_manager, scheduler_now_ms));
    const vg_tooltip_manager_t *tooltip_manager =
        rt_gui_get_active_app() == app ? vg_tooltip_manager_get() : &app->tooltip_manager_state;
    if (tooltip_manager->active_tooltip)
        deadline_ms = rt_gui_min_deadline_ms(
            deadline_ms,
            rt_gui_widget_tree_next_deadline_ms(&tooltip_manager->active_tooltip->base));
    deadline_ms = rt_gui_min_deadline_ms(
        deadline_ms, vg_tooltip_manager_next_deadline_ms(tooltip_manager, scheduler_now_ms));
    if (app->manual_tooltip)
        deadline_ms = rt_gui_min_deadline_ms(
            deadline_ms, rt_gui_widget_tree_next_deadline_ms(&app->manual_tooltip->base));
    if (app->active_context_menu)
        deadline_ms = rt_gui_min_deadline_ms(
            deadline_ms, rt_gui_widget_tree_next_deadline_ms(&app->active_context_menu->base));
    deadline_ms = rt_gui_min_deadline_ms(deadline_ms, rt_videowidget_next_deadline_ms(app));
    return deadline_ms;
}

/// @brief Poll, schedule, and render one real-time GUI frame.
/// @details Invalid apps and accepted close requests return false without touching stale state.
/// @param app_ptr Runtime-facing app handle.
/// @return 1 while the app remains runnable; otherwise 0.
int64_t rt_gui_app_run_frame(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return 0;
    rt_gui_app_poll(app);
    if (rt_gui_app_should_close(app))
        return 0;
    rt_gui_app_render(app);
    return rt_gui_app_should_close(app) ? 0 : 1;
}

/// @brief Poll and render one frame using a one-shot deterministic delta.
/// @details The render path consumes the override exactly once. Invalid values are normalized here
///          and checked again at consumption so no NaN/negative time reaches widget schedulers.
/// @param app_ptr Runtime-facing app handle.
/// @param delta_ms Requested deterministic elapsed time in milliseconds.
/// @return 1 while the app remains runnable; otherwise 0.
int64_t rt_gui_app_run_frame_with_delta(void *app_ptr, double delta_ms) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return 0;
    if (!isfinite(delta_ms) || delta_ms < 0.0)
        delta_ms = 0.0;
    app->frame_delta_override_ms = delta_ms;
    app->frame_delta_override_pending = 1;
    return rt_gui_app_run_frame(app);
}

/// @brief Query the app's central scheduler deadline without consuming it.
/// @param app_ptr Runtime-facing app handle.
/// @return Milliseconds until work, 0 for immediate work, or -1 for none/invalid.
int64_t rt_gui_app_get_next_deadline_ms(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_app_next_deadline_ms(rt_gui_app_handle_checked(app_ptr));
}

/// @brief Select the app used by app-scoped GUI services and widget constructors.
/// @details Handle validation occurs before activation so stale pointers cannot displace a live
///          current application.
/// @param app_ptr Runtime-facing app handle; invalid values are ignored.
void rt_gui_app_make_current(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (app)
        rt_gui_activate_app(app);
}

/// @brief `App.PollWait` — block up to timeout_ms for OS events, then poll.
/// @details Sleeps on the OS event queue while the window is idle so the frame
///          loop does not busy-poll, then drains events exactly like
///          rt_gui_app_poll (a drop-in replacement for App.Poll). Callers that
///          animate must cap timeout_ms at their next animation deadline.
/// @param app_ptr Pointer to the app.
/// @param timeout_ms Maximum idle wait (clamped to [0, 1000] inside vgfx).
/// @return 1 if events arrived (wake-from-input), 0 on timeout.
int64_t rt_gui_app_poll_wait(void *app_ptr, int64_t timeout_ms) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app || !app->window) {
        rt_gui_app_poll(app_ptr);
        return 0;
    }
    if (timeout_ms < 0)
        timeout_ms = 0;
    if (timeout_ms > 1000)
        timeout_ms = 1000;
    int64_t deadline_ms = rt_gui_app_next_deadline_ms(app);
    if (deadline_ms >= 0 && timeout_ms > deadline_ms)
        timeout_ms = deadline_ms;
    int32_t had_events = vgfx_wait_events(app->window, (int32_t)timeout_ms);
    rt_gui_app_poll(app_ptr);
    return had_events ? 1 : 0;
}

/// @brief Return the root container widget of the app's widget tree.
/// @details The root is a plain VG_WIDGET_CONTAINER that fills the window. All
///          user-created widgets are added as children (or descendants) of this
///          root. Layout is driven by the window's physical dimensions.
/// @param app_ptr Pointer to the app.
/// @return Root vg_widget_t pointer, or NULL if the app is NULL.
void *rt_gui_app_get_root(void *app_ptr) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return NULL;
    return app->root;
}

/// @brief Replace the app's default font and propagate it to all existing widgets.
/// @details Sets the new font as the app's default (used by all future widget
///          constructors), then walks the entire widget tree, all dialogs,
///          command palettes, the notification manager, and the manual tooltip,
///          updating each to the new font. The old font is not freed immediately
///          — it is retained in the app's retired-fonts list and freed at
///          app_destroy, because widgets may still reference it during the
///          current frame.
/// @param app_ptr Pointer to the app.
/// @param font    New font to use (vg_font_t*).
/// @param size    Font size in logical points.
void rt_gui_app_set_font(void *app_ptr, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_handle_checked(app_ptr);
    if (!app)
        return;
    rt_gui_activate_app(app);
    vg_font_t *new_font = rt_gui_font_handle_checked(font);
    if (!new_font)
        return;
    vg_font_t *old_font = app->default_font;
    int old_owned = app->default_font_owned;
    app->default_font = new_font;
    app->default_font_size = (float)rt_gui_sanitize_font_size(size, 14.0);
    app->default_font_owned = 0;
    if (app->theme && (!app->theme_base || !app->theme_base->typography.font_regular ||
                       app->theme->typography.font_regular == old_font)) {
        app->theme->typography.font_regular = new_font;
    }
    rt_gui_reapply_default_font(app);
    if (old_owned && old_font && old_font != app->default_font)
        rt_gui_retire_font(app, old_font);
}

/// @brief Recursively paint a widget subtree, accumulating absolute screen coordinates.
/// @details Adds parent_abs_x/y to widget->x/y so paint functions see absolute screen
///          positions, calls the vtable paint function, then immediately restores the
///          relative coordinates. Restoration is critical: hit testing in the next poll
///          calls vg_widget_get_screen_bounds which walks the parent chain — leaving
///          absolute coords would double-count every ancestor offset. Widgets that paint
///          their own children (e.g. ScrollView) are skipped by the child recursion.
/// @param window       Platform window handle passed to vtable paint.
/// @param widget       Root of the subtree to paint (skipped if NULL or invisible).
/// @param parent_abs_x Accumulated absolute X of the parent widget.
/// @param parent_abs_y Accumulated absolute Y of the parent widget.
typedef struct rt_gui_render_frame {
    vg_widget_t *widget;
    float parent_abs_x;
    float parent_abs_y;
} rt_gui_render_frame_t;

/// @brief Push a render frame (widget + accumulated parent absolute origin) onto the
///        explicit paint stack, growing it as needed.
/// @details Backs render_widget_tree's iterative (non-recursive) traversal so deeply
///          nested layouts cannot overflow the C stack. Capacity doubles from 64 via
///          an overflow-guarded realloc.
/// @param frames In/out render-frame array.
/// @param count In/out active frame count.
/// @param cap In/out allocated frame capacity.
/// @param widget Borrowed widget for the new frame; NULL is a successful no-op.
/// @param parent_abs_x Accumulated parent X origin.
/// @param parent_abs_y Accumulated parent Y origin.
/// @return true on success — and on a NULL widget, treated as a no-op; false on
///         capacity overflow or realloc failure.
static bool rt_gui_render_stack_push(rt_gui_render_frame_t **frames,
                                     size_t *count,
                                     size_t *cap,
                                     vg_widget_t *widget,
                                     float parent_abs_x,
                                     float parent_abs_y) {
    if (!widget)
        return true;
    if (*count == *cap) {
        size_t new_cap = 0;
        if (!rt_gui_next_collection_capacity(*cap, *count + 1u, 64u, sizeof(**frames), &new_cap))
            return false;
        rt_gui_render_frame_t *new_frames =
            (rt_gui_render_frame_t *)realloc(*frames, new_cap * sizeof(*new_frames));
        if (!new_frames)
            return false;
        *frames = new_frames;
        *cap = new_cap;
    }
    (*frames)[(*count)++] = (rt_gui_render_frame_t){widget, parent_abs_x, parent_abs_y};
    return true;
}

/// @brief Paint a widget subtree into @p window, iteratively (no recursion).
/// @details Uses an explicit stack of frames so deep layouts can't overflow the C
///          stack. Each frame carries the parent's absolute origin; a widget's
///          stored relative (x,y) is temporarily promoted to absolute for painting
///          and its children pushed with the new origin. Invisible widgets are skipped.
/// @param window Borrowed platform window receiving paint operations.
/// @param widget Borrowed subtree root.
/// @param parent_abs_x Accumulated parent X origin.
/// @param parent_abs_y Accumulated parent Y origin.
static void render_widget_tree(vgfx_window_t window,
                               vg_widget_t *widget,
                               float parent_abs_x,
                               float parent_abs_y) {
    rt_gui_render_frame_t *frames = NULL;
    size_t count = 0;
    size_t cap = 0;
    if (!rt_gui_render_stack_push(&frames, &count, &cap, widget, parent_abs_x, parent_abs_y))
        return;

    size_t visited = 0;
    while (count > 0) {
        if (visited++ >= RT_GUI_MAX_COLLECTION_ITEMS) {
            free(frames);
            return;
        }
        rt_gui_render_frame_t frame = frames[--count];
        widget = frame.widget;
        if (!widget || !widget->visible)
            continue;

        float abs_x = widget->x + frame.parent_abs_x;
        float abs_y = widget->y + frame.parent_abs_y;
        float rel_x = widget->x;
        float rel_y = widget->y;
        widget->x = abs_x;
        widget->y = abs_y;

        if (widget->vtable && widget->vtable->paint) {
            bool was_screen_space = widget->_paint_screen_space;
            widget->_paint_screen_space = true;
            widget->vtable->paint(widget, (void *)window);
            widget->_paint_screen_space = was_screen_space;
        }

        widget->x = rel_x;
        widget->y = rel_y;

        // Record complete visual bounds after restoring retained coordinates.
        // The query includes shadows, focus glow, and any popup drawn later by the
        // overlay pass, allowing the next frame to repair both moved and vacated pixels.
        float visual_x = 0.0f, visual_y = 0.0f, visual_w = 0.0f, visual_h = 0.0f;
        vg_widget_get_visual_bounds(widget, &visual_x, &visual_y, &visual_w, &visual_h);
        widget->last_paint_x = visual_x;
        widget->last_paint_y = visual_y;
        widget->last_paint_w = visual_w;
        widget->last_paint_h = visual_h;
        widget->last_paint_valid = visual_w > 0.0f && visual_h > 0.0f;

        if (rt_gui_widget_paints_children_internally(widget))
            continue;

        for (vg_widget_t *child = widget->last_child; child; child = child->prev_sibling) {
            if (!rt_gui_render_stack_push(&frames, &count, &cap, child, abs_x, abs_y)) {
                free(frames);
                return;
            }
        }
    }
    free(frames);
}

/// @brief Second-pass overlay paint: draws popup menus, focus rings, floating panels, and drag
///        previews above the main widget tree.
/// @details Same absolute-coordinate trick as render_widget_tree: temporarily converts
///          widget->x/y to screen space for paint_overlay, then restores relative. This
///          pass runs after the primary tree walk so overlays always appear on top. Widgets
///          that internally paint children (ScrollView) skip the recursive child walk.
/// @param window       Platform window handle.
/// @param widget       Root of the subtree (skipped if NULL or invisible).
/// @param parent_abs_x Accumulated absolute X of the parent widget.
/// @param parent_abs_y Accumulated absolute Y of the parent widget.
static void render_widget_overlay_tree(vgfx_window_t window,
                                       vg_widget_t *widget,
                                       float parent_abs_x,
                                       float parent_abs_y) {
    rt_gui_render_frame_t *frames = NULL;
    size_t count = 0;
    size_t cap = 0;
    if (!rt_gui_render_stack_push(&frames, &count, &cap, widget, parent_abs_x, parent_abs_y))
        return;

    size_t visited = 0;
    while (count > 0) {
        if (visited++ >= RT_GUI_MAX_COLLECTION_ITEMS) {
            free(frames);
            return;
        }
        rt_gui_render_frame_t frame = frames[--count];
        widget = frame.widget;
        if (!widget || !widget->visible)
            continue;

        float abs_x = widget->x + frame.parent_abs_x;
        float abs_y = widget->y + frame.parent_abs_y;
        float rel_x = widget->x;
        float rel_y = widget->y;
        widget->x = abs_x;
        widget->y = abs_y;

        if (widget->vtable && widget->vtable->paint_overlay) {
            bool was_screen_space = widget->_paint_screen_space;
            widget->_paint_screen_space = true;
            widget->vtable->paint_overlay(widget, (void *)window);
            widget->_paint_screen_space = was_screen_space;
        }

        widget->x = rel_x;
        widget->y = rel_y;

        if (rt_gui_widget_paints_children_internally(widget))
            continue;

        for (vg_widget_t *child = widget->last_child; child; child = child->prev_sibling) {
            if (!rt_gui_render_stack_push(&frames, &count, &cap, child, abs_x, abs_y)) {
                free(frames);
                return;
            }
        }
    }
    free(frames);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics-disabled applications have no scheduler generation.
/// @param app_ptr Ignored application handle.
/// @return Always 0.
uint64_t rt_gui_app_frame_generation_for_owner(void *app_ptr) {
    (void)app_ptr;
    return 0u;
}

// ===========================================================================
// Headless stubs — match the public-API prototypes above so that
// non-graphical builds (server / CLI) link cleanly. Each
// stub safely no-ops or returns a sentinel (NULL, 0, 1 for "should
// close"). Doc comments inherit from the real implementations above.
// ===========================================================================

rt_gui_app_t *s_current_app = NULL;

/// @brief Stub capability query for a graphics-disabled runtime.
/// @return Always 0 because no GUI backend is compiled into this branch.
int64_t rt_gui_system_is_available(void) {
    return 0;
}

/// @brief Return the stable reason that GUI support is unavailable.
/// @return Caller-owned runtime string containing the graphics-disabled capability message.
rt_string rt_gui_system_get_unavailable_reason(void) {
    return rt_const_cstr("GUI support is not available in this build");
}

/// @brief Stub: graphics disabled — no-op; modal dialog management requires a live app.
/// @param dlg Ignored dialog handle.
void rt_gui_set_active_dialog(void *dlg) {
    (void)dlg;
}

/// @brief Stub: graphics disabled — returns NULL; no window or widget tree is created.
/// @param title Ignored application title.
/// @param width Ignored requested width.
/// @param height Ignored requested height.
/// @return Always NULL.
void *rt_gui_app_new(rt_string title, int64_t width, int64_t height) {
    (void)title;
    (void)width;
    (void)height;
    return NULL;
}

/// @brief Stub fallible constructor that reports unavailable GUI support without trapping.
/// @param title Ignored window title.
/// @param width Ignored initial width.
/// @param height Ignored initial height.
/// @return Caller-owned `Result.ErrStr` containing the stable capability message.
void *rt_gui_app_try_new(rt_string title, int64_t width, int64_t height) {
    (void)title;
    (void)width;
    (void)height;
    return rt_result_err_str(rt_const_cstr("GUI support is not available in this build"));
}

/// @brief No-op stub: default font loading (graphics disabled).
void rt_gui_ensure_default_font(void) {}

/// @brief No-op stub: app destruction (graphics disabled).
/// @param app_ptr Ignored application handle.
void rt_gui_app_destroy(void *app_ptr) {
    (void)app_ptr;
}

/// @brief Stub: always returns 1 (close immediately when graphics disabled).
/// @param app_ptr Ignored application handle.
/// @return Always 1.
int64_t rt_gui_app_should_close(void *app_ptr) {
    (void)app_ptr;
    return 1;
}

/// @brief Poll the app.
/// @param app_ptr Ignored application handle.
void rt_gui_app_poll(void *app_ptr) {
    (void)app_ptr;
}

/// @brief Stub: `App.PollWait` returns 0 without graphics.
/// @param app_ptr Ignored application handle.
/// @param timeout_ms Ignored maximum wait.
/// @return Always 0.
int64_t rt_gui_app_poll_wait(void *app_ptr, int64_t timeout_ms) {
    (void)app_ptr;
    (void)timeout_ms;
    return 0;
}

/// @brief Render the app.
/// @param app_ptr Ignored application handle.
void rt_gui_app_render(void *app_ptr) {
    (void)app_ptr;
}

/// @brief Stub: a graphics-disabled app cannot run a GUI frame.
/// @details The handle is ignored and no polling, timing, or rendering work occurs.
/// @param app_ptr Ignored app handle.
/// @return Always 0 because GUI construction is unavailable.
int64_t rt_gui_app_run_frame(void *app_ptr) {
    (void)app_ptr;
    return 0;
}

/// @brief Stub: deterministic GUI frames are unavailable without graphics.
/// @details Both arguments are ignored; no scheduler state exists in this build configuration.
/// @param app_ptr Ignored app handle.
/// @param delta_ms Ignored deterministic elapsed time.
/// @return Always 0.
int64_t rt_gui_app_run_frame_with_delta(void *app_ptr, double delta_ms) {
    (void)app_ptr;
    (void)delta_ms;
    return 0;
}

/// @brief Stub: report that no graphics-disabled GUI deadline exists.
/// @param app_ptr Ignored app handle.
/// @return Always -1.
int64_t rt_gui_app_get_next_deadline_ms(void *app_ptr) {
    (void)app_ptr;
    return -1;
}

/// @brief Stub: no app can become current without a GUI backend.
/// @param app_ptr Ignored app handle.
void rt_gui_app_make_current(void *app_ptr) {
    (void)app_ptr;
}

/// @brief Stub: graphics disabled — returns NULL; no root widget exists without a live app.
/// @param app_ptr Ignored application handle.
/// @return Always NULL.
void *rt_gui_app_get_root(void *app_ptr) {
    (void)app_ptr;
    return NULL;
}

/// @brief Set the font of the app.
/// @param app_ptr Ignored application handle.
/// @param font Ignored font handle.
/// @param size Ignored font size.
void rt_gui_app_set_font(void *app_ptr, void *font, double size) {
    (void)app_ptr;
    (void)font;
    (void)size;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
