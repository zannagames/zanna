//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_atspi_linux.c
/// @brief Implements the Linux AT-SPI D-Bus accessibility projection.
///
/// @details
/// The bridge snapshots the retained widget tree on the GUI thread, publishes
/// immutable node metadata through dynamically loaded GIO interfaces on a
/// private worker context, and marshals requested actions back to the GUI
/// thread. Missing GIO or accessibility-bus services leave the headless
/// semantic tree operational without adding a product dependency.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_atspi_linux.c
// Purpose: Export Zanna GUI applications on the dedicated AT-SPI D-Bus.
// Key invariants:
//   - The root object implements both Application and Accessible before Socket.Embed.
//   - GIO is dynamically loaded and serviced on a private worker main context.
// Ownership/Lifetime:
//   - One bridge owns its GIO connection, registrations, loop, worker, and copied metadata.
//   - The process list is protected independently from each bridge's worker lifecycle.
// Links: https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/,
//        docs/adr/0167-spinner-mixed-value-state.md
//
//===----------------------------------------------------------------------===//

#define _POSIX_C_SOURCE 200809L

#include "rt_gui_atspi_linux.h"

#include "rt_gui_internal.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct GDBusConnection GDBusConnection;
typedef struct GDBusMethodInvocation GDBusMethodInvocation;
typedef struct GDBusNodeInfo GDBusNodeInfo;
typedef struct GDBusInterfaceInfo GDBusInterfaceInfo;
typedef struct GMainContext GMainContext;
typedef struct GMainLoop GMainLoop;
typedef struct GSource GSource;
typedef struct GVariant GVariant;
typedef struct GVariantBuilder GVariantBuilder;

typedef struct GError {
    uint32_t domain;
    int code;
    char *message;
} GError;

/// @brief GDBus method-dispatch callback ABI used by the AT-SPI object vtable.
/// @details Receives the connection, sender, object path, interface and method
///          names, borrowed parameters, invocation handle, and registered user data.
typedef void (*rt_gdbus_method_call_t)(GDBusConnection *,
                                       const char *,
                                       const char *,
                                       const char *,
                                       const char *,
                                       GVariant *,
                                       GDBusMethodInvocation *,
                                       void *);
/// @brief GDBus property-read callback ABI used by the AT-SPI object vtable.
/// @details Receives connection and routing metadata, the property name, an
///          optional error destination, and registered user data.
/// @return Newly constructed property variant, or NULL when unavailable.
typedef GVariant *(*rt_gdbus_get_property_t)(
    GDBusConnection *, const char *, const char *, const char *, const char *, GError **, void *);
/// @brief GDBus property-write callback ABI used by the AT-SPI object vtable.
/// @details Receives connection and routing metadata, the property name and
///          borrowed replacement variant, an optional error destination, and
///          registered user data.
/// @return Nonzero when the replacement was accepted; otherwise zero.
typedef int (*rt_gdbus_set_property_t)(GDBusConnection *,
                                       const char *,
                                       const char *,
                                       const char *,
                                       const char *,
                                       GVariant *,
                                       GError **,
                                       void *);

typedef struct rt_gdbus_interface_vtable {
    rt_gdbus_method_call_t method_call;
    rt_gdbus_get_property_t get_property;
    rt_gdbus_set_property_t set_property;
    void *padding[8];
} rt_gdbus_interface_vtable_t;

typedef struct rt_gui_atspi_api {
    void *library;
    GDBusConnection *(*bus_get_sync)(int, void *, GError **);
    GVariant *(*connection_call_sync)(GDBusConnection *,
                                      const char *,
                                      const char *,
                                      const char *,
                                      const char *,
                                      GVariant *,
                                      const void *,
                                      int,
                                      int,
                                      void *,
                                      GError **);
    GDBusConnection *(*connection_new_for_address_sync)(
        const char *, int, void *, void *, GError **);
    const char *(*connection_get_unique_name)(GDBusConnection *);
    uint32_t (*connection_register_object)(GDBusConnection *,
                                           const char *,
                                           GDBusInterfaceInfo *,
                                           const rt_gdbus_interface_vtable_t *,
                                           void *,
                                           void (*)(void *),
                                           GError **);
    int (*connection_unregister_object)(GDBusConnection *, uint32_t);
    int (*connection_emit_signal)(GDBusConnection *,
                                  const char *,
                                  const char *,
                                  const char *,
                                  const char *,
                                  GVariant *,
                                  GError **);
    GVariant *(*variant_new)(const char *, ...);
    GVariant *(*variant_new_string)(const char *);
    GVariant *(*variant_new_int32)(int32_t);
    GVariant *(*variant_new_double)(double);
    GVariant *(*variant_new_fixed_array)(const void *, const void *, size_t, size_t);
    GVariant *(*variant_get_child_value)(GVariant *, size_t);
    const char *(*variant_get_string)(GVariant *, size_t *);
    int32_t (*variant_get_int32)(GVariant *);
    uint32_t (*variant_get_uint32)(GVariant *);
    double (*variant_get_double)(GVariant *);
    size_t (*variant_n_children)(GVariant *);
    void (*variant_unref)(GVariant *);
    GVariantBuilder *(*variant_builder_new)(const void *);
    void (*variant_builder_add)(GVariantBuilder *, const char *, ...);
    GVariant *(*variant_builder_end)(GVariantBuilder *);
    void (*variant_builder_unref)(GVariantBuilder *);
    GDBusNodeInfo *(*node_info_new_for_xml)(const char *, GError **);
    GDBusInterfaceInfo *(*node_info_lookup_interface)(GDBusNodeInfo *, const char *);
    void (*node_info_unref)(GDBusNodeInfo *);
    void (*invocation_return_value)(GDBusMethodInvocation *, GVariant *);
    void (*invocation_return_dbus_error)(GDBusMethodInvocation *, const char *, const char *);
    GMainContext *(*main_context_new)(void);
    void (*main_context_push_thread_default)(GMainContext *);
    void (*main_context_pop_thread_default)(GMainContext *);
    void (*main_context_unref)(GMainContext *);
    GMainLoop *(*main_loop_new)(GMainContext *, int);
    void (*main_loop_run)(GMainLoop *);
    void (*main_loop_quit)(GMainLoop *);
    void (*main_loop_unref)(GMainLoop *);
    GSource *(*idle_source_new)(void);
    void (*source_set_callback)(GSource *, int (*)(void *), void *, void (*)(void *));
    uint32_t (*source_attach)(GSource *, GMainContext *);
    void (*source_unref)(GSource *);
    void (*object_unref)(void *);
    void (*error_free)(GError *);
} rt_gui_atspi_api_t;

struct rt_gui_atspi_bridge;

typedef struct rt_gui_atspi_node {
    struct rt_gui_atspi_bridge *bridge;
    const vg_widget_t *source;
    uint64_t widget_id;
    char path[96];
    char name[256];
    char description[256];
    char accessible_id[96];
    char text[1024];
    char value_text[128];
    uint32_t role;
    uint32_t states[16];
    size_t state_count;
    int32_t parent_index;
    int32_t *children;
    int32_t child_count;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t text_characters;
    int32_t caret_offset;
    int32_t selection_start;
    int32_t selection_end;
    double value;
    double value_minimum;
    double value_maximum;
    double value_increment;
    int has_action;
    int has_text;
    int has_value;
    uint32_t accessible_registration;
    uint32_t component_registration;
    uint32_t action_registration;
    uint32_t text_registration;
    uint32_t value_registration;
} rt_gui_atspi_node_t;

/// @brief One pending visible-tree visit with its already-known snapshot parent.
typedef struct rt_gui_atspi_visit {
    vg_widget_t *widget;  ///< Borrowed live widget to snapshot.
    int32_t parent_index; ///< Snapshot index of the parent, or -1 for the root.
} rt_gui_atspi_visit_t;

typedef struct rt_gui_atspi_bridge {
    vgfx_window_t window;
    char name[256];
    char locale[64];
    int32_t application_id;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
    int started;
    int dirty;
    int active_test_requests;
    int request_pending;
    int request_kind;
    int request_result;
    uint64_t request_widget_id;
    uint64_t request_serial;
    uint64_t completed_serial;
    double request_value;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;
    GDBusNodeInfo *node_info;
    uint32_t accessible_registration;
    uint32_t application_registration;
    uint32_t cache_registration;
    rt_gui_atspi_node_t *nodes;
    size_t node_count;
    int32_t *child_storage;
    struct rt_gui_atspi_bridge *next;
} rt_gui_atspi_bridge_t;

static rt_gui_atspi_api_t g_rt_gui_atspi_api;
static pthread_once_t g_rt_gui_atspi_api_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_rt_gui_atspi_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static rt_gui_atspi_bridge_t *g_rt_gui_atspi_bridges;

static const char *const g_rt_gui_atspi_xml_parts[] = {
    "<node>"
    "<interface name='org.a11y.atspi.Application'>"
    "<property name='ToolkitName' type='s' access='read'/>"
    "<property name='Version' type='s' access='read'/>"
    "<property name='AtspiVersion' type='s' access='read'/>"
    "<property name='Id' type='i' access='readwrite'/>"
    "<method name='GetLocale'><arg direction='in' type='u'/><arg direction='out' "
    "type='s'/></method>"
    "</interface>",
    "<interface name='org.a11y.atspi.Cache'>"
    "<property name='version' type='u' access='read'/>"
    "<method name='GetItems'><arg direction='out' type='a((so)(so)(so)iiassusau)'/></method>"
    "<signal name='AddAccessible'><arg type='((so)(so)(so)iiassusau)'/></signal>"
    "<signal name='RemoveAccessible'><arg type='(so)'/></signal>"
    "</interface>",
    "<interface name='org.a11y.atspi.Accessible'>"
    "<property name='Name' type='s' access='read'/>"
    "<property name='Description' type='s' access='read'/>"
    "<property name='Parent' type='(so)' access='read'/>"
    "<property name='ChildCount' type='i' access='read'/>"
    "<property name='Locale' type='s' access='read'/>"
    "<property name='AccessibleId' type='s' access='read'/>"
    "<property name='HelpText' type='s' access='read'/>"
    "<method name='GetChildAtIndex'><arg direction='in' type='i'/><arg direction='out' "
    "type='(so)'/></method>"
    "<method name='GetChildren'><arg direction='out' type='a(so)'/></method>"
    "<method name='GetIndexInParent'><arg direction='out' type='i'/></method>"
    "<method name='GetRelationSet'><arg direction='out' type='a(ua(so))'/></method>"
    "<method name='GetRole'><arg direction='out' type='u'/></method>"
    "<method name='GetRoleName'><arg direction='out' type='s'/></method>"
    "<method name='GetLocalizedRoleName'><arg direction='out' type='s'/></method>"
    "<method name='GetState'><arg direction='out' type='au'/></method>"
    "<method name='GetAttributes'><arg direction='out' type='a{ss}'/></method>"
    "<method name='GetApplication'><arg direction='out' type='(so)'/></method>"
    "<method name='GetInterfaces'><arg direction='out' type='as'/></method>"
    "</interface>",
    "<interface name='org.a11y.atspi.Component'>"
    "<method name='Contains'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='in' type='u'/><arg direction='out' type='b'/></method>"
    "<method name='GetAccessibleAtPoint'><arg direction='in' type='i'/><arg direction='in' "
    "type='i'/><arg direction='in' type='u'/><arg direction='out' type='(so)'/></method>"
    "<method name='GetExtents'><arg direction='in' type='u'/><arg direction='out' "
    "type='(iiii)'/></method>"
    "<method name='GetPosition'><arg direction='in' type='u'/><arg direction='out' type='i'/><arg "
    "direction='out' type='i'/></method>"
    "<method name='GetSize'><arg direction='out' type='i'/><arg direction='out' type='i'/></method>"
    "<method name='GetLayer'><arg direction='out' type='u'/></method>"
    "<method name='GetMDIZOrder'><arg direction='out' type='n'/></method>"
    "<method name='GrabFocus'><arg direction='out' type='b'/></method>"
    "<method name='GetAlpha'><arg direction='out' type='d'/></method>"
    "<method name='SetExtents'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='in' type='i'/><arg direction='in' type='u'/><arg "
    "direction='out' type='b'/></method>"
    "<method name='SetPosition'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='in' type='u'/><arg direction='out' type='b'/></method>"
    "<method name='SetSize'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='out' type='b'/></method>"
    "<method name='ScrollTo'><arg direction='in' type='u'/><arg direction='out' type='b'/></method>"
    "<method name='ScrollToPoint'><arg direction='in' type='u'/><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='out' type='b'/></method>"
    "</interface>",
    "<interface name='org.a11y.atspi.Action'>"
    "<property name='NActions' type='i' access='read'/>"
    "<method name='GetDescription'><arg direction='in' type='i'/><arg direction='out' "
    "type='s'/></method>"
    "<method name='GetName'><arg direction='in' type='i'/><arg direction='out' type='s'/></method>"
    "<method name='GetLocalizedName'><arg direction='in' type='i'/><arg direction='out' "
    "type='s'/></method>"
    "<method name='GetKeyBinding'><arg direction='in' type='i'/><arg direction='out' "
    "type='s'/></method>"
    "<method name='GetActions'><arg direction='out' type='a(sss)'/></method>"
    "<method name='DoAction'><arg direction='in' type='i'/><arg direction='out' type='b'/></method>"
    "</interface>",
    "<interface name='org.a11y.atspi.Text'>"
    "<property name='CharacterCount' type='i' access='read'/>"
    "<property name='CaretOffset' type='i' access='read'/>"
    "<property name='version' type='u' access='read'/>"
    "<method name='GetStringAtOffset'><arg direction='in' type='i'/><arg direction='in' "
    "type='u'/><arg direction='out' type='s'/><arg direction='out' type='i'/><arg direction='out' "
    "type='i'/></method>"
    "<method name='GetText'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='out' type='s'/></method>"
    "<method name='GetTextBeforeOffset'><arg direction='in' type='i'/><arg direction='in' "
    "type='u'/><arg direction='out' type='s'/><arg direction='out' type='i'/><arg direction='out' "
    "type='i'/></method>"
    "<method name='GetTextAtOffset'><arg direction='in' type='i'/><arg direction='in' "
    "type='u'/><arg direction='out' type='s'/><arg direction='out' type='i'/><arg direction='out' "
    "type='i'/></method>"
    "<method name='GetTextAfterOffset'><arg direction='in' type='i'/><arg direction='in' "
    "type='u'/><arg direction='out' type='s'/><arg direction='out' type='i'/><arg direction='out' "
    "type='i'/></method>"
    "<method name='GetCharacterAtOffset'><arg direction='in' type='i'/><arg direction='out' "
    "type='i'/></method>"
    "<method name='GetAttributeValue'><arg direction='in' type='i'/><arg direction='in' "
    "type='s'/><arg direction='out' type='s'/></method>"
    "<method name='GetAttributes'><arg direction='in' type='i'/><arg direction='out' "
    "type='a{ss}'/><arg direction='out' type='i'/><arg direction='out' type='i'/></method>"
    "<method name='GetDefaultAttributes'><arg direction='out' type='a{ss}'/></method>"
    "<method name='GetCharacterExtents'><arg direction='in' type='i'/><arg direction='in' "
    "type='u'/><arg direction='out' type='i'/><arg direction='out' type='i'/><arg direction='out' "
    "type='i'/><arg direction='out' type='i'/></method>"
    "<method name='GetOffsetAtPoint'><arg direction='in' type='i'/><arg direction='in' "
    "type='i'/><arg direction='in' type='u'/><arg direction='out' type='i'/></method>"
    "<method name='GetNSelections'><arg direction='out' type='i'/></method>"
    "<method name='GetSelection'><arg direction='in' type='i'/><arg direction='out' type='i'/><arg "
    "direction='out' type='i'/></method>"
    "<method name='SetCaretOffset'><arg direction='in' type='i'/><arg direction='out' "
    "type='b'/></method>"
    "</interface>",
    "<interface name='org.a11y.atspi.Value'>"
    "<property name='MinimumValue' type='d' access='read'/>"
    "<property name='MaximumValue' type='d' access='read'/>"
    "<property name='MinimumIncrement' type='d' access='read'/>"
    "<property name='CurrentValue' type='d' access='readwrite'/>"
    "<property name='Text' type='s' access='read'/>"
    "</interface>"
    "</node>",
    NULL,
};

/// @brief Emit an opt-in AT-SPI diagnostic for one bridge stage.
/// @param stage Stable description of the operation that failed.
/// @param error Optional borrowed GIO error supplying additional text.
static void rt_gui_atspi_diagnostic(const char *stage, const GError *error) {
    if (getenv("ZANNA_ATSPI_DIAGNOSTICS"))
        (void)fprintf(stderr,
                      "Zanna AT-SPI: %s%s%s\n",
                      stage,
                      error && error->message ? ": " : "",
                      error && error->message ? error->message : "");
}

/// @brief Map a Zanna accessible role to its stable AT-SPI role identifier.
/// @param role Zanna role to translate.
/// @return AT-SPI role value, defaulting to application for invalid input.
static uint32_t rt_gui_atspi_role(vg_accessible_role_t role) {
    static const uint32_t roles[VG_ACCESSIBLE_ROLE_COUNT] = {
        39, 75, 69, 99, 29, 43, 7,  44, 79, 79, 11, 31, 32, 65,  91, 38,
        37, 55, 90, 56, 51, 42, 16, 2,  33, 35, 63, 54, 27, 107, 88,
    };
    return role >= 0 && role < VG_ACCESSIBLE_ROLE_COUNT ? roles[role] : 39;
}

/// @brief Infer the accessible name exported for a widget snapshot.
/// @param widget Borrowed live widget.
/// @return Borrowed explicit or control-derived name; may be NULL.
static const char *rt_gui_atspi_node_name(const vg_widget_t *widget) {
    if (widget->accessibility.name)
        return widget->accessibility.name;
    switch (widget->type) {
        case VG_WIDGET_LABEL:
            return ((const vg_label_t *)widget)->text;
        case VG_WIDGET_BUTTON:
            return ((const vg_button_t *)widget)->text;
        case VG_WIDGET_TEXTINPUT: {
            const vg_textinput_t *input = (const vg_textinput_t *)widget;
            return input->placeholder ? input->placeholder : widget->name;
        }
        case VG_WIDGET_CHECKBOX:
            return ((const vg_checkbox_t *)widget)->text;
        case VG_WIDGET_RADIO:
            return ((const vg_radiobutton_t *)widget)->text;
        case VG_WIDGET_DROPDOWN: {
            const vg_dropdown_t *dropdown = (const vg_dropdown_t *)widget;
            return dropdown->placeholder ? dropdown->placeholder : widget->name;
        }
        case VG_WIDGET_GROUPBOX:
            return ((const vg_groupbox_t *)widget)->title;
        case VG_WIDGET_DIALOG:
            return ((const vg_dialog_t *)widget)->title;
        default:
            return widget->name;
    }
}

/// @brief Check whether a widget is enabled through its full ancestor chain.
/// @param widget Borrowed widget to inspect.
/// @return 1 when every node in the chain is enabled, otherwise 0.
static int rt_gui_atspi_effectively_enabled(const vg_widget_t *widget) {
    size_t depth = 0;
    for (const vg_widget_t *cursor = widget; cursor; cursor = cursor->parent) {
        if (depth++ >= RT_GUI_MAX_ACCESSIBILITY_NODES)
            return 0;
        if (!cursor->enabled)
            return 0;
    }
    return 1;
}

/// @brief Round a finite toolkit coordinate to the nearest signed integer.
/// @param value Coordinate value to round.
/// @return Nearest integer using symmetric half-away-from-zero behavior.
static int32_t rt_gui_atspi_round(float value) {
    double rounded = value >= 0.0f ? (double)value + 0.5 : (double)value - 0.5;
    return rt_gui_saturating_f64_to_i32(rounded);
}

/// @brief Count UTF-8 code points up to the AT-SPI signed range.
/// @param text Borrowed NUL-terminated UTF-8 text; may be NULL.
/// @return Code-point count saturated at `INT32_MAX`.
static int32_t rt_gui_atspi_utf8_count(const char *text) {
    int32_t count = 0;
    if (!text)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor)
        if ((*cursor & 0xC0u) != 0x80u && count < INT32_MAX)
            count++;
    return count;
}

/// @brief Populate optional Action, Text, and Value snapshot fields for a widget.
/// @param node Mutable destination node initialized for @p widget.
/// @param widget Borrowed live widget supplying interface state.
static void rt_gui_atspi_snapshot_interfaces(rt_gui_atspi_node_t *node, const vg_widget_t *widget) {
    const char *text = NULL;
    switch (widget->type) {
        case VG_WIDGET_LABEL:
            text = ((const vg_label_t *)widget)->text;
            break;
        case VG_WIDGET_BUTTON:
            text = ((const vg_button_t *)widget)->text;
            node->has_action = 1;
            break;
        case VG_WIDGET_TEXTINPUT: {
            const vg_textinput_t *input = (const vg_textinput_t *)widget;
            text = input->password_mode ? "" : input->text;
            node->has_text = 1;
            node->text_characters = input->password_mode ? (input->text_char_count > INT32_MAX
                                                                ? INT32_MAX
                                                                : (int32_t)input->text_char_count)
                                                         : 0;
            node->caret_offset =
                input->cursor_pos > INT32_MAX ? INT32_MAX : (int32_t)input->cursor_pos;
            node->selection_start =
                input->selection_start > INT32_MAX ? INT32_MAX : (int32_t)input->selection_start;
            node->selection_end =
                input->selection_end > INT32_MAX ? INT32_MAX : (int32_t)input->selection_end;
            break;
        }
        case VG_WIDGET_CHECKBOX:
            text = ((const vg_checkbox_t *)widget)->text;
            node->has_action = 1;
            break;
        case VG_WIDGET_RADIO:
            text = ((const vg_radiobutton_t *)widget)->text;
            node->has_action = 1;
            break;
        case VG_WIDGET_SLIDER: {
            const vg_slider_t *slider = (const vg_slider_t *)widget;
            node->has_value = 1;
            node->value = slider->value;
            node->value_minimum = slider->min_value;
            node->value_maximum = slider->max_value;
            node->value_increment = slider->step;
            break;
        }
        case VG_WIDGET_PROGRESS: {
            const vg_progressbar_t *progress = (const vg_progressbar_t *)widget;
            node->has_value = 1;
            node->value = progress->value;
            node->value_minimum = 0.0;
            node->value_maximum = 1.0;
            break;
        }
        case VG_WIDGET_SPINNER: {
            const vg_spinner_t *spinner = (const vg_spinner_t *)widget;
            node->has_value = 1;
            node->value = spinner->value;
            node->value_minimum = spinner->min_value;
            node->value_maximum = spinner->max_value;
            node->value_increment = spinner->step;
            break;
        }
        default:
            if (widget->accessibility.role == VG_ACCESSIBLE_ROLE_MENUITEM ||
                widget->accessibility.role == VG_ACCESSIBLE_ROLE_LINK)
                node->has_action = 1;
            break;
    }
    if (text) {
        node->has_text = 1;
        (void)snprintf(node->text, sizeof(node->text), "%s", text);
        if (node->text_characters == 0)
            node->text_characters = rt_gui_atspi_utf8_count(node->text);
    }
    if (node->has_value)
        (void)snprintf(node->value_text, sizeof(node->value_text), "%.17g", node->value);
    if (widget->type == VG_WIDGET_SPINNER && ((const vg_spinner_t *)widget)->indeterminate)
        (void)snprintf(node->value_text, sizeof(node->value_text), "mixed");
}

/// @brief Copy one live widget's stable accessible state into a snapshot node.
/// @param node Mutable destination node.
/// @param bridge Borrowed bridge that owns the resulting snapshot.
/// @param widget Borrowed live widget to copy.
/// @param index Preorder node index used to select the root object path.
static void rt_gui_atspi_snapshot_node(rt_gui_atspi_node_t *node,
                                       rt_gui_atspi_bridge_t *bridge,
                                       const vg_widget_t *widget,
                                       size_t index) {
    memset(node, 0, sizeof(*node));
    node->bridge = bridge;
    node->source = widget;
    node->widget_id = widget->id;
    node->parent_index = -1;
    if (index == 0)
        (void)snprintf(node->path, sizeof(node->path), "/org/a11y/atspi/accessible/root");
    else
        (void)snprintf(node->path,
                       sizeof(node->path),
                       "/org/a11y/atspi/accessible/widget_%llu",
                       (unsigned long long)widget->id);
    const char *name = rt_gui_atspi_node_name(widget);
    (void)snprintf(node->name, sizeof(node->name), "%s", name ? name : "");
    (void)snprintf(node->description,
                   sizeof(node->description),
                   "%s",
                   widget->accessibility.description ? widget->accessibility.description : "");
    (void)snprintf(
        node->accessible_id, sizeof(node->accessible_id), "%s", widget->name ? widget->name : "");
    node->role = rt_gui_atspi_role(widget->accessibility.role);
    rt_gui_atspi_snapshot_interfaces(node, widget);
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    vg_widget_get_screen_bounds(widget, &x, &y, &width, &height);
    node->x = rt_gui_atspi_round(x);
    node->y = rt_gui_atspi_round(y);
    node->width = rt_gui_atspi_round(width);
    node->height = rt_gui_atspi_round(height);
    if (rt_gui_atspi_effectively_enabled(widget)) {
        node->states[node->state_count++] = 8;
        node->states[node->state_count++] = 24;
    }
    if (widget->visible) {
        node->states[node->state_count++] = 25;
        node->states[node->state_count++] = 30;
    }
    if (widget->state & VG_STATE_FOCUSED)
        node->states[node->state_count++] = 12;
    if (widget->state & VG_STATE_PRESSED)
        node->states[node->state_count++] = 20;
    if (widget->state & VG_STATE_SELECTED)
        node->states[node->state_count++] = 23;
    if (widget->state & VG_STATE_CHECKED)
        node->states[node->state_count++] = 4;
    if (widget->accessibility.role == VG_ACCESSIBLE_ROLE_BUTTON ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_CHECKBOX ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_RADIOBUTTON ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_TEXTBOX ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_SEARCHBOX ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_COMBOBOX ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_SLIDER ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_LINK)
        node->states[node->state_count++] = 11;
    if (widget->type == VG_WIDGET_TEXTINPUT) {
        const vg_textinput_t *input = (const vg_textinput_t *)widget;
        node->states[node->state_count++] = input->multiline ? 17 : 26;
        if (!input->read_only)
            node->states[node->state_count++] = 7;
    }
    if (widget->accessibility.role == VG_ACCESSIBLE_ROLE_CHECKBOX ||
        widget->accessibility.role == VG_ACCESSIBLE_ROLE_RADIOBUTTON)
        node->states[node->state_count++] = 41;
}

/// @brief Build an iterative visible-tree snapshot and parent/child index storage.
/// @param bridge Bridge receiving ownership of the allocated snapshot arrays.
/// @param root Borrowed live semantic root.
/// @return 1 after a complete non-empty snapshot, otherwise 0.
static int rt_gui_atspi_build_snapshot(rt_gui_atspi_bridge_t *bridge, vg_widget_t *root) {
    size_t capacity = 64;
    size_t stack_count = 0;
    size_t node_count = 0;
    rt_gui_atspi_visit_t *stack = malloc(capacity * sizeof(*stack));
    rt_gui_atspi_node_t *nodes = malloc(capacity * sizeof(*nodes));
    if (!stack || !nodes) {
        free(stack);
        free(nodes);
        return 0;
    }
    stack[stack_count++] = (rt_gui_atspi_visit_t){root, -1};
    while (stack_count > 0) {
        rt_gui_atspi_visit_t visit = stack[--stack_count];
        vg_widget_t *widget = visit.widget;
        if (!widget || !widget->visible)
            continue;
        if (node_count >= RT_GUI_MAX_ACCESSIBILITY_NODES) {
            free(stack);
            free(nodes);
            return 0;
        }
        if (node_count == capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity > RT_GUI_MAX_ACCESSIBILITY_NODES)
                next_capacity = RT_GUI_MAX_ACCESSIBILITY_NODES;
            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*nodes) ||
                next_capacity > SIZE_MAX / sizeof(*stack)) {
                free(stack);
                free(nodes);
                return 0;
            }
            void *next_nodes = realloc(nodes, next_capacity * sizeof(*nodes));
            void *next_stack = realloc(stack, next_capacity * sizeof(*stack));
            if (!next_nodes || !next_stack) {
                free(next_nodes ? next_nodes : nodes);
                free(next_stack ? next_stack : stack);
                return 0;
            }
            nodes = next_nodes;
            stack = next_stack;
            capacity = next_capacity;
        }
        rt_gui_atspi_snapshot_node(&nodes[node_count], bridge, widget, node_count);
        nodes[node_count].parent_index = visit.parent_index;
        if (visit.parent_index >= 0)
            nodes[visit.parent_index].child_count++;
        int32_t parent_index = (int32_t)node_count;
        node_count++;
        for (vg_widget_t *child = widget->last_child; child; child = child->prev_sibling) {
            if (stack_count == capacity) {
                size_t next_capacity = capacity * 2;
                if (next_capacity > RT_GUI_MAX_ACCESSIBILITY_NODES)
                    next_capacity = RT_GUI_MAX_ACCESSIBILITY_NODES;
                if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*stack) ||
                    next_capacity > SIZE_MAX / sizeof(*nodes)) {
                    free(stack);
                    free(nodes);
                    return 0;
                }
                void *next_stack = realloc(stack, next_capacity * sizeof(*stack));
                void *next_nodes = realloc(nodes, next_capacity * sizeof(*nodes));
                if (!next_stack || !next_nodes) {
                    free(next_stack ? next_stack : stack);
                    free(next_nodes ? next_nodes : nodes);
                    return 0;
                }
                stack = next_stack;
                nodes = next_nodes;
                capacity = next_capacity;
            }
            stack[stack_count++] = (rt_gui_atspi_visit_t){child, parent_index};
        }
    }
    free(stack);
    if (node_count == 0) {
        free(nodes);
        return 0;
    }
    int32_t *children = node_count > 1 ? malloc((node_count - 1) * sizeof(*children)) : NULL;
    if (node_count > 1 && !children) {
        free(nodes);
        return 0;
    }
    size_t child_offset = 0;
    for (size_t parent = 0; parent < node_count; ++parent) {
        nodes[parent].children = children ? children + child_offset : NULL;
        child_offset += (size_t)nodes[parent].child_count;
    }
    int32_t *child_counts = calloc(node_count, sizeof(*child_counts));
    if (!child_counts) {
        free(children);
        free(nodes);
        return 0;
    }
    for (size_t i = 1; i < node_count; ++i) {
        int32_t parent = nodes[i].parent_index;
        if (parent >= 0)
            nodes[parent].children[child_counts[parent]++] = (int32_t)i;
    }
    free(child_counts);
    bridge->nodes = nodes;
    bridge->node_count = node_count;
    bridge->child_storage = children;
    return 1;
}

/// @brief Dynamically resolve the complete GIO surface required by the bridge.
/// @details Any missing library or symbol clears the process API table so later attachment remains
///          a deterministic no-op.
static void rt_gui_atspi_load_once(void) {
    const char *names[] = {"libgio-2.0.so.0", "libgio-2.0.so"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_rt_gui_atspi_api.library; ++i)
        g_rt_gui_atspi_api.library = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!g_rt_gui_atspi_api.library)
        return;
#define RT_ATSPI_LOAD(field, symbol)                                                               \
    do {                                                                                           \
        *(void **)(&g_rt_gui_atspi_api.field) = dlsym(g_rt_gui_atspi_api.library, symbol);         \
        if (!g_rt_gui_atspi_api.field) {                                                           \
            dlclose(g_rt_gui_atspi_api.library);                                                   \
            memset(&g_rt_gui_atspi_api, 0, sizeof(g_rt_gui_atspi_api));                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)
    RT_ATSPI_LOAD(bus_get_sync, "g_bus_get_sync");
    RT_ATSPI_LOAD(connection_call_sync, "g_dbus_connection_call_sync");
    RT_ATSPI_LOAD(connection_new_for_address_sync, "g_dbus_connection_new_for_address_sync");
    RT_ATSPI_LOAD(connection_get_unique_name, "g_dbus_connection_get_unique_name");
    RT_ATSPI_LOAD(connection_register_object, "g_dbus_connection_register_object");
    RT_ATSPI_LOAD(connection_unregister_object, "g_dbus_connection_unregister_object");
    RT_ATSPI_LOAD(connection_emit_signal, "g_dbus_connection_emit_signal");
    RT_ATSPI_LOAD(variant_new, "g_variant_new");
    RT_ATSPI_LOAD(variant_new_string, "g_variant_new_string");
    RT_ATSPI_LOAD(variant_new_int32, "g_variant_new_int32");
    RT_ATSPI_LOAD(variant_new_double, "g_variant_new_double");
    RT_ATSPI_LOAD(variant_new_fixed_array, "g_variant_new_fixed_array");
    RT_ATSPI_LOAD(variant_get_child_value, "g_variant_get_child_value");
    RT_ATSPI_LOAD(variant_get_string, "g_variant_get_string");
    RT_ATSPI_LOAD(variant_get_int32, "g_variant_get_int32");
    RT_ATSPI_LOAD(variant_get_uint32, "g_variant_get_uint32");
    RT_ATSPI_LOAD(variant_get_double, "g_variant_get_double");
    RT_ATSPI_LOAD(variant_n_children, "g_variant_n_children");
    RT_ATSPI_LOAD(variant_unref, "g_variant_unref");
    RT_ATSPI_LOAD(variant_builder_new, "g_variant_builder_new");
    RT_ATSPI_LOAD(variant_builder_add, "g_variant_builder_add");
    RT_ATSPI_LOAD(variant_builder_end, "g_variant_builder_end");
    RT_ATSPI_LOAD(variant_builder_unref, "g_variant_builder_unref");
    RT_ATSPI_LOAD(node_info_new_for_xml, "g_dbus_node_info_new_for_xml");
    RT_ATSPI_LOAD(node_info_lookup_interface, "g_dbus_node_info_lookup_interface");
    RT_ATSPI_LOAD(node_info_unref, "g_dbus_node_info_unref");
    RT_ATSPI_LOAD(invocation_return_value, "g_dbus_method_invocation_return_value");
    RT_ATSPI_LOAD(invocation_return_dbus_error, "g_dbus_method_invocation_return_dbus_error");
    RT_ATSPI_LOAD(main_context_new, "g_main_context_new");
    RT_ATSPI_LOAD(main_context_push_thread_default, "g_main_context_push_thread_default");
    RT_ATSPI_LOAD(main_context_pop_thread_default, "g_main_context_pop_thread_default");
    RT_ATSPI_LOAD(main_context_unref, "g_main_context_unref");
    RT_ATSPI_LOAD(main_loop_new, "g_main_loop_new");
    RT_ATSPI_LOAD(main_loop_run, "g_main_loop_run");
    RT_ATSPI_LOAD(main_loop_quit, "g_main_loop_quit");
    RT_ATSPI_LOAD(main_loop_unref, "g_main_loop_unref");
    RT_ATSPI_LOAD(idle_source_new, "g_idle_source_new");
    RT_ATSPI_LOAD(source_set_callback, "g_source_set_callback");
    RT_ATSPI_LOAD(source_attach, "g_source_attach");
    RT_ATSPI_LOAD(source_unref, "g_source_unref");
    RT_ATSPI_LOAD(object_unref, "g_object_unref");
    RT_ATSPI_LOAD(error_free, "g_error_free");
#undef RT_ATSPI_LOAD
}

/// @brief Return the standard AT-SPI not-implemented D-Bus error for a method.
/// @param invocation Borrowed method invocation to complete.
/// @param method Borrowed method name included in the diagnostic message.
static void rt_gui_atspi_return_error(GDBusMethodInvocation *invocation, const char *method) {
    char message[160];
    (void)snprintf(
        message, sizeof(message), "AT-SPI method %s is unavailable for this object", method);
    g_rt_gui_atspi_api.invocation_return_dbus_error(
        invocation, "org.a11y.atspi.Error.NotImplemented", message);
}

/// @brief Return a snapshot node's extents in screen or parent-relative coordinates.
/// @param node Borrowed immutable snapshot node.
/// @param coordinate_type AT-SPI coordinate system; value 2 selects parent-relative coordinates.
/// @param x Receives the left coordinate.
/// @param y Receives the top coordinate.
/// @param width Receives the width.
/// @param height Receives the height.
static void rt_gui_atspi_node_extents(const rt_gui_atspi_node_t *node,
                                      uint32_t coordinate_type,
                                      int32_t *x,
                                      int32_t *y,
                                      int32_t *width,
                                      int32_t *height) {
    *x = node->x;
    *y = node->y;
    *width = node->width;
    *height = node->height;
    if (coordinate_type == 2 && node->parent_index >= 0) {
        const rt_gui_atspi_node_t *parent = &node->bridge->nodes[node->parent_index];
        *x -= parent->x;
        *y -= parent->y;
    }
}

/// @brief Retain one child value from a D-Bus parameter tuple.
/// @param parameters Borrowed tuple variant.
/// @param index Zero-based child index.
/// @return Retained child variant, or NULL when unavailable.
static GVariant *rt_gui_atspi_child(GVariant *parameters, size_t index) {
    return g_rt_gui_atspi_api.variant_get_child_value(parameters, index);
}

/// @brief Read and release one signed 32-bit D-Bus parameter.
/// @param parameters Borrowed tuple variant.
/// @param index Zero-based child index.
/// @return Decoded value, or 0 when the child is unavailable.
static int32_t rt_gui_atspi_parameter_i32(GVariant *parameters, size_t index) {
    GVariant *value = rt_gui_atspi_child(parameters, index);
    int32_t result = value ? g_rt_gui_atspi_api.variant_get_int32(value) : 0;
    if (value)
        g_rt_gui_atspi_api.variant_unref(value);
    return result;
}

/// @brief Read and release one unsigned 32-bit D-Bus parameter.
/// @param parameters Borrowed tuple variant.
/// @param index Zero-based child index.
/// @return Decoded value, or 0 when the child is unavailable.
static uint32_t rt_gui_atspi_parameter_u32(GVariant *parameters, size_t index) {
    GVariant *value = rt_gui_atspi_child(parameters, index);
    uint32_t result = value ? g_rt_gui_atspi_api.variant_get_uint32(value) : 0;
    if (value)
        g_rt_gui_atspi_api.variant_unref(value);
    return result;
}

/// @brief Return the bridge connection's unique D-Bus name.
/// @param node Borrowed snapshot node whose bridge supplies the connection.
/// @return Borrowed unique name, or an empty string when unavailable.
static const char *rt_gui_atspi_unique(const rt_gui_atspi_node_t *node) {
    const char *unique = g_rt_gui_atspi_api.connection_get_unique_name(node->bridge->connection);
    return unique ? unique : "";
}

/// @brief Build an AT-SPI object-reference array for a node's children.
/// @param node Borrowed snapshot node.
/// @return Newly constructed array variant, or NULL on builder failure.
static GVariant *rt_gui_atspi_reference_array(const rt_gui_atspi_node_t *node) {
    GVariantBuilder *builder = g_rt_gui_atspi_api.variant_builder_new((const void *)"a(so)");
    if (!builder)
        return NULL;
    const char *unique = rt_gui_atspi_unique(node);
    for (int32_t i = 0; i < node->child_count; ++i) {
        const rt_gui_atspi_node_t *child = &node->bridge->nodes[node->children[i]];
        g_rt_gui_atspi_api.variant_builder_add(builder, "(so)", unique, child->path);
    }
    GVariant *result = g_rt_gui_atspi_api.variant_builder_end(builder);
    g_rt_gui_atspi_api.variant_builder_unref(builder);
    return result;
}

/// @brief Build an AT-SPI state-set variant from a snapshot node.
/// @param node Borrowed snapshot node.
/// @return Newly constructed fixed-array variant.
static GVariant *rt_gui_atspi_state_array(const rt_gui_atspi_node_t *node) {
    return g_rt_gui_atspi_api.variant_new_fixed_array(
        (const void *)"u", node->states, node->state_count, sizeof(node->states[0]));
}

/// @brief Construct an empty GVariant array of a requested signature.
/// @param type Borrowed GVariant array type string.
/// @return Newly constructed empty array variant, or NULL on builder failure.
static GVariant *rt_gui_atspi_empty_array(const char *type) {
    GVariantBuilder *builder = g_rt_gui_atspi_api.variant_builder_new((const void *)type);
    if (!builder)
        return NULL;
    GVariant *result = g_rt_gui_atspi_api.variant_builder_end(builder);
    g_rt_gui_atspi_api.variant_builder_unref(builder);
    return result;
}

/// @brief Return the stable English AT-SPI role name for a numeric role.
/// @param role AT-SPI role identifier.
/// @return Borrowed static role name, or `"unknown"`.
static const char *rt_gui_atspi_role_name(uint32_t role) {
    switch (role) {
        case 2:
            return "alert";
        case 7:
            return "check box";
        case 11:
            return "combo box";
        case 16:
            return "dialog";
        case 27:
            return "image";
        case 29:
            return "label";
        case 31:
            return "list";
        case 32:
            return "list item";
        case 33:
            return "menu";
        case 35:
            return "menu item";
        case 37:
            return "page tab";
        case 38:
            return "page tab list";
        case 39:
            return "panel";
        case 42:
            return "progress bar";
        case 43:
            return "push button";
        case 44:
            return "radio button";
        case 51:
            return "slider";
        case 54:
            return "status bar";
        case 55:
            return "table";
        case 56:
            return "table cell";
        case 63:
            return "tool bar";
        case 65:
            return "tree";
        case 69:
            return "window";
        case 75:
            return "application";
        case 79:
            return "entry";
        case 88:
            return "link";
        case 90:
            return "table row";
        case 91:
            return "tree item";
        case 99:
            return "grouping";
        case 107:
            return "video";
        default:
            return "unknown";
    }
}

/// @brief Find the deepest snapshotted descendant containing a screen-space point.
/// @param node Borrowed subtree root.
/// @param x Screen-space X coordinate.
/// @param y Screen-space Y coordinate.
/// @return Borrowed deepest matching node, or NULL when the point misses the subtree.
static const rt_gui_atspi_node_t *rt_gui_atspi_at_point(const rt_gui_atspi_node_t *node,
                                                        int32_t x,
                                                        int32_t y) {
    const rt_gui_atspi_node_t *match = NULL;
    for (size_t i = 0; i < node->bridge->node_count; ++i) {
        const rt_gui_atspi_node_t *candidate = &node->bridge->nodes[i];
        int32_t ancestor = (int32_t)i;
        while (ancestor >= 0 && &node->bridge->nodes[ancestor] != node)
            ancestor = node->bridge->nodes[ancestor].parent_index;
        if (ancestor < 0)
            continue;
        if (x >= candidate->x && y >= candidate->y && x < candidate->x + candidate->width &&
            y < candidate->y + candidate->height)
            match = candidate;
    }
    return match;
}

/// @brief Locate a code-point offset within a NUL-terminated UTF-8 string.
/// @param text Borrowed UTF-8 string; NULL is treated as empty.
/// @param offset Non-negative code-point offset.
/// @return Borrowed pointer to the selected byte or the terminating NUL.
static const char *rt_gui_atspi_utf8_offset(const char *text, int32_t offset) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    int32_t index = 0;
    while (*cursor && index < offset) {
        cursor++;
        while ((*cursor & 0xC0u) == 0x80u)
            cursor++;
        index++;
    }
    return (const char *)cursor;
}

/// @brief Decode the UTF-8 character at a code-point offset.
/// @param text Borrowed UTF-8 string.
/// @param offset Non-negative code-point offset.
/// @return Unicode scalar value in the signed runtime range, or 0 when unavailable/invalid.
static int32_t rt_gui_atspi_utf8_character(const char *text, int32_t offset) {
    const unsigned char *cursor = (const unsigned char *)rt_gui_atspi_utf8_offset(text, offset);
    if (!*cursor)
        return 0;
    uint32_t result = 0;
    int continuation = 0;
    if (*cursor < 0x80u) {
        result = *cursor;
    } else if ((*cursor & 0xE0u) == 0xC0u) {
        result = *cursor & 0x1Fu;
        continuation = 1;
    } else if ((*cursor & 0xF0u) == 0xE0u) {
        result = *cursor & 0x0Fu;
        continuation = 2;
    } else if ((*cursor & 0xF8u) == 0xF0u) {
        result = *cursor & 0x07u;
        continuation = 3;
    }
    for (int i = 0; i < continuation && (cursor[i + 1] & 0xC0u) == 0x80u; ++i)
        result = (result << 6u) | (cursor[i + 1] & 0x3Fu);
    return result <= INT32_MAX ? (int32_t)result : 0;
}

/// @brief Classify a character for AT-SPI word-boundary grouping.
/// @param character Unicode scalar value.
/// @return Non-zero for ASCII identifier characters or non-ASCII characters.
static int rt_gui_atspi_word_character(int32_t character) {
    return character >= 128 || (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           character == '_';
}

/// @brief Resolve an AT-SPI text granularity around a character offset.
/// @param node Borrowed text-bearing snapshot node.
/// @param offset Requested code-point offset.
/// @param granularity AT-SPI character, word, sentence, or line granularity.
/// @param start Receives the inclusive normalized code-point offset.
/// @param end Receives the exclusive normalized code-point offset.
static void rt_gui_atspi_text_range(const rt_gui_atspi_node_t *node,
                                    int32_t offset,
                                    uint32_t granularity,
                                    int32_t *start,
                                    int32_t *end) {
    int32_t count = node->text_characters;
    if (offset < 0)
        offset = 0;
    if (offset >= count) {
        *start = count;
        *end = count;
        return;
    }
    *start = offset;
    *end = offset + 1;
    if (granularity == 0)
        return;
    if (granularity == 1) {
        int word = rt_gui_atspi_word_character(rt_gui_atspi_utf8_character(node->text, offset));
        while (*start > 0 && rt_gui_atspi_word_character(
                                 rt_gui_atspi_utf8_character(node->text, *start - 1)) == word)
            (*start)--;
        while (*end < count &&
               rt_gui_atspi_word_character(rt_gui_atspi_utf8_character(node->text, *end)) == word)
            (*end)++;
        return;
    }
    int32_t separator = granularity == 2 ? '.' : '\n';
    while (*start > 0) {
        int32_t previous = rt_gui_atspi_utf8_character(node->text, *start - 1);
        if (previous == separator || (granularity == 2 && (previous == '!' || previous == '?')))
            break;
        (*start)--;
    }
    while (*end < count) {
        int32_t current = rt_gui_atspi_utf8_character(node->text, *end - 1);
        if (current == separator || (granularity == 2 && (current == '!' || current == '?')))
            break;
        (*end)++;
    }
}

/// @brief Return a bounded UTF-8 substring and its code-point range to D-Bus.
/// @param invocation Borrowed method invocation to complete.
/// @param node Borrowed text-bearing snapshot node.
/// @param start Inclusive code-point offset.
/// @param end Exclusive code-point offset.
static void rt_gui_atspi_return_text_range(GDBusMethodInvocation *invocation,
                                           const rt_gui_atspi_node_t *node,
                                           int32_t start,
                                           int32_t end) {
    const char *begin = rt_gui_atspi_utf8_offset(node->text, start);
    const char *finish = rt_gui_atspi_utf8_offset(node->text, end);
    char substring[sizeof(node->text)];
    size_t length = (size_t)(finish - begin);
    if (length >= sizeof(substring))
        length = sizeof(substring) - 1;
    memcpy(substring, begin, length);
    substring[length] = '\0';
    g_rt_gui_atspi_api.invocation_return_value(
        invocation, g_rt_gui_atspi_api.variant_new("(sii)", substring, start, end));
}

enum {
    RT_GUI_ATSPI_REQUEST_ACTION = 1,
    RT_GUI_ATSPI_REQUEST_CARET = 2,
    RT_GUI_ATSPI_REQUEST_VALUE = 3,
};

/// @brief Queue one GUI-thread action and wait briefly for its completion result.
/// @param bridge Bridge whose synchronized request slot is used.
/// @param widget_id Stable target widget ID.
/// @param kind Action, caret, or value request kind.
/// @param value Numeric request payload.
/// @return GUI-thread result, or 0 when busy or timed out.
static int rt_gui_atspi_request_values(rt_gui_atspi_bridge_t *bridge,
                                       uint64_t widget_id,
                                       int kind,
                                       double value) {
    pthread_mutex_lock(&bridge->mutex);
    if (bridge->request_pending) {
        pthread_mutex_unlock(&bridge->mutex);
        return 0;
    }
    bridge->request_pending = 1;
    bridge->request_kind = kind;
    bridge->request_widget_id = widget_id;
    bridge->request_value = value;
    bridge->request_serial++;
    if (bridge->request_serial == 0)
        bridge->request_serial = 1;
    uint64_t serial = bridge->request_serial;
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 750000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (bridge->completed_serial < serial) {
        int result = pthread_cond_timedwait(&bridge->condition, &bridge->mutex, &deadline);
        if (result != 0)
            break;
    }
    int result = bridge->completed_serial >= serial ? bridge->request_result : 0;
    pthread_mutex_unlock(&bridge->mutex);
    return result;
}

/// @brief Submit a GUI-thread request using a snapshot node's guarded identity.
/// @param node Snapshot node naming the target widget and bridge.
/// @param kind Action, caret, or value request kind.
/// @param value Numeric request payload.
/// @return GUI-thread result, or 0 when unavailable.
static int rt_gui_atspi_request(rt_gui_atspi_node_t *node, int kind, double value) {
    return rt_gui_atspi_request_values(node->bridge, node->widget_id, kind, value);
}

/// @brief Find a snapshot node's zero-based index among its parent's children.
/// @param node Borrowed snapshot node.
/// @return Child index, or -1 for the root or inconsistent storage.
static int32_t rt_gui_atspi_index_in_parent(const rt_gui_atspi_node_t *node) {
    if (node->parent_index < 0)
        return -1;
    const rt_gui_atspi_node_t *parent = &node->bridge->nodes[node->parent_index];
    for (int32_t i = 0; i < parent->child_count; ++i)
        if (&node->bridge->nodes[parent->children[i]] == node)
            return i;
    return -1;
}

/// @brief Serialize every snapshot node into the AT-SPI cache item schema.
/// @param bridge Borrowed bridge containing immutable snapshot nodes.
/// @return Newly constructed cache-item array variant, or NULL on builder failure.
static GVariant *rt_gui_atspi_cache_items(const rt_gui_atspi_bridge_t *bridge) {
    const char *unique = g_rt_gui_atspi_api.connection_get_unique_name(bridge->connection);
    GVariantBuilder *items =
        g_rt_gui_atspi_api.variant_builder_new((const void *)"a((so)(so)(so)iiassusau)");
    if (!items)
        return NULL;
    for (size_t i = 0; i < bridge->node_count; ++i) {
        const rt_gui_atspi_node_t *node = &bridge->nodes[i];
        const char *interfaces[7] = {
            "org.a11y.atspi.Accessible", "org.a11y.atspi.Component", NULL, NULL, NULL, NULL, NULL};
        size_t count = 2;
        if (i == 0)
            interfaces[count++] = "org.a11y.atspi.Application";
        if (node->has_action)
            interfaces[count++] = "org.a11y.atspi.Action";
        if (node->has_text)
            interfaces[count++] = "org.a11y.atspi.Text";
        if (node->has_value)
            interfaces[count++] = "org.a11y.atspi.Value";
        GVariantBuilder *interface_builder =
            g_rt_gui_atspi_api.variant_builder_new((const void *)"as");
        for (size_t j = 0; j < count; ++j)
            g_rt_gui_atspi_api.variant_builder_add(interface_builder, "s", interfaces[j]);
        GVariant *interface_array = g_rt_gui_atspi_api.variant_builder_end(interface_builder);
        g_rt_gui_atspi_api.variant_builder_unref(interface_builder);
        GVariant *states = rt_gui_atspi_state_array(node);
        const char *parent_path = node->parent_index >= 0 ? bridge->nodes[node->parent_index].path
                                                          : "/org/a11y/atspi/null";
        g_rt_gui_atspi_api.variant_builder_add(items,
                                               "((so)(so)(so)ii@assus@au)",
                                               unique ? unique : "",
                                               node->path,
                                               unique ? unique : "",
                                               "/org/a11y/atspi/accessible/root",
                                               node->parent_index >= 0 && unique ? unique : "",
                                               parent_path,
                                               rt_gui_atspi_index_in_parent(node),
                                               node->child_count,
                                               interface_array,
                                               node->name,
                                               node->role,
                                               node->description,
                                               states);
    }
    GVariant *result = g_rt_gui_atspi_api.variant_builder_end(items);
    g_rt_gui_atspi_api.variant_builder_unref(items);
    return result;
}

/// @brief Dispatch one AT-SPI method call against immutable snapshot state.
/// @param connection Borrowed D-Bus connection; currently unused.
/// @param sender Borrowed sender name; currently unused.
/// @param path Borrowed object path; registration already selected @p data.
/// @param interface_name Borrowed AT-SPI interface name.
/// @param method Borrowed method name.
/// @param parameters Borrowed method parameter tuple.
/// @param invocation Borrowed invocation completed by this callback.
/// @param data Borrowed registered snapshot node.
static void rt_gui_atspi_method(GDBusConnection *connection,
                                const char *sender,
                                const char *path,
                                const char *interface_name,
                                const char *method,
                                GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                void *data) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)parameters;
    rt_gui_atspi_node_t *node = data;
    rt_gui_atspi_bridge_t *bridge = node->bridge;
    if (strcmp(interface_name, "org.a11y.atspi.Application") == 0 &&
        strcmp(method, "GetLocale") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(s)", bridge->locale));
    } else if (strcmp(interface_name, "org.a11y.atspi.Cache") == 0 &&
               strcmp(method, "GetItems") == 0) {
        GVariant *items = rt_gui_atspi_cache_items(bridge);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(@a((so)(so)(so)iiassusau))", items));
    } else if (strcmp(interface_name, "org.a11y.atspi.Accessible") == 0 &&
               strcmp(method, "GetChildAtIndex") == 0) {
        int32_t index = rt_gui_atspi_parameter_i32(parameters, 0);
        if (index < 0 || index >= node->child_count) {
            rt_gui_atspi_return_error(invocation, method);
        } else {
            const rt_gui_atspi_node_t *child = &bridge->nodes[node->children[index]];
            g_rt_gui_atspi_api.invocation_return_value(
                invocation,
                g_rt_gui_atspi_api.variant_new("((so))", rt_gui_atspi_unique(node), child->path));
        }
    } else if (strcmp(interface_name, "org.a11y.atspi.Accessible") == 0 &&
               strcmp(method, "GetChildren") == 0) {
        GVariant *children = rt_gui_atspi_reference_array(node);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(@a(so))", children));
    } else if (strcmp(method, "GetIndexInParent") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(i)", rt_gui_atspi_index_in_parent(node)));
    } else if (strcmp(method, "GetRelationSet") == 0) {
        GVariant *relations = rt_gui_atspi_empty_array("a(ua(so))");
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(@a(ua(so)))", relations));
    } else if (strcmp(method, "GetRole") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(u)", node->role));
    } else if (strcmp(method, "GetRoleName") == 0 || strcmp(method, "GetLocalizedRoleName") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(s)", rt_gui_atspi_role_name(node->role)));
    } else if (strcmp(method, "GetState") == 0) {
        GVariant *array = rt_gui_atspi_state_array(node);
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(@au)", array));
    } else if (strcmp(method, "GetAttributes") == 0) {
        GVariant *attributes = rt_gui_atspi_empty_array("a{ss}");
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(@a{ss})", attributes));
    } else if (strcmp(method, "GetApplication") == 0) {
        const char *unique = g_rt_gui_atspi_api.connection_get_unique_name(bridge->connection);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new(
                "((so))", unique ? unique : "", "/org/a11y/atspi/accessible/root"));
    } else if (strcmp(method, "GetInterfaces") == 0) {
        const char *interfaces[7] = {
            "org.a11y.atspi.Accessible", "org.a11y.atspi.Component", NULL, NULL, NULL, NULL, NULL};
        size_t interface_count = 2;
        if (node == &bridge->nodes[0])
            interfaces[interface_count++] = "org.a11y.atspi.Application";
        if (node->has_action)
            interfaces[interface_count++] = "org.a11y.atspi.Action";
        if (node->has_text)
            interfaces[interface_count++] = "org.a11y.atspi.Text";
        if (node->has_value)
            interfaces[interface_count++] = "org.a11y.atspi.Value";
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(^as)", interfaces));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "Contains") == 0) {
        int32_t x = rt_gui_atspi_parameter_i32(parameters, 0);
        int32_t y = rt_gui_atspi_parameter_i32(parameters, 1);
        uint32_t coordinate_type = rt_gui_atspi_parameter_u32(parameters, 2);
        int32_t nx, ny, nw, nh;
        rt_gui_atspi_node_extents(node, coordinate_type, &nx, &ny, &nw, &nh);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new("(b)",
                                           x >= nx && y >= ny && x < nx + nw && y < ny + nh));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetAccessibleAtPoint") == 0) {
        int32_t x = rt_gui_atspi_parameter_i32(parameters, 0);
        int32_t y = rt_gui_atspi_parameter_i32(parameters, 1);
        uint32_t coordinate_type = rt_gui_atspi_parameter_u32(parameters, 2);
        if (coordinate_type == 2) {
            x += node->x;
            y += node->y;
        }
        const rt_gui_atspi_node_t *match = rt_gui_atspi_at_point(node, x, y);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new("((so))",
                                           match ? rt_gui_atspi_unique(node) : "",
                                           match ? match->path : "/org/a11y/atspi/null"));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetExtents") == 0) {
        int32_t x, y, width, height;
        rt_gui_atspi_node_extents(
            node, rt_gui_atspi_parameter_u32(parameters, 0), &x, &y, &width, &height);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("((iiii))", x, y, width, height));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetPosition") == 0) {
        int32_t x, y, width, height;
        rt_gui_atspi_node_extents(
            node, rt_gui_atspi_parameter_u32(parameters, 0), &x, &y, &width, &height);
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(ii)", x, y));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetSize") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(ii)", node->width, node->height));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetLayer") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(u)", node->parent_index < 0 ? 7u : 3u));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetMDIZOrder") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(n)", (int)-1));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               strcmp(method, "GetAlpha") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(d)", 1.0));
    } else if (strcmp(interface_name, "org.a11y.atspi.Component") == 0 &&
               (strcmp(method, "GrabFocus") == 0 || strcmp(method, "SetExtents") == 0 ||
                strcmp(method, "SetPosition") == 0 || strcmp(method, "SetSize") == 0 ||
                strcmp(method, "ScrollTo") == 0 || strcmp(method, "ScrollToPoint") == 0)) {
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(b)", 0));
    } else if (strcmp(interface_name, "org.a11y.atspi.Action") == 0 &&
               (strcmp(method, "GetDescription") == 0 || strcmp(method, "GetName") == 0 ||
                strcmp(method, "GetLocalizedName") == 0 || strcmp(method, "GetKeyBinding") == 0)) {
        int32_t index = rt_gui_atspi_parameter_i32(parameters, 0);
        const char *result = "";
        if (index == 0) {
            if (strcmp(method, "GetDescription") == 0)
                result = node->description[0] ? node->description : "Activate this control";
            else if (strcmp(method, "GetKeyBinding") != 0)
                result = "activate";
        }
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(s)", result));
    } else if (strcmp(interface_name, "org.a11y.atspi.Action") == 0 &&
               strcmp(method, "GetActions") == 0) {
        GVariantBuilder *actions = g_rt_gui_atspi_api.variant_builder_new((const void *)"a(sss)");
        g_rt_gui_atspi_api.variant_builder_add(actions, "(sss)", "activate", node->description, "");
        GVariant *array = g_rt_gui_atspi_api.variant_builder_end(actions);
        g_rt_gui_atspi_api.variant_builder_unref(actions);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(@a(sss))", array));
    } else if (strcmp(interface_name, "org.a11y.atspi.Action") == 0 &&
               strcmp(method, "DoAction") == 0) {
        int32_t index = rt_gui_atspi_parameter_i32(parameters, 0);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new(
                "(b)",
                index == 0 ? rt_gui_atspi_request(node, RT_GUI_ATSPI_REQUEST_ACTION, 0.0) : 0));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetStringAtOffset") == 0) {
        int32_t start, end;
        rt_gui_atspi_text_range(node,
                                rt_gui_atspi_parameter_i32(parameters, 0),
                                rt_gui_atspi_parameter_u32(parameters, 1),
                                &start,
                                &end);
        rt_gui_atspi_return_text_range(invocation, node, start, end);
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               (strcmp(method, "GetTextBeforeOffset") == 0 ||
                strcmp(method, "GetTextAtOffset") == 0 ||
                strcmp(method, "GetTextAfterOffset") == 0)) {
        int32_t offset = rt_gui_atspi_parameter_i32(parameters, 0);
        uint32_t boundary = rt_gui_atspi_parameter_u32(parameters, 1);
        uint32_t granularity = boundary == 0 ? 0 : boundary <= 2 ? 1 : boundary <= 4 ? 2 : 3;
        int32_t start, end;
        rt_gui_atspi_text_range(node, offset, granularity, &start, &end);
        if (strcmp(method, "GetTextBeforeOffset") == 0)
            rt_gui_atspi_text_range(node, start - 1, granularity, &start, &end);
        else if (strcmp(method, "GetTextAfterOffset") == 0)
            rt_gui_atspi_text_range(node, end, granularity, &start, &end);
        rt_gui_atspi_return_text_range(invocation, node, start, end);
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetText") == 0) {
        int32_t start = rt_gui_atspi_parameter_i32(parameters, 0);
        int32_t end = rt_gui_atspi_parameter_i32(parameters, 1);
        if (start < 0)
            start = 0;
        if (end < 0 || end > node->text_characters)
            end = node->text_characters;
        if (start > end)
            start = end;
        const char *begin = rt_gui_atspi_utf8_offset(node->text, start);
        const char *finish = rt_gui_atspi_utf8_offset(node->text, end);
        char substring[sizeof(node->text)];
        size_t length = (size_t)(finish - begin);
        if (length >= sizeof(substring))
            length = sizeof(substring) - 1;
        memcpy(substring, begin, length);
        substring[length] = '\0';
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(s)", substring));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetCharacterAtOffset") == 0) {
        int32_t offset = rt_gui_atspi_parameter_i32(parameters, 0);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new(
                "(i)", offset >= 0 ? rt_gui_atspi_utf8_character(node->text, offset) : 0));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetAttributeValue") == 0) {
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(s)", ""));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               (strcmp(method, "GetAttributes") == 0 ||
                strcmp(method, "GetDefaultAttributes") == 0)) {
        GVariant *attributes = rt_gui_atspi_empty_array("a{ss}");
        if (strcmp(method, "GetAttributes") == 0)
            g_rt_gui_atspi_api.invocation_return_value(
                invocation,
                g_rt_gui_atspi_api.variant_new("(@a{ss}ii)", attributes, 0, node->text_characters));
        else
            g_rt_gui_atspi_api.invocation_return_value(
                invocation, g_rt_gui_atspi_api.variant_new("(@a{ss})", attributes));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetCharacterExtents") == 0) {
        int32_t offset = rt_gui_atspi_parameter_i32(parameters, 0);
        int32_t width = node->text_characters > 0 ? node->width / node->text_characters : 0;
        int32_t x = node->x + width * offset;
        if (offset < 0 || offset >= node->text_characters)
            x = width = 0;
        g_rt_gui_atspi_api.invocation_return_value(
            invocation, g_rt_gui_atspi_api.variant_new("(iiii)", x, node->y, width, node->height));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetOffsetAtPoint") == 0) {
        int32_t x = rt_gui_atspi_parameter_i32(parameters, 0);
        int32_t width = node->text_characters > 0 ? node->width / node->text_characters : 0;
        int32_t offset = width > 0 ? (x - node->x) / width : -1;
        if (offset < 0 || offset >= node->text_characters)
            offset = -1;
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(i)", offset));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetNSelections") == 0) {
        int selected = node->selection_start != node->selection_end;
        g_rt_gui_atspi_api.invocation_return_value(invocation,
                                                   g_rt_gui_atspi_api.variant_new("(i)", selected));
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "GetSelection") == 0) {
        int32_t selection = rt_gui_atspi_parameter_i32(parameters, 0);
        if (selection != 0 || node->selection_start == node->selection_end) {
            rt_gui_atspi_return_error(invocation, method);
        } else {
            g_rt_gui_atspi_api.invocation_return_value(
                invocation,
                g_rt_gui_atspi_api.variant_new("(ii)", node->selection_start, node->selection_end));
        }
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0 &&
               strcmp(method, "SetCaretOffset") == 0) {
        int32_t offset = rt_gui_atspi_parameter_i32(parameters, 0);
        g_rt_gui_atspi_api.invocation_return_value(
            invocation,
            g_rt_gui_atspi_api.variant_new(
                "(b)",
                offset >= 0 && offset <= node->text_characters
                    ? rt_gui_atspi_request(node, RT_GUI_ATSPI_REQUEST_CARET, offset)
                    : 0));
    } else if (strcmp(interface_name, "org.a11y.atspi.Accessible") == 0) {
        rt_gui_atspi_return_error(invocation, method);
    }
}

/// @brief Materialize one readable AT-SPI property from immutable snapshot state.
/// @param connection Borrowed D-Bus connection; currently unused.
/// @param sender Borrowed sender name; currently unused.
/// @param path Borrowed object path; registration already selected @p data.
/// @param interface_name Borrowed AT-SPI interface name.
/// @param property Borrowed property name.
/// @param error Optional error destination; currently unused.
/// @param data Borrowed registered snapshot node.
/// @return Newly constructed property variant, or NULL for an unsupported property.
static GVariant *rt_gui_atspi_get_property(GDBusConnection *connection,
                                           const char *sender,
                                           const char *path,
                                           const char *interface_name,
                                           const char *property,
                                           GError **error,
                                           void *data) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)error;
    rt_gui_atspi_node_t *node = data;
    rt_gui_atspi_bridge_t *bridge = node->bridge;
    if (strcmp(interface_name, "org.a11y.atspi.Application") == 0) {
        if (strcmp(property, "ToolkitName") == 0)
            return g_rt_gui_atspi_api.variant_new_string("ZannaGUI");
        if (strcmp(property, "Version") == 0)
            return g_rt_gui_atspi_api.variant_new_string("0.3.0");
        if (strcmp(property, "AtspiVersion") == 0)
            return g_rt_gui_atspi_api.variant_new_string("2.1");
        if (strcmp(property, "Id") == 0)
            return g_rt_gui_atspi_api.variant_new_int32(bridge->application_id);
    } else if (strcmp(interface_name, "org.a11y.atspi.Cache") == 0) {
        if (strcmp(property, "version") == 0)
            return g_rt_gui_atspi_api.variant_new("u", 2u);
    } else if (strcmp(interface_name, "org.a11y.atspi.Accessible") == 0) {
        if (strcmp(property, "Name") == 0)
            return g_rt_gui_atspi_api.variant_new_string(node->name);
        if (strcmp(property, "Description") == 0)
            return g_rt_gui_atspi_api.variant_new_string(node->description);
        if (strcmp(property, "AccessibleId") == 0)
            return g_rt_gui_atspi_api.variant_new_string(node->accessible_id);
        if (strcmp(property, "HelpText") == 0)
            return g_rt_gui_atspi_api.variant_new_string("");
        if (strcmp(property, "Parent") == 0) {
            if (node->parent_index < 0)
                return g_rt_gui_atspi_api.variant_new("(so)", "", "/org/a11y/atspi/null");
            return g_rt_gui_atspi_api.variant_new(
                "(so)", rt_gui_atspi_unique(node), bridge->nodes[node->parent_index].path);
        }
        if (strcmp(property, "ChildCount") == 0)
            return g_rt_gui_atspi_api.variant_new_int32(node->child_count);
        if (strcmp(property, "Locale") == 0)
            return g_rt_gui_atspi_api.variant_new_string(bridge->locale);
    } else if (strcmp(interface_name, "org.a11y.atspi.Action") == 0 &&
               strcmp(property, "NActions") == 0) {
        return g_rt_gui_atspi_api.variant_new_int32(node->has_action ? 1 : 0);
    } else if (strcmp(interface_name, "org.a11y.atspi.Text") == 0) {
        if (strcmp(property, "version") == 0)
            return g_rt_gui_atspi_api.variant_new("u", 1u);
        if (strcmp(property, "CharacterCount") == 0)
            return g_rt_gui_atspi_api.variant_new_int32(node->text_characters);
        if (strcmp(property, "CaretOffset") == 0)
            return g_rt_gui_atspi_api.variant_new_int32(node->caret_offset);
    } else if (strcmp(interface_name, "org.a11y.atspi.Value") == 0) {
        if (strcmp(property, "MinimumValue") == 0)
            return g_rt_gui_atspi_api.variant_new_double(node->value_minimum);
        if (strcmp(property, "MaximumValue") == 0)
            return g_rt_gui_atspi_api.variant_new_double(node->value_maximum);
        if (strcmp(property, "MinimumIncrement") == 0)
            return g_rt_gui_atspi_api.variant_new_double(node->value_increment);
        if (strcmp(property, "CurrentValue") == 0)
            return g_rt_gui_atspi_api.variant_new_double(node->value);
        if (strcmp(property, "Text") == 0)
            return g_rt_gui_atspi_api.variant_new_string(node->value_text);
    }
    return NULL;
}

/// @brief Apply one writable AT-SPI property through bridge or GUI-thread state.
/// @param connection Borrowed D-Bus connection; currently unused.
/// @param sender Borrowed sender name; currently unused.
/// @param path Borrowed object path; registration already selected @p data.
/// @param interface_name Borrowed AT-SPI interface name.
/// @param property Borrowed property name.
/// @param value Borrowed replacement value.
/// @param error Optional error destination; currently unused.
/// @param data Borrowed registered snapshot node.
/// @return Non-zero when the property was accepted, otherwise 0.
static int rt_gui_atspi_set_property(GDBusConnection *connection,
                                     const char *sender,
                                     const char *path,
                                     const char *interface_name,
                                     const char *property,
                                     GVariant *value,
                                     GError **error,
                                     void *data) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)error;
    rt_gui_atspi_node_t *node = data;
    rt_gui_atspi_bridge_t *bridge = node->bridge;
    if (strcmp(interface_name, "org.a11y.atspi.Application") == 0 && strcmp(property, "Id") == 0) {
        bridge->application_id = g_rt_gui_atspi_api.variant_get_int32(value);
        return 1;
    }
    if (strcmp(interface_name, "org.a11y.atspi.Value") == 0 &&
        strcmp(property, "CurrentValue") == 0) {
        return rt_gui_atspi_request(
            node, RT_GUI_ATSPI_REQUEST_VALUE, g_rt_gui_atspi_api.variant_get_double(value));
    }
    return 0;
}

static const rt_gdbus_interface_vtable_t g_rt_gui_atspi_vtable = {
    rt_gui_atspi_method,
    rt_gui_atspi_get_property,
    rt_gui_atspi_set_property,
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

/// @brief Publish worker initialization completion and wake the attaching thread.
/// @param bridge Bridge whose synchronized readiness fields are updated.
/// @param started Non-zero when the private D-Bus main loop can run.
static void rt_gui_atspi_signal_ready(rt_gui_atspi_bridge_t *bridge, int started) {
    pthread_mutex_lock(&bridge->mutex);
    bridge->started = started;
    bridge->ready = 1;
    pthread_cond_signal(&bridge->condition);
    pthread_mutex_unlock(&bridge->mutex);
}

/// @brief Mark the bridge started from the first idle turn of its private context.
/// @param data Borrowed bridge pointer.
/// @return 0 so the one-shot idle source is removed.
static int rt_gui_atspi_signal_loop_running(void *data) {
    rt_gui_atspi_signal_ready(data, 1);
    return 0;
}

/// @brief Concatenate the split AT-SPI introspection document.
/// @return Malloc-owned NUL-terminated XML, or NULL on overflow/allocation failure.
static char *rt_gui_atspi_xml(void) {
    size_t total = 1;
    for (size_t i = 0; g_rt_gui_atspi_xml_parts[i]; ++i) {
        size_t length = strlen(g_rt_gui_atspi_xml_parts[i]);
        if (length > SIZE_MAX - total)
            return NULL;
        total += length;
    }
    char *xml = malloc(total);
    if (!xml)
        return NULL;
    size_t offset = 0;
    for (size_t i = 0; g_rt_gui_atspi_xml_parts[i]; ++i) {
        size_t length = strlen(g_rt_gui_atspi_xml_parts[i]);
        memcpy(xml + offset, g_rt_gui_atspi_xml_parts[i], length);
        offset += length;
    }
    xml[offset] = '\0';
    return xml;
}

/// @brief Discover the accessibility bus, register snapshot objects, and run the private loop.
/// @param data Borrowed bridge whose worker-owned GIO resources are initialized and released.
/// @return Always NULL after the loop exits or initialization fails.
static void *rt_gui_atspi_worker(void *data) {
    rt_gui_atspi_bridge_t *bridge = data;
    rt_gui_atspi_api_t *api = &g_rt_gui_atspi_api;
    GError *error = NULL;
    bridge->context = api->main_context_new();
    if (!bridge->context) {
        rt_gui_atspi_signal_ready(bridge, 0);
        return NULL;
    }
    api->main_context_push_thread_default(bridge->context);
    GDBusConnection *session = api->bus_get_sync(2, NULL, &error);
    GVariant *address_result = session ? api->connection_call_sync(session,
                                                                   "org.a11y.Bus",
                                                                   "/org/a11y/bus",
                                                                   "org.a11y.Bus",
                                                                   "GetAddress",
                                                                   NULL,
                                                                   NULL,
                                                                   0,
                                                                   500,
                                                                   NULL,
                                                                   &error)
                                       : NULL;
    if (session)
        api->object_unref(session);
    if (!address_result)
        rt_gui_atspi_diagnostic("accessibility bus discovery failed", error);
    GVariant *address_value =
        address_result ? api->variant_get_child_value(address_result, 0) : NULL;
    const char *address = address_value ? api->variant_get_string(address_value, NULL) : NULL;
    if (address)
        bridge->connection = api->connection_new_for_address_sync(address, 9, NULL, NULL, &error);
    if (!bridge->connection)
        rt_gui_atspi_diagnostic("accessibility bus connection failed", error);
    if (address_value)
        api->variant_unref(address_value);
    if (address_result)
        api->variant_unref(address_result);
    char *xml = rt_gui_atspi_xml();
    bridge->node_info = xml ? api->node_info_new_for_xml(xml, &error) : NULL;
    free(xml);
    if (!bridge->node_info)
        rt_gui_atspi_diagnostic("interface metadata parse failed", error);
    GDBusInterfaceInfo *accessible =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Accessible")
            : NULL;
    GDBusInterfaceInfo *application =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Application")
            : NULL;
    GDBusInterfaceInfo *cache =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Cache")
            : NULL;
    GDBusInterfaceInfo *component =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Component")
            : NULL;
    GDBusInterfaceInfo *action =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Action")
            : NULL;
    GDBusInterfaceInfo *text =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Text")
            : NULL;
    GDBusInterfaceInfo *value =
        bridge->node_info
            ? api->node_info_lookup_interface(bridge->node_info, "org.a11y.atspi.Value")
            : NULL;
    int all_nodes_registered = 1;
    if (bridge->connection && accessible && application && component) {
        for (size_t i = 0; i < bridge->node_count; ++i) {
            rt_gui_atspi_node_t *node = &bridge->nodes[i];
            node->accessible_registration = api->connection_register_object(bridge->connection,
                                                                            node->path,
                                                                            accessible,
                                                                            &g_rt_gui_atspi_vtable,
                                                                            node,
                                                                            NULL,
                                                                            &error);
            node->component_registration = api->connection_register_object(bridge->connection,
                                                                           node->path,
                                                                           component,
                                                                           &g_rt_gui_atspi_vtable,
                                                                           node,
                                                                           NULL,
                                                                           &error);
            if (node->has_action && action)
                node->action_registration = api->connection_register_object(bridge->connection,
                                                                            node->path,
                                                                            action,
                                                                            &g_rt_gui_atspi_vtable,
                                                                            node,
                                                                            NULL,
                                                                            &error);
            if (node->has_text && text)
                node->text_registration = api->connection_register_object(bridge->connection,
                                                                          node->path,
                                                                          text,
                                                                          &g_rt_gui_atspi_vtable,
                                                                          node,
                                                                          NULL,
                                                                          &error);
            if (node->has_value && value)
                node->value_registration = api->connection_register_object(bridge->connection,
                                                                           node->path,
                                                                           value,
                                                                           &g_rt_gui_atspi_vtable,
                                                                           node,
                                                                           NULL,
                                                                           &error);
            if (!node->accessible_registration || !node->component_registration ||
                (node->has_action && !node->action_registration) ||
                (node->has_text && !node->text_registration) ||
                (node->has_value && !node->value_registration)) {
                all_nodes_registered = 0;
                break;
            }
        }
        bridge->accessible_registration = bridge->nodes[0].accessible_registration;
        bridge->application_registration =
            api->connection_register_object(bridge->connection,
                                            "/org/a11y/atspi/accessible/root",
                                            application,
                                            &g_rt_gui_atspi_vtable,
                                            &bridge->nodes[0],
                                            NULL,
                                            &error);
        if (cache)
            bridge->cache_registration = api->connection_register_object(bridge->connection,
                                                                         "/org/a11y/atspi/cache",
                                                                         cache,
                                                                         &g_rt_gui_atspi_vtable,
                                                                         &bridge->nodes[0],
                                                                         NULL,
                                                                         &error);
    }
    const char *unique =
        bridge->connection ? api->connection_get_unique_name(bridge->connection) : NULL;
    GVariant *embedded =
        unique && bridge->accessible_registration && bridge->application_registration
            ? api->connection_call_sync(
                  bridge->connection,
                  "org.a11y.atspi.Registry",
                  "/org/a11y/atspi/accessible/root",
                  "org.a11y.atspi.Socket",
                  "Embed",
                  api->variant_new("((so))", unique, "/org/a11y/atspi/accessible/root"),
                  NULL,
                  0,
                  500,
                  NULL,
                  &error)
            : NULL;
    int embedded_success = embedded != NULL;
    if (embedded)
        api->variant_unref(embedded);
    else
        rt_gui_atspi_diagnostic("registry embed failed", error);
    bridge->loop = api->main_loop_new(bridge->context, 0);
    int started = bridge->loop && unique && all_nodes_registered &&
                  bridge->accessible_registration && bridge->application_registration &&
                  bridge->cache_registration && embedded_success;
    GSource *ready_source = started ? api->idle_source_new() : NULL;
    if (ready_source) {
        api->source_set_callback(ready_source, rt_gui_atspi_signal_loop_running, bridge, NULL);
        started = api->source_attach(ready_source, bridge->context) != 0;
        api->source_unref(ready_source);
    } else {
        started = 0;
    }
    if (started)
        api->main_loop_run(bridge->loop);
    else
        rt_gui_atspi_signal_ready(bridge, 0);
    if (bridge->application_registration)
        api->connection_unregister_object(bridge->connection, bridge->application_registration);
    if (bridge->cache_registration)
        api->connection_unregister_object(bridge->connection, bridge->cache_registration);
    for (size_t i = bridge->node_count; i > 0; --i) {
        rt_gui_atspi_node_t *node = &bridge->nodes[i - 1];
        if (node->value_registration)
            api->connection_unregister_object(bridge->connection, node->value_registration);
        if (node->text_registration)
            api->connection_unregister_object(bridge->connection, node->text_registration);
        if (node->action_registration)
            api->connection_unregister_object(bridge->connection, node->action_registration);
        if (node->component_registration)
            api->connection_unregister_object(bridge->connection, node->component_registration);
        if (node->accessible_registration)
            api->connection_unregister_object(bridge->connection, node->accessible_registration);
    }
    if (bridge->loop)
        api->main_loop_unref(bridge->loop);
    if (bridge->node_info)
        api->node_info_unref(bridge->node_info);
    if (bridge->connection)
        api->object_unref(bridge->connection);
    if (error)
        api->error_free(error);
    api->main_context_pop_thread_default(bridge->context);
    api->main_context_unref(bridge->context);
    return NULL;
}

/// @brief Snapshot and publish one GUI window on the AT-SPI accessibility bus.
/// @details Existing projection for @p window is detached first. Attachment initializes
///          synchronization primitives, starts the private GIO worker, and registers the bridge
///          globally only after the worker confirms a successful registry embed.
/// @param window Borrowed live ZannaGFX window used as the bridge key.
/// @param root Borrowed live semantic root to snapshot.
void rt_gui_atspi_linux_attach(vgfx_window_t window, vg_widget_t *root) {
    if (!window || !root)
        return;
    (void)pthread_once(&g_rt_gui_atspi_api_once, rt_gui_atspi_load_once);
    if (!g_rt_gui_atspi_api.library)
        return;
    rt_gui_atspi_linux_detach(window);
    rt_gui_atspi_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return;
    bridge->window = window;
    const char *name = root->accessibility.name ? root->accessibility.name : root->name;
    (void)snprintf(bridge->name, sizeof(bridge->name), "%s", name ? name : "Zanna application");
    const char *locale = getenv("LC_ALL");
    if (!locale || !locale[0])
        locale = getenv("LC_MESSAGES");
    if (!locale || !locale[0])
        locale = getenv("LANG");
    (void)snprintf(bridge->locale, sizeof(bridge->locale), "%s", locale ? locale : "C");
    if (!rt_gui_atspi_build_snapshot(bridge, root)) {
        free(bridge);
        return;
    }
    if (pthread_mutex_init(&bridge->mutex, NULL) != 0) {
        free(bridge->child_storage);
        free(bridge->nodes);
        free(bridge);
        return;
    }
    if (pthread_cond_init(&bridge->condition, NULL) != 0) {
        pthread_mutex_destroy(&bridge->mutex);
        free(bridge->child_storage);
        free(bridge->nodes);
        free(bridge);
        return;
    }
    if (pthread_create(&bridge->worker, NULL, rt_gui_atspi_worker, bridge) != 0) {
        pthread_cond_destroy(&bridge->condition);
        pthread_mutex_destroy(&bridge->mutex);
        free(bridge->child_storage);
        free(bridge->nodes);
        free(bridge);
        return;
    }
    pthread_mutex_lock(&bridge->mutex);
    while (!bridge->ready)
        pthread_cond_wait(&bridge->condition, &bridge->mutex);
    pthread_mutex_unlock(&bridge->mutex);
    if (!bridge->started) {
        pthread_join(bridge->worker, NULL);
        pthread_cond_destroy(&bridge->condition);
        pthread_mutex_destroy(&bridge->mutex);
        free(bridge->child_storage);
        free(bridge->nodes);
        free(bridge);
        return;
    }
    rt_gui_atspi_diagnostic("application registered", NULL);
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    bridge->next = g_rt_gui_atspi_bridges;
    g_rt_gui_atspi_bridges = bridge;
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
}

/// @brief Remove and destroy the AT-SPI bridge for a window.
/// @details Pending requests are failed, test-request waiters drain, the worker loop is stopped and
///          joined, and all bridge-owned snapshot and synchronization storage is released.
/// @param window Borrowed window whose projection is removed.
void rt_gui_atspi_linux_detach(vgfx_window_t window) {
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    rt_gui_atspi_bridge_t **link = &g_rt_gui_atspi_bridges;
    while (*link && (*link)->window != window)
        link = &(*link)->next;
    rt_gui_atspi_bridge_t *bridge = *link;
    if (bridge)
        *link = bridge->next;
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    if (!bridge)
        return;
    pthread_mutex_lock(&bridge->mutex);
    if (bridge->request_pending) {
        bridge->request_result = 0;
        bridge->request_pending = 0;
        bridge->completed_serial = bridge->request_serial;
        pthread_cond_broadcast(&bridge->condition);
    }
    while (bridge->active_test_requests > 0)
        pthread_cond_wait(&bridge->condition, &bridge->mutex);
    pthread_mutex_unlock(&bridge->mutex);
    g_rt_gui_atspi_api.main_loop_quit(bridge->loop);
    pthread_join(bridge->worker, NULL);
    pthread_cond_destroy(&bridge->condition);
    pthread_mutex_destroy(&bridge->mutex);
    free(bridge->child_storage);
    free(bridge->nodes);
    free(bridge);
}

/// @brief Emit a visible-data change for a snapshotted widget and mark the bridge dirty.
/// @param window Borrowed window whose bridge is searched.
/// @param widget Borrowed changed widget; may be NULL when only rebuild invalidation is needed.
void rt_gui_atspi_linux_notify(vgfx_window_t window, vg_widget_t *widget) {
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge; bridge = bridge->next) {
        if (bridge->window == window) {
            if (widget && bridge->connection) {
                for (size_t i = 0; i < bridge->node_count; ++i) {
                    if (bridge->nodes[i].widget_id != widget->id)
                        continue;
                    GVariant *properties = rt_gui_atspi_empty_array("a{sv}");
                    GVariant *parameters =
                        g_rt_gui_atspi_api.variant_new("(siiv@a{sv})",
                                                       "",
                                                       0,
                                                       0,
                                                       g_rt_gui_atspi_api.variant_new_string(""),
                                                       properties);
                    (void)g_rt_gui_atspi_api.connection_emit_signal(bridge->connection,
                                                                    NULL,
                                                                    bridge->nodes[i].path,
                                                                    "org.a11y.atspi.Event.Object",
                                                                    "VisibleDataChanged",
                                                                    parameters,
                                                                    NULL);
                    break;
                }
            }
            bridge->dirty = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
}

/// @brief Iteratively find a live widget by immutable ID.
/// @param root Borrowed tree root.
/// @param id Widget ID to locate.
/// @return Borrowed matching widget, or NULL on absence/allocation failure.
static vg_widget_t *rt_gui_atspi_find_widget(vg_widget_t *root, uint64_t id) {
    size_t count = 0;
    size_t capacity = 64;
    vg_widget_t **stack = malloc(capacity * sizeof(*stack));
    if (!stack)
        return NULL;
    stack[count++] = root;
    size_t visited = 0;
    while (count > 0) {
        if (visited++ >= RT_GUI_MAX_ACCESSIBILITY_NODES) {
            free(stack);
            return NULL;
        }
        vg_widget_t *widget = stack[--count];
        if (widget->id == id) {
            free(stack);
            return widget;
        }
        for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling) {
            if (count == capacity) {
                size_t next_capacity = capacity * 2;
                if (next_capacity > RT_GUI_MAX_ACCESSIBILITY_NODES)
                    next_capacity = RT_GUI_MAX_ACCESSIBILITY_NODES;
                if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*stack)) {
                    free(stack);
                    return NULL;
                }
                void *replacement = realloc(stack, next_capacity * sizeof(*stack));
                if (!replacement) {
                    free(stack);
                    return NULL;
                }
                stack = replacement;
                capacity = next_capacity;
            }
            stack[count++] = child;
        }
    }
    free(stack);
    return NULL;
}

/// @brief Execute one worker-originated accessibility action on the GUI thread.
/// @param bridge Bridge containing the synchronized pending request slot.
/// @param root Borrowed current semantic root used to resolve the guarded widget ID.
static void rt_gui_atspi_process_request(rt_gui_atspi_bridge_t *bridge, vg_widget_t *root) {
    pthread_mutex_lock(&bridge->mutex);
    if (!bridge->request_pending) {
        pthread_mutex_unlock(&bridge->mutex);
        return;
    }
    int kind = bridge->request_kind;
    uint64_t widget_id = bridge->request_widget_id;
    uint64_t serial = bridge->request_serial;
    double requested_value = bridge->request_value;
    pthread_mutex_unlock(&bridge->mutex);

    int result = 0;
    vg_widget_t *widget = rt_gui_atspi_find_widget(root, widget_id);
    if (widget && rt_gui_atspi_effectively_enabled(widget)) {
        if (kind == RT_GUI_ATSPI_REQUEST_ACTION && widget->vtable && widget->vtable->handle_event) {
            float x = 0.0f;
            float y = 0.0f;
            float width = 0.0f;
            float height = 0.0f;
            vg_widget_get_screen_bounds(widget, &x, &y, &width, &height);
            vg_event_t event = vg_event_mouse(
                VG_EVENT_CLICK, x + width * 0.5f, y + height * 0.5f, VG_MOUSE_LEFT, 0);
            result = vg_event_send(widget, &event) ? 1 : 0;
        } else if (kind == RT_GUI_ATSPI_REQUEST_CARET && widget->type == VG_WIDGET_TEXTINPUT) {
            vg_textinput_t *input = (vg_textinput_t *)widget;
            if (requested_value >= 0.0 && requested_value <= (double)input->text_char_count) {
                vg_textinput_set_cursor(input, (size_t)requested_value);
                result = 1;
            }
        } else if (kind == RT_GUI_ATSPI_REQUEST_VALUE) {
            if (widget->type == VG_WIDGET_SLIDER) {
                vg_slider_set_value((vg_slider_t *)widget, (float)requested_value);
                result = 1;
            } else if (widget->type == VG_WIDGET_SPINNER) {
                vg_spinner_set_value((vg_spinner_t *)widget, requested_value);
                result = 1;
            }
        }
    }
    pthread_mutex_lock(&bridge->mutex);
    if (bridge->request_pending && bridge->request_serial == serial) {
        bridge->request_result = result;
        bridge->request_pending = 0;
        bridge->completed_serial = serial;
        pthread_cond_broadcast(&bridge->condition);
    }
    pthread_mutex_unlock(&bridge->mutex);
    if (result) {
        pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
        bridge->dirty = 1;
        pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    }
}

/// @brief Service pending requests and rebuild a dirty immutable AT-SPI snapshot.
/// @param window Borrowed window whose bridge is synchronized.
/// @param root Borrowed current semantic root.
void rt_gui_atspi_linux_sync(vgfx_window_t window, vg_widget_t *root) {
    int dirty = 0;
    rt_gui_atspi_bridge_t *matched = NULL;
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge; bridge = bridge->next) {
        if (bridge->window == window) {
            matched = bridge;
            dirty = bridge->dirty;
            bridge->dirty = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    if (matched)
        rt_gui_atspi_process_request(matched, root);
    if (dirty) {
        rt_gui_atspi_linux_detach(window);
        rt_gui_atspi_linux_attach(window, root);
    }
}

/// @brief Return the number of nodes in a window's current AT-SPI snapshot.
/// @param window Borrowed window used as the bridge key.
/// @return Snapshot node count, or 0 when no bridge is attached.
size_t rt_gui_atspi_linux_snapshot_count(vgfx_window_t window) {
    size_t count = 0;
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge; bridge = bridge->next) {
        if (bridge->window == window) {
            count = bridge->node_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    return count;
}

/// @brief Materialize the current cache array and return its item count.
/// @param window Borrowed window used as the bridge key.
/// @return Cache item count, or 0 when no bridge/array is available.
size_t rt_gui_atspi_linux_cache_item_count(vgfx_window_t window) {
    size_t count = 0;
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge; bridge = bridge->next) {
        if (bridge->window == window) {
            GVariant *items = rt_gui_atspi_cache_items(bridge);
            if (items) {
                count = g_rt_gui_atspi_api.variant_n_children(items);
                g_rt_gui_atspi_api.variant_unref(items);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    return count;
}

/// @brief Submit a guarded accessibility request through the synchronous bridge test seam.
/// @param window Borrowed window used as the bridge key.
/// @param widget_id Stable snapshot widget ID.
/// @param kind Action, caret, or value request kind.
/// @param value Numeric request payload.
/// @return GUI-thread request result, or 0 for invalid/unavailable input.
int rt_gui_atspi_linux_test_request(vgfx_window_t window,
                                    uint64_t widget_id,
                                    int kind,
                                    double value) {
    if (kind < RT_GUI_ATSPI_REQUEST_ACTION || kind > RT_GUI_ATSPI_REQUEST_VALUE)
        return 0;
    rt_gui_atspi_bridge_t *matched = NULL;
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge && !matched;
         bridge = bridge->next) {
        if (bridge->window != window)
            continue;
        for (size_t i = 0; i < bridge->node_count; ++i)
            if (bridge->nodes[i].widget_id == widget_id) {
                pthread_mutex_lock(&bridge->mutex);
                bridge->active_test_requests++;
                pthread_mutex_unlock(&bridge->mutex);
                matched = bridge;
                break;
            }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
    if (!matched)
        return 0;
    int result = rt_gui_atspi_request_values(matched, widget_id, kind, value);
    pthread_mutex_lock(&matched->mutex);
    matched->active_test_requests--;
    pthread_cond_broadcast(&matched->condition);
    pthread_mutex_unlock(&matched->mutex);
    return result;
}

/// @brief Emit an AT-SPI live-region announcement for a snapshotted widget.
/// @param window Borrowed window whose bridge owns the widget snapshot.
/// @param widget Borrowed announcement source widget.
/// @param text Borrowed non-empty UTF-8 announcement text.
/// @param mode Polite or assertive live-region urgency; off is ignored.
void rt_gui_atspi_linux_announce(vgfx_window_t window,
                                 vg_widget_t *widget,
                                 const char *text,
                                 vg_live_region_mode_t mode) {
    if (!widget || !text || !text[0] || mode == VG_LIVE_REGION_OFF)
        return;
    pthread_mutex_lock(&g_rt_gui_atspi_list_mutex);
    for (rt_gui_atspi_bridge_t *bridge = g_rt_gui_atspi_bridges; bridge; bridge = bridge->next) {
        if (bridge->window != window || !bridge->connection)
            continue;
        for (size_t i = 0; i < bridge->node_count; ++i) {
            if (bridge->nodes[i].widget_id != widget->id)
                continue;
            GVariant *properties = rt_gui_atspi_empty_array("a{sv}");
            GVariant *parameters =
                g_rt_gui_atspi_api.variant_new("(siiv@a{sv})",
                                               "",
                                               mode == VG_LIVE_REGION_ASSERTIVE ? 2 : 1,
                                               0,
                                               g_rt_gui_atspi_api.variant_new_string(text),
                                               properties);
            (void)g_rt_gui_atspi_api.connection_emit_signal(bridge->connection,
                                                            NULL,
                                                            bridge->nodes[i].path,
                                                            "org.a11y.atspi.Event.Object",
                                                            "Announcement",
                                                            parameters,
                                                            NULL);
            pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_rt_gui_atspi_list_mutex);
}
