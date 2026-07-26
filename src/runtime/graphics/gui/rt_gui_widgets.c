//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_widgets.c
// Purpose: Runtime bindings for the ZannaGUI base widget API and fundamental
//   widgets: font loading/destroy, widget visibility/enabled/size/flex/margin,
//   Container, Label, Button (with icon support), TextInput (with undo/redo),
//   Checkbox, RadioButton, Slider, ProgressBar, Image, ListBox, ComboBox,
//   and the tab-order focus system. This file is the foundational widget layer
//   on which all other GUI runtime files depend.
//
// Key invariants:
//   - All widget functions guard against NULL widget pointer before delegating
//     to vg_widget_* or the specific widget's vg_* API.
//   - Tab order is built lazily by vg_build_tab_order; explicit tab_index values
//     sort before default (-1) entries in DFS order.
//   - TextInput undo stack uses a "push after edit" model: the initial empty
//     string is pushed at creation; each insert/delete pushes the new state.
//   - Button icon is stored as an owned char* (icon_text) in the vg_button_t;
//     icon_pos 0 = left, 1 = right; drawn 4 px gap from the label.
//   - ListBox and ComboBox item indices are zero-based; out-of-range indices
//     return -1 / NULL from get_selected calls.
//
// Ownership/Lifetime:
//   - All widget objects are vg_widget_t* (or subtype) owned by the vg widget
//     tree; vg_widget_destroy() on any ancestor frees the full subtree.
//   - Public Font values are managed wrappers over vg_font_t. Legacy raw handles remain accepted;
//     backing fonts referenced by retained surfaces are retired through a safe frame generation.
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/lib/gui/include/vg.h (ZannaGUI C API),
//        src/runtime/graphics/rt_gui_app.c (default font, s_current_app),
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md,
//        docs/adr/0165-scrollview-descendant-reveal.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_font_platform.h"
#include "rt_gui_internal.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"

#include "fonts/embedded_font.h"

#ifdef ZANNA_ENABLE_GRAPHICS

#include "vg_icon_vector.h"

static void rt_widget_notify_accessibility_tree(vg_widget_t *widget) {
    rt_gui_app_t *app = rt_gui_app_from_widget(widget);
    if (app)
        rt_gui_accessibility_platform_notify(app->window, app->root);
}

/// @brief Resolve a parent-container handle to its widget.
/// @details Three-state contract: a NULL handle returns NULL (legitimate top-level
///          placement); a valid handle returns its container widget; a non-NULL
///          handle that fails to resolve also returns NULL — an error the caller
///          must treat as "invalid parent", not "no parent".
static vg_widget_t *rt_widget_parent_or_null_if_invalid(void *parent) {
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    return parent_widget;
}

/// @brief Wrap a borrowed live widget handle in an owned `Zanna.Option`.
/// @details Runtime object retention is a no-op for lower-toolkit pointers, so finalizing the
///          Option does not destroy or otherwise alter the widget. Invalid or absent widgets map
///          to `None` defensively.
/// @param widget Candidate borrowed widget payload.
/// @return Fresh owned `Some(widget)` or `None` runtime object.
static void *rt_widget_borrowed_option(vg_widget_t *widget) {
    return vg_widget_is_live(widget) ? rt_option_some(widget) : rt_option_none();
}

/// @brief External shim for modules that need parent-container validation
///        without including rt_gui_internal.h inline helpers.
void *rt_gui_widget_parent_container_checked(void *handle) {
    return rt_gui_widget_parent_container_from_handle(handle);
}

/// @brief Resolve a live widget handle to its borrowed owning GUI application.
/// @details This exported shim keeps media modules independent of private widget/app layouts while
///          retaining the same liveness/type validation as the rest of the runtime boundary.
void *rt_gui_widget_owner_app(void *handle) {
    vg_widget_t *widget = rt_gui_widget_handle_checked(handle);
    return widget ? rt_gui_app_from_widget(widget) : NULL;
}

/// @brief Validate a borrowed widget handle for the C++ virtual-model binding bridge.
void *rt_gui_widget_checked_for_binding(void *handle, int64_t widget_type) {
    if (widget_type < 0 || widget_type > (int64_t)VG_WIDGET_CUSTOM)
        return NULL;
    return rt_gui_widget_handle_checked_type(handle, (vg_widget_type_t)widget_type);
}

//=============================================================================
// Font Functions
//=============================================================================

#define RT_GUI_FONT_HANDLE_MAGIC UINT64_C(0x52544755464F4E54)

/// @brief Runtime-managed public Font wrapper around one lower-toolkit font.
/// @details The wrapper makes Font values safe payloads for Option/Result and ordinary runtime
///          reference counting. Widgets keep the lower pointer for rendering; finalization uses
///          the app-wide retirement scanner so those borrowed render references remain valid
///          through a presentation boundary.
typedef struct rt_gui_font_handle {
    uint64_t magic;  ///< Live-wrapper discriminator.
    vg_font_t *font; ///< Owned lower font until explicit destroy/finalization transfers retirement.
} rt_gui_font_handle_t;

/// @brief Validate a managed Font wrapper without reading arbitrary memory.
/// @param handle Candidate runtime object.
/// @return Live wrapper, or NULL for null/wrong/freed/corrupt values.
static rt_gui_font_handle_t *rt_gui_managed_font_checked(void *handle) {
    if (!rt_obj_is_instance(handle, RT_GUI_FONT_HANDLE_CLASS_ID, sizeof(rt_gui_font_handle_t))) {
        return NULL;
    }
    rt_gui_font_handle_t *managed = (rt_gui_font_handle_t *)handle;
    return managed->magic == RT_GUI_FONT_HANDLE_MAGIC ? managed : NULL;
}

int rt_gui_font_handle_is_managed(void *handle) {
    return rt_gui_managed_font_checked(handle) != NULL;
}

vg_font_t *rt_gui_font_handle_checked(void *handle) {
    if (!handle)
        return NULL;
    rt_gui_font_handle_t *managed = rt_gui_managed_font_checked(handle);
    if (managed)
        return vg_font_is_live(managed->font) ? managed->font : NULL;
    vg_font_t *legacy = (vg_font_t *)handle;
    return vg_font_is_live(legacy) ? legacy : NULL;
}

/// @brief Release or retire a wrapper's backing font exactly once.
/// @details A font still referenced by an app/widget is transferred into the app's generation
///          retirement queue. Otherwise it is destroyed immediately. The wrapper is made inert
///          before either path, making explicit Destroy and finalization idempotent.
/// @param managed Live managed wrapper; NULL is ignored.
static void rt_gui_font_release_backing(rt_gui_font_handle_t *managed) {
    if (!managed || !managed->font)
        return;
    vg_font_t *font = managed->font;
    managed->font = NULL;
    if (rt_gui_retire_font_if_in_use(font))
        return;
    if (vg_font_is_live(font))
        vg_font_destroy(font);
}

/// @brief Runtime finalizer for a managed Font wrapper.
/// @param object Wrapper payload supplied by the runtime object manager; may be NULL.
static void rt_gui_font_finalize(void *object) {
    rt_gui_font_handle_t *managed = rt_gui_managed_font_checked(object);
    if (!managed)
        return;
    rt_gui_font_release_backing(managed);
    managed->magic = 0;
}

/// @brief Wrap an owned lower font in a runtime-managed public Font value.
/// @details Ownership transfers on entry. Allocation follows the runtime object's trap-on-OOM
///          contract. A successful wrapper starts with one caller-owned reference.
/// @param font Owned live lower font; must not be NULL.
/// @return Managed Font wrapper, or NULL only for an invalid input.
static void *rt_gui_font_wrap(vg_font_t *font) {
    if (!vg_font_is_live(font))
        return NULL;
    rt_gui_font_handle_t *managed = (rt_gui_font_handle_t *)rt_obj_new_i64(
        RT_GUI_FONT_HANDLE_CLASS_ID, (int64_t)sizeof(rt_gui_font_handle_t));
    if (!managed)
        return NULL;
    memset(managed, 0, sizeof(*managed));
    managed->magic = RT_GUI_FONT_HANDLE_MAGIC;
    managed->font = font;
    rt_obj_set_finalizer(managed, rt_gui_font_finalize);
    return managed;
}

/// @brief Load a font from a file path and return an opaque handle.
/// @details Converts the runtime string path to a C string, loads the font via
///          vg_font_load_file, and wraps it in a reference-counted runtime object. The loaded font
///          is not automatically applied to any widget; use a SetFont operation to apply it.
/// @param path File path to a .ttf or .ttc font file (runtime string).
/// @return Opaque font handle, or NULL if the file could not be loaded.
/// @brief Return the process-shared embedded fallback face (lazy, never freed).
/// @details Guarantees per-glyph coverage for codepoints a user or system face
///          cannot map (plan 06). Lives for the process lifetime by design.
static vg_font_t *rt_gui_font_embedded_fallback(void) {
    static vg_font_t *s_fallback;
    if (!s_fallback)
        s_fallback = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    return s_fallback;
}

void *rt_font_load(rt_string path) {
    RT_ASSERT_MAIN_THREAD();
    char *cpath = rt_string_to_cstr_no_nul(path);
    if (!cpath)
        return NULL;

    vg_font_t *font = vg_font_load_file(cpath);
    free(cpath);
    if (!font)
        return NULL;
    vg_font_t *fallback = rt_gui_font_embedded_fallback();
    if (fallback && fallback != font)
        vg_font_set_fallback(font, fallback);
    return rt_gui_font_wrap(font);
}

/// @brief Validate a public system-font logical size.
/// @param size Requested logical-point size.
/// @return Non-zero only for finite values in the supported [1,512] range.
static int rt_gui_font_system_size_is_valid(double size) {
    return isfinite(size) && size >= 1.0 && size <= 512.0;
}

/// @brief Load one system UI role with deterministic embedded fallback and return Result<Font>.
/// @details The proportional platform adapter is attempted first. If no host candidate parses, a
///          private copy of the embedded JetBrains Mono font guarantees deterministic availability.
///          The requested unscaled logical size is stored as font metadata. The returned Result
///          owns its Font payload; callers can unwrap and retain it using ordinary runtime rules.
/// @param size Requested logical-point size in [1,512].
/// @param bold Non-zero selects the bold UI role; zero selects regular.
/// @return Caller-owned Result.Ok(Font), or Result.ErrStr for invalid input/load failure.
static void *rt_gui_font_load_system_ui_result(double size, int bold) {
    if (!rt_gui_font_system_size_is_valid(size)) {
        return rt_result_err_str(
            rt_const_cstr("font size must be finite and between 1 and 512 logical points"));
    }
    vg_font_t *font = rt_gui_font_platform_load_system_ui(bold != 0);
    if (font)
        vg_font_set_fallback(font, rt_gui_font_embedded_fallback());
    else
        font = vg_font_load(vg_embedded_font_data, (size_t)vg_embedded_font_size);
    if (!font) {
        return rt_result_err_str(
            rt_const_cstr("unable to load the system UI font or embedded fallback"));
    }
    vg_font_set_logical_size(font, (float)size);
    void *managed = rt_gui_font_wrap(font);
    if (!managed) {
        vg_font_destroy(font);
        return rt_result_err_str(rt_const_cstr("unable to create a managed GUI font"));
    }
    void *result = rt_result_ok(managed);
    if (rt_obj_release_check0(managed))
        rt_obj_free(managed);
    return result;
}

/// @brief Load the regular proportional system UI role.
/// @param size Requested logical-point size in [1,512].
/// @return Caller-owned Result.Ok(Font), or Result.ErrStr with a stable diagnostic.
void *rt_font_load_system_ui(double size) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_font_load_system_ui_result(size, 0);
}

/// @brief Load the bold proportional system UI role.
/// @param size Requested logical-point size in [1,512].
/// @return Caller-owned Result.Ok(Font), or Result.ErrStr with a stable diagnostic.
void *rt_font_load_system_ui_bold(double size) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_font_load_system_ui_result(size, 1);
}

/// @brief Return a Font's preserved unscaled logical size.
/// @details Legacy file-loaded fonts report zero unless annotated internally. Null, stale,
///          explicitly destroyed, and unrelated handles are rejected without dereferencing them.
/// @param font Managed Font wrapper or legacy raw font handle.
/// @return Finite positive logical-point size, or zero when unavailable.
double rt_font_get_logical_size(void *font) {
    RT_ASSERT_MAIN_THREAD();
    return (double)vg_font_get_logical_size(rt_gui_font_handle_checked(font));
}

/// @brief Free a previously loaded font and release its resources.
/// @details Destroys the vg_font_t, freeing rasterized glyph caches and the
///          font data buffer. If a live app or widget tree still references the
///          font, destruction is deferred until the owning app is torn down.
/// @param font Opaque font handle from rt_font_load (safe to pass NULL).
void rt_font_destroy(void *font) {
    RT_ASSERT_MAIN_THREAD();
    if (!font)
        return;
    rt_gui_font_handle_t *managed = rt_gui_managed_font_checked(font);
    if (managed) {
        rt_gui_font_release_backing(managed);
        return;
    }
    vg_font_t *legacy = (vg_font_t *)font;
    if (rt_gui_retire_font_if_in_use(legacy))
        return;
    if (vg_font_is_live(legacy))
        vg_font_destroy(legacy);
}

//=============================================================================
// Widget Functions
//=============================================================================

/// @brief Return non-zero if `candidate` is `root` or any descendant in the widget tree.
/// @details Used by rt_widget_forget_runtime_refs to check whether a cached app-level
///          pointer (last_clicked, drag_source, etc.) falls inside the subtree that is
///          about to be destroyed, so it can be nulled out before the widget is freed.
int rt_gui_widget_tree_contains(vg_widget_t *root, const vg_widget_t *candidate) {
    if (!root || !candidate)
        return 0;
    for (vg_widget_t *node = root; node;) {
        if (node == candidate)
            return 1;
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
    return 0;
}

/// @brief Return non-zero if @p root contains the owner statusbar for @p item.
static int rt_widget_tree_contains_statusbar_item(vg_widget_t *root, vg_statusbar_item_t *item) {
    if (!root || !item)
        return 0;
    if (root->type == VG_WIDGET_STATUSBAR) {
        vg_statusbar_t *bar = (vg_statusbar_t *)root;
        for (size_t i = 0; i < bar->left_count; i++) {
            if (bar->left_items[i] == item)
                return 1;
        }
        for (size_t i = 0; i < bar->center_count; i++) {
            if (bar->center_items[i] == item)
                return 1;
        }
        for (size_t i = 0; i < bar->right_count; i++) {
            if (bar->right_items[i] == item)
                return 1;
        }
    }
    for (vg_widget_t *child = root->first_child; child; child = child->next_sibling) {
        if (rt_widget_tree_contains_statusbar_item(child, item))
            return 1;
    }
    return 0;
}

/// @brief Return non-zero if @p root contains the owner toolbar for @p item.
static int rt_widget_tree_contains_toolbar_item(vg_widget_t *root, vg_toolbar_item_t *item) {
    if (!root || !item)
        return 0;
    if (root->type == VG_WIDGET_TOOLBAR) {
        vg_toolbar_t *bar = (vg_toolbar_t *)root;
        for (size_t i = 0; i < bar->item_count; i++) {
            if (bar->items[i] == item)
                return 1;
        }
    }
    for (vg_widget_t *child = root->first_child; child; child = child->next_sibling) {
        if (rt_widget_tree_contains_toolbar_item(child, item))
            return 1;
    }
    return 0;
}

/// @brief Clear any rt_gui_app_t back-pointers that reference `widget` or its subtree.
/// @details Called immediately before vg_widget_destroy to prevent the app's cached
///          raw pointers (last_clicked, drag_source, drag_over_widget,
///          last_statusbar_clicked, last_toolbar_clicked) from becoming dangling after
///          the widget tree is freed.
void rt_widget_forget_runtime_refs(rt_gui_app_t *app, vg_widget_t *widget) {
    if (!widget)
        return;
    if (app) {
        if (rt_gui_widget_tree_contains(widget, app->last_clicked))
            app->last_clicked = NULL;
        if (rt_gui_widget_tree_contains(widget, app->drag_candidate))
            app->drag_candidate = NULL;
        if (rt_gui_widget_tree_contains(widget, app->drag_source)) {
            if (app->drag_source)
                app->drag_source->_is_being_dragged = false;
            app->drag_source = NULL;
        }
        if (rt_gui_widget_tree_contains(widget, app->drag_over_widget)) {
            if (app->drag_over_widget)
                app->drag_over_widget->_is_drag_over = false;
            app->drag_over_widget = NULL;
        }
        if (rt_widget_tree_contains_statusbar_item(widget, app->last_statusbar_clicked))
            app->last_statusbar_clicked = NULL;
        if (rt_widget_tree_contains_toolbar_item(widget, app->last_toolbar_clicked))
            app->last_toolbar_clicked = NULL;
    }
    rt_findbar_forget_editor_subtree(widget);
    rt_minimap_forget_editor_subtree(widget);
    rt_gui_invalidate_widget_subhandles(widget);
}

/// @brief Destroy a widget and its entire subtree, freeing all resources.
/// @details Delegates to vg_widget_destroy, which recursively frees all child
///          widgets. After this call, all pointers to the widget or its
///          descendants are invalid. Safe to call with NULL.
/// @param widget Widget to destroy (opaque handle).
void rt_widget_destroy(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;

    rt_gui_app_t *app = rt_gui_app_from_widget(w);
    if (app && app->root == w)
        return;

    rt_widget_forget_runtime_refs(app, w);
    if (app)
        rt_gui_accessibility_platform_notify(app->window, app->root);
    vg_widget_destroy(w);
}

/// @brief Show or hide a widget.
/// @details Hidden widgets are skipped during layout, painting, and event
///          dispatch. Child widgets inherit the parent's visibility — hiding
///          a container hides its entire subtree.
/// @param widget  Widget to modify.
/// @param visible Non-zero to show, zero to hide.
void rt_widget_set_visible(void *widget, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        vg_widget_set_visible(w, visible != 0);
        rt_widget_notify_accessibility_tree(w);
    }
}

/// @brief Enable or disable user interaction with a widget.
/// @details Disabled widgets are painted with reduced opacity and do not
///          receive mouse/keyboard events. Unlike visibility, disabled widgets
///          still participate in layout and occupy space.
/// @param widget  Widget to modify.
/// @param enabled Non-zero to enable, zero to disable.
void rt_widget_set_enabled(void *widget, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        vg_widget_set_enabled(w, enabled != 0);
        rt_widget_notify_accessibility_tree(w);
    }
}

/// @brief Set a fixed width and height on the widget.
/// @details Overrides the layout engine's automatic sizing. When a fixed size
///          is set, the widget ignores flex-grow and measures at exactly these
///          dimensions. Pass 0 for either dimension to revert to auto-sizing.
///          Do NOT set a fixed size on the root widget — it is resized
///          dynamically from the window dimensions each frame.
/// @param widget Widget to modify.
/// @param width  Fixed width in logical pixels.
/// @param height Fixed height in logical pixels.
void rt_widget_set_size(void *widget, int64_t width, int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        rt_gui_app_t *app = rt_gui_app_from_widget(w);
        if (app && app->root == w)
            return;
        vg_widget_set_fixed_size(w,
                                 rt_gui_logical_length_to_physical(w, (double)width),
                                 rt_gui_logical_length_to_physical(w, (double)height));
        rt_widget_notify_accessibility_tree(w);
    }
}

/// @brief Set the preferred (natural) size hint for the layout engine.
/// @details Unlike a fixed size, a preferred size is a soft request — the layout
///          engine will try to honor it but may shrink or grow the widget to satisfy
///          flex constraints or container limits. Use when you want a "default" size
///          without preventing the widget from stretching.
/// @param widget Widget to modify.
/// @param width  Preferred width in logical pixels (>= 0).
/// @param height Preferred height in logical pixels (>= 0).
void rt_widget_set_preferred_size(void *widget, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        rt_gui_app_t *app = rt_gui_app_from_widget(w);
        if (app && app->root == w)
            return;
        vg_widget_set_preferred_size(w,
                                     rt_gui_logical_length_to_physical(w, width),
                                     rt_gui_logical_length_to_physical(w, height));
    }
}

/// @brief Set the maximum size the widget may grow to during layout.
/// @details Caps the upper bound on the widget's computed dimensions. A flex-grow
///          widget will not exceed these values even if there is extra space.
///          Passing 0 for either dimension removes the maximum constraint for that axis.
/// @param widget Widget to modify.
/// @param width  Maximum width in logical pixels (0 = unconstrained).
/// @param height Maximum height in logical pixels (0 = unconstrained).
void rt_widget_set_max_size(void *widget, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        rt_gui_app_t *app = rt_gui_app_from_widget(w);
        if (app && app->root == w)
            return;
        vg_widget_set_max_size(w,
                               rt_gui_logical_length_to_physical(w, width),
                               rt_gui_logical_length_to_physical(w, height));
    }
}

/// @brief Set the widget's minimum size from logical public values.
/// @details Root constraints remain application-owned. Every other live widget converts through
///          its effective scale exactly once; detached widgets use scale 1. Numeric normalization
///          is atomic at the per-value boundary and the lower widget API maintains constraint
///          consistency.
/// @param widget Widget to modify; invalid, stale, and app handles are ignored.
/// @param width Non-negative logical minimum width.
/// @param height Non-negative logical minimum height.
void rt_widget_set_min_size(void *widget, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(w);
    if (app && app->root == w)
        return;
    vg_widget_set_min_size(w,
                           rt_gui_logical_length_to_physical(w, width),
                           rt_gui_logical_length_to_physical(w, height));
}

/// @brief Return the widget's stored minimum width converted to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical minimum width.
double rt_widget_get_min_width(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->constraints.min_width) : 0.0;
}

/// @brief Return the widget's stored minimum height converted to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical minimum height.
double rt_widget_get_min_height(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->constraints.min_height) : 0.0;
}

/// @brief Set the flex-grow factor for a widget within a VBox/HBox container.
/// @details The flex value determines how much of the remaining space (after
///          fixed-size and auto-sized widgets) this widget claims. A flex of 1.0
///          means equal share; 2.0 means double share relative to flex-1 siblings.
///          A flex of 0.0 means the widget only takes its natural/fixed size.
/// @param widget Widget to modify.
/// @param flex   Flex-grow factor (>= 0.0).
void rt_widget_set_flex(void *widget, double flex) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        vg_widget_set_flex(w, rt_gui_sanitize_nonnegative_float(flex, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Add a child widget to a parent container.
/// @details The parent handle can be either an app handle (uses app->root) or
///          a widget pointer. The child is appended to the parent's child list
///          and will participate in layout and painting during the next frame.
///          The child's lifetime is now tied to the parent — destroying the
///          parent destroys all children recursively.
/// @param parent Parent container or app handle.
/// @param child  Widget to add as a child.
void rt_widget_add_child(void *parent, void *child) {
    RT_ASSERT_MAIN_THREAD();
    if (parent && child) {
        vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
        vg_widget_t *child_widget = rt_gui_widget_handle_checked(child);
        if (parent_widget && child_widget) {
            rt_gui_app_t *old_app = rt_gui_app_from_widget(child_widget);
            rt_gui_app_t *new_app = rt_gui_app_from_widget(parent_widget);
            if (old_app && old_app != new_app)
                rt_widget_forget_runtime_refs(old_app, child_widget);
            vg_widget_add_child(parent_widget, child_widget);
            if (new_app && old_app != new_app)
                rt_gui_apply_default_font(child_widget);
            if (new_app)
                rt_gui_accessibility_platform_notify(new_app->window, new_app->root);
        }
    }
}

/// @brief Return the direct parent of a widget as an owned Option.
/// @details The payload, when present, is a borrowed lower-toolkit handle. Root, detached, stale,
///          NULL, and app handles all produce `None` without dereferencing invalid storage.
/// @param widget Widget whose parent should be queried.
/// @return Fresh owned `Zanna.Option<Widget>` object.
void *rt_widget_get_parent_option(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return rt_widget_borrowed_option(w ? w->parent : NULL);
}

/// @brief Return a widget's number of direct children.
/// @details The lower widget invariant keeps this count non-negative. A defensive clamp protects
///          the public contract if corrupted private state is encountered.
/// @param widget Parent widget to query.
/// @return Non-negative direct-child count, or zero for an invalid handle.
int64_t rt_widget_get_child_count(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w && w->child_count > 0 ? (int64_t)w->child_count : 0;
}

/// @brief Return one direct child by index as an owned Option.
/// @details Index conversion is checked before calling the lower API so a 64-bit public index can
///          never wrap the toolkit's `int` parameter. The successful payload remains borrowed.
/// @param widget Parent widget to query.
/// @param index Zero-based direct-child index.
/// @return Fresh `Some(child)` or `None` runtime object.
void *rt_widget_get_child_at_option(void *widget, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w || index < 0 || index > INT_MAX)
        return rt_option_none();
    return rt_widget_borrowed_option(vg_widget_get_child(w, (int)index));
}

/// @brief Detach one direct child while preserving the child subtree.
/// @details Runtime-owned click, drag, editor-helper, and subhandle references are invalidated
///          before the lower toolkit clears focus/capture and unlinks the child. The child remains
///          live and becomes caller-owned. Invalid or non-direct relationships are non-mutating.
/// @param parent Candidate direct parent widget.
/// @param child Candidate direct child widget.
/// @return 1 when detachment occurred, otherwise 0.
int64_t rt_widget_remove_child(void *parent, void *child) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_gui_widget_handle_checked(parent);
    vg_widget_t *child_widget = rt_gui_widget_handle_checked(child);
    if (!parent_widget || !child_widget || child_widget->parent != parent_widget)
        return 0;

    rt_gui_app_t *app = rt_gui_app_from_widget(parent_widget);
    rt_widget_forget_runtime_refs(app, child_widget);
    vg_widget_remove_child(parent_widget, child_widget);
    if (app)
        rt_gui_accessibility_platform_notify(app->window, app->root);
    return 1;
}

/// @brief Detach all direct children without destroying their subtrees.
/// @details The function first clears runtime-owned references for every subtree while app
///          ownership can still be resolved, then delegates the topology change to the lower
///          toolkit. Existing caller handles continue to designate live detached widgets.
/// @param parent Parent widget whose direct children should be detached.
void rt_widget_clear_children(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_gui_widget_handle_checked(parent);
    if (!parent_widget)
        return;

    rt_gui_app_t *app = rt_gui_app_from_widget(parent_widget);
    for (vg_widget_t *child = parent_widget->first_child; child; child = child->next_sibling)
        rt_widget_forget_runtime_refs(app, child);
    vg_widget_clear_children(parent_widget);
    if (app)
        rt_gui_accessibility_platform_notify(app->window, app->root);
}

/// @brief Copy a runtime UTF-8 lookup name into a widget.
/// @details Empty input clears the name. Embedded NUL input and conversion/allocation failure are
///          rejected before the lower atomic setter, preserving the previous name and revision.
/// @param widget Widget to rename.
/// @param name Runtime UTF-8 identifier text.
void rt_widget_set_name(void *widget, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    char *cname = rt_string_to_cstr_no_nul(name);
    if (!cname)
        return;
    vg_widget_set_name(w, cname);
    free(cname);
    rt_widget_notify_accessibility_tree(w);
}

/// @brief Return a widget's copied lookup name as an owned runtime string.
/// @param widget Widget to inspect.
/// @return Owned name, or an owned empty string when invalid or unnamed.
rt_string rt_widget_get_name(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    const char *name = w ? vg_widget_get_name(w) : NULL;
    return name ? rt_string_from_bytes(name, strlen(name)) : rt_str_empty();
}

/// @brief Return the stable ID assigned when a widget was initialized.
/// @details The internal unsigned counter is saturated to the signed public ABI rather than
///          wrapping negative. Zero remains reserved for invalid handles.
/// @param widget Widget to inspect.
/// @return Positive stable ID, `INT64_MAX` on theoretical unsigned overflow, or zero if invalid.
int64_t rt_widget_get_id(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return w->id > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)w->id;
}

/// @brief Find a positive public widget ID in a subtree and wrap the borrowed result.
/// @param root Live subtree root.
/// @param id Positive signed ID returned by @ref rt_widget_get_id.
/// @return Fresh `Some(widget)` when found, otherwise `None`.
void *rt_widget_find_by_id_option(void *root, int64_t id) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *root_widget = rt_gui_widget_handle_checked(root);
    if (!root_widget || id <= 0)
        return rt_option_none();
    return rt_widget_borrowed_option(vg_widget_find_by_id(root_widget, (uint64_t)id));
}

/// @brief Find the first exact UTF-8 widget name in a subtree.
/// @details Embedded NUL and empty names are rejected because lower lookup is C-string based and
///          empty names are normalized to unset by the setter. Conversion storage is always freed.
/// @param root Live subtree root.
/// @param name Case-sensitive runtime UTF-8 name.
/// @return Fresh `Some(widget)` when found, otherwise `None`.
void *rt_widget_find_by_name_option(void *root, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *root_widget = rt_gui_widget_handle_checked(root);
    if (!root_widget)
        return rt_option_none();
    char *cname = rt_string_to_cstr_no_nul(name);
    if (!cname || !cname[0]) {
        free(cname);
        return rt_option_none();
    }
    vg_widget_t *found = vg_widget_find_by_name(root_widget, cname);
    free(cname);
    return rt_widget_borrowed_option(found);
}

/// @brief Set uniform margin (external spacing) around a widget.
/// @details Margin is the space between this widget's outer edge and its
///          siblings or parent boundary. Applied equally on all four sides.
/// @param widget Widget to modify.
/// @param margin Margin in logical pixels.
void rt_widget_set_margin(void *widget, int64_t margin) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w)
        vg_widget_set_margin(w, rt_gui_logical_length_to_physical(w, (double)margin));
}

/// @brief Set uniform widget padding from one logical length.
/// @param widget Widget to modify; invalid handles are ignored.
/// @param padding Non-negative logical padding applied to every edge.
void rt_widget_set_padding(void *widget, double padding) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w)
        vg_widget_set_padding(w, rt_gui_logical_length_to_physical(w, padding));
}

/// @brief Set four independently converted logical padding edges.
/// @param widget Widget to modify; invalid handles are ignored.
/// @param left Logical left padding.
/// @param top Logical top padding.
/// @param right Logical right padding.
/// @param bottom Logical bottom padding.
void rt_widget_set_padding_edges(
    void *widget, double left, double top, double right, double bottom) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    vg_widget_set_paddings(w,
                           rt_gui_logical_length_to_physical(w, left),
                           rt_gui_logical_length_to_physical(w, top),
                           rt_gui_logical_length_to_physical(w, right),
                           rt_gui_logical_length_to_physical(w, bottom));
}

/// @brief Set four independently converted logical margin edges.
/// @param widget Widget to modify; invalid handles are ignored.
/// @param left Logical left margin.
/// @param top Logical top margin.
/// @param right Logical right margin.
/// @param bottom Logical bottom margin.
void rt_widget_set_margin_edges(
    void *widget, double left, double top, double right, double bottom) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    vg_widget_set_margins(w,
                          rt_gui_logical_length_to_physical(w, left),
                          rt_gui_logical_length_to_physical(w, top),
                          rt_gui_logical_length_to_physical(w, right),
                          rt_gui_logical_length_to_physical(w, bottom));
}

/// @brief Set the tab-order index for keyboard navigation.
/// @details Widgets with explicit tab indices (>= 0) are visited in ascending
///          order during Tab/Shift+Tab navigation. Widgets with index -1
///          (default) are visited in document order (depth-first traversal).
/// @param widget Widget to modify.
/// @param idx    Tab index (>= 0 for explicit ordering, -1 for default DFS).
void rt_widget_set_tab_index(void *widget, int64_t idx) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w)
        vg_widget_set_tab_index(w, rt_gui_clamp_i64_to_i32(idx, -1, INT32_MAX));
}

/// @brief Check whether the widget is currently visible.
/// @param widget Widget to query.
/// @return 1 if visible, 0 if hidden or NULL.
int64_t rt_widget_is_visible(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return w->visible ? 1 : 0;
}

/// @brief Check whether the widget is currently enabled for interaction.
/// @param widget Widget to query.
/// @return 1 if enabled, 0 if disabled or NULL.
int64_t rt_widget_is_enabled(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return w->enabled ? 1 : 0;
}

/// @brief Get the current laid-out width of the widget in physical pixels.
/// @param widget Widget to query.
/// @return Width in pixels, or 0 if NULL.
int64_t rt_widget_get_width(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (int64_t)w->width;
}

/// @brief Get the current laid-out height of the widget in physical pixels.
/// @param widget Widget to query.
/// @return Height in pixels, or 0 if NULL.
int64_t rt_widget_get_height(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (int64_t)w->height;
}

/// @brief Get the widget's X position relative to its parent.
/// @param widget Widget to query.
/// @return X offset in pixels, or 0 if NULL.
int64_t rt_widget_get_x(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (int64_t)w->x;
}

/// @brief Get the widget's Y position relative to its parent.
/// @param widget Widget to query.
/// @return Y offset in pixels, or 0 if NULL.
int64_t rt_widget_get_y(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (int64_t)w->y;
}

/// @brief Get the widget's flex-grow factor.
/// @param widget Widget to query.
/// @return Flex value, or 0.0 if NULL.
double rt_widget_get_flex(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0.0;
    return (double)w->layout.flex;
}

/// @brief Return parent-relative X converted from framebuffer to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Parent-relative logical X.
double rt_widget_get_logical_x(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->x) : 0.0;
}

/// @brief Return parent-relative Y converted from framebuffer to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Parent-relative logical Y.
double rt_widget_get_logical_y(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->y) : 0.0;
}

/// @brief Return arranged width converted from framebuffer to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical width.
double rt_widget_get_logical_width(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->width) : 0.0;
}

/// @brief Return arranged height converted from framebuffer to logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical height.
double rt_widget_get_logical_height(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? rt_gui_physical_to_logical(w, w->height) : 0.0;
}

/// @brief Query root-relative X and convert it to public logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Root-relative logical X.
double rt_widget_get_screen_x(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0.0;
    float x = 0.0f;
    vg_widget_get_screen_bounds(w, &x, NULL, NULL, NULL);
    return rt_gui_physical_to_logical(w, x);
}

/// @brief Query root-relative Y and convert it to public logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Root-relative logical Y.
double rt_widget_get_screen_y(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0.0;
    float y = 0.0f;
    vg_widget_get_screen_bounds(w, NULL, &y, NULL, NULL);
    return rt_gui_physical_to_logical(w, y);
}

/// @brief Query screen-space width and convert it to public logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical screen width.
double rt_widget_get_screen_width(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0.0;
    float width = 0.0f;
    vg_widget_get_screen_bounds(w, NULL, NULL, &width, NULL);
    return rt_gui_physical_to_logical(w, width);
}

/// @brief Query screen-space height and convert it to public logical units.
/// @param widget Widget to query; invalid handles return zero.
/// @return Non-negative logical screen height.
double rt_widget_get_screen_height(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0.0;
    float height = 0.0f;
    vg_widget_get_screen_bounds(w, NULL, NULL, NULL, &height);
    return rt_gui_physical_to_logical(w, height);
}

/// @brief Test a logical root-relative point against one widget's effective bounds.
/// @details Both coordinates are converted with the same effective scale as arranged storage.
///          The lower predicate includes visibility, enabled ancestry, and clipping checks.
/// @param widget Widget to test.
/// @param screen_x Logical root-relative X.
/// @param screen_y Logical root-relative Y.
/// @return 1 when inside, otherwise 0.
int64_t rt_widget_hit_test(void *widget, double screen_x, double screen_y) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    float physical_x = rt_gui_logical_coordinate_to_physical(w, screen_x);
    float physical_y = rt_gui_logical_coordinate_to_physical(w, screen_y);
    return vg_widget_contains_point(w, physical_x, physical_y) ? 1 : 0;
}

/// @brief Mark a live widget and its clipping ancestors for repaint.
/// @param widget Widget to invalidate; invalid handles are ignored.
void rt_widget_invalidate_paint(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w)
        vg_widget_invalidate(w);
}

/// @brief Mark a live widget subtree and its ancestors for layout and repaint.
/// @param widget Widget to invalidate; invalid handles are ignored.
void rt_widget_invalidate_layout(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w)
        vg_widget_invalidate_layout(w);
}

//=============================================================================
// Label Widget
//=============================================================================

/// @brief Create a new text label widget.
/// @details Creates a vg_label_t as a child of the given parent, sets its
///          initial text, and applies the app's default font. Labels are
///          read-only display widgets — they show static text and do not
///          accept user input or focus.
/// @param parent Parent container or app handle.
/// @param text   Initial display text (runtime string).
/// @return Opaque label widget handle, or NULL on failure.
void *rt_label_new(void *parent, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_label_t *label = vg_label_create(parent_widget, ctext ? ctext : "");
    free(ctext);
    rt_gui_apply_default_font((vg_widget_t *)label);
    return label;
}

/// @brief Update the display text of a label widget.
/// @details The vg layer copies the text internally, so the temporary C string
///          is freed immediately after the call.
/// @param label Label widget handle.
/// @param text  New text content (runtime string).
void rt_label_set_text(void *label, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (!lbl)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_label_set_text(lbl, ctext);
    free(ctext);
}

/// @brief Override the font and size used by a label widget.
/// @details Replaces the label's font with a user-provided font. The font
///          pointer is borrowed. Font.Destroy defers freeing while a live app
///          tree references the font; detached widgets still need an owner to
///          keep the font alive.
/// @param label Label widget handle.
/// @param font  Font handle from rt_font_load.
/// @param size  Font size in points.
void rt_label_set_font(void *label, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (!lbl)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_label_set_font(lbl, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    lbl->base.runtime_font_reference = checked_font;
}

/// @brief Set the text color of a label as a packed ARGB integer.
void rt_label_set_color(void *label, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (lbl)
        vg_label_set_color(lbl, (uint32_t)color);
}

/// @brief Enable or disable word wrapping on a label.
void rt_label_set_word_wrap(void *label, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (lbl)
        vg_label_set_word_wrap(lbl, enabled != 0);
}

/// @brief Set (or clear) a named scalable vector icon before a label's text (ADR 0137).
/// @details Unknown names are ignored; an empty name clears the icon. Rendered
///          on non-wrapped labels only.
void rt_label_set_icon_name(void *label, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (!lbl)
        return;
    char *cname = rt_string_to_gui_cstr(name);
    if (!cname || !cname[0]) {
        vg_label_set_vector_icon(lbl, -1);
        free(cname);
        return;
    }
    int32_t icon_id = vg_icon_vector_find(cname);
    free(cname);
    if (icon_id != VG_ICON_VECTOR_INVALID)
        vg_label_set_vector_icon(lbl, icon_id);
}

/// @brief Set a label's horizontal text alignment.
/// @details Accepted values are zero (left), one (center), and two (right). Invalid values and
///          invalid/stale handles are ignored without changing the label.
/// @param label Label widget handle.
/// @param alignment Horizontal alignment enum value.
void rt_label_set_alignment(void *label, int64_t alignment) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (!lbl || alignment < (int64_t)VG_ALIGN_H_LEFT || alignment > (int64_t)VG_ALIGN_H_RIGHT)
        return;
    vg_label_set_alignment(lbl, (vg_h_align_t)alignment, lbl->v_align);
}

/// @brief Return a label's horizontal text alignment.
/// @param label Label widget handle.
/// @return Zero (left), one (center), two (right), or -1 for an invalid handle.
int64_t rt_label_get_alignment(void *label) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    return lbl ? (int64_t)vg_label_get_alignment(lbl) : -1;
}

/// @brief Enable or disable ellipsis rendering for truncated label text.
/// @details Single-line text is fitted to the arranged width; wrapped text receives U+2026 on the
///          final line only when MaxLines omits source content.
/// @param label Label widget handle; invalid handles are ignored.
/// @param enabled Non-zero to enable ellipsis rendering.
void rt_label_set_ellipsis(void *label, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (lbl)
        vg_label_set_ellipsis(lbl, enabled != 0);
}

/// @brief Set the maximum number of visible wrapped lines.
/// @details Zero and negative values mean unlimited; larger values are clamped to INT_MAX.
/// @param label Label widget handle; invalid handles are ignored.
/// @param count Maximum line count or zero for unlimited.
void rt_label_set_max_lines(void *label, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (!lbl)
        return;
    int normalized = count <= 0 ? 0 : count > INT_MAX ? INT_MAX : (int)count;
    vg_label_set_max_lines(lbl, normalized);
}

/// @brief Enable or disable read-only label text selection.
/// @details Selectable labels support pointer drag, Shift-click, Ctrl/Cmd+A, Ctrl/Cmd+C, and
///          Escape. Disabling clears the selection and releases input capture.
/// @param label Label widget handle; invalid handles are ignored.
/// @param enabled Non-zero to make the label focusable and selectable.
void rt_label_set_selectable(void *label, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    if (lbl)
        vg_label_set_selectable(lbl, enabled != 0);
}

/// @brief Return the label's currently selected source text.
/// @details The toolkit exposes a borrowed byte slice; the runtime copies it into a fresh string.
///          Hidden text selected through Ctrl/Cmd+A remains part of the result.
/// @param label Label widget handle.
/// @return Fresh selected UTF-8 string, or empty when no selection exists or the handle is invalid.
rt_string rt_label_get_selected_text(void *label) {
    RT_ASSERT_MAIN_THREAD();
    vg_label_t *lbl = (vg_label_t *)rt_gui_widget_handle_checked_type(label, VG_WIDGET_LABEL);
    size_t length = 0;
    const char *selection = lbl ? vg_label_get_selected_text(lbl, &length) : NULL;
    return selection && length > 0 ? rt_string_from_bytes(selection, length) : rt_str_empty();
}

//=============================================================================
// Button Widget
//=============================================================================

/// @brief Create a new push button widget.
/// @details Creates a vg_button_t with the given label text, adds it as a child
///          of the parent container, and applies the app's default font. Buttons
///          support click detection (via rt_widget_was_clicked), optional icons,
///          and visual styles (primary, secondary, danger, etc.).
/// @param parent Parent container or app handle.
/// @param text   Button label text (runtime string).
/// @return Opaque button widget handle, or NULL on failure.
void *rt_button_new(void *parent, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_button_t *button = vg_button_create(parent_widget, rt_gui_cstr_or_empty(ctext));
    free(ctext);
    rt_gui_apply_default_font((vg_widget_t *)button);
    return button;
}

/// @brief Update the label text displayed on a button.
void rt_button_set_text(void *button, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (!btn)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_button_set_text(btn, ctext);
    free(ctext);
}

/// @brief Override the font and size used by a button widget.
/// @param button Button widget handle.
/// @param font   Font handle from rt_font_load (borrowed, not owned).
/// @param size   Font size in points.
void rt_button_set_font(void *button, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (!btn)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_button_set_font(btn, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    btn->base.runtime_font_reference = checked_font;
}

/// @brief Set the visual style preset for a button (primary, secondary, danger, etc.).
/// @details Button styles control the background/border/text color scheme. The
///          style enum maps to vg_button_style_t values.
/// @param button Button widget handle.
/// @param style  Style enum value (0 = default, 1 = primary, 2 = danger, etc.).
void rt_button_set_style(void *button, int64_t style) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (btn) {
        if (style < (int64_t)VG_BUTTON_STYLE_DEFAULT || style > (int64_t)VG_BUTTON_STYLE_ICON)
            style = (int64_t)VG_BUTTON_STYLE_DEFAULT;
        vg_button_set_style(btn, (vg_button_style_t)style);
    }
}

/// @brief Set a text/glyph icon to display alongside the button label.
/// @details The icon string is typically a single Unicode glyph (e.g., from an
///          icon font). The vg layer copies the string, so the temporary C
///          string is freed immediately. Use rt_button_set_icon_pos to control
///          whether the icon appears left or right of the label.
/// @param button Button widget handle.
/// @param icon   Icon glyph or text (runtime string).
void rt_button_set_icon(void *button, rt_string icon) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (!btn)
        return;
    char *cicon = rt_string_to_gui_cstr(icon);
    vg_button_set_icon(btn, cicon);
    free(cicon);
}

/// @brief Set (or clear) a named scalable vector icon on a button (ADR 0137).
/// @details Unknown names are ignored so callers can probe icon availability;
///          an empty name clears the current vector icon.
void rt_button_set_icon_name(void *button, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (!btn)
        return;
    char *cname = rt_string_to_gui_cstr(name);
    if (!cname || !cname[0]) {
        vg_button_set_vector_icon(btn, -1);
        free(cname);
        return;
    }
    int32_t icon_id = vg_icon_vector_find(cname);
    free(cname);
    if (icon_id != VG_ICON_VECTOR_INVALID)
        vg_button_set_vector_icon(btn, icon_id);
}

/// @brief Set the icon position relative to the button label.
/// @details 0 = icon on the left (default), 1 = icon on the right. The icon
///          is drawn with a 4 px gap from the label text.
void rt_button_set_icon_pos(void *button, int64_t pos) {
    RT_ASSERT_MAIN_THREAD();
    vg_button_t *btn = (vg_button_t *)rt_gui_widget_handle_checked_type(button, VG_WIDGET_BUTTON);
    if (btn)
        vg_button_set_icon_position(btn, pos == 1 ? 1 : 0);
}

//=============================================================================
// TextInput Widget
//=============================================================================

/// @brief Create a new single-line text input widget.
/// @details Creates a vg_textinput_t with an integrated undo/redo stack. The
///          initial empty string is pushed onto the undo stack at creation.
///          Each subsequent edit (insert/delete) pushes the new state. The
///          widget supports keyboard focus, cursor movement, text selection,
///          clipboard operations (Ctrl+C/V/X), and Tab-based focus traversal.
/// @param parent Parent container or app handle.
/// @return Opaque text input widget handle, or NULL on failure.
void *rt_textinput_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_textinput_t *input = vg_textinput_create(parent_widget);
    rt_gui_apply_default_font((vg_widget_t *)input);
    return input;
}

/// @brief Programmatically set the text content of a text input.
/// @details Replaces the entire content. Does not push to the undo stack
///          (programmatic changes are not undoable by the user).
/// @param input Text input widget handle.
/// @param text  New text content (runtime string).
void rt_textinput_set_text(void *input, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti =
        (vg_textinput_t *)rt_gui_widget_handle_checked_type(input, VG_WIDGET_TEXTINPUT);
    if (!ti)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_textinput_set_text(ti, ctext);
    free(ctext);
}

/// @brief Retrieve the current text content of a text input.
/// @details Returns a runtime string copy of the widget's internal buffer.
///          The returned string is GC-managed and safe to use from Zia code.
/// @param input Text input widget handle.
/// @return Current text as a runtime string, or empty string if NULL.
rt_string rt_textinput_get_text(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti =
        (vg_textinput_t *)rt_gui_widget_handle_checked_type(input, VG_WIDGET_TEXTINPUT);
    if (!ti)
        return rt_str_empty();
    const char *text = vg_textinput_get_text(ti);
    if (!text)
        return rt_str_empty();
    return rt_string_from_bytes(text, ti->text_len);
}

/// @brief Set the placeholder text shown when the input is empty.
/// @details The placeholder appears in a dimmed style and disappears when the
///          user starts typing. Useful for hinting at expected input format.
void rt_textinput_set_placeholder(void *input, rt_string placeholder) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti =
        (vg_textinput_t *)rt_gui_widget_handle_checked_type(input, VG_WIDGET_TEXTINPUT);
    if (!ti)
        return;
    char *ctext = rt_string_to_gui_cstr(placeholder);
    vg_textinput_set_placeholder(ti, ctext);
    free(ctext);
}

/// @brief Set the font of the textinput.
void rt_textinput_set_font(void *input, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti =
        (vg_textinput_t *)rt_gui_widget_handle_checked_type(input, VG_WIDGET_TEXTINPUT);
    if (!ti)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_textinput_set_font(ti, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    ti->base.runtime_font_reference = checked_font;
}

/// @brief Convert a signed runtime index into the lower editor's size type.
/// @details Negative values clamp to zero. Positive values that exceed SIZE_MAX on a 32-bit
///          target clamp to SIZE_MAX, allowing the lower grapheme API to perform its normal
///          content-length clamp without an integer truncation.
/// @param value Signed runtime integer supplied by a public GUI call.
/// @return Safely representable non-negative size value.
static size_t rt_textinput_size_from_i64(int64_t value) {
    if (value <= 0)
        return 0;
    const uint64_t unsigned_value = (uint64_t)value;
    if (unsigned_value > (uint64_t)SIZE_MAX)
        return SIZE_MAX;
    return (size_t)unsigned_value;
}

/// @brief Convert a lower editor size or revision to a runtime integer without wraparound.
/// @details The public runtime ABI uses signed 64-bit integers. Values above INT64_MAX saturate,
///          preserving monotonicity for revisions and avoiding implementation-defined casts.
/// @param value Non-negative value produced by the lower GUI editor.
/// @return Signed runtime representation, saturated at INT64_MAX.
static int64_t rt_textinput_i64_from_u64(uint64_t value) {
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

/// @brief Resolve and type-check a public TextInput handle.
/// @details Centralizing validation keeps every editor method consistent with retained-handle
///          lifetime rules: null, stale, and wrong-widget-type handles resolve to NULL.
/// @param input Candidate public widget handle.
/// @return Live lower TextInput pointer, or NULL when validation fails.
static vg_textinput_t *rt_textinput_checked(void *input) {
    return (vg_textinput_t *)rt_gui_widget_handle_checked_type(input, VG_WIDGET_TEXTINPUT);
}

/// @brief Configure the committed-text limit in Unicode grapheme clusters.
/// @details Zero removes the limit; negative runtime values clamp to zero. The lower editor
///          truncates an existing over-limit value only at a complete grapheme boundary.
/// @param input TextInput widget handle.
/// @param max_length Maximum user-perceived characters, or zero for unlimited.
void rt_textinput_set_max_length(void *input, int64_t max_length) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_set_max_length(ti, rt_textinput_size_from_i64(max_length));
}

/// @brief Read the configured committed-text grapheme limit.
/// @param input TextInput widget handle.
/// @return Maximum grapheme count, or zero for unlimited or an invalid handle.
int64_t rt_textinput_get_max_length(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_max_length(ti)) : 0;
}

/// @brief Enable or disable presentation-only password masking.
/// @details The lower editor retains committed text and history and paints one mask glyph per
///          extended grapheme cluster.
/// @param input TextInput widget handle.
/// @param password Non-zero to mask the rendered value.
void rt_textinput_set_password(void *input, int64_t password) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_set_password(ti, password != 0);
}

/// @brief Query password presentation state.
/// @param input TextInput widget handle.
/// @return One only when a live input has masking enabled.
int64_t rt_textinput_is_password(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_is_password(ti) ? 1 : 0;
}

/// @brief Enable or disable text mutation while preserving selection and copying.
/// @details Enabling the mode cancels any active IME composition and makes edit/history
///          operations return false until mutation is enabled again.
/// @param input TextInput widget handle.
/// @param read_only Non-zero to reject committed-text changes.
void rt_textinput_set_read_only(void *input, int64_t read_only) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_set_read_only(ti, read_only != 0);
}

/// @brief Query the editor's read-only state.
/// @param input TextInput widget handle.
/// @return One only when a live input rejects mutation.
int64_t rt_textinput_is_read_only(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_is_read_only(ti) ? 1 : 0;
}

/// @brief Select multiline editing or single-line submission behavior.
/// @details Disabling multiline mode asks the lower editor to remove existing line separators
///          deterministically and to resume Enter-as-submit behavior.
/// @param input TextInput widget handle.
/// @param multiline Non-zero for multiline editing.
void rt_textinput_set_multiline(void *input, int64_t multiline) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_set_multiline(ti, multiline != 0);
}

/// @brief Query whether the editor accepts multiple logical lines.
/// @param input TextInput widget handle.
/// @return One only for a valid multiline TextInput.
int64_t rt_textinput_is_multiline(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_is_multiline(ti) ? 1 : 0;
}

/// @brief Move the caret using the public Unicode-grapheme index contract.
/// @details The lower editor clamps out-of-range boundaries and never places the caret inside a
///          combining sequence, emoji ZWJ sequence, regional-indicator pair, or other cluster.
/// @param input TextInput widget handle.
/// @param grapheme_index Requested non-negative user-perceived-character boundary.
void rt_textinput_set_cursor(void *input, int64_t grapheme_index) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_set_cursor_grapheme(ti, rt_textinput_size_from_i64(grapheme_index));
}

/// @brief Return the current caret boundary in Unicode grapheme units.
/// @param input TextInput widget handle.
/// @return Zero-based grapheme boundary, or zero for an invalid handle.
int64_t rt_textinput_get_cursor(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_cursor_grapheme(ti)) : 0;
}

/// @brief Select a half-open range expressed in Unicode grapheme indices.
/// @details The requested direction is retained by the editor while ordered getters expose the
///          minimum inclusive and maximum exclusive endpoints.
/// @param input TextInput widget handle.
/// @param start Selection anchor, clamped to valid committed text.
/// @param end Selection caret, clamped to valid committed text.
void rt_textinput_select_range(void *input, int64_t start, int64_t end) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_select_graphemes(
            ti, rt_textinput_size_from_i64(start), rt_textinput_size_from_i64(end));
}

/// @brief Collapse the current selection at the caret without changing text.
/// @param input TextInput widget handle.
void rt_textinput_clear_selection(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (ti)
        vg_textinput_clear_selection(ti);
}

/// @brief Return the ordered inclusive selection start in grapheme units.
/// @param input TextInput widget handle.
/// @return Minimum endpoint, or zero for an invalid handle.
int64_t rt_textinput_get_selection_start(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_selection_start_grapheme(ti))
              : 0;
}

/// @brief Return the ordered exclusive selection end in grapheme units.
/// @param input TextInput widget handle.
/// @return Maximum endpoint, or zero for an invalid handle.
int64_t rt_textinput_get_selection_end(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_selection_end_grapheme(ti))
              : 0;
}

/// @brief Copy the currently selected UTF-8 bytes into a runtime string.
/// @details The lower allocation is consumed immediately after the managed runtime string is
///          created. Missing selection, allocation failure, and invalid handles return empty.
/// @param input TextInput widget handle.
/// @return Caller-owned runtime string containing the selection.
rt_string rt_textinput_get_selected_text(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (!ti)
        return rt_str_empty();
    char *selection = vg_textinput_get_selection(ti);
    if (!selection)
        return rt_str_empty();
    rt_string result = rt_string_from_bytes(selection, strlen(selection));
    free(selection);
    return result ? result : rt_str_empty();
}

/// @brief Insert a runtime string as one grapheme-safe undoable edit.
/// @details Embedded NUL bytes are converted to U+FFFD by the GUI-string bridge, preventing
///          silent truncation. The lower editor enforces read-only and max-length contracts.
/// @param input TextInput widget handle.
/// @param text Runtime string to insert at the caret or replace the selection.
/// @return One only when committed text changed.
int64_t rt_textinput_insert_text(void *input, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (!ti)
        return 0;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return 0;
    const bool changed = vg_textinput_insert_text(ti, ctext);
    free(ctext);
    return changed ? 1 : 0;
}

/// @brief Delete the active grapheme-safe selection as one undoable edit.
/// @param input TextInput widget handle.
/// @return One only when editable committed text was removed.
int64_t rt_textinput_delete_selection(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_delete_selection_checked(ti) ? 1 : 0;
}

/// @brief Restore the preceding committed-text snapshot.
/// @details Active preedit is cancelled first by the lower editor; no synthetic history entry is
///          appended while restoring the previous snapshot.
/// @param input TextInput widget handle.
/// @return One when committed text changed to an older snapshot.
int64_t rt_textinput_undo(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_undo(ti) ? 1 : 0;
}

/// @brief Reapply the next committed-text snapshot.
/// @details Active preedit is cancelled first and the restored value emits one independent text
///          change edge without creating another history record.
/// @param input TextInput widget handle.
/// @return One when committed text changed to a newer snapshot.
int64_t rt_textinput_redo(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_redo(ti) ? 1 : 0;
}

/// @brief Query whether an older committed-text snapshot is available.
/// @param input TextInput widget handle.
/// @return One when Undo can succeed; otherwise zero.
int64_t rt_textinput_can_undo(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_can_undo(ti) ? 1 : 0;
}

/// @brief Query whether a newer committed-text snapshot is available.
/// @param input TextInput widget handle.
/// @return One when Redo can succeed; otherwise zero.
int64_t rt_textinput_can_redo(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_can_redo(ti) ? 1 : 0;
}

/// @brief Consume the editor's committed-text change edge.
/// @details This state is independent of submission polling, so observing a change never clears
///          a pending Enter submission and vice versa.
/// @param input TextInput widget handle.
/// @return One once after one or more unobserved changes; otherwise zero.
int64_t rt_textinput_was_changed(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_was_changed(ti) ? 1 : 0;
}

/// @brief Consume the editor's single-line Enter-submission edge.
/// @details Submission polling is independent of committed-text change polling.
/// @param input TextInput widget handle.
/// @return One once after one or more unobserved submissions; otherwise zero.
int64_t rt_textinput_was_submitted(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_was_submitted(ti) ? 1 : 0;
}

/// @brief Return the non-consuming monotonic TextInput state revision.
/// @details Unsigned lower revisions above the runtime's signed range saturate at INT64_MAX,
///          preserving ordering rather than wrapping negative.
/// @param input TextInput widget handle.
/// @return Current editor revision, or zero for an invalid handle.
int64_t rt_textinput_get_revision(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64(vg_textinput_get_revision(ti)) : 0;
}

/// @brief Query whether uncommitted native IME preedit is active.
/// @param input TextInput widget handle.
/// @return One while the lower editor owns active composition state.
int64_t rt_textinput_is_composing(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti && vg_textinput_is_composing(ti) ? 1 : 0;
}

/// @brief Copy current uncommitted IME preedit into a runtime string.
/// @details Reading preedit does not commit text, mutate history, or consume any edge state.
/// @param input TextInput widget handle.
/// @return Caller-owned UTF-8 preedit string, or the canonical empty string if unavailable.
rt_string rt_textinput_get_composition_text(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    if (!ti)
        return rt_str_empty();
    const char *composition = vg_textinput_get_composition_text(ti);
    return composition ? rt_string_from_bytes(composition, strlen(composition)) : rt_str_empty();
}

/// @brief Return the committed-text insertion boundary for active IME preedit.
/// @param input TextInput widget handle.
/// @return Grapheme boundary, or zero for an invalid or inactive input.
int64_t rt_textinput_get_composition_start(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_composition_start(ti)) : 0;
}

/// @brief Return active IME preedit length in Unicode grapheme clusters.
/// @param input TextInput widget handle.
/// @return Number of user-perceived preedit characters, or zero when unavailable.
int64_t rt_textinput_get_composition_length(void *input) {
    RT_ASSERT_MAIN_THREAD();
    vg_textinput_t *ti = rt_textinput_checked(input);
    return ti ? rt_textinput_i64_from_u64((uint64_t)vg_textinput_get_composition_length(ti)) : 0;
}

//=============================================================================
// Checkbox Widget
//=============================================================================

/// @brief Create a new checkbox widget with a label.
/// @details Creates a vg_checkbox_t with the given label text. Checkboxes
///          toggle between checked and unchecked states when clicked. Use
///          rt_checkbox_is_checked to poll the current state each frame.
/// @param parent Parent container or app handle.
/// @param text   Label text displayed next to the checkbox (runtime string).
/// @return Opaque checkbox widget handle, or NULL on failure.
void *rt_checkbox_new(void *parent, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_checkbox_t *checkbox = vg_checkbox_create(parent_widget, ctext);
    free(ctext);
    rt_gui_apply_default_font((vg_widget_t *)checkbox);
    return checkbox;
}

/// @brief Programmatically set the checked state of a checkbox.
/// @param checkbox Checkbox widget handle.
/// @param checked  Non-zero to check, zero to uncheck.
void rt_checkbox_set_checked(void *checkbox, int64_t checked) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    if (cb)
        vg_checkbox_set_checked(cb, checked != 0);
}

/// @brief Query whether a checkbox is currently checked.
/// @param checkbox Checkbox widget handle.
/// @return 1 if checked, 0 if unchecked or NULL.
int64_t rt_checkbox_is_checked(void *checkbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    if (!cb)
        return 0;
    return vg_checkbox_is_checked(cb) ? 1 : 0;
}

/// @brief Update the label text displayed next to a checkbox.
/// @param checkbox Checkbox widget handle.
/// @param text     New label text (runtime string).
void rt_checkbox_set_text(void *checkbox, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    if (!cb)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_checkbox_set_text(cb, ctext);
    free(ctext);
}

/// @brief Set the indeterminate (mixed) state of a checkbox.
/// @details Indeterminate is a tri-state used when a checkbox represents a group
///          of items whose states are mixed (some checked, some not). Visually
///          the checkbox shows a dash or filled square rather than a tick mark.
///          Setting this clears the checked state so the control has one
///          unambiguous logical value.
/// @param checkbox      Checkbox widget handle.
/// @param indeterminate Non-zero to show mixed state, zero to clear it.
void rt_checkbox_set_indeterminate(void *checkbox, int64_t indeterminate) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    if (cb)
        vg_checkbox_set_indeterminate(cb, indeterminate != 0);
}

/// @brief Query whether the checkbox is in the indeterminate (mixed) state.
/// @param checkbox Checkbox widget handle.
/// @return 1 if indeterminate, 0 if determined (checked or unchecked) or NULL.
int64_t rt_checkbox_is_indeterminate(void *checkbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    if (!cb)
        return 0;
    return vg_checkbox_is_indeterminate(cb) ? 1 : 0;
}

/// @brief Consume the checkbox's common value-change edge.
/// @details Checked and indeterminate transitions share one independent edge;
///          consuming it does not change the widget's monotonic revision.
/// @param checkbox Checkbox widget handle.
/// @return 1 once after one or more unreported value transitions, otherwise 0.
int64_t rt_checkbox_was_changed(void *checkbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    return cb ? (vg_widget_was_changed(&cb->base) ? 1 : 0) : 0;
}

/// @brief Return the checkbox's non-consuming state revision.
/// @details The value can be observed independently by multiple pollers and is
///          unaffected by calls to rt_checkbox_was_changed.
/// @param checkbox Checkbox widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_checkbox_get_revision(void *checkbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_checkbox_t *cb =
        (vg_checkbox_t *)rt_gui_widget_handle_checked_type(checkbox, VG_WIDGET_CHECKBOX);
    return cb ? rt_widget_get_revision(&cb->base) : 0;
}

//=============================================================================
// ScrollView Widget
//=============================================================================

/// @brief Create a new scrollable container widget.
/// @details Creates a vg_scrollview_t that clips its content to its viewport
///          bounds and provides scrollbars when the content exceeds the
///          viewport. Children are added to the scroll view's internal
///          content container, not directly to the scroll view itself.
/// @param parent Parent container or app handle.
/// @return Opaque scroll view widget handle, or NULL on failure.
void *rt_scrollview_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_scrollview_create(parent_widget);
}

/// @brief Programmatically set the scroll position of a scroll view.
/// @details Scrolls the content to the specified (x, y) offset. Values are
///          clamped to [0, content_size - viewport_size] by the vg layout engine.
/// @param scroll Scroll view widget handle.
/// @param x      Horizontal scroll offset in pixels.
/// @param y      Vertical scroll offset in pixels.
void rt_scrollview_set_scroll(void *scroll, double x, double y) {
    RT_ASSERT_MAIN_THREAD();
    vg_scrollview_t *sv =
        (vg_scrollview_t *)rt_gui_widget_handle_checked_type(scroll, VG_WIDGET_SCROLLVIEW);
    if (!sv)
        return;
    vg_scrollview_set_scroll(sv,
                             rt_gui_sanitize_signed_float(x, RT_GUI_MAX_LAYOUT_VALUE),
                             rt_gui_sanitize_signed_float(y, RT_GUI_MAX_LAYOUT_VALUE));
}

/// @brief Set the total content size of a scroll view (determines scroll range).
/// @details The content size defines the virtual area that can be scrolled. If
///          the content is larger than the viewport, scrollbars appear. Set
///          this to match the actual size of the content you're displaying.
/// @param scroll Scroll view widget handle.
/// @param width  Total content width in pixels.
/// @param height Total content height in pixels.
void rt_scrollview_set_content_size(void *scroll, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    vg_scrollview_t *sv =
        (vg_scrollview_t *)rt_gui_widget_handle_checked_type(scroll, VG_WIDGET_SCROLLVIEW);
    if (!sv)
        return;
    vg_scrollview_set_content_size(
        sv,
        rt_gui_sanitize_nonnegative_float(width, RT_GUI_MAX_LAYOUT_VALUE),
        rt_gui_sanitize_nonnegative_float(height, RT_GUI_MAX_LAYOUT_VALUE));
}

/// @brief Scroll until one live descendant widget is fully visible.
/// @details The lower toolkit owns descendant geometry, scrollbar viewport
///          reduction, axis-specific adjustment, final clamping, and one
///          live-ID-guarded resolution after the next layout pass. Foreign,
///          stale, null, and wrong-type handles are inert.
/// @param scroll Scroll view widget handle.
/// @param widget Descendant widget handle to reveal.
void rt_scrollview_scroll_to(void *scroll, void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_scrollview_t *sv =
        (vg_scrollview_t *)rt_gui_widget_handle_checked_type(scroll, VG_WIDGET_SCROLLVIEW);
    vg_widget_t *descendant = rt_gui_widget_handle_checked(widget);
    if (!sv || !descendant)
        return;
    vg_scrollview_scroll_to_widget(sv, descendant);
}

/// @brief Get the current horizontal scroll offset.
double rt_scrollview_get_scroll_x(void *scroll) {
    RT_ASSERT_MAIN_THREAD();
    vg_scrollview_t *sv =
        (vg_scrollview_t *)rt_gui_widget_handle_checked_type(scroll, VG_WIDGET_SCROLLVIEW);
    if (!sv)
        return 0.0;
    float x = 0.0f, y = 0.0f;
    vg_scrollview_get_scroll(sv, &x, &y);
    return (double)x;
}

/// @brief Get the current vertical scroll offset.
double rt_scrollview_get_scroll_y(void *scroll) {
    RT_ASSERT_MAIN_THREAD();
    vg_scrollview_t *sv =
        (vg_scrollview_t *)rt_gui_widget_handle_checked_type(scroll, VG_WIDGET_SCROLLVIEW);
    if (!sv)
        return 0.0;
    float x = 0.0f, y = 0.0f;
    vg_scrollview_get_scroll(sv, &x, &y);
    return (double)y;
}

//=============================================================================
// TreeView Widget
//=============================================================================

/// @brief Create a new hierarchical tree view widget.
/// @details Creates a vg_treeview_t for displaying expandable/collapsible
///          tree structures (e.g., file browsers, scene graphs). Nodes can
///          be expanded, collapsed, selected, and carry user-data strings.
///          Selection changes are edge-triggered via rt_treeview_was_selection_changed.
/// @param parent Parent container or app handle.
/// @return Opaque tree view widget handle, or NULL on failure.
void *rt_treeview_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_treeview_t *tv = vg_treeview_create(parent_widget);
    if (tv)
        rt_gui_apply_default_font((vg_widget_t *)tv);
    return tv;
}

/// @brief Add a child node to the tree view (or to a parent node).
/// @details If parent_node is NULL, the node is added at the root level.
///          The text is copied by the vg layer. Returns the new node handle,
///          which can be used to add further children or set user data.
/// @param tree        Tree view widget handle.
/// @param parent_node Parent node handle, or NULL for root-level.
/// @param text        Node label text (runtime string).
/// @return New node handle, or NULL on failure.
void *rt_treeview_add_node(void *tree, void *parent_node, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return NULL;
    vg_tree_node_t *parent = parent_node ? rt_gui_tree_node_from_handle(parent_node) : NULL;
    if (parent_node && (!parent || parent->owner != tv))
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_tree_node_t *node = vg_treeview_add_node(tv, parent, ctext);
    free(ctext);
    return rt_gui_wrap_tree_node(node);
}

/// @brief Remove a node and its subtree from the tree view.
void rt_treeview_remove_node(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (tv && n && n->owner == tv) {
        vg_treeview_remove_node(tv, n);
        rt_gui_collect_retired_subhandles(&tv->base);
    }
}

/// @brief Remove all nodes from the tree view, leaving it empty.
void rt_treeview_clear(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (tv) {
        vg_treeview_clear(tv);
        rt_gui_collect_retired_subhandles(&tv->base);
    }
}

/// @brief Reclaim retired node tombstones after invalidating their managed wrappers.
/// @details Existing `Zanna.GUI.TreeView.Node` values remain valid managed objects, but their
///          targets are cleared before the lower toolkit frees tombstone storage. Subsequent calls
///          through a pruned node therefore return the established empty result without reading
///          reclaimed memory. Wrappers for nodes still present in the tree are preserved.
void rt_treeview_prune_retired_nodes(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (tv) {
        rt_gui_invalidate_retired_tree_node_subhandles(tv);
        vg_treeview_prune_retired_nodes(tv);
    }
}

/// @brief Expand a tree node to show its children.
void rt_treeview_expand(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (tv && n && n->owner == tv)
        vg_treeview_expand(tv, n);
}

/// @brief Collapse a tree node to hide its children.
void rt_treeview_collapse(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (tv && n && n->owner == tv)
        vg_treeview_collapse(tv, n);
}

/// @brief Toggle a live node owned by the supplied TreeView.
/// @details Foreign and stale subhandles are rejected before the lower toolkit is called, keeping
///          a node from one tree from mutating another tree's expansion and lazy-load state.
/// @param tree TreeView widget handle.
/// @param node Tree node handle owned by @p tree.
void rt_treeview_toggle(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (tv && n && n->owner == tv)
        vg_treeview_toggle(tv, n);
}

/// @brief Programmatically select a tree node (NULL to clear selection).
void rt_treeview_select(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return;
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (node && (!n || n->owner != tv))
        return;
    vg_treeview_select(tv, n);
}

/// @brief Enable or disable retained-node Ctrl/Command and Shift multi-selection.
void rt_treeview_set_multi_select(void *tree, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (tv)
        vg_treeview_set_multi_select(tv, enabled != 0);
}

/// @brief Bring a live node owned by the supplied TreeView into view.
/// @param tree TreeView widget handle.
/// @param node Tree node handle owned by @p tree.
void rt_treeview_scroll_to(void *tree, void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (tv && n && n->owner == tv)
        vg_treeview_scroll_to(tv, n);
}

/// @brief Set the font of the treeview.
void rt_treeview_set_font(void *tree, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_treeview_set_font(tv, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    tv->base.runtime_font_reference = checked_font;
}

/// @brief Currently-selected tree node handle (NULL if none / null tree).
void *rt_treeview_get_selected(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return NULL;
    return rt_gui_wrap_tree_node(tv->selected);
}

/// @brief Tree node under a window-space point (NULL if outside rows).
void *rt_treeview_get_node_at(void *tree, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return NULL;
    return rt_gui_wrap_tree_node(vg_treeview_node_at(tv, (float)x, (float)y));
}

/// @brief Extract a tree node's runtime string data, or empty string.
static rt_string rt_treeview_node_data_string(vg_tree_node_t *n) {
    if (!n || !n->user_data || !n->owns_user_data)
        return rt_str_empty();
    return rt_gui_string_data_to_rt_string(n->user_data);
}

/// @brief Return byte-exact data for every selected retained node in complete preorder.
void *rt_treeview_get_selected_data(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    void *result = rt_seq_new_owned();
    if (!result)
        return NULL;

    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv || tv->virtual_mode || !tv->root)
        return result;

    vg_tree_node_t *node = tv->root->first_child;
    while (node) {
        if (node->selected) {
            rt_string data = rt_treeview_node_data_string(node);
            rt_seq_push(result, data);
            rt_str_release_maybe(data);
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != tv->root && !node->next_sibling)
            node = node->parent;
        if (!node || node == tv->root)
            break;
        node = node->next_sibling;
    }
    return result;
}

/// @brief `TreeView.SetDragDropEnabled` — enable poll-model drag-and-drop.
void rt_treeview_set_drag_drop_enabled(void *tree, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return;
    vg_treeview_set_app_directed_dnd(tv, enabled != 0);
}

/// @brief Select disabled, legacy container-only, or row-aware poll-model drag-and-drop.
void rt_treeview_set_drag_drop_mode(void *tree, int64_t mode) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv || mode < (int64_t)VG_TREEVIEW_APP_DND_DISABLED ||
        mode > (int64_t)VG_TREEVIEW_APP_DND_ROW_AWARE)
        return;
    vg_treeview_set_app_directed_dnd_mode(tv, (int)mode);
}

/// @brief `TreeView.WasDropReceived` — true while a completed drop is pending.
int64_t rt_treeview_was_drop_received(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return (tv && vg_treeview_has_pending_drop(tv)) ? 1 : 0;
}

/// @brief `TreeView.GetDropSourceData` — data string of the dragged node.
rt_string rt_treeview_get_drop_source_data(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return rt_str_empty();
    return rt_treeview_node_data_string(vg_treeview_drop_source(tv));
}

/// @brief `TreeView.GetDropTargetData` — data string of the target node.
rt_string rt_treeview_get_drop_target_data(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return rt_str_empty();
    return rt_treeview_node_data_string(vg_treeview_drop_target_node(tv));
}

/// @brief `TreeView.GetDropPosition` — 0=before, 1=into, 2=after.
int64_t rt_treeview_get_drop_position(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return 1;
    return (int64_t)vg_treeview_drop_position_value(tv);
}

/// @brief `TreeView.ClearDrop` — consume the latched drop.
void rt_treeview_clear_drop(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return;
    vg_treeview_clear_drop(tv);
}

/// @brief Check if the tree view selection changed since the last call (edge-triggered).
int64_t rt_treeview_was_selection_changed(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (!tv)
        return 0;

    if (tv->selection_revision != tv->reported_selection_revision) {
        tv->reported_selection_revision = tv->selection_revision;
        tv->prev_selected = tv->selected;
        return 1;
    }
    return 0;
}

/// @brief Consume the TreeView's common selection-change edge.
/// @details This edge is intentionally independent from the legacy
///          rt_treeview_was_selection_changed edge so either API can be polled
///          without hiding a transition from the other.
/// @param tree TreeView widget handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_treeview_was_changed(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv ? (vg_widget_was_changed(&tv->base) ? 1 : 0) : 0;
}

/// @brief Consume the TreeView's node-activation edge.
/// @details Double-clicking a node or pressing Enter with a selected node
///          records this event independently from selection changes.
/// @param tree TreeView widget handle.
/// @return 1 once after one or more unreported activations, otherwise 0.
int64_t rt_treeview_was_activated(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv ? (vg_widget_was_activated(&tv->base) ? 1 : 0) : 0;
}

/// @brief Consume the TreeView's independent lazy-child-request edge.
/// @param tree TreeView widget handle.
/// @return 1 once after one or more unreported requests, otherwise 0.
int64_t rt_treeview_was_load_children_requested(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv && vg_treeview_was_load_children_requested(tv) ? 1 : 0;
}

/// @brief Wrap a borrowed live tree node in a fresh owned `Zanna.Option`.
/// @details The subhandle registry preserves one managed identity per lower node. The Option
///          retains that managed wrapper while it is alive and `None` represents absence or a
///          failed/stale wrapper lookup.
/// @param node Borrowed candidate node.
/// @return Owned Option containing the node handle, or None.
static void *rt_treeview_node_option(vg_tree_node_t *node) {
    if (!vg_tree_node_is_live(node))
        return rt_option_none();
    void *handle = rt_gui_wrap_tree_node(node);
    return handle ? rt_option_some(handle) : rt_option_none();
}

/// @brief Return the most recent lazy-child request target without consuming its edge.
/// @param tree TreeView widget handle.
/// @return Owned Some(TreeView.Node), or None when unavailable.
void *rt_treeview_get_load_requested_node_option(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return rt_treeview_node_option(tv ? vg_treeview_get_load_requested_node(tv) : NULL);
}

/// @brief Return the most recently activated node without consuming the activation edge.
/// @param tree TreeView widget handle.
/// @return Owned Some(TreeView.Node), or None when unavailable.
void *rt_treeview_get_activated_node_option(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return rt_treeview_node_option(tv ? vg_treeview_get_activated_node(tv) : NULL);
}

/// @brief `TreeView.BeginEditNode` — start an inline edit of one row.
/// @details The row is revealed and overlaid with a focused single-line
///          editor pre-filled with @p initial_text. Enter and focus loss
///          commit; Escape cancels. Virtual-mode trees decline.
/// @param tree TreeView widget handle.
/// @param node Live node handle owned by @p tree.
/// @param initial_text Editor contents at start.
/// @return 1 when the edit began, otherwise 0.
int64_t rt_treeview_begin_edit_node(void *tree, void *node, rt_string initial_text) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (!tv || !n || n->owner != tv)
        return 0;
    char *ctext = rt_string_to_gui_cstr(initial_text);
    bool began = vg_treeview_begin_edit(tv, n, ctext ? ctext : "");
    free(ctext);
    return began ? 1 : 0;
}

/// @brief `TreeView.IsEditing` — whether an inline row edit is in progress.
/// @param tree TreeView widget handle.
/// @return 1 while editing, otherwise 0.
int64_t rt_treeview_is_editing(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv && vg_treeview_is_editing(tv) ? 1 : 0;
}

/// @brief `TreeView.WasEditCommitted` — consume the inline-edit commit edge.
/// @param tree TreeView widget handle.
/// @return 1 exactly once per committed edit, otherwise 0.
int64_t rt_treeview_was_edit_committed(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv && vg_treeview_was_edit_committed(tv) ? 1 : 0;
}

/// @brief `TreeView.GetEditText` — most recently committed inline-edit text.
/// @param tree TreeView widget handle.
/// @return Owned runtime string; empty when nothing has committed.
rt_string rt_treeview_get_edit_text(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    const char *text = tv ? vg_treeview_get_edit_text(tv) : NULL;
    return text ? rt_string_from_bytes(text, strlen(text)) : rt_str_empty();
}

/// @brief `TreeView.GetEditedNodeOption` — node whose edit most recently committed.
/// @param tree TreeView widget handle.
/// @return Owned Some(TreeView.Node), or None when unavailable.
void *rt_treeview_get_edited_node_option(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return rt_treeview_node_option(tv ? vg_treeview_get_edited_node(tv) : NULL);
}

/// @brief `TreeView.CancelEdit` — cancel any inline edit without committing.
/// @param tree TreeView widget handle.
void rt_treeview_cancel_edit(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    if (tv)
        vg_treeview_cancel_edit(tv);
}

/// @brief Return the TreeView's non-consuming state revision.
/// @param tree TreeView widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_treeview_get_revision(void *tree) {
    RT_ASSERT_MAIN_THREAD();
    vg_treeview_t *tv =
        (vg_treeview_t *)rt_gui_widget_handle_checked_type(tree, VG_WIDGET_TREEVIEW);
    return tv ? rt_widget_get_revision(&tv->base) : 0;
}

/// @brief Get the display text of a tree node.
rt_string rt_treeview_node_get_text(void *node) {
    RT_ASSERT_MAIN_THREAD();
    if (!node)
        return rt_str_empty();
    vg_tree_node_t *n = rt_gui_tree_node_from_handle(node);
    if (!n)
        return rt_str_empty();
    if (!n->text)
        return rt_str_empty();
    return rt_string_from_bytes(n->text, n->text_len);
}

/// @brief Replace a tree node's visible text using allocation-safe GUI text conversion.
/// @param node Tree node managed subhandle.
/// @param text New runtime string; NULL becomes empty and embedded NUL becomes U+FFFD.
void rt_treeview_node_set_text(void *node, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (!n)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    (void)vg_tree_node_set_text(n, ctext);
    free(ctext);
}

/// @brief Replace a tree node's UTF-8 icon text.
/// @param node Tree node managed subhandle.
/// @param icon Icon glyph or short text; NULL/empty clears it.
void rt_treeview_node_set_icon(void *node, rt_string icon) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (!n)
        return;
    char *cicon = rt_string_to_gui_cstr(icon);
    if (!cicon)
        return;
    (void)vg_tree_node_set_icon_text(n, cicon);
    free(cicon);
}

/// @brief Return a tree node's copied UTF-8 icon text, or empty when absent.
rt_string rt_treeview_node_get_icon(void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    const char *icon = n ? vg_tree_node_get_icon_text(n) : NULL;
    return icon ? rt_string_from_bytes(icon, n->icon_text_len) : rt_str_empty();
}

/// @brief Set the node's materialized/lazy-child affordance.
void rt_treeview_node_set_has_children(void *node, int64_t has_children) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (n)
        vg_tree_node_set_has_children(n, has_children != 0);
}

/// @brief Return whether the node has real or advertised children.
int64_t rt_treeview_node_has_children(void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    return n && vg_tree_node_has_children(n) ? 1 : 0;
}

/// @brief Set or clear a node's asynchronous loading indicator.
void rt_treeview_node_set_loading(void *node, int64_t loading) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (n)
        vg_tree_node_set_loading(n, loading != 0);
}

/// @brief Return whether a node's loading indicator is active.
int64_t rt_treeview_node_is_loading(void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    return n && vg_tree_node_is_loading(n) ? 1 : 0;
}

/// @brief Replace a node's stable identifier after rejecting embedded NUL bytes.
void rt_treeview_node_set_stable_id(void *node, rt_string stable_id) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    if (!n)
        return;
    if (!stable_id) {
        (void)vg_tree_node_set_stable_id(n, "");
        return;
    }
    char *cid = rt_string_to_cstr_no_nul(stable_id);
    if (!cid)
        return;
    (void)vg_tree_node_set_stable_id(n, cid);
    free(cid);
}

/// @brief Return a node's copied stable identifier, or empty when absent.
rt_string rt_treeview_node_get_stable_id(void *node) {
    RT_ASSERT_MAIN_THREAD();
    vg_tree_node_t *n = node ? rt_gui_tree_node_from_handle(node) : NULL;
    const char *stable_id = n ? vg_tree_node_get_stable_id(n) : NULL;
    return stable_id ? rt_string_from_bytes(stable_id, n->stable_id_len) : rt_str_empty();
}

/// @brief Attach arbitrary string data to a tree node (replaces any previous data).
void rt_treeview_node_set_data(void *node, rt_string data) {
    RT_ASSERT_MAIN_THREAD();
    if (!node)
        return;
    vg_tree_node_t *n = rt_gui_tree_node_from_handle(node);
    if (!n)
        return;
    rt_gui_string_data_t *new_data = data ? rt_gui_string_data_new(data) : NULL;
    if (data && !new_data)
        return;
    // Free old data if it exists and is owned by the runtime string wrapper.
    if (n->owns_user_data && n->user_data)
        rt_gui_string_data_free_if_owned(n->user_data);
    // Store length-tagged data so embedded NUL bytes round-trip correctly.
    n->user_data = new_data;
    n->owns_user_data = new_data != NULL;
}

/// @brief Retrieve the string data previously attached to a tree node.
rt_string rt_treeview_node_get_data(void *node) {
    RT_ASSERT_MAIN_THREAD();
    if (!node)
        return rt_str_empty();
    vg_tree_node_t *n = rt_gui_tree_node_from_handle(node);
    if (!n)
        return rt_str_empty();
    if (!n->user_data || !n->owns_user_data)
        return rt_str_empty();
    return rt_gui_string_data_to_rt_string(n->user_data);
}

/// @brief Check whether a tree node is currently in the expanded state.
int64_t rt_treeview_node_is_expanded(void *node) {
    RT_ASSERT_MAIN_THREAD();
    if (!node)
        return 0;
    vg_tree_node_t *n = rt_gui_tree_node_from_handle(node);
    if (!n)
        return 0;
    return n->expanded ? 1 : 0;
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics disabled — widgets cannot have an owning GUI app.
void *rt_gui_widget_owner_app(void *handle) {
    (void)handle;
    return NULL;
}

// ===========================================================================
// Headless stubs — same prototypes as the real implementations above so
// non-graphical builds (server / CLI) link without pulling in
// the GUI subsystem. Each stub no-ops or returns a sentinel; doc comments
// inherit from the real impls above by virtue of identical names.
// ===========================================================================

/// @brief Stub: graphics-disabled builds cannot validate or bind lower GUI widgets.
void *rt_gui_widget_checked_for_binding(void *handle, int64_t widget_type) {
    (void)handle;
    (void)widget_type;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no font data is loaded.
void *rt_font_load(rt_string path) {
    (void)path;
    return NULL;
}

/// @brief Stub: graphics-disabled builds return an explicit capability failure.
/// @param size Ignored logical-point size.
/// @return Caller-owned Result.ErrStr containing the stable GUI-unavailable reason.
void *rt_font_load_system_ui(double size) {
    (void)size;
    return rt_result_err_str(rt_const_cstr("GUI support is not available in this build"));
}

/// @brief Stub: graphics-disabled builds return an explicit capability failure.
/// @param size Ignored logical-point size.
/// @return Caller-owned Result.ErrStr containing the stable GUI-unavailable reason.
void *rt_font_load_system_ui_bold(double size) {
    (void)size;
    return rt_result_err_str(rt_const_cstr("GUI support is not available in this build"));
}

/// @brief Stub: graphics-disabled builds have no live GUI Font metadata.
/// @param font Ignored opaque handle.
/// @return Always zero.
double rt_font_get_logical_size(void *font) {
    (void)font;
    return 0.0;
}

/// @brief Release resources and destroy the font.
void rt_font_destroy(void *font) {
    (void)font;
}

/// @brief Release resources and destroy the widget.
void rt_widget_destroy(void *widget) {
    (void)widget;
}

/// @brief Show or hide a widget.
void rt_widget_set_visible(void *widget, int64_t visible) {
    (void)widget;
    (void)visible;
}

/// @brief Enable or disable user interaction with a widget.
void rt_widget_set_enabled(void *widget, int64_t enabled) {
    (void)widget;
    (void)enabled;
}

/// @brief Set a fixed width and height on the widget.
void rt_widget_set_size(void *widget, int64_t width, int64_t height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Graphics-disabled preferred-size compatibility stub.
/// @details No widget exists in a headless runtime, so the logical constraint is ignored.
/// @param widget Ignored widget handle.
/// @param width Ignored logical width.
/// @param height Ignored logical height.
void rt_widget_set_preferred_size(void *widget, double width, double height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Graphics-disabled maximum-size compatibility stub.
/// @details No widget exists in a headless runtime, so the logical constraint is ignored.
/// @param widget Ignored widget handle.
/// @param width Ignored logical width.
/// @param height Ignored logical height.
void rt_widget_set_max_size(void *widget, double width, double height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Graphics-disabled minimum-size stub.
/// @details Preserves the complete C ABI while deliberately performing no GUI mutation.
/// @param widget Ignored widget handle.
/// @param width Ignored logical width.
/// @param height Ignored logical height.
void rt_widget_set_min_size(void *widget, double width, double height) {
    (void)widget;
    (void)width;
    (void)height;
}

/// @brief Return the absent minimum width in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero logical units.
double rt_widget_get_min_width(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return the absent minimum height in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero logical units.
double rt_widget_get_min_height(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Set the flex-grow factor for a widget.
void rt_widget_set_flex(void *widget, double flex) {
    (void)widget;
    (void)flex;
}

/// @brief Add a child widget to a parent container.
void rt_widget_add_child(void *parent, void *child) {
    (void)parent;
    (void)child;
}

/// @brief Return no parent in a graphics-disabled runtime.
/// @details The API returns an owned `None`, preserving explicit absence rather than a NULL
///          runtime object even when graphics support is unavailable.
/// @param widget Ignored widget handle.
/// @return Fresh owned `Zanna.Option.None` object.
void *rt_widget_get_parent_option(void *widget) {
    (void)widget;
    return rt_option_none();
}

/// @brief Return the absent direct-child count in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
int64_t rt_widget_get_child_count(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Return no indexed child in a graphics-disabled runtime.
/// @details Both the widget and index are ignored; explicit `None` matches enabled-build absence.
/// @param widget Ignored widget handle.
/// @param index Ignored child index.
/// @return Fresh owned `Zanna.Option.None` object.
void *rt_widget_get_child_at_option(void *widget, int64_t index) {
    (void)widget;
    (void)index;
    return rt_option_none();
}

/// @brief Reject child detachment in a graphics-disabled runtime.
/// @param parent Ignored parent handle.
/// @param child Ignored child handle.
/// @return Always zero because no GUI tree exists.
int64_t rt_widget_remove_child(void *parent, void *child) {
    (void)parent;
    (void)child;
    return 0;
}

/// @brief Graphics-disabled clear-children stub.
/// @details Preserves the full ABI without inventing a headless widget tree.
/// @param parent Ignored parent handle.
void rt_widget_clear_children(void *parent) {
    (void)parent;
}

/// @brief Graphics-disabled widget-name setter stub.
/// @details The value is ignored because no widget identity record exists.
/// @param widget Ignored widget handle.
/// @param name Ignored runtime string.
void rt_widget_set_name(void *widget, rt_string name) {
    (void)widget;
    (void)name;
}

/// @brief Return an owned empty widget name in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Owned empty runtime string.
rt_string rt_widget_get_name(void *widget) {
    (void)widget;
    return rt_str_empty();
}

/// @brief Return the reserved invalid widget ID in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
int64_t rt_widget_get_id(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Return no ID match in a graphics-disabled runtime.
/// @param root Ignored root handle.
/// @param id Ignored widget ID.
/// @return Fresh owned `Zanna.Option.None` object.
void *rt_widget_find_by_id_option(void *root, int64_t id) {
    (void)root;
    (void)id;
    return rt_option_none();
}

/// @brief Return no name match in a graphics-disabled runtime.
/// @param root Ignored root handle.
/// @param name Ignored lookup name.
/// @return Fresh owned `Zanna.Option.None` object.
void *rt_widget_find_by_name_option(void *root, rt_string name) {
    (void)root;
    (void)name;
    return rt_option_none();
}

/// @brief Set the margin of the widget.
void rt_widget_set_margin(void *widget, int64_t margin) {
    (void)widget;
    (void)margin;
}

/// @brief Graphics-disabled uniform-padding stub.
/// @param widget Ignored widget handle.
/// @param padding Ignored logical padding.
void rt_widget_set_padding(void *widget, double padding) {
    (void)widget;
    (void)padding;
}

/// @brief Graphics-disabled per-edge padding stub.
/// @details All values are ignored while retaining the enabled build's exact ABI.
/// @param widget Ignored widget handle.
/// @param left Ignored logical left padding.
/// @param top Ignored logical top padding.
/// @param right Ignored logical right padding.
/// @param bottom Ignored logical bottom padding.
void rt_widget_set_padding_edges(
    void *widget, double left, double top, double right, double bottom) {
    (void)widget;
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
}

/// @brief Graphics-disabled per-edge margin stub.
/// @details All values are ignored while retaining the enabled build's exact ABI.
/// @param widget Ignored widget handle.
/// @param left Ignored logical left margin.
/// @param top Ignored logical top margin.
/// @param right Ignored logical right margin.
/// @param bottom Ignored logical bottom margin.
void rt_widget_set_margin_edges(
    void *widget, double left, double top, double right, double bottom) {
    (void)widget;
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
}

/// @brief Set the tab index of the widget.
void rt_widget_set_tab_index(void *widget, int64_t idx) {
    (void)widget;
    (void)idx;
}

/// @brief Check whether the widget is currently visible.
int64_t rt_widget_is_visible(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Check whether the widget is currently enabled.
int64_t rt_widget_is_enabled(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the width of the widget.
int64_t rt_widget_get_width(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the height of the widget.
int64_t rt_widget_get_height(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the x of the widget.
int64_t rt_widget_get_x(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the y of the widget.
int64_t rt_widget_get_y(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the flex of the widget.
double rt_widget_get_flex(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent logical X geometry in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_logical_x(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent logical Y geometry in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_logical_y(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent logical width in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_logical_width(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent logical height in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_logical_height(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent root-relative logical X in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_screen_x(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent root-relative logical Y in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_screen_y(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent root-relative logical width in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_screen_width(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Return absent root-relative logical height in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @return Always zero.
double rt_widget_get_screen_height(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Reject hit testing in a graphics-disabled runtime.
/// @param widget Ignored widget handle.
/// @param screen_x Ignored logical X coordinate.
/// @param screen_y Ignored logical Y coordinate.
/// @return Always zero.
int64_t rt_widget_hit_test(void *widget, double screen_x, double screen_y) {
    (void)widget;
    (void)screen_x;
    (void)screen_y;
    return 0;
}

/// @brief Graphics-disabled repaint invalidation stub.
/// @details No frame scheduler or widget tree exists, so invalidation is a deliberate no-op.
/// @param widget Ignored widget handle.
void rt_widget_invalidate_paint(void *widget) {
    (void)widget;
}

/// @brief Graphics-disabled layout invalidation stub.
/// @details No frame scheduler or widget tree exists, so invalidation is a deliberate no-op.
/// @param widget Ignored widget handle.
void rt_widget_invalidate_layout(void *widget) {
    (void)widget;
}

/// @brief Stub: graphics disabled — returns NULL; no label widget is created.
void *rt_label_new(void *parent, rt_string text) {
    (void)parent;
    (void)text;
    return NULL;
}

/// @brief Set the text of the label.
void rt_label_set_text(void *label, rt_string text) {
    (void)label;
    (void)text;
}

/// @brief Set the font of the label.
void rt_label_set_font(void *label, void *font, double size) {
    (void)label;
    (void)font;
    (void)size;
}

/// @brief Set the color of the label.
void rt_label_set_color(void *label, int64_t color) {
    (void)label;
    (void)color;
}

/// @brief Set word wrap stub (graphics disabled).
void rt_label_set_word_wrap(void *label, int64_t enabled) {
    (void)label;
    (void)enabled;
}

/// @brief Graphics-disabled label vector-icon setter stub.
void rt_label_set_icon_name(void *label, rt_string name) {
    (void)label;
    (void)name;
}

/// @brief Graphics-disabled button vector-icon setter stub.
void rt_button_set_icon_name(void *button, rt_string name) {
    (void)button;
    (void)name;
}

/// @brief Graphics-disabled label alignment setter stub.
/// @param label Ignored handle.
/// @param alignment Ignored alignment.
void rt_label_set_alignment(void *label, int64_t alignment) {
    (void)label;
    (void)alignment;
}

/// @brief Graphics-disabled label alignment query stub.
/// @param label Ignored handle.
/// @return Always -1 because no label exists.
int64_t rt_label_get_alignment(void *label) {
    (void)label;
    return -1;
}

/// @brief Graphics-disabled label ellipsis setter stub.
/// @param label Ignored handle.
/// @param enabled Ignored state.
void rt_label_set_ellipsis(void *label, int64_t enabled) {
    (void)label;
    (void)enabled;
}

/// @brief Graphics-disabled label line-limit setter stub.
/// @param label Ignored handle.
/// @param count Ignored line count.
void rt_label_set_max_lines(void *label, int64_t count) {
    (void)label;
    (void)count;
}

/// @brief Graphics-disabled label selectability setter stub.
/// @param label Ignored handle.
/// @param enabled Ignored state.
void rt_label_set_selectable(void *label, int64_t enabled) {
    (void)label;
    (void)enabled;
}

/// @brief Graphics-disabled selected-label-text query stub.
/// @param label Ignored handle.
/// @return Empty runtime string.
rt_string rt_label_get_selected_text(void *label) {
    (void)label;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — returns NULL; no button widget is created.
void *rt_button_new(void *parent, rt_string text) {
    (void)parent;
    (void)text;
    return NULL;
}

/// @brief Set the text of the button.
void rt_button_set_text(void *button, rt_string text) {
    (void)button;
    (void)text;
}

/// @brief Set the font of the button.
void rt_button_set_font(void *button, void *font, double size) {
    (void)button;
    (void)font;
    (void)size;
}

/// @brief Set the style of the button.
void rt_button_set_style(void *button, int64_t style) {
    (void)button;
    (void)style;
}

/// @brief Set the icon of the button.
void rt_button_set_icon(void *button, rt_string icon) {
    (void)button;
    (void)icon;
}

/// @brief Set the icon pos of the button.
void rt_button_set_icon_pos(void *button, int64_t pos) {
    (void)button;
    (void)pos;
}

/// @brief Stub: graphics disabled — returns NULL; no text input widget is created.
void *rt_textinput_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Set the text of the textinput.
void rt_textinput_set_text(void *input, rt_string text) {
    (void)input;
    (void)text;
}

/// @brief Get the text of the textinput.
rt_string rt_textinput_get_text(void *input) {
    (void)input;
    return rt_str_empty();
}

/// @brief Set the placeholder of the textinput.
void rt_textinput_set_placeholder(void *input, rt_string placeholder) {
    (void)input;
    (void)placeholder;
}

/// @brief Set the font of the textinput.
void rt_textinput_set_font(void *input, void *font, double size) {
    (void)input;
    (void)font;
    (void)size;
}

/// @brief Stub: ignore a grapheme-count limit when graphics support is disabled.
/// @details Preserves the public ABI without constructing editor state.
/// @param input Ignored TextInput handle.
/// @param max_length Ignored maximum grapheme count.
void rt_textinput_set_max_length(void *input, int64_t max_length) {
    (void)input;
    (void)max_length;
}

/// @brief Stub: report no configured text limit when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero, which is the public unlimited/invalid sentinel.
int64_t rt_textinput_get_max_length(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: ignore password-presentation changes when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param password Ignored masking state.
void rt_textinput_set_password(void *input, int64_t password) {
    (void)input;
    (void)password;
}

/// @brief Stub: report password masking disabled when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_is_password(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: ignore read-only changes when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param read_only Ignored mutation policy.
void rt_textinput_set_read_only(void *input, int64_t read_only) {
    (void)input;
    (void)read_only;
}

/// @brief Stub: report editable state when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_is_read_only(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: ignore multiline-mode changes when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param multiline Ignored line-editing mode.
void rt_textinput_set_multiline(void *input, int64_t multiline) {
    (void)input;
    (void)multiline;
}

/// @brief Stub: report single-line mode when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_is_multiline(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: ignore grapheme-caret movement when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param grapheme_index Ignored caret boundary.
void rt_textinput_set_cursor(void *input, int64_t grapheme_index) {
    (void)input;
    (void)grapheme_index;
}

/// @brief Stub: report the initial caret boundary when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_cursor(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: ignore grapheme-range selection when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param start Ignored selection anchor.
/// @param end Ignored selection caret.
void rt_textinput_select_range(void *input, int64_t start, int64_t end) {
    (void)input;
    (void)start;
    (void)end;
}

/// @brief Stub: ignore selection collapse when graphics support is disabled.
/// @param input Ignored TextInput handle.
void rt_textinput_clear_selection(void *input) {
    (void)input;
}

/// @brief Stub: report an empty selection start when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_selection_start(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report an empty selection end when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_selection_end(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: return an empty selected-text value when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Canonical caller-owned empty runtime string.
rt_string rt_textinput_get_selected_text(void *input) {
    (void)input;
    return rt_str_empty();
}

/// @brief Stub: reject text insertion when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @param text Ignored runtime string.
/// @return Always zero because no committed text can change.
int64_t rt_textinput_insert_text(void *input, rt_string text) {
    (void)input;
    (void)text;
    return 0;
}

/// @brief Stub: reject selection deletion when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero because no committed text can change.
int64_t rt_textinput_delete_selection(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: reject undo when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero because no history exists.
int64_t rt_textinput_undo(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: reject redo when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero because no history exists.
int64_t rt_textinput_redo(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report no undo snapshot when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_can_undo(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report no redo snapshot when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_can_redo(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report no text-change edge when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_was_changed(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report no submission edge when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_was_submitted(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report the initial editor revision when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_revision(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report no active IME composition when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_is_composing(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: return empty IME preedit when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Canonical caller-owned empty runtime string.
rt_string rt_textinput_get_composition_text(void *input) {
    (void)input;
    return rt_str_empty();
}

/// @brief Stub: report no composition insertion boundary when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_composition_start(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: report zero preedit graphemes when graphics support is disabled.
/// @param input Ignored TextInput handle.
/// @return Always zero.
int64_t rt_textinput_get_composition_length(void *input) {
    (void)input;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no checkbox widget is created.
void *rt_checkbox_new(void *parent, rt_string text) {
    (void)parent;
    (void)text;
    return NULL;
}

/// @brief Programmatically set the checked state of a checkbox.
/// @param checkbox
/// @param checked
void rt_checkbox_set_checked(void *checkbox, int64_t checked) {
    (void)checkbox;
    (void)checked;
}

/// @brief Query whether a checkbox is currently checked.
/// @param checkbox
/// @return Result value.
int64_t rt_checkbox_is_checked(void *checkbox) {
    (void)checkbox;
    return 0;
}

/// @brief Update the label text displayed next to a checkbox.
/// @param checkbox
/// @param text
void rt_checkbox_set_text(void *checkbox, rt_string text) {
    (void)checkbox;
    (void)text;
}

/// @brief Programmatically set the indeterminate state of a checkbox.
/// @param checkbox
/// @param indeterminate
void rt_checkbox_set_indeterminate(void *checkbox, int64_t indeterminate) {
    (void)checkbox;
    (void)indeterminate;
}

/// @brief Query whether a checkbox is currently indeterminate.
/// @param checkbox
/// @return Result value.
int64_t rt_checkbox_is_indeterminate(void *checkbox) {
    (void)checkbox;
    return 0;
}

/// @brief Stub: no checkbox change edge exists when graphics is disabled.
/// @param checkbox Ignored checkbox handle.
/// @return Always zero.
int64_t rt_checkbox_was_changed(void *checkbox) {
    (void)checkbox;
    return 0;
}

/// @brief Stub: no checkbox revision exists when graphics is disabled.
/// @param checkbox Ignored checkbox handle.
/// @return Always zero.
int64_t rt_checkbox_get_revision(void *checkbox) {
    (void)checkbox;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no scroll view widget is created.
void *rt_scrollview_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Set the scroll of the scrollview.
void rt_scrollview_set_scroll(void *scroll, double x, double y) {
    (void)scroll;
    (void)x;
    (void)y;
}

/// @brief Set the content size of a scroll view.
void rt_scrollview_set_content_size(void *scroll, double width, double height) {
    (void)scroll;
    (void)width;
    (void)height;
}

/// @brief Stub: no descendant can be revealed when graphics is disabled.
void rt_scrollview_scroll_to(void *scroll, void *widget) {
    (void)scroll;
    (void)widget;
}

/// @brief Get the scroll x of the scrollview.
double rt_scrollview_get_scroll_x(void *scroll) {
    (void)scroll;
    return 0.0;
}

/// @brief Get the current vertical scroll offset.
double rt_scrollview_get_scroll_y(void *scroll) {
    (void)scroll;
    return 0.0;
}

/// @brief Stub: graphics disabled — returns NULL; no tree view widget is created.
void *rt_treeview_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no tree node is created or added.
void *rt_treeview_add_node(void *tree, void *parent_node, rt_string text) {
    (void)tree;
    (void)parent_node;
    (void)text;
    return NULL;
}

/// @brief Remove a node and its subtree from the tree view.
void rt_treeview_remove_node(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Remove all nodes from the tree view, leaving it empty.
void rt_treeview_clear(void *tree) {
    (void)tree;
}

/// @brief Stub: retired node pruning is a no-op without graphics.
void rt_treeview_prune_retired_nodes(void *tree) {
    (void)tree;
}

/// @brief Expand a tree node to show its children.
void rt_treeview_expand(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Collapse a tree node to hide its children.
void rt_treeview_collapse(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Stub: ignore tree-node expansion toggles without graphics.
void rt_treeview_toggle(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Programmatically select a tree node (NULL to clear selection).
void rt_treeview_select(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Stub: retained TreeView multi-selection is unavailable without graphics.
void rt_treeview_set_multi_select(void *tree, int64_t enabled) {
    (void)tree;
    (void)enabled;
}

/// @brief Stub: ignore TreeView scrolling without graphics.
void rt_treeview_scroll_to(void *tree, void *node) {
    (void)tree;
    (void)node;
}

/// @brief Set the font of the treeview.
void rt_treeview_set_font(void *tree, void *font, double size) {
    (void)tree;
    (void)font;
    (void)size;
}

/// @brief Stub: graphics disabled — returns NULL; no selection exists without a tree view.
void *rt_treeview_get_selected(void *tree) {
    (void)tree;
    return NULL;
}

/// @brief Stub: graphics disabled — no hit-tested nodes exist.
void *rt_treeview_get_node_at(void *tree, int64_t x, int64_t y) {
    (void)tree;
    (void)x;
    (void)y;
    return NULL;
}

/// @brief Stub: no retained TreeView selection data exists without graphics.
void *rt_treeview_get_selected_data(void *tree) {
    (void)tree;
    return rt_seq_new_owned();
}

/// @brief Stub: poll-model drag-and-drop is a no-op without graphics.
void rt_treeview_set_drag_drop_enabled(void *tree, int64_t enabled) {
    (void)tree;
    (void)enabled;
}

/// @brief Stub: poll-model TreeView drag-and-drop is unavailable without graphics.
void rt_treeview_set_drag_drop_mode(void *tree, int64_t mode) {
    (void)tree;
    (void)mode;
}

int64_t rt_treeview_was_drop_received(void *tree) {
    (void)tree;
    return 0;
}

rt_string rt_treeview_get_drop_source_data(void *tree) {
    (void)tree;
    return rt_str_empty();
}

rt_string rt_treeview_get_drop_target_data(void *tree) {
    (void)tree;
    return rt_str_empty();
}

int64_t rt_treeview_get_drop_position(void *tree) {
    (void)tree;
    return 1;
}

void rt_treeview_clear_drop(void *tree) {
    (void)tree;
}

/// @brief Check if the tree view selection changed since the last call (edge-triggered).
int64_t rt_treeview_was_selection_changed(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: no TreeView change edge exists when graphics is disabled.
/// @param tree Ignored TreeView handle.
/// @return Always zero.
int64_t rt_treeview_was_changed(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: no TreeView activation edge exists when graphics is disabled.
/// @param tree Ignored TreeView handle.
/// @return Always zero.
int64_t rt_treeview_was_activated(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: no lazy-child request edge exists without graphics.
int64_t rt_treeview_was_load_children_requested(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: return an owned empty Option for a lazy-child request target.
void *rt_treeview_get_load_requested_node_option(void *tree) {
    (void)tree;
    return rt_option_none();
}

/// @brief Stub: return an owned empty Option for an activation target.
void *rt_treeview_get_activated_node_option(void *tree) {
    (void)tree;
    return rt_option_none();
}

/// @brief Stub: inline row editing never begins without graphics.
int64_t rt_treeview_begin_edit_node(void *tree, void *node, rt_string initial_text) {
    (void)tree;
    (void)node;
    (void)initial_text;
    return 0;
}

/// @brief Stub: no inline row edit is ever in progress without graphics.
int64_t rt_treeview_is_editing(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: no inline-edit commit edge exists without graphics.
int64_t rt_treeview_was_edit_committed(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Stub: return empty committed inline-edit text without graphics.
rt_string rt_treeview_get_edit_text(void *tree) {
    (void)tree;
    return rt_str_empty();
}

/// @brief Stub: return an owned empty Option for the edited node.
void *rt_treeview_get_edited_node_option(void *tree) {
    (void)tree;
    return rt_option_none();
}

/// @brief Stub: ignore inline-edit cancellation without graphics.
void rt_treeview_cancel_edit(void *tree) {
    (void)tree;
}

/// @brief Stub: no TreeView revision exists when graphics is disabled.
/// @param tree Ignored TreeView handle.
/// @return Always zero.
int64_t rt_treeview_get_revision(void *tree) {
    (void)tree;
    return 0;
}

/// @brief Get the display text of a tree node.
rt_string rt_treeview_node_get_text(void *node) {
    (void)node;
    return rt_str_empty();
}

/// @brief Stub: ignore tree-node text changes without graphics.
void rt_treeview_node_set_text(void *node, rt_string text) {
    (void)node;
    (void)text;
}

/// @brief Stub: ignore tree-node icon changes without graphics.
void rt_treeview_node_set_icon(void *node, rt_string icon) {
    (void)node;
    (void)icon;
}

/// @brief Stub: return empty tree-node icon text without graphics.
rt_string rt_treeview_node_get_icon(void *node) {
    (void)node;
    return rt_str_empty();
}

/// @brief Stub: ignore lazy-child metadata without graphics.
void rt_treeview_node_set_has_children(void *node, int64_t has_children) {
    (void)node;
    (void)has_children;
}

/// @brief Stub: report no children without graphics.
int64_t rt_treeview_node_has_children(void *node) {
    (void)node;
    return 0;
}

/// @brief Stub: ignore tree-node loading state without graphics.
void rt_treeview_node_set_loading(void *node, int64_t loading) {
    (void)node;
    (void)loading;
}

/// @brief Stub: report no tree-node loading state without graphics.
int64_t rt_treeview_node_is_loading(void *node) {
    (void)node;
    return 0;
}

/// @brief Stub: ignore stable tree-node identifiers without graphics.
void rt_treeview_node_set_stable_id(void *node, rt_string stable_id) {
    (void)node;
    (void)stable_id;
}

/// @brief Stub: return an empty stable tree-node identifier without graphics.
rt_string rt_treeview_node_get_stable_id(void *node) {
    (void)node;
    return rt_str_empty();
}

/// @brief Attach arbitrary string data to a tree node (replaces any previous data).
void rt_treeview_node_set_data(void *node, rt_string data) {
    (void)node;
    (void)data;
}

/// @brief Retrieve the string data previously attached to a tree node.
rt_string rt_treeview_node_get_data(void *node) {
    (void)node;
    return rt_str_empty();
}

/// @brief Check whether a tree node is currently in the expanded state.
int64_t rt_treeview_node_is_expanded(void *node) {
    (void)node;
    return 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
