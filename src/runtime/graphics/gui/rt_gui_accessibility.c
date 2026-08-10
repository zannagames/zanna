//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_accessibility.c
/// @brief Implements GUI accessibility semantics, snapshots, preferences, and announcements.
///
/// @details
/// The runtime keeps a deterministic toolkit-level semantic tree even when no
/// native accessibility adapter is available. This translation layer validates
/// public handles, converts ownership at the C ABI boundary, projects changes
/// to an optional platform adapter, and builds iterative managed snapshots.
///
// File: src/runtime/graphics/gui/rt_gui_accessibility.c
// Purpose: Runtime bindings for widget semantics, deterministic accessibility
//          snapshots, live-region announcements, and app accessibility preferences.
//
// Key invariants:
//   - Semantic strings are copied into toolkit-owned widget records atomically.
//   - Snapshot traversal is iterative, so adversarially deep widget trees cannot
//     overflow the native stack.
//   - Snapshot bounds use logical units exactly once at the active app boundary.
//   - Native adapter availability never removes or weakens the headless tree.
//
// Ownership/Lifetime:
//   - Widget semantic records own their UTF-8 strings and borrow label targets by
//     address plus immutable widget ID.
//   - Snapshot Maps and child Seqs are managed runtime objects returned to callers.
//   - No accessibility preference or relationship retains a widget or app handle.
//
// Links: src/runtime/graphics/gui/rt_gui.h,
//        src/runtime/graphics/gui/rt_gui_internal.h,
//        src/lib/gui/include/vg_widget.h,
//        docs/adr/0107-gui-theme-accessibility-input-and-render-policy.md,
//        docs/adr/0167-spinner-mixed-value-state.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_internal.h"

#include "rt_heap.h"
#include "rt_map.h"
#include "rt_seq.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Release one local reference to a managed runtime object.
/// @param object Managed object pointer; NULL is ignored.
static void rt_gui_accessibility_release_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Convert a nullable C string to an owned runtime string.
/// @param text UTF-8 C string; NULL becomes the canonical empty string.
/// @return Owned runtime string.
static rt_string rt_gui_accessibility_string(const char *text) {
    if (!text)
        return rt_str_empty();
    return rt_string_from_bytes(text, strlen(text));
}

/// @brief Store a UTF-8 string field in a runtime Map without leaking its local reference.
/// @param map Destination Map; NULL is ignored.
/// @param key Stable ASCII key.
/// @param value Nullable UTF-8 value.
static void rt_gui_accessibility_map_set_string(void *map, const char *key, const char *value) {
    if (!map || !key)
        return;
    rt_string text = rt_gui_accessibility_string(value);
    rt_map_set_str(map, rt_const_cstr(key), text);
    rt_string_unref(text);
}

/// @brief Forward one changed semantic record to the owning native adapter.
/// @param widget Borrowed live widget; detached and invalid widgets have no adapter target.
static void rt_gui_accessibility_notify_widget(vg_widget_t *widget) {
    rt_gui_app_t *app = rt_gui_app_from_widget(widget);
    if (app)
        rt_gui_accessibility_platform_notify(app->window, widget);
}

/// @brief Return whether a widget is effectively enabled through its full ancestor chain.
/// @param widget Widget to inspect.
/// @return True only when the widget and every parent are enabled.
static bool rt_gui_accessibility_effectively_enabled(const vg_widget_t *widget) {
    size_t depth = 0;
    for (const vg_widget_t *current = widget; current; current = current->parent) {
        if (depth++ >= RT_GUI_MAX_ACCESSIBILITY_NODES)
            return false;
        if (!current->enabled)
            return false;
    }
    return widget != NULL;
}

/// @brief Infer an accessible name from built-in control text when no override exists.
/// @param widget Live widget to inspect.
/// @return Borrowed UTF-8 string, never NULL.
static const char *rt_gui_accessibility_inferred_name(const vg_widget_t *widget) {
    if (!widget)
        return "";
    if (widget->accessibility.name)
        return widget->accessibility.name;
    switch (widget->type) {
        case VG_WIDGET_LABEL:
            return ((const vg_label_t *)widget)->text ? ((const vg_label_t *)widget)->text : "";
        case VG_WIDGET_BUTTON:
            return ((const vg_button_t *)widget)->text ? ((const vg_button_t *)widget)->text : "";
        case VG_WIDGET_TEXTINPUT: {
            const vg_textinput_t *input = (const vg_textinput_t *)widget;
            return input->placeholder ? input->placeholder : (widget->name ? widget->name : "");
        }
        case VG_WIDGET_CHECKBOX:
            return ((const vg_checkbox_t *)widget)->text ? ((const vg_checkbox_t *)widget)->text
                                                         : "";
        case VG_WIDGET_RADIO:
            return ((const vg_radiobutton_t *)widget)->text
                       ? ((const vg_radiobutton_t *)widget)->text
                       : "";
        case VG_WIDGET_DROPDOWN: {
            const vg_dropdown_t *dropdown = (const vg_dropdown_t *)widget;
            return dropdown->placeholder ? dropdown->placeholder
                                         : (widget->name ? widget->name : "");
        }
        case VG_WIDGET_GROUPBOX:
            return ((const vg_groupbox_t *)widget)->title ? ((const vg_groupbox_t *)widget)->title
                                                          : "";
        case VG_WIDGET_DIALOG:
            return ((const vg_dialog_t *)widget)->title ? ((const vg_dialog_t *)widget)->title : "";
        default:
            return widget->name ? widget->name : "";
    }
}

/// @brief Infer a built-in control's current semantic value.
/// @param widget Live widget to inspect.
/// @param buffer Scratch buffer used for numeric/boolean formatting.
/// @param buffer_size Size of @p buffer in bytes.
/// @return Borrowed string pointing into the widget or @p buffer, never NULL.
static const char *rt_gui_accessibility_inferred_value(const vg_widget_t *widget,
                                                       char *buffer,
                                                       size_t buffer_size) {
    if (!widget)
        return "";
    if (widget->accessibility.value)
        return widget->accessibility.value;
    switch (widget->type) {
        case VG_WIDGET_TEXTINPUT:
            return ((const vg_textinput_t *)widget)->text ? ((const vg_textinput_t *)widget)->text
                                                          : "";
        case VG_WIDGET_CHECKBOX: {
            const vg_checkbox_t *checkbox = (const vg_checkbox_t *)widget;
            return checkbox->indeterminate
                       ? "mixed"
                       : ((widget->state & VG_STATE_CHECKED) ? "true" : "false");
        }
        case VG_WIDGET_RADIO:
            return (widget->state & VG_STATE_CHECKED) ? "true" : "false";
        case VG_WIDGET_DROPDOWN: {
            const vg_dropdown_t *dropdown = (const vg_dropdown_t *)widget;
            return dropdown->selected_index >= 0 &&
                           dropdown->selected_index < dropdown->item_count && dropdown->items
                       ? dropdown->items[dropdown->selected_index]
                       : "";
        }
        case VG_WIDGET_SLIDER:
            snprintf(buffer, buffer_size, "%.9g", (double)((const vg_slider_t *)widget)->value);
            return buffer;
        case VG_WIDGET_PROGRESS:
            snprintf(
                buffer, buffer_size, "%.9g", (double)((const vg_progressbar_t *)widget)->value);
            return buffer;
        case VG_WIDGET_SPINNER:
            if (((const vg_spinner_t *)widget)->indeterminate)
                return "mixed";
            snprintf(buffer, buffer_size, "%.17g", ((const vg_spinner_t *)widget)->value);
            return buffer;
        default:
            return "";
    }
}

/// @brief Allocate and populate one accessibility node Map and its empty owned child Seq.
/// @details The caller receives one local reference to each returned object. The Map also retains
///          the child Seq under `children`, allowing the caller to release the Seq after traversal.
/// @param widget Live widget represented by the node.
/// @param scale Positive physical-to-logical divisor.
/// @param out_children Receives the node's owned child sequence.
/// @return Managed node Map, or NULL on allocation failure.
static void *rt_gui_accessibility_make_node(vg_widget_t *widget, float scale, void **out_children) {
    if (out_children)
        *out_children = NULL;
    if (!widget || !vg_widget_is_live(widget))
        return NULL;
    void *map = rt_map_new();
    if (!map)
        return NULL;
    void *children = rt_seq_new_owned();
    if (!children) {
        rt_gui_accessibility_release_object(map);
        return NULL;
    }
    if (!isfinite(scale) || scale <= 0.0f)
        scale = 1.0f;

    float screen_x = 0.0f, screen_y = 0.0f, screen_w = 0.0f, screen_h = 0.0f;
    vg_widget_get_screen_bounds(widget, &screen_x, &screen_y, &screen_w, &screen_h);
    screen_x = rt_gui_sanitize_signed_float(screen_x, RT_GUI_MAX_LAYOUT_VALUE);
    screen_y = rt_gui_sanitize_signed_float(screen_y, RT_GUI_MAX_LAYOUT_VALUE);
    screen_w = rt_gui_sanitize_nonnegative_float(screen_w, RT_GUI_MAX_LAYOUT_VALUE);
    screen_h = rt_gui_sanitize_nonnegative_float(screen_h, RT_GUI_MAX_LAYOUT_VALUE);
    char value_buffer[64] = {0};
    vg_widget_t *label_target = vg_widget_get_accessible_label_for(widget);

    rt_map_set_int(map, rt_const_cstr("schemaVersion"), 1);
    rt_map_set_int(map, rt_const_cstr("id"), rt_gui_saturating_u64_to_i64(widget->id));
    rt_map_set_int(map, rt_const_cstr("type"), (int64_t)widget->type);
    rt_map_set_int(map, rt_const_cstr("role"), (int64_t)vg_widget_get_accessible_role(widget));
    rt_gui_accessibility_map_set_string(map, "name", rt_gui_accessibility_inferred_name(widget));
    rt_gui_accessibility_map_set_string(
        map, "description", vg_widget_get_accessible_description(widget));
    rt_gui_accessibility_map_set_string(
        map,
        "value",
        rt_gui_accessibility_inferred_value(widget, value_buffer, sizeof(value_buffer)));
    rt_map_set_bool(map, rt_const_cstr("visible"), widget->visible ? 1 : 0);
    rt_map_set_bool(
        map, rt_const_cstr("enabled"), rt_gui_accessibility_effectively_enabled(widget) ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("checked"), (widget->state & VG_STATE_CHECKED) ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("selected"), (widget->state & VG_STATE_SELECTED) ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("focused"), (widget->state & VG_STATE_FOCUSED) ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("expanded"), 0);
    rt_map_set_bool(map,
                    rt_const_cstr("readOnly"),
                    widget->type == VG_WIDGET_TEXTINPUT && ((vg_textinput_t *)widget)->read_only);
    rt_map_set_bool(map,
                    rt_const_cstr("multiline"),
                    widget->type == VG_WIDGET_TEXTINPUT && ((vg_textinput_t *)widget)->multiline);
    rt_map_set_float(map, rt_const_cstr("logicalX"), (double)screen_x / scale);
    rt_map_set_float(map, rt_const_cstr("logicalY"), (double)screen_y / scale);
    rt_map_set_float(map, rt_const_cstr("logicalWidth"), (double)screen_w / scale);
    rt_map_set_float(map, rt_const_cstr("logicalHeight"), (double)screen_h / scale);
    rt_map_set_float(map, rt_const_cstr("screenX"), screen_x);
    rt_map_set_float(map, rt_const_cstr("screenY"), screen_y);
    rt_map_set_float(map, rt_const_cstr("screenWidth"), screen_w);
    rt_map_set_float(map, rt_const_cstr("screenHeight"), screen_h);
    rt_map_set_int(map,
                   rt_const_cstr("labelForId"),
                   label_target ? rt_gui_saturating_u64_to_i64(label_target->id) : 0);
    rt_map_set_int(map, rt_const_cstr("liveRegion"), (int64_t)vg_widget_get_live_region(widget));
    rt_map_set_int(map, rt_const_cstr("revision"), rt_gui_saturating_u64_to_i64(widget->revision));
    rt_map_set_int(map,
                   rt_const_cstr("semanticRevision"),
                   rt_gui_saturating_u64_to_i64(widget->accessibility.revision));
    rt_map_set_int(map,
                   rt_const_cstr("announcementRevision"),
                   rt_gui_saturating_u64_to_i64(widget->accessibility.announcement_revision));
    rt_map_set_int(
        map, rt_const_cstr("announcementMode"), (int64_t)widget->accessibility.announcement_mode);
    rt_gui_accessibility_map_set_string(map, "announcement", widget->accessibility.announcement);
    rt_map_set(map, rt_const_cstr("children"), children);

    if (out_children)
        *out_children = children;
    else
        rt_gui_accessibility_release_object(children);
    return map;
}

typedef struct rt_gui_accessibility_frame {
    vg_widget_t *next_child; ///< Next visible candidate in this node's sibling list.
    void *children;          ///< Locally retained Seq receiving child node Maps.
} rt_gui_accessibility_frame_t;

/// @brief Grow the iterative snapshot frame stack and append a frame.
/// @param frames In/out frame-array pointer.
/// @param count In/out active frame count.
/// @param capacity In/out allocated frame capacity.
/// @param next_child First child candidate for the new frame.
/// @param children Locally retained child Seq owned by the new frame.
/// @return True on success; false on overflow/allocation failure.
static bool rt_gui_accessibility_push_frame(rt_gui_accessibility_frame_t **frames,
                                            size_t *count,
                                            size_t *capacity,
                                            vg_widget_t *next_child,
                                            void *children) {
    if (!frames || !count || !capacity || !children)
        return false;
    if (*count >= RT_GUI_MAX_ACCESSIBILITY_NODES)
        return false;
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2u : 32u;
        if (next_capacity > RT_GUI_MAX_ACCESSIBILITY_NODES)
            next_capacity = RT_GUI_MAX_ACCESSIBILITY_NODES;
        if (next_capacity < *capacity ||
            next_capacity > SIZE_MAX / sizeof(rt_gui_accessibility_frame_t)) {
            return false;
        }
        void *replacement = realloc(*frames, next_capacity * sizeof(rt_gui_accessibility_frame_t));
        if (!replacement)
            return false;
        *frames = (rt_gui_accessibility_frame_t *)replacement;
        *capacity = next_capacity;
    }
    (*frames)[(*count)++] = (rt_gui_accessibility_frame_t){next_child, children};
    return true;
}

#endif

/// @brief Build a deterministic accessibility tree snapshot without recursion.
/// @details Invisible descendants are omitted. If a later allocation fails, the valid prefix is
///          returned with root field `truncated=true`; the root allocation itself is the only
///          failure that returns NULL.
/// @param root Root widget handle to snapshot.
/// @return Managed versioned Map, empty/versioned for invalid roots, or NULL on root OOM.
void *rt_accessibility_snapshot(void *root) {
    RT_ASSERT_MAIN_THREAD();
    void *empty = NULL;
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *root_widget = rt_gui_widget_handle_checked(root);
    if (!root_widget) {
        empty = rt_map_new();
        if (empty)
            rt_map_set_int(empty, rt_const_cstr("schemaVersion"), 1);
        return empty;
    }

    rt_gui_app_t *app = rt_gui_app_from_widget(root_widget);
    float scale = rt_gui_app_effective_scale(app);
    void *root_children = NULL;
    void *root_map = rt_gui_accessibility_make_node(root_widget, scale, &root_children);
    if (!root_map)
        return NULL;

    rt_gui_accessibility_frame_t *frames = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t node_count = 1;
    bool truncated = !rt_gui_accessibility_push_frame(
        &frames, &count, &capacity, root_widget->first_child, root_children);
    if (truncated)
        rt_gui_accessibility_release_object(root_children);

    while (!truncated && count > 0) {
        rt_gui_accessibility_frame_t *frame = &frames[count - 1u];
        vg_widget_t *child = frame->next_child;
        size_t skipped = 0;
        while (child && !child->visible) {
            if (skipped++ >= RT_GUI_MAX_ACCESSIBILITY_NODES) {
                truncated = true;
                break;
            }
            child = child->next_sibling;
        }
        if (truncated)
            break;
        if (!child) {
            rt_gui_accessibility_release_object(frame->children);
            --count;
            continue;
        }
        frame->next_child = child->next_sibling;

        if (node_count >= RT_GUI_MAX_ACCESSIBILITY_NODES) {
            truncated = true;
            break;
        }

        void *child_children = NULL;
        void *child_map = rt_gui_accessibility_make_node(child, scale, &child_children);
        if (!child_map) {
            truncated = true;
            break;
        }
        rt_seq_push(frame->children, child_map);
        rt_gui_accessibility_release_object(child_map);
        ++node_count;
        if (!rt_gui_accessibility_push_frame(
                &frames, &count, &capacity, child->first_child, child_children)) {
            rt_gui_accessibility_release_object(child_children);
            truncated = true;
            break;
        }
    }

    while (count > 0) {
        rt_gui_accessibility_release_object(frames[count - 1u].children);
        --count;
    }
    free(frames);
    rt_map_set_bool(root_map, rt_const_cstr("truncated"), truncated ? 1 : 0);
    return root_map;
#else
    (void)root;
    empty = rt_map_new();
    if (empty)
        rt_map_set_int(empty, rt_const_cstr("schemaVersion"), 1);
    return empty;
#endif
}

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Convert a runtime UTF-8 string to a temporary NUL-terminated copy and invoke a setter.
/// @param widget Runtime widget handle.
/// @param text Runtime string to validate/copy.
/// @param setter Toolkit semantic-string setter.
static void rt_gui_accessibility_set_widget_string(void *widget,
                                                   rt_string text,
                                                   void (*setter)(vg_widget_t *, const char *)) {
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    if (!resolved || !setter || rt_string_contains_nul(text))
        return;
    char *copy = text ? rt_string_to_cstr(text) : NULL;
    if (text && !copy)
        return;
    setter(resolved, copy);
    free(copy);
}

/// @brief Return one toolkit semantic C string as an owned runtime string.
/// @param widget Runtime widget handle.
/// @param getter Toolkit semantic-string getter.
/// @return Owned runtime string, empty for invalid handles.
static rt_string rt_gui_accessibility_get_widget_string(
    void *widget, const char *(*getter)(const vg_widget_t *)) {
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    return rt_gui_accessibility_string(resolved && getter ? getter(resolved) : "");
}

#endif

/// @brief Set a widget's stable semantic role.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param role Value from the public accessible-role enumeration.
void rt_widget_set_accessible_role(void *widget, int64_t role) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    if (resolved) {
        vg_widget_set_accessible_role(resolved, (vg_accessible_role_t)role);
        rt_gui_accessibility_notify_widget(resolved);
    }
#else
    (void)widget;
    (void)role;
#endif
}

/// @brief Return a widget's stable semantic role or none for invalid handles.
/// @param widget Runtime widget handle.
/// @return Accessible-role value, or the none role for an invalid handle.
int64_t rt_widget_get_accessible_role(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    return resolved ? (int64_t)vg_widget_get_accessible_role(resolved) : 0;
#else
    (void)widget;
    return 0;
#endif
}

/// @brief Set or clear a widget's explicit accessible name.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param name UTF-8 name to copy, or an empty runtime string to clear the override.
void rt_widget_set_accessible_name(void *widget, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_accessibility_set_widget_string(widget, name, vg_widget_set_accessible_name);
    rt_gui_accessibility_notify_widget(rt_gui_widget_handle_checked(widget));
#else
    (void)widget;
    (void)name;
#endif
}

/// @brief Return a widget's explicit accessible name as an owned runtime string.
/// @param widget Runtime widget handle.
/// @return Owned explicit name, or an empty runtime string for an invalid handle or no override.
rt_string rt_widget_get_accessible_name(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    return rt_gui_accessibility_get_widget_string(widget, vg_widget_get_accessible_name);
#else
    (void)widget;
    return rt_str_empty();
#endif
}

/// @brief Set or clear a widget's accessible description.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param description UTF-8 description to copy, or an empty runtime string to clear it.
void rt_widget_set_accessible_description(void *widget, rt_string description) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_accessibility_set_widget_string(
        widget, description, vg_widget_set_accessible_description);
    rt_gui_accessibility_notify_widget(rt_gui_widget_handle_checked(widget));
#else
    (void)widget;
    (void)description;
#endif
}

/// @brief Return a widget's accessible description as an owned runtime string.
/// @param widget Runtime widget handle.
/// @return Owned description, or an empty runtime string for an invalid handle or no description.
rt_string rt_widget_get_accessible_description(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    return rt_gui_accessibility_get_widget_string(widget, vg_widget_get_accessible_description);
#else
    (void)widget;
    return rt_str_empty();
#endif
}

/// @brief Set or clear a widget's explicit accessible value.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param value UTF-8 value to copy, or an empty runtime string to clear the override.
void rt_widget_set_accessible_value(void *widget, rt_string value) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_accessibility_set_widget_string(widget, value, vg_widget_set_accessible_value);
    rt_gui_accessibility_notify_widget(rt_gui_widget_handle_checked(widget));
#else
    (void)widget;
    (void)value;
#endif
}

/// @brief Return a widget's explicit accessible value as an owned runtime string.
/// @param widget Runtime widget handle.
/// @return Owned explicit value, or an empty runtime string for an invalid handle or no override.
rt_string rt_widget_get_accessible_value(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    return rt_gui_accessibility_get_widget_string(widget, vg_widget_get_accessible_value);
#else
    (void)widget;
    return rt_str_empty();
#endif
}

/// @brief Install a same-tree, non-owning accessibility label relationship.
/// @param widget Runtime handle for the widget that supplies the label.
/// @param target Runtime handle for the widget described by the label.
void rt_widget_set_accessible_label_for(void *widget, void *target) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    vg_widget_t *resolved_target = rt_gui_widget_handle_checked(target);
    if (resolved && resolved_target) {
        (void)vg_widget_set_accessible_label_for(resolved, resolved_target);
        rt_gui_accessibility_notify_widget(resolved);
    }
#else
    (void)widget;
    (void)target;
#endif
}

/// @brief Clear a widget's non-owning accessibility label relationship.
/// @param widget Runtime handle for the labeling widget; invalid handles are ignored.
void rt_widget_clear_accessible_label_for(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    if (resolved) {
        (void)vg_widget_set_accessible_label_for(resolved, NULL);
        rt_gui_accessibility_notify_widget(resolved);
    }
#else
    (void)widget;
#endif
}

/// @brief Set a widget's default live-region urgency.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param mode Value from the public live-region mode enumeration.
void rt_widget_set_live_region(void *widget, int64_t mode) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    if (resolved) {
        vg_widget_set_live_region(resolved, (vg_live_region_mode_t)mode);
        rt_gui_accessibility_notify_widget(resolved);
    }
#else
    (void)widget;
    (void)mode;
#endif
}

/// @brief Return a widget's default live-region urgency.
/// @param widget Runtime widget handle.
/// @return Live-region mode, or the off mode for an invalid handle.
int64_t rt_widget_get_live_region(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    return resolved ? (int64_t)vg_widget_get_live_region(resolved) : 0;
#else
    (void)widget;
    return 0;
#endif
}

/// @brief Return a widget's non-consuming monotonic revision.
/// @param widget Runtime widget handle.
/// @return Revision saturated at `INT64_MAX`, or 0 for an invalid handle.
int64_t rt_widget_get_revision(void *widget) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    uint64_t revision = resolved ? vg_widget_get_revision(resolved) : 0;
    return rt_gui_saturating_u64_to_i64(revision);
#else
    (void)widget;
    return 0;
#endif
}

/// @brief Enable or disable the active app's deterministic high-contrast palette.
/// @param enabled Non-zero to enable high contrast; zero to restore normal contrast.
void rt_accessibility_set_high_contrast(int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    int32_t normalized = enabled != 0;
    if (!app || app->accessibility_high_contrast == normalized)
        return;
    app->accessibility_high_contrast = normalized;
    if (app->accessibility_revision < UINT64_MAX)
        ++app->accessibility_revision;
    app->theme_base = NULL;
    rt_gui_refresh_theme(app);
#else
    (void)enabled;
#endif
}

/// @brief Return the active app's explicit high-contrast preference.
/// @return 1 when high contrast is enabled, otherwise 0.
int64_t rt_accessibility_is_high_contrast(void) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    return app && app->accessibility_high_contrast ? 1 : 0;
#else
    return 0;
#endif
}

/// @brief Enable or disable the active app's reduced-motion preference.
/// @param enabled Non-zero to request reduced motion; zero to allow normal animation.
void rt_accessibility_set_reduced_motion(int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    int32_t normalized = enabled != 0;
    if (!app || app->accessibility_reduced_motion == normalized)
        return;
    app->accessibility_reduced_motion = normalized;
    if (app->accessibility_revision < UINT64_MAX)
        ++app->accessibility_revision;
    app->theme_base = NULL;
    rt_gui_refresh_theme(app);
#else
    (void)enabled;
#endif
}

/// @brief Return the active app's explicit reduced-motion preference.
/// @return 1 when reduced motion is enabled, otherwise 0.
int64_t rt_accessibility_is_reduced_motion(void) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    return app && app->accessibility_reduced_motion ? 1 : 0;
#else
    return 0;
#endif
}

/// @brief Return the native platform high-contrast preference when available.
/// @details Native projection hooks are deliberately optional; zero is the deterministic fallback
///          until the selected ZannaGFX backend reports a preference.
/// @return 1 when the platform reports high contrast, otherwise 0.
int64_t rt_accessibility_get_system_high_contrast(void) {
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    return rt_gui_accessibility_platform_high_contrast(app ? app->window : NULL);
#else
    return 0;
#endif
}

/// @brief Return the native platform reduced-motion preference when available.
/// @details Native projection hooks are deliberately optional; zero is the deterministic fallback
///          until the selected ZannaGFX backend reports a preference.
/// @return 1 when the platform reports reduced motion, otherwise 0.
int64_t rt_accessibility_get_system_reduced_motion(void) {
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_app_t *app = rt_gui_get_active_app();
    return rt_gui_accessibility_platform_reduced_motion(app ? app->window : NULL);
#else
    return 0;
#endif
}

/// @brief Record a live-region announcement in the widget semantic tree.
/// @param widget Runtime widget handle; invalid handles are ignored.
/// @param text UTF-8 announcement text copied into the widget record.
/// @param mode Requested live-region mode; the toolkit normalizes invalid values.
void rt_accessibility_announce(void *widget, rt_string text, int64_t mode) {
    RT_ASSERT_MAIN_THREAD();
#ifdef ZANNA_ENABLE_GRAPHICS
    vg_widget_t *resolved = rt_gui_widget_handle_checked(widget);
    if (!resolved || rt_string_contains_nul(text))
        return;
    char *copy = text ? rt_string_to_cstr(text) : NULL;
    if (text && !copy)
        return;
    vg_widget_accessibility_announce(resolved, copy, (vg_live_region_mode_t)mode);
    rt_gui_app_t *app = rt_gui_app_from_widget(resolved);
    if (app) {
        rt_gui_accessibility_platform_announce(
            app->window, resolved, copy, resolved->accessibility.announcement_mode);
        if (app->accessibility_revision < UINT64_MAX)
            ++app->accessibility_revision;
    }
    free(copy);
#else
    (void)widget;
    (void)text;
    (void)mode;
#endif
}
