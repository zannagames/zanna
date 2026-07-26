//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_accessibility_win32.c
/// @brief Implements the Win32 UI Automation accessibility adapter.
///
/// @details
/// The adapter dynamically resolves UI Automation, exposes the retained widget
/// tree through reference-counted COM providers, guards borrowed widgets with
/// immutable IDs and bridge generations, raises semantic change notifications,
/// and queries native accessibility and appearance preferences. Missing Windows
/// APIs degrade to the runtime's deterministic headless accessibility behavior.
///
// File: src/runtime/graphics/gui/rt_gui_accessibility_win32.c
// Purpose: Win32 accessibility adapter for the Zanna GUI runtime — system
//          preference queries plus a full UI Automation server-side provider
//          (hand-rolled C COM) so Narrator/NVDA can read and drive the
//          semantic widget tree, mirroring the macOS VoiceOver bridge.
//
// Key invariants:
//   - Providers borrow live widgets guarded by the immutable widget ID; a
//     stale node degrades to empty properties, never a dangling dereference.
//   - Provider calls arrive on the window's owning thread: WM_GETOBJECT and
//     subsequent UIA callbacks are marshalled to the HWND thread while the
//     platform pumps messages, matching the single-threaded widget contract.
//   - uiautomationcore.dll is resolved dynamically (ConPTY precedent); when
//     unavailable every entry point degrades to the headless-snapshot no-op.
//   - Failed or unsupported preference queries use the deterministic zero.
//
// Ownership/Lifetime:
//   - Providers are reference counted COM objects owned by UIA; the per-window
//     bridge owns exactly one reference to the root provider until detach.
//   - The supplied ZannaGFX window is borrowed and not retained past detach.
//
// Links: src/runtime/graphics/gui/rt_gui_accessibility_platform.h,
//        src/runtime/graphics/gui/rt_gui_accessibility.c,
//        src/runtime/graphics/gui/rt_gui_accessibility_macos.m
//
//===----------------------------------------------------------------------===//

#include "rt_gui_accessibility_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <oleauto.h>
#include <uiautomationcore.h>
#include <uiautomationcoreapi.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vg_event.h"
#include "vg_ide_widgets.h"
#include "vg_widgets.h"

//=============================================================================
// COM GUIDs
//=============================================================================

/* Define GUIDs we need so native Zanna links do not depend on uuid.lib. */
static const GUID RT_UIA_IID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID RT_UIA_IID_IRawElementProviderSimple = {
    0xD6DD68D1, 0x86FD, 0x4332, {0x86, 0x66, 0x9A, 0xBE, 0xDE, 0xA2, 0xD2, 0x4C}};
static const GUID RT_UIA_IID_IRawElementProviderFragment = {
    0xF7063DA8, 0x8359, 0x439C, {0x92, 0x97, 0xBB, 0xC5, 0x29, 0x9A, 0x7D, 0x87}};
static const GUID RT_UIA_IID_IRawElementProviderFragmentRoot = {
    0x620CE2A5, 0xAB8F, 0x40A9, {0x86, 0xCB, 0xDE, 0x3C, 0x75, 0x59, 0x9B, 0x58}};
static const GUID RT_UIA_IID_IInvokeProvider = {
    0x54FCB24B, 0xE18E, 0x47A2, {0xB4, 0xD3, 0xEC, 0xCB, 0xE7, 0x75, 0x99, 0xA2}};
static const GUID RT_UIA_IID_IToggleProvider = {
    0x56D00BD0, 0xC4F4, 0x433C, {0xA8, 0x36, 0x1A, 0x52, 0xA5, 0x7E, 0x08, 0x92}};
static const GUID RT_UIA_IID_IValueProvider = {
    0xC7935180, 0x6FB3, 0x4201, {0xB1, 0x74, 0x7D, 0xF7, 0x3A, 0xDB, 0xF6, 0x4A}};
static const GUID RT_UIA_IID_IRangeValueProvider = {
    0x36DC7AEF, 0x33E6, 0x4691, {0xAF, 0xE1, 0x2B, 0xE7, 0x27, 0x4B, 0x3D, 0x33}};
static const GUID RT_UIA_IID_ISelectionItemProvider = {
    0x2ACAD808, 0xB2D4, 0x452D, {0xA4, 0x07, 0x91, 0xFF, 0x1A, 0xD1, 0x67, 0xB2}};

//=============================================================================
// Protocol constants (stable UIA wire values, guarded for older SDKs)
//=============================================================================

#ifndef UIA_InvokePatternId
#define UIA_InvokePatternId 10000
#endif
#ifndef UIA_ValuePatternId
#define UIA_ValuePatternId 10002
#endif
#ifndef UIA_RangeValuePatternId
#define UIA_RangeValuePatternId 10003
#endif
#ifndef UIA_SelectionItemPatternId
#define UIA_SelectionItemPatternId 10010
#endif
#ifndef UIA_TogglePatternId
#define UIA_TogglePatternId 10015
#endif

#ifndef UIA_ControlTypePropertyId
#define UIA_ControlTypePropertyId 30003
#endif
#ifndef UIA_NamePropertyId
#define UIA_NamePropertyId 30005
#endif
#ifndef UIA_HasKeyboardFocusPropertyId
#define UIA_HasKeyboardFocusPropertyId 30008
#endif
#ifndef UIA_IsKeyboardFocusablePropertyId
#define UIA_IsKeyboardFocusablePropertyId 30009
#endif
#ifndef UIA_IsEnabledPropertyId
#define UIA_IsEnabledPropertyId 30010
#endif
#ifndef UIA_AutomationIdPropertyId
#define UIA_AutomationIdPropertyId 30011
#endif
#ifndef UIA_HelpTextPropertyId
#define UIA_HelpTextPropertyId 30013
#endif
#ifndef UIA_IsControlElementPropertyId
#define UIA_IsControlElementPropertyId 30016
#endif
#ifndef UIA_IsContentElementPropertyId
#define UIA_IsContentElementPropertyId 30017
#endif
#ifndef UIA_ValueValuePropertyId
#define UIA_ValueValuePropertyId 30045
#endif

#ifndef UIA_AutomationFocusChangedEventId
#define UIA_AutomationFocusChangedEventId 20005
#endif

// Control types (UIA_ControlTypeIds).
#define RT_UIA_CT_Button 50000
#define RT_UIA_CT_CheckBox 50002
#define RT_UIA_CT_ComboBox 50003
#define RT_UIA_CT_Edit 50004
#define RT_UIA_CT_Hyperlink 50005
#define RT_UIA_CT_Image 50006
#define RT_UIA_CT_ListItem 50007
#define RT_UIA_CT_List 50008
#define RT_UIA_CT_Menu 50009
#define RT_UIA_CT_MenuItem 50011
#define RT_UIA_CT_ProgressBar 50012
#define RT_UIA_CT_RadioButton 50013
#define RT_UIA_CT_Slider 50015
#define RT_UIA_CT_Spinner 50016
#define RT_UIA_CT_StatusBar 50017
#define RT_UIA_CT_Tab 50018
#define RT_UIA_CT_TabItem 50019
#define RT_UIA_CT_Text 50020
#define RT_UIA_CT_ToolBar 50021
#define RT_UIA_CT_Tree 50023
#define RT_UIA_CT_TreeItem 50024
#define RT_UIA_CT_Group 50026
#define RT_UIA_CT_DataItem 50029
#define RT_UIA_CT_Document 50030
#define RT_UIA_CT_Table 50036
#define RT_UIA_CT_Pane 50033

#define RT_UIA_NOTIFICATION_KIND_OTHER 4
#define RT_UIA_NOTIFICATION_PROCESSING_IMPORTANT_ALL 0
#define RT_UIA_NOTIFICATION_PROCESSING_ALL 2

//=============================================================================
// Dynamic uiautomationcore.dll surface (loaded once, tolerated when absent)
//=============================================================================

typedef LRESULT(WINAPI *rt_uia_return_raw_fn)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *);
typedef HRESULT(WINAPI *rt_uia_host_from_hwnd_fn)(HWND, IRawElementProviderSimple **);
typedef HRESULT(WINAPI *rt_uia_raise_event_fn)(IRawElementProviderSimple *, EVENTID);
typedef HRESULT(WINAPI *rt_uia_raise_prop_fn)(IRawElementProviderSimple *,
                                              PROPERTYID,
                                              VARIANT,
                                              VARIANT);
typedef BOOL(WINAPI *rt_uia_clients_listening_fn)(void);
typedef HRESULT(WINAPI *rt_uia_disconnect_fn)(IRawElementProviderSimple *);
typedef HRESULT(WINAPI *rt_uia_raise_notification_fn)(
    IRawElementProviderSimple *, int, int, BSTR, BSTR);
typedef HRESULT(WINAPI *rt_uia_safearray_destroy_fn)(SAFEARRAY *);

typedef struct rt_uia_api {
    HMODULE module;
    HMODULE oleaut_module;
    rt_uia_return_raw_fn return_raw_element_provider;
    rt_uia_host_from_hwnd_fn host_provider_from_hwnd;
    rt_uia_raise_event_fn raise_automation_event;
    rt_uia_raise_prop_fn raise_property_changed_event;
    rt_uia_clients_listening_fn clients_are_listening;
    rt_uia_disconnect_fn disconnect_provider;
    rt_uia_raise_notification_fn raise_notification_event;
    rt_uia_safearray_destroy_fn safearray_destroy;
} rt_uia_api_t;

static rt_uia_api_t g_rt_uia_api;
static INIT_ONCE g_rt_uia_api_once = INIT_ONCE_STATIC_INIT;

/// @brief Resolve UI Automation exports for the process-wide API table.
/// @param once Windows one-time initialization token.
/// @param param Optional initialization context; unused.
/// @param context Optional result context; unused.
/// @return `TRUE` after attempting to populate the API table.
static BOOL CALLBACK rt_uia_api_init(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    g_rt_uia_api.module = LoadLibraryW(L"uiautomationcore.dll");
    if (g_rt_uia_api.module) {
        g_rt_uia_api.return_raw_element_provider = (rt_uia_return_raw_fn)GetProcAddress(
            g_rt_uia_api.module, "UiaReturnRawElementProvider");
        g_rt_uia_api.host_provider_from_hwnd = (rt_uia_host_from_hwnd_fn)GetProcAddress(
            g_rt_uia_api.module, "UiaHostProviderFromHwnd");
        g_rt_uia_api.raise_automation_event =
            (rt_uia_raise_event_fn)GetProcAddress(g_rt_uia_api.module, "UiaRaiseAutomationEvent");
        g_rt_uia_api.raise_property_changed_event = (rt_uia_raise_prop_fn)GetProcAddress(
            g_rt_uia_api.module, "UiaRaiseAutomationPropertyChangedEvent");
        g_rt_uia_api.clients_are_listening = (rt_uia_clients_listening_fn)GetProcAddress(
            g_rt_uia_api.module, "UiaClientsAreListening");
        g_rt_uia_api.disconnect_provider =
            (rt_uia_disconnect_fn)GetProcAddress(g_rt_uia_api.module, "UiaDisconnectProvider");
        g_rt_uia_api.raise_notification_event = (rt_uia_raise_notification_fn)GetProcAddress(
            g_rt_uia_api.module, "UiaRaiseNotificationEvent");
    }
    g_rt_uia_api.oleaut_module = LoadLibraryW(L"oleaut32.dll");
    if (g_rt_uia_api.oleaut_module) {
        g_rt_uia_api.safearray_destroy = (rt_uia_safearray_destroy_fn)GetProcAddress(
            g_rt_uia_api.oleaut_module, "SafeArrayDestroy");
    }
    return TRUE;
}

/// @brief Resolve the UI Automation core exports once per process.
/// @return Borrowed API table; entries are NULL when the DLL or export is missing.
static rt_uia_api_t *rt_uia_api(void) {
    (void)InitOnceExecuteOnce(&g_rt_uia_api_once, rt_uia_api_init, NULL, NULL);
    return &g_rt_uia_api;
}

/// @brief Return whether the provider bridge can operate at all.
/// @return 1 when the required UI Automation exports are available, otherwise 0.
static int32_t rt_uia_available(void) {
    rt_uia_api_t *api = rt_uia_api();
    return api->return_raw_element_provider != NULL && api->host_provider_from_hwnd != NULL;
}

//=============================================================================
// Semantic helpers (ports of the macOS bridge inference rules)
//=============================================================================

/// @brief Return whether a widget and all ancestors are enabled and visible.
/// @param widget Borrowed widget to inspect.
/// @return 1 when the widget is live, visible, and enabled through its ancestor chain.
static int32_t rt_uia_is_available(const vg_widget_t *widget) {
    if (!vg_widget_is_live(widget))
        return 0;
    for (const vg_widget_t *current = widget; current; current = current->parent) {
        if (!current->visible || !current->enabled)
            return 0;
    }
    return 1;
}

/// @brief Infer the same built-in accessible label used by the headless snapshot.
/// @param widget Borrowed widget to inspect.
/// @return Borrowed UTF-8 accessible name, never NULL.
static const char *rt_uia_name(const vg_widget_t *widget) {
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

/// @brief Map a Zanna semantic role to the closest UIA control type.
/// @param widget Borrowed semantic widget; may be NULL.
/// @return Stable UI Automation control-type identifier.
static long rt_uia_control_type(const vg_widget_t *widget) {
    vg_accessible_role_t role = widget ? widget->accessibility.role : VG_ACCESSIBLE_ROLE_NONE;
    switch (role) {
        case VG_ACCESSIBLE_ROLE_APPLICATION:
        case VG_ACCESSIBLE_ROLE_WINDOW:
        case VG_ACCESSIBLE_ROLE_DIALOG:
        case VG_ACCESSIBLE_ROLE_ALERT:
            return RT_UIA_CT_Pane;
        case VG_ACCESSIBLE_ROLE_LABEL:
            return RT_UIA_CT_Text;
        case VG_ACCESSIBLE_ROLE_BUTTON:
            return RT_UIA_CT_Button;
        case VG_ACCESSIBLE_ROLE_CHECKBOX:
            return RT_UIA_CT_CheckBox;
        case VG_ACCESSIBLE_ROLE_RADIOBUTTON:
            return RT_UIA_CT_RadioButton;
        case VG_ACCESSIBLE_ROLE_TEXTBOX:
        case VG_ACCESSIBLE_ROLE_SEARCHBOX:
            return widget && widget->type == VG_WIDGET_CODEEDITOR ? RT_UIA_CT_Document
                                                                  : RT_UIA_CT_Edit;
        case VG_ACCESSIBLE_ROLE_COMBOBOX:
            return RT_UIA_CT_ComboBox;
        case VG_ACCESSIBLE_ROLE_LIST:
            return RT_UIA_CT_List;
        case VG_ACCESSIBLE_ROLE_LISTITEM:
            return RT_UIA_CT_ListItem;
        case VG_ACCESSIBLE_ROLE_TREE:
            return RT_UIA_CT_Tree;
        case VG_ACCESSIBLE_ROLE_TREEITEM:
            return RT_UIA_CT_TreeItem;
        case VG_ACCESSIBLE_ROLE_TABLIST:
            return RT_UIA_CT_Tab;
        case VG_ACCESSIBLE_ROLE_TAB:
            return RT_UIA_CT_TabItem;
        case VG_ACCESSIBLE_ROLE_TABLE:
            return RT_UIA_CT_Table;
        case VG_ACCESSIBLE_ROLE_ROW:
        case VG_ACCESSIBLE_ROLE_CELL:
            return RT_UIA_CT_DataItem;
        case VG_ACCESSIBLE_ROLE_SLIDER:
            return RT_UIA_CT_Slider;
        case VG_ACCESSIBLE_ROLE_PROGRESSBAR:
            return RT_UIA_CT_ProgressBar;
        case VG_ACCESSIBLE_ROLE_MENU:
            return RT_UIA_CT_Menu;
        case VG_ACCESSIBLE_ROLE_MENUITEM:
            return RT_UIA_CT_MenuItem;
        case VG_ACCESSIBLE_ROLE_TOOLBAR:
            return RT_UIA_CT_ToolBar;
        case VG_ACCESSIBLE_ROLE_STATUSBAR:
            return RT_UIA_CT_StatusBar;
        case VG_ACCESSIBLE_ROLE_IMAGE:
        case VG_ACCESSIBLE_ROLE_VIDEO:
            return RT_UIA_CT_Image;
        case VG_ACCESSIBLE_ROLE_LINK:
            return RT_UIA_CT_Hyperlink;
        case VG_ACCESSIBLE_ROLE_GROUP:
        case VG_ACCESSIBLE_ROLE_NONE:
        default:
            return widget && widget->type == VG_WIDGET_SPINNER ? RT_UIA_CT_Spinner
                                                               : RT_UIA_CT_Group;
    }
}

/// @brief Return whether a role represents an interactive, focusable control.
/// @param role Zanna accessible role to classify.
/// @return 1 for focusable roles, otherwise 0.
static int32_t rt_uia_role_focusable(vg_accessible_role_t role) {
    switch (role) {
        case VG_ACCESSIBLE_ROLE_BUTTON:
        case VG_ACCESSIBLE_ROLE_CHECKBOX:
        case VG_ACCESSIBLE_ROLE_RADIOBUTTON:
        case VG_ACCESSIBLE_ROLE_TEXTBOX:
        case VG_ACCESSIBLE_ROLE_SEARCHBOX:
        case VG_ACCESSIBLE_ROLE_COMBOBOX:
        case VG_ACCESSIBLE_ROLE_LIST:
        case VG_ACCESSIBLE_ROLE_LISTITEM:
        case VG_ACCESSIBLE_ROLE_TREE:
        case VG_ACCESSIBLE_ROLE_TREEITEM:
        case VG_ACCESSIBLE_ROLE_TAB:
        case VG_ACCESSIBLE_ROLE_SLIDER:
        case VG_ACCESSIBLE_ROLE_MENU:
        case VG_ACCESSIBLE_ROLE_MENUITEM:
        case VG_ACCESSIBLE_ROLE_LINK:
            return 1;
        default:
            return 0;
    }
}

/// @brief Convert UTF-8 into a caller-owned BSTR; NULL/invalid become empty.
/// @param text Borrowed NUL-terminated UTF-8 text; may be NULL.
/// @return Allocated BSTR owned by the caller, or NULL on allocation failure.
static BSTR rt_uia_bstr(const char *text) {
    if (!text || !*text)
        return SysAllocString(L"");
    int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (wide_count <= 1)
        return SysAllocString(L"");
    BSTR result = SysAllocStringLen(NULL, (UINT)(wide_count - 1));
    if (!result)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, wide_count) !=
        wide_count) {
        SysFreeString(result);
        return SysAllocString(L"");
    }
    return result;
}

/// @brief Activate a widget through its keyboard event path (Space press).
/// @param widget Borrowed widget to focus and activate.
/// @return 1 when the synthetic activation event was handled, otherwise 0.
static int32_t rt_uia_activate(vg_widget_t *widget) {
    if (!rt_uia_is_available(widget))
        return 0;
    vg_widget_set_focus(widget);
    vg_event_t event = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_SPACE, 0, VG_MOD_NONE);
    return vg_event_send(widget, &event) ? 1 : 0;
}

//=============================================================================
// Per-window bridge state
//=============================================================================

typedef struct rt_uia_provider rt_uia_provider_t;

typedef struct rt_uia_window_bridge {
    vgfx_window_t window;             ///< Borrowed owning ZannaGFX window; NULL = free slot.
    HWND hwnd;                        ///< Native window answering WM_GETOBJECT.
    vg_widget_t *root;                ///< Borrowed semantic root widget.
    uint64_t root_id;                 ///< Immutable ID guarding the root pointer.
    uint64_t generation;              ///< Slot generation invalidating detached providers.
    rt_uia_provider_t *root_provider; ///< Bridge-owned root provider reference.
} rt_uia_window_bridge_t;

#define RT_UIA_MAX_BRIDGES 4
static rt_uia_window_bridge_t g_rt_uia_bridges[RT_UIA_MAX_BRIDGES];

/// @brief Find the bridge slot for a window, or NULL.
/// @param window Borrowed ZannaGFX window used as the slot key.
/// @return Borrowed bridge slot, or NULL when the window is not attached.
static rt_uia_window_bridge_t *rt_uia_bridge_for_window(vgfx_window_t window) {
    if (!window)
        return NULL;
    for (int i = 0; i < RT_UIA_MAX_BRIDGES; ++i) {
        if (g_rt_uia_bridges[i].window == window)
            return &g_rt_uia_bridges[i];
    }
    return NULL;
}

//=============================================================================
// Provider object (one COM identity, several interfaces)
//=============================================================================

struct rt_uia_provider {
    IRawElementProviderSimple simple;
    IRawElementProviderFragment fragment;
    IRawElementProviderFragmentRoot fragment_root;
    IInvokeProvider invoke;
    IToggleProvider toggle;
    IValueProvider value;
    IRangeValueProvider range_value;
    ISelectionItemProvider selection_item;
    LONG ref_count;
    vg_widget_t *widget;
    uint64_t widget_id;
    rt_uia_window_bridge_t *bridge;
    uint64_t bridge_generation;
    int32_t is_root;
};

#define RT_UIA_FROM(iface, member)                                                                 \
    ((rt_uia_provider_t *)((char *)(iface) - offsetof(rt_uia_provider_t, member)))

static rt_uia_provider_t *rt_uia_provider_create(rt_uia_window_bridge_t *bridge,
                                                 vg_widget_t *widget);

/// @brief Invalidate a bridge slot while keeping its generation monotonic.
/// @param bridge Bridge slot to clear; NULL is ignored.
static void rt_uia_bridge_reset(rt_uia_window_bridge_t *bridge) {
    if (!bridge)
        return;
    uint64_t generation = bridge->generation + 1U;
    if (generation == 0)
        generation = 1;
    memset(bridge, 0, sizeof(*bridge));
    bridge->generation = generation;
}

/// @brief Resolve a provider's bridge only while the slot still represents its attachment.
/// @param provider Borrowed provider whose captured bridge generation is checked.
/// @return Borrowed live bridge, or NULL when detached or stale.
static rt_uia_window_bridge_t *rt_uia_resolve_bridge(rt_uia_provider_t *provider) {
    rt_uia_window_bridge_t *bridge = provider ? provider->bridge : NULL;
    return bridge && bridge->window && bridge->generation == provider->bridge_generation &&
                   vg_widget_is_live(bridge->root) && bridge->root->id == bridge->root_id
               ? bridge
               : NULL;
}

/// @brief Return whether a live widget belongs to this bridge's semantic tree.
/// @param bridge Borrowed bridge whose root defines the tree.
/// @param widget Borrowed candidate widget.
/// @return 1 when @p widget is a live descendant of the guarded root, otherwise 0.
static int rt_uia_bridge_contains_widget(const rt_uia_window_bridge_t *bridge,
                                         const vg_widget_t *widget) {
    if (!bridge || !vg_widget_is_live(bridge->root) || bridge->root->id != bridge->root_id ||
        !vg_widget_is_live(widget))
        return 0;
    for (const vg_widget_t *current = widget; current;) {
        if (!vg_widget_is_live(current))
            return 0;
        if (current == bridge->root)
            return 1;
        current = current->parent;
    }
    return 0;
}

/// @brief Resolve the borrowed widget only while its immutable ID still matches.
/// @param provider Borrowed provider carrying the guarded widget identity.
/// @return Borrowed live widget, or NULL when the provider or tree is stale.
static vg_widget_t *rt_uia_resolve(rt_uia_provider_t *provider) {
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!bridge)
        return NULL;
    vg_widget_t *widget = provider->widget;
    return rt_uia_bridge_contains_widget(bridge, widget) && widget->id == provider->widget_id
               ? widget
               : NULL;
}

//----------------------------------------------------------------------------
// Shared IUnknown behavior
//----------------------------------------------------------------------------

/// @brief Resolve one supported COM interface for a provider and retain the provider.
/// @param provider Borrowed provider whose interfaces are queried.
/// @param riid Requested interface identifier.
/// @param out Receives the retained interface pointer on success.
/// @return `S_OK`, `E_POINTER`, or `E_NOINTERFACE`.
static HRESULT rt_uia_provider_qi(rt_uia_provider_t *provider, REFIID riid, void **out) {
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *widget = rt_uia_resolve(provider);
    vg_accessible_role_t role = widget ? widget->accessibility.role : VG_ACCESSIBLE_ROLE_NONE;

    if (IsEqualIID(riid, &RT_UIA_IID_IUnknown) ||
        IsEqualIID(riid, &RT_UIA_IID_IRawElementProviderSimple)) {
        *out = &provider->simple;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IRawElementProviderFragment)) {
        *out = &provider->fragment;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IRawElementProviderFragmentRoot) && provider->is_root) {
        *out = &provider->fragment_root;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IInvokeProvider) &&
               (role == VG_ACCESSIBLE_ROLE_BUTTON || role == VG_ACCESSIBLE_ROLE_MENUITEM ||
                role == VG_ACCESSIBLE_ROLE_LINK)) {
        *out = &provider->invoke;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IToggleProvider) &&
               role == VG_ACCESSIBLE_ROLE_CHECKBOX) {
        *out = &provider->toggle;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IValueProvider) &&
               (role == VG_ACCESSIBLE_ROLE_TEXTBOX || role == VG_ACCESSIBLE_ROLE_SEARCHBOX ||
                role == VG_ACCESSIBLE_ROLE_COMBOBOX)) {
        *out = &provider->value;
    } else if (IsEqualIID(riid, &RT_UIA_IID_IRangeValueProvider) &&
               (role == VG_ACCESSIBLE_ROLE_SLIDER || role == VG_ACCESSIBLE_ROLE_PROGRESSBAR ||
                (widget && widget->type == VG_WIDGET_SPINNER))) {
        *out = &provider->range_value;
    } else if (IsEqualIID(riid, &RT_UIA_IID_ISelectionItemProvider) &&
               (role == VG_ACCESSIBLE_ROLE_RADIOBUTTON || role == VG_ACCESSIBLE_ROLE_LISTITEM ||
                role == VG_ACCESSIBLE_ROLE_TREEITEM || role == VG_ACCESSIBLE_ROLE_TAB ||
                role == VG_ACCESSIBLE_ROLE_ROW)) {
        *out = &provider->selection_item;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&provider->ref_count);
    return S_OK;
}

/// @brief Add one COM reference to a provider.
/// @param provider Provider whose reference count is incremented.
/// @return New reference count.
static ULONG rt_uia_provider_addref(rt_uia_provider_t *provider) {
    return (ULONG)InterlockedIncrement(&provider->ref_count);
}

/// @brief Release one COM reference and free the provider when the count reaches zero.
/// @param provider Provider whose reference count is decremented.
/// @return Remaining reference count.
static ULONG rt_uia_provider_release(rt_uia_provider_t *provider) {
    LONG remaining = InterlockedDecrement(&provider->ref_count);
    if (remaining == 0)
        free(provider);
    return (ULONG)remaining;
}

/// @brief Generate IUnknown methods that recover and delegate to a provider's shared identity.
#define RT_UIA_IUNKNOWN_THUNKS(prefix, iface_type, member)                                         \
    static HRESULT STDMETHODCALLTYPE prefix##_QueryInterface(                                      \
        iface_type *iface, REFIID riid, void **out) {                                              \
        return rt_uia_provider_qi(RT_UIA_FROM(iface, member), riid, out);                          \
    }                                                                                              \
    static ULONG STDMETHODCALLTYPE prefix##_AddRef(iface_type *iface) {                            \
        return rt_uia_provider_addref(RT_UIA_FROM(iface, member));                                 \
    }                                                                                              \
    static ULONG STDMETHODCALLTYPE prefix##_Release(iface_type *iface) {                           \
        return rt_uia_provider_release(RT_UIA_FROM(iface, member));                                \
    }

//----------------------------------------------------------------------------
// IRawElementProviderSimple
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_simple, IRawElementProviderSimple, simple)

/// @brief Report that each element is a server-side UI Automation provider.
/// @param iface Borrowed simple-provider interface.
/// @param out Receives the provider option flags.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE rt_uia_simple_get_ProviderOptions(IRawElementProviderSimple *iface,
                                                                   enum ProviderOptions *out) {
    (void)iface;
    if (!out)
        return E_POINTER;
    *out = ProviderOptions_ServerSideProvider;
    return S_OK;
}

/// @brief Return the role-appropriate UI Automation pattern interface.
/// @param iface Borrowed simple-provider interface.
/// @param pattern_id Requested UI Automation pattern identifier.
/// @param out Receives a retained pattern interface, or NULL when unsupported.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE rt_uia_simple_GetPatternProvider(IRawElementProviderSimple *iface,
                                                                  PATTERNID pattern_id,
                                                                  IUnknown **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, simple);
    if (!out)
        return E_POINTER;
    *out = NULL;
    const IID *riid = NULL;
    switch (pattern_id) {
        case UIA_InvokePatternId:
            riid = &RT_UIA_IID_IInvokeProvider;
            break;
        case UIA_TogglePatternId:
            riid = &RT_UIA_IID_IToggleProvider;
            break;
        case UIA_ValuePatternId:
            riid = &RT_UIA_IID_IValueProvider;
            break;
        case UIA_RangeValuePatternId:
            riid = &RT_UIA_IID_IRangeValueProvider;
            break;
        case UIA_SelectionItemPatternId:
            riid = &RT_UIA_IID_ISelectionItemProvider;
            break;
        default:
            return S_OK;
    }
    // Note: editors surface their content through the Value pattern in this
    // bridge revision (their default role is TEXTBOX); a full TextPattern is
    // staged separately.
    HRESULT hr = rt_uia_provider_qi(provider, riid, (void **)out);
    if (FAILED(hr))
        *out = NULL;
    return S_OK;
}

/// @brief Materialize one UI Automation property from the guarded semantic widget.
/// @param iface Borrowed simple-provider interface.
/// @param property_id Requested UI Automation property identifier.
/// @param out Receives an initialized property VARIANT.
/// @return COM success, pointer, or allocation status.
static HRESULT STDMETHODCALLTYPE rt_uia_simple_GetPropertyValue(IRawElementProviderSimple *iface,
                                                                PROPERTYID property_id,
                                                                VARIANT *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, simple);
    if (!out)
        return E_POINTER;
    VariantInit(out);
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (!widget)
        return S_OK;
    switch (property_id) {
        case UIA_NamePropertyId:
            out->vt = VT_BSTR;
            out->bstrVal = rt_uia_bstr(rt_uia_name(widget));
            break;
        case UIA_HelpTextPropertyId:
            out->vt = VT_BSTR;
            out->bstrVal = rt_uia_bstr(widget->accessibility.description);
            break;
        case UIA_ControlTypePropertyId:
            out->vt = VT_I4;
            out->lVal = rt_uia_control_type(widget);
            break;
        case UIA_IsEnabledPropertyId:
            out->vt = VT_BOOL;
            out->boolVal = rt_uia_is_available(widget) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_HasKeyboardFocusPropertyId:
            out->vt = VT_BOOL;
            out->boolVal = (widget->state & VG_STATE_FOCUSED) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsKeyboardFocusablePropertyId:
            out->vt = VT_BOOL;
            out->boolVal =
                rt_uia_role_focusable(widget->accessibility.role) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_AutomationIdPropertyId: {
            out->vt = VT_BSTR;
            if (widget->name) {
                out->bstrVal = rt_uia_bstr(widget->name);
            } else {
                char generated[48];
                (void)snprintf(generated,
                               sizeof(generated),
                               "zanna-widget-%llu",
                               (unsigned long long)widget->id);
                out->bstrVal = rt_uia_bstr(generated);
            }
            break;
        }
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId:
            out->vt = VT_BOOL;
            out->boolVal = widget->accessibility.role != VG_ACCESSIBLE_ROLE_NONE ? VARIANT_TRUE
                                                                                 : VARIANT_FALSE;
            break;
        default:
            break;
    }
    if (out->vt == VT_BSTR && !out->bstrVal) {
        VariantInit(out);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

/// @brief Return the HWND host provider for the attached root element.
/// @param iface Borrowed simple-provider interface.
/// @param out Receives the retained host provider, or NULL for non-root or stale elements.
/// @return COM success or pointer status from the host-provider query.
static HRESULT STDMETHODCALLTYPE rt_uia_simple_get_HostRawElementProvider(
    IRawElementProviderSimple *iface, IRawElementProviderSimple **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, simple);
    if (!out)
        return E_POINTER;
    *out = NULL;
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!provider->is_root || !bridge)
        return S_OK;
    rt_uia_api_t *api = rt_uia_api();
    if (!api->host_provider_from_hwnd)
        return S_OK;
    return api->host_provider_from_hwnd(bridge->hwnd, out);
}

static IRawElementProviderSimpleVtbl g_rt_uia_simple_vtbl = {
    rt_uia_simple_QueryInterface,
    rt_uia_simple_AddRef,
    rt_uia_simple_Release,
    rt_uia_simple_get_ProviderOptions,
    rt_uia_simple_GetPatternProvider,
    rt_uia_simple_GetPropertyValue,
    rt_uia_simple_get_HostRawElementProvider,
};

//----------------------------------------------------------------------------
// IRawElementProviderFragment
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_fragment, IRawElementProviderFragment, fragment)

/// @brief Wrap a live widget in a new provider fragment interface, or NULL.
/// @param bridge Borrowed bridge owning the semantic tree.
/// @param widget Borrowed live widget to wrap.
/// @return Newly allocated fragment interface with one reference, or NULL on failure.
static IRawElementProviderFragment *rt_uia_fragment_for(rt_uia_window_bridge_t *bridge,
                                                        vg_widget_t *widget) {
    rt_uia_provider_t *provider = rt_uia_provider_create(bridge, widget);
    return provider ? &provider->fragment : NULL;
}

/// @brief Navigate between visible parent, child, and sibling semantic fragments.
/// @param iface Borrowed fragment interface.
/// @param direction Requested UI Automation navigation direction.
/// @param out Receives a retained target fragment, or NULL when no target exists.
/// @return COM success, pointer, or allocation status.
static HRESULT STDMETHODCALLTYPE rt_uia_fragment_Navigate(IRawElementProviderFragment *iface,
                                                          enum NavigateDirection direction,
                                                          IRawElementProviderFragment **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment);
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *widget = rt_uia_resolve(provider);
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!widget || !bridge)
        return S_OK;

    vg_widget_t *target = NULL;
    switch (direction) {
        case NavigateDirection_Parent:
            if (!provider->is_root)
                target = widget->parent;
            break;
        case NavigateDirection_FirstChild:
            for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling) {
                if (child->visible) {
                    target = child;
                    break;
                }
            }
            break;
        case NavigateDirection_LastChild: {
            for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling) {
                if (child->visible)
                    target = child;
            }
            break;
        }
        case NavigateDirection_NextSibling:
            if (!provider->is_root) {
                for (vg_widget_t *sibling = widget->next_sibling; sibling;
                     sibling = sibling->next_sibling) {
                    if (sibling->visible) {
                        target = sibling;
                        break;
                    }
                }
            }
            break;
        case NavigateDirection_PreviousSibling:
            if (!provider->is_root && widget->parent) {
                vg_widget_t *previous = NULL;
                for (vg_widget_t *child = widget->parent->first_child; child && child != widget;
                     child = child->next_sibling) {
                    if (child->visible)
                        previous = child;
                }
                target = previous;
            }
            break;
        default:
            break;
    }
    if (target) {
        *out = rt_uia_fragment_for(bridge, target);
        if (!*out)
            return E_OUTOFMEMORY;
    }
    return S_OK;
}

/// @brief Build a stable UI Automation runtime ID from the guarded widget ID.
/// @param iface Borrowed fragment interface.
/// @param out Receives the allocated integer SAFEARRAY, or NULL for the root or a stale widget.
/// @return COM success, pointer, allocation, or SAFEARRAY operation status.
static HRESULT STDMETHODCALLTYPE rt_uia_fragment_GetRuntimeId(IRawElementProviderFragment *iface,
                                                              SAFEARRAY **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment);
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (provider->is_root || !rt_uia_resolve(provider))
        return S_OK; // The host HWND supplies the root identity.
    SAFEARRAY *runtime_id = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!runtime_id)
        return E_OUTOFMEMORY;
    LONG index = 0;
    LONG value = UiaAppendRuntimeId;
    HRESULT hr = SafeArrayPutElement(runtime_id, &index, &value);
    if (FAILED(hr)) {
        if (rt_uia_api()->safearray_destroy)
            (void)rt_uia_api()->safearray_destroy(runtime_id);
        return hr;
    }
    index = 1;
    value = (LONG)(uint32_t)(provider->widget_id & UINT32_MAX);
    hr = SafeArrayPutElement(runtime_id, &index, &value);
    if (FAILED(hr)) {
        if (rt_uia_api()->safearray_destroy)
            (void)rt_uia_api()->safearray_destroy(runtime_id);
        return hr;
    }
    index = 2;
    value = (LONG)(uint32_t)(provider->widget_id >> 32U);
    hr = SafeArrayPutElement(runtime_id, &index, &value);
    if (FAILED(hr)) {
        if (rt_uia_api()->safearray_destroy)
            (void)rt_uia_api()->safearray_destroy(runtime_id);
        return hr;
    }
    *out = runtime_id;
    return S_OK;
}

/// @brief Return a fragment's absolute screen rectangle in physical pixels.
/// @param iface Borrowed fragment interface.
/// @param out Receives the initialized UI Automation rectangle.
/// @return COM success, pointer, validation, or Win32 coordinate-conversion status.
static HRESULT STDMETHODCALLTYPE
rt_uia_fragment_get_BoundingRectangle(IRawElementProviderFragment *iface, struct UiaRect *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment);
    if (!out)
        return E_POINTER;
    out->left = out->top = out->width = out->height = 0.0;
    vg_widget_t *widget = rt_uia_resolve(provider);
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!widget || !bridge)
        return S_OK;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    vg_widget_get_screen_bounds(widget, &x, &y, &width, &height);
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || width < 0.0f ||
        height < 0.0f)
        return E_FAIL;
    POINT origin = {0, 0};
    if (bridge->hwnd && !ClientToScreen(bridge->hwnd, &origin)) {
        DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    out->left = (double)origin.x + (double)x;
    out->top = (double)origin.y + (double)y;
    out->width = (double)width;
    out->height = (double)height;
    return S_OK;
}

/// @brief Report that Zanna fragments contain no separately hosted fragment roots.
/// @param iface Borrowed fragment interface.
/// @param out Receives NULL.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE
rt_uia_fragment_GetEmbeddedFragmentRoots(IRawElementProviderFragment *iface, SAFEARRAY **out) {
    (void)iface;
    if (!out)
        return E_POINTER;
    *out = NULL;
    return S_OK;
}

/// @brief Move toolkit focus to an available fragment.
/// @param iface Borrowed fragment interface.
/// @return `S_OK` when focus was assigned, otherwise `E_FAIL`.
static HRESULT STDMETHODCALLTYPE rt_uia_fragment_SetFocus(IRawElementProviderFragment *iface) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment);
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (!rt_uia_is_available(widget))
        return E_FAIL;
    vg_widget_set_focus(widget);
    return S_OK;
}

/// @brief Return the retained fragment-root interface for an attached element.
/// @param iface Borrowed fragment interface.
/// @param out Receives the retained root interface, or NULL for a stale bridge.
/// @return COM success, pointer, or interface-query status.
static HRESULT STDMETHODCALLTYPE rt_uia_fragment_get_FragmentRoot(
    IRawElementProviderFragment *iface, IRawElementProviderFragmentRoot **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment);
    if (!out)
        return E_POINTER;
    *out = NULL;
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!bridge || !bridge->root_provider)
        return S_OK;
    return rt_uia_provider_qi(
        bridge->root_provider, &RT_UIA_IID_IRawElementProviderFragmentRoot, (void **)out);
}

static IRawElementProviderFragmentVtbl g_rt_uia_fragment_vtbl = {
    rt_uia_fragment_QueryInterface,
    rt_uia_fragment_AddRef,
    rt_uia_fragment_Release,
    rt_uia_fragment_Navigate,
    rt_uia_fragment_GetRuntimeId,
    rt_uia_fragment_get_BoundingRectangle,
    rt_uia_fragment_GetEmbeddedFragmentRoots,
    rt_uia_fragment_SetFocus,
    rt_uia_fragment_get_FragmentRoot,
};

//----------------------------------------------------------------------------
// IRawElementProviderFragmentRoot (root provider only)
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_root, IRawElementProviderFragmentRoot, fragment_root)

/// @brief Return the deepest visible widget containing a client-area point.
/// @param widget Borrowed subtree root to test.
/// @param x Client-area X coordinate.
/// @param y Client-area Y coordinate.
/// @return Borrowed topmost matching widget, or NULL when the point misses the subtree.
static vg_widget_t *rt_uia_hit_test(vg_widget_t *widget, float x, float y) {
    if (!widget || !widget->visible)
        return NULL;
    float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
    vg_widget_get_screen_bounds(widget, &wx, &wy, &ww, &wh);
    if (!isfinite(wx) || !isfinite(wy) || !isfinite(ww) || !isfinite(wh) || ww < 0.0f || wh < 0.0f)
        return NULL;
    float right = wx + ww;
    float bottom = wy + wh;
    if (!isfinite(right) || !isfinite(bottom) || x < wx || y < wy || x >= right || y >= bottom)
        return NULL;
    // Later siblings paint on top; prefer the last matching child.
    vg_widget_t *best = widget;
    for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling) {
        vg_widget_t *hit = rt_uia_hit_test(child, x, y);
        if (hit)
            best = hit;
    }
    return best;
}

/// @brief Resolve an absolute screen point to its deepest visible provider fragment.
/// @param iface Borrowed fragment-root interface.
/// @param screen_x Absolute screen X coordinate.
/// @param screen_y Absolute screen Y coordinate.
/// @param out Receives a retained fragment, or NULL when no widget contains the point.
/// @return COM success, pointer, argument, Win32 conversion, or allocation status.
static HRESULT STDMETHODCALLTYPE
rt_uia_root_ElementProviderFromPoint(IRawElementProviderFragmentRoot *iface,
                                     double screen_x,
                                     double screen_y,
                                     IRawElementProviderFragment **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment_root);
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *root = rt_uia_resolve(provider);
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!root || !bridge)
        return S_OK;
    if (!isfinite(screen_x) || !isfinite(screen_y) || screen_x < (double)LONG_MIN ||
        screen_x > (double)LONG_MAX || screen_y < (double)LONG_MIN || screen_y > (double)LONG_MAX)
        return E_INVALIDARG;
    POINT point = {(LONG)screen_x, (LONG)screen_y};
    if (bridge->hwnd && !ScreenToClient(bridge->hwnd, &point)) {
        DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    vg_widget_t *hit = rt_uia_hit_test(root, (float)point.x, (float)point.y);
    if (hit) {
        *out = rt_uia_fragment_for(bridge, hit);
        if (!*out)
            return E_OUTOFMEMORY;
    }
    return S_OK;
}

/// @brief Return the focused descendant, or NULL when none is focused.
/// @param widget Borrowed subtree root to search.
/// @return Borrowed focused visible widget, or NULL when none is found.
static vg_widget_t *rt_uia_find_focus(vg_widget_t *widget) {
    if (!widget || !widget->visible)
        return NULL;
    if (widget->state & VG_STATE_FOCUSED)
        return widget;
    for (vg_widget_t *child = widget->first_child; child; child = child->next_sibling) {
        vg_widget_t *focused = rt_uia_find_focus(child);
        if (focused)
            return focused;
    }
    return NULL;
}

/// @brief Return the currently focused descendant as a provider fragment.
/// @param iface Borrowed fragment-root interface.
/// @param out Receives a retained focused fragment, or NULL when the root or no descendant is
/// focused.
/// @return COM success, pointer, or allocation status.
static HRESULT STDMETHODCALLTYPE rt_uia_root_GetFocus(IRawElementProviderFragmentRoot *iface,
                                                      IRawElementProviderFragment **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, fragment_root);
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *root = rt_uia_resolve(provider);
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!root || !bridge)
        return S_OK;
    vg_widget_t *focused = rt_uia_find_focus(root);
    if (focused && focused != root) {
        *out = rt_uia_fragment_for(bridge, focused);
        if (!*out)
            return E_OUTOFMEMORY;
    }
    return S_OK;
}

static IRawElementProviderFragmentRootVtbl g_rt_uia_root_vtbl = {
    rt_uia_root_QueryInterface,
    rt_uia_root_AddRef,
    rt_uia_root_Release,
    rt_uia_root_ElementProviderFromPoint,
    rt_uia_root_GetFocus,
};

//----------------------------------------------------------------------------
// IInvokeProvider
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_invoke, IInvokeProvider, invoke)

/// @brief Invoke the guarded widget through its ordinary activation event path.
/// @param iface Borrowed invoke-pattern interface.
/// @return `S_OK` when activation was handled, otherwise `E_FAIL`.
static HRESULT STDMETHODCALLTYPE rt_uia_invoke_Invoke(IInvokeProvider *iface) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, invoke);
    return rt_uia_activate(rt_uia_resolve(provider)) ? S_OK : E_FAIL;
}

static IInvokeProviderVtbl g_rt_uia_invoke_vtbl = {
    rt_uia_invoke_QueryInterface,
    rt_uia_invoke_AddRef,
    rt_uia_invoke_Release,
    rt_uia_invoke_Invoke,
};

//----------------------------------------------------------------------------
// IToggleProvider
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_toggle, IToggleProvider, toggle)

/// @brief Toggle the guarded checkbox through its ordinary activation event path.
/// @param iface Borrowed toggle-pattern interface.
/// @return `S_OK` when activation was handled, otherwise `E_FAIL`.
static HRESULT STDMETHODCALLTYPE rt_uia_toggle_Toggle(IToggleProvider *iface) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, toggle);
    return rt_uia_activate(rt_uia_resolve(provider)) ? S_OK : E_FAIL;
}

/// @brief Return the checked, unchecked, or indeterminate state of the guarded widget.
/// @param iface Borrowed toggle-pattern interface.
/// @param out Receives the normalized UI Automation toggle state.
/// @return `S_OK`, `E_POINTER`, or `E_FAIL` for a stale widget.
static HRESULT STDMETHODCALLTYPE rt_uia_toggle_get_ToggleState(IToggleProvider *iface,
                                                               enum ToggleState *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, toggle);
    if (!out)
        return E_POINTER;
    *out = ToggleState_Off;
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (!widget)
        return E_FAIL;
    if (widget->type == VG_WIDGET_CHECKBOX && ((const vg_checkbox_t *)widget)->indeterminate) {
        *out = ToggleState_Indeterminate;
        return S_OK;
    }
    *out = (widget->state & VG_STATE_CHECKED) ? ToggleState_On : ToggleState_Off;
    return S_OK;
}

static IToggleProviderVtbl g_rt_uia_toggle_vtbl = {
    rt_uia_toggle_QueryInterface,
    rt_uia_toggle_AddRef,
    rt_uia_toggle_Release,
    rt_uia_toggle_Toggle,
    rt_uia_toggle_get_ToggleState,
};

//----------------------------------------------------------------------------
// IValueProvider
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_value, IValueProvider, value)

/// @brief Replace editable text-input content from a UTF-16 UI Automation value.
/// @param iface Borrowed value-pattern interface.
/// @param value Borrowed UTF-16 value; NULL is treated as an empty string.
/// @return COM success, validation, allocation, or unsupported-operation status.
static HRESULT STDMETHODCALLTYPE rt_uia_value_SetValue(IValueProvider *iface, LPCWSTR value) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, value);
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (!rt_uia_is_available(widget))
        return E_FAIL;
    if (widget->type != VG_WIDGET_TEXTINPUT)
        return E_FAIL;
    vg_textinput_t *input = (vg_textinput_t *)widget;
    if (input->read_only)
        return E_FAIL;
    if (!value)
        value = L"";
    int utf8_count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (utf8_count <= 0)
        return E_INVALIDARG;
    char *utf8 = (char *)malloc((size_t)utf8_count);
    if (!utf8)
        return E_OUTOFMEMORY;
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, utf8_count, NULL, NULL) != utf8_count) {
        free(utf8);
        return E_INVALIDARG;
    }
    vg_textinput_set_text(input, utf8);
    free(utf8);
    return S_OK;
}

/// @brief Materialize the guarded control's current textual value.
/// @param iface Borrowed value-pattern interface.
/// @param out Receives a caller-owned BSTR.
/// @return COM success, pointer, stale-widget, or allocation status.
static HRESULT STDMETHODCALLTYPE rt_uia_value_get_Value(IValueProvider *iface, BSTR *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, value);
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (!widget)
        return E_FAIL;
    if (widget->accessibility.value) {
        *out = rt_uia_bstr(widget->accessibility.value);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    switch (widget->type) {
        case VG_WIDGET_TEXTINPUT:
            *out = rt_uia_bstr(((const vg_textinput_t *)widget)->text);
            return *out ? S_OK : E_OUTOFMEMORY;
        case VG_WIDGET_DROPDOWN: {
            const vg_dropdown_t *dropdown = (const vg_dropdown_t *)widget;
            const char *selected = dropdown->selected_index >= 0 &&
                                           dropdown->selected_index < dropdown->item_count &&
                                           dropdown->items
                                       ? dropdown->items[dropdown->selected_index]
                                       : "";
            *out = rt_uia_bstr(selected);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        case VG_WIDGET_CODEEDITOR: {
            char *text = vg_codeeditor_get_text((vg_codeeditor_t *)widget);
            if (!text)
                return E_OUTOFMEMORY;
            *out = rt_uia_bstr(text);
            free(text);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        default:
            *out = rt_uia_bstr("");
            return *out ? S_OK : E_OUTOFMEMORY;
    }
}

/// @brief Report whether UI Automation may mutate the guarded value.
/// @param iface Borrowed value-pattern interface.
/// @param out Receives `FALSE` only for a writable text input.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE rt_uia_value_get_IsReadOnly(IValueProvider *iface, BOOL *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, value);
    if (!out)
        return E_POINTER;
    vg_widget_t *widget = rt_uia_resolve(provider);
    if (widget && widget->type == VG_WIDGET_TEXTINPUT)
        *out = ((const vg_textinput_t *)widget)->read_only ? TRUE : FALSE;
    else
        *out = TRUE;
    return S_OK;
}

static IValueProviderVtbl g_rt_uia_value_vtbl = {
    rt_uia_value_QueryInterface,
    rt_uia_value_AddRef,
    rt_uia_value_Release,
    rt_uia_value_SetValue,
    rt_uia_value_get_Value,
    rt_uia_value_get_IsReadOnly,
};

//----------------------------------------------------------------------------
// IRangeValueProvider (read-only projection of slider/progress/spinner)
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_range, IRangeValueProvider, range_value)

/// @brief Normalize a slider, progress bar, or spinner into a bounded numeric range.
/// @param widget Borrowed range-like widget.
/// @param value Receives the finite value clamped to the normalized bounds.
/// @param minimum Receives the finite lower bound.
/// @param maximum Receives the finite upper bound.
/// @return 1 for a supported widget and valid destinations, otherwise 0.
static int32_t rt_uia_range_read(vg_widget_t *widget,
                                 double *value,
                                 double *minimum,
                                 double *maximum) {
    if (!widget || !value || !minimum || !maximum)
        return 0;
    switch (widget->type) {
        case VG_WIDGET_SLIDER: {
            const vg_slider_t *slider = (const vg_slider_t *)widget;
            *value = slider->value;
            *minimum = slider->min_value;
            *maximum = slider->max_value;
            break;
        }
        case VG_WIDGET_PROGRESS: {
            const vg_progressbar_t *progress = (const vg_progressbar_t *)widget;
            *value = progress->value;
            *minimum = 0.0;
            *maximum = 1.0;
            break;
        }
        case VG_WIDGET_SPINNER: {
            const vg_spinner_t *spinner = (const vg_spinner_t *)widget;
            *value = spinner->value;
            *minimum = spinner->min_value;
            *maximum = spinner->max_value;
            break;
        }
        default:
            return 0;
    }
    if (!isfinite(*minimum) || !isfinite(*maximum)) {
        *minimum = 0.0;
        *maximum = 0.0;
    } else if (*maximum < *minimum) {
        double swap = *minimum;
        *minimum = *maximum;
        *maximum = swap;
    }
    if (!isfinite(*maximum - *minimum))
        *maximum = *minimum;
    if (!isfinite(*value))
        *value = *minimum;
    if (*value < *minimum)
        *value = *minimum;
    if (*value > *maximum)
        *value = *maximum;
    return 1;
}

/// @brief Reject direct UI Automation range mutation in the current read-only projection.
/// @param iface Borrowed range-pattern interface.
/// @param value Requested numeric value.
/// @return `E_FAIL`; keyboard adjustment remains the supported mutation path.
static HRESULT STDMETHODCALLTYPE rt_uia_range_SetValue(IRangeValueProvider *iface, double value) {
    (void)iface;
    (void)value;
    // Focus-and-arrow-keys remains the adjustment path in this bridge revision.
    return E_FAIL;
}

/// @brief Generate a COM range-property getter from normalized value and bounds.
#define RT_UIA_RANGE_GETTER(name, expression)                                                      \
    static HRESULT STDMETHODCALLTYPE name(IRangeValueProvider *iface, double *out) {               \
        rt_uia_provider_t *provider = RT_UIA_FROM(iface, range_value);                             \
        if (!out)                                                                                  \
            return E_POINTER;                                                                      \
        *out = 0.0;                                                                                \
        double value = 0.0, minimum = 0.0, maximum = 0.0;                                          \
        if (!rt_uia_range_read(rt_uia_resolve(provider), &value, &minimum, &maximum))              \
            return E_FAIL;                                                                         \
        *out = (expression);                                                                       \
        return S_OK;                                                                               \
    }

RT_UIA_RANGE_GETTER(rt_uia_range_get_Value, value)
RT_UIA_RANGE_GETTER(rt_uia_range_get_Maximum, maximum)
RT_UIA_RANGE_GETTER(rt_uia_range_get_Minimum, minimum)

/// @brief Report that the projected range pattern is read-only.
/// @param iface Borrowed range-pattern interface.
/// @param out Receives `TRUE`.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE rt_uia_range_get_IsReadOnly(IRangeValueProvider *iface,
                                                             BOOL *out) {
    (void)iface;
    if (!out)
        return E_POINTER;
    *out = TRUE;
    return S_OK;
}

RT_UIA_RANGE_GETTER(rt_uia_range_get_LargeChange, (maximum - minimum) / 10.0)
RT_UIA_RANGE_GETTER(rt_uia_range_get_SmallChange, (maximum - minimum) / 100.0)

static IRangeValueProviderVtbl g_rt_uia_range_vtbl = {
    rt_uia_range_QueryInterface,
    rt_uia_range_AddRef,
    rt_uia_range_Release,
    rt_uia_range_SetValue,
    rt_uia_range_get_Value,
    rt_uia_range_get_IsReadOnly,
    rt_uia_range_get_Maximum,
    rt_uia_range_get_Minimum,
    rt_uia_range_get_LargeChange,
    rt_uia_range_get_SmallChange,
};

//----------------------------------------------------------------------------
// ISelectionItemProvider
//----------------------------------------------------------------------------

RT_UIA_IUNKNOWN_THUNKS(rt_uia_selitem, ISelectionItemProvider, selection_item)

/// @brief Select the guarded item through its ordinary activation event path.
/// @param iface Borrowed selection-item interface.
/// @return `S_OK` when activation was handled, otherwise `E_FAIL`.
static HRESULT STDMETHODCALLTYPE rt_uia_selitem_Select(ISelectionItemProvider *iface) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, selection_item);
    return rt_uia_activate(rt_uia_resolve(provider)) ? S_OK : E_FAIL;
}

/// @brief Reject additive selection for the current single-selection projection.
/// @param iface Borrowed selection-item interface.
/// @return `E_FAIL` because additive selection is unsupported.
static HRESULT STDMETHODCALLTYPE rt_uia_selitem_AddToSelection(ISelectionItemProvider *iface) {
    (void)iface;
    return E_FAIL; // Single-selection surfaces only in this bridge revision.
}

/// @brief Reject direct removal from the current single-selection projection.
/// @param iface Borrowed selection-item interface.
/// @return `E_FAIL` because direct removal is unsupported.
static HRESULT STDMETHODCALLTYPE rt_uia_selitem_RemoveFromSelection(ISelectionItemProvider *iface) {
    (void)iface;
    return E_FAIL;
}

/// @brief Report whether the guarded item is selected or checked.
/// @param iface Borrowed selection-item interface.
/// @param out Receives the normalized selection state.
/// @return `S_OK`, or `E_POINTER` when @p out is NULL.
static HRESULT STDMETHODCALLTYPE rt_uia_selitem_get_IsSelected(ISelectionItemProvider *iface,
                                                               BOOL *out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, selection_item);
    if (!out)
        return E_POINTER;
    vg_widget_t *widget = rt_uia_resolve(provider);
    *out = widget && (widget->state & (VG_STATE_SELECTED | VG_STATE_CHECKED)) ? TRUE : FALSE;
    return S_OK;
}

/// @brief Find the nearest list, tree, tab list, or table selection container.
/// @param iface Borrowed selection-item interface.
/// @param out Receives a retained simple-provider interface, or NULL when no container exists.
/// @return COM success, pointer, or allocation status.
static HRESULT STDMETHODCALLTYPE rt_uia_selitem_get_SelectionContainer(
    ISelectionItemProvider *iface, IRawElementProviderSimple **out) {
    rt_uia_provider_t *provider = RT_UIA_FROM(iface, selection_item);
    if (!out)
        return E_POINTER;
    *out = NULL;
    vg_widget_t *widget = rt_uia_resolve(provider);
    rt_uia_window_bridge_t *bridge = rt_uia_resolve_bridge(provider);
    if (!widget || !bridge)
        return S_OK;
    for (vg_widget_t *ancestor = widget->parent; ancestor; ancestor = ancestor->parent) {
        vg_accessible_role_t role = ancestor->accessibility.role;
        if (role == VG_ACCESSIBLE_ROLE_LIST || role == VG_ACCESSIBLE_ROLE_TREE ||
            role == VG_ACCESSIBLE_ROLE_TABLIST || role == VG_ACCESSIBLE_ROLE_TABLE) {
            rt_uia_provider_t *container = rt_uia_provider_create(bridge, ancestor);
            if (!container)
                return E_OUTOFMEMORY;
            *out = &container->simple;
            break;
        }
    }
    return S_OK;
}

static ISelectionItemProviderVtbl g_rt_uia_selitem_vtbl = {
    rt_uia_selitem_QueryInterface,
    rt_uia_selitem_AddRef,
    rt_uia_selitem_Release,
    rt_uia_selitem_Select,
    rt_uia_selitem_AddToSelection,
    rt_uia_selitem_RemoveFromSelection,
    rt_uia_selitem_get_IsSelected,
    rt_uia_selitem_get_SelectionContainer,
};

//----------------------------------------------------------------------------
// Provider construction
//----------------------------------------------------------------------------

/// @brief Create a provider for one live widget with a caller-owned reference.
/// @param bridge Borrowed live bridge whose generation the provider captures.
/// @param widget Borrowed live widget within the bridge's semantic tree.
/// @return Allocated provider with one caller-owned reference, or NULL on validation/allocation
/// failure.
static rt_uia_provider_t *rt_uia_provider_create(rt_uia_window_bridge_t *bridge,
                                                 vg_widget_t *widget) {
    if (!bridge || !bridge->window || bridge->generation == 0 ||
        !rt_uia_bridge_contains_widget(bridge, widget))
        return NULL;
    rt_uia_provider_t *provider = (rt_uia_provider_t *)calloc(1, sizeof(*provider));
    if (!provider)
        return NULL;
    provider->simple.lpVtbl = &g_rt_uia_simple_vtbl;
    provider->fragment.lpVtbl = &g_rt_uia_fragment_vtbl;
    provider->fragment_root.lpVtbl = &g_rt_uia_root_vtbl;
    provider->invoke.lpVtbl = &g_rt_uia_invoke_vtbl;
    provider->toggle.lpVtbl = &g_rt_uia_toggle_vtbl;
    provider->value.lpVtbl = &g_rt_uia_value_vtbl;
    provider->range_value.lpVtbl = &g_rt_uia_range_vtbl;
    provider->selection_item.lpVtbl = &g_rt_uia_selitem_vtbl;
    provider->ref_count = 1;
    provider->widget = widget;
    provider->widget_id = widget->id;
    provider->bridge = bridge;
    provider->bridge_generation = bridge->generation;
    provider->is_root = bridge->root == widget;
    return provider;
}

//=============================================================================
// WM_GETOBJECT hook
//=============================================================================

/// @brief Answer WM_GETOBJECT with this window's root UIA provider.
/// @param user Borrowed bridge registered as the native message-hook context.
/// @param native_window Native HWND receiving the message.
/// @param msg Win32 message identifier.
/// @param wparam Message word parameter.
/// @param lparam Message long parameter.
/// @param result Receives the UI Automation provider result when handled.
/// @param handled Receives 1 when this hook answers the message.
static void rt_uia_msg_hook(void *user,
                            void *native_window,
                            uint32_t msg,
                            uintptr_t wparam,
                            intptr_t lparam,
                            intptr_t *result,
                            int32_t *handled) {
    rt_uia_window_bridge_t *bridge = (rt_uia_window_bridge_t *)user;
    if (!bridge || msg != WM_GETOBJECT || (LONG)lparam != UiaRootObjectId)
        return;
    rt_uia_api_t *api = rt_uia_api();
    if (!api->return_raw_element_provider || !bridge->root_provider ||
        rt_uia_resolve_bridge(bridge->root_provider) != bridge)
        return;
    if (!vg_widget_is_live(bridge->root) || bridge->root->id != bridge->root_id)
        return;
    *result = (intptr_t)api->return_raw_element_provider(
        (HWND)native_window, (WPARAM)wparam, (LPARAM)lparam, &bridge->root_provider->simple);
    *handled = 1;
}

//=============================================================================
// Preference queries
//=============================================================================

/// @brief Query the Win32 HIGHCONTRAST system parameter.
/// @param window Borrowed window handle; unused because the setting is process-independent.
/// @return One when the HCF_HIGHCONTRASTON flag is active, otherwise zero.
int32_t rt_gui_accessibility_platform_high_contrast(vgfx_window_t window) {
    (void)window;
    HIGHCONTRASTW contrast = {0};
    contrast.cbSize = sizeof(contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0))
        return 0;
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0 ? 1 : 0;
}

/// @brief Query whether Win32 client-area animation has been disabled.
/// @details SPI_GETCLIENTAREAANIMATION is the closest native equivalent to a reduced-motion
///          preference. Older SDKs fall back to the general animation setting.
/// @param window Borrowed window handle; unused because the setting is process-independent.
/// @return One when interface animation is disabled, otherwise zero.
int32_t rt_gui_accessibility_platform_reduced_motion(vgfx_window_t window) {
    (void)window;
#ifdef SPI_GETCLIENTAREAANIMATION
    BOOL animations_enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0)) {
        return animations_enabled ? 0 : 1;
    }
#endif
    ANIMATIONINFO animation = {0};
    animation.cbSize = sizeof(animation);
    if (!SystemParametersInfoW(SPI_GETANIMATION, sizeof(animation), &animation, 0))
        return 0;
    return animation.iMinAnimate ? 0 : 1;
}

/// @brief Query Windows' per-user application light/dark preference.
/// @details `AppsUseLightTheme` is a DWORD maintained by Windows personalization settings. A
///          missing key, access failure, unexpected type, or malformed size uses the stable light
///          fallback. The registry handle is predefined and therefore requires no cleanup.
/// @param window Borrowed ZannaGFX window; unused because the setting is per-user.
/// @return One when applications should use dark mode, otherwise zero.
int32_t rt_gui_accessibility_platform_prefers_dark(vgfx_window_t window) {
    (void)window;
    DWORD apps_use_light = 1;
    DWORD byte_count = (DWORD)sizeof(apps_use_light);
    LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD,
                     NULL,
                     &apps_use_light,
                     &byte_count);
    if (status != ERROR_SUCCESS || byte_count != (DWORD)sizeof(apps_use_light))
        return 0;
    return apps_use_light == 0 ? 1 : 0;
}

//=============================================================================
// Bridge lifecycle
//=============================================================================

/// @brief Attach the UI Automation projection to one live GUI window.
/// @param window Borrowed ZannaGFX window whose HWND answers WM_GETOBJECT.
/// @param root Borrowed semantic widget root; NULL delegates to detach behavior.
void rt_gui_accessibility_platform_attach(vgfx_window_t window, vg_widget_t *root) {
    if (!window)
        return;
    if (!vg_widget_is_live(root)) {
        rt_gui_accessibility_platform_detach(window);
        return;
    }
    if (!rt_uia_available())
        return;
    HWND hwnd = (HWND)vgfx_get_native_view(window);
    if (!hwnd)
        return;

    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window(window);
    if (!bridge) {
        for (int i = 0; i < RT_UIA_MAX_BRIDGES; ++i) {
            if (!g_rt_uia_bridges[i].window) {
                bridge = &g_rt_uia_bridges[i];
                break;
            }
        }
    }
    if (!bridge)
        return;

    if (bridge->root_provider) {
        rt_uia_api_t *api = rt_uia_api();
        if (api->disconnect_provider)
            api->disconnect_provider(&bridge->root_provider->simple);
        rt_uia_provider_release(bridge->root_provider);
    }
    rt_uia_bridge_reset(bridge);
    bridge->window = window;
    bridge->hwnd = hwnd;
    bridge->root = root;
    bridge->root_id = root->id;
    bridge->root_provider = rt_uia_provider_create(bridge, root);
    if (!bridge->root_provider) {
        rt_uia_bridge_reset(bridge);
        return;
    }
    vgfx_set_native_msg_hook(window, rt_uia_msg_hook, bridge);
}

/// @brief Detach the UI Automation projection before window/root destruction.
/// @param window Borrowed ZannaGFX window being detached; NULL is a no-op.
void rt_gui_accessibility_platform_detach(vgfx_window_t window) {
    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window(window);
    if (!bridge)
        return;
    vgfx_set_native_msg_hook(window, NULL, NULL);
    rt_uia_api_t *api = rt_uia_api();
    if (bridge->root_provider) {
        if (api->disconnect_provider)
            api->disconnect_provider(&bridge->root_provider->simple);
        rt_uia_provider_release(bridge->root_provider);
    }
    rt_uia_bridge_reset(bridge);
}

/// @brief Raise UIA change events for one live semantic node.
/// @param window Borrowed ZannaGFX window owning @p widget; may be NULL.
/// @param widget Borrowed changed widget; NULL/stale handles are ignored.
void rt_gui_accessibility_platform_notify(vgfx_window_t window, vg_widget_t *widget) {
    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window(window);
    if (!bridge || !vg_widget_is_live(widget))
        return;
    rt_uia_api_t *api = rt_uia_api();
    if (api->clients_are_listening && !api->clients_are_listening())
        return;
    rt_uia_provider_t *provider = rt_uia_provider_create(bridge, widget);
    if (!provider)
        return;
    if (api->raise_property_changed_event) {
        VARIANT empty_old, empty_new;
        VariantInit(&empty_old);
        VariantInit(&empty_new);
        api->raise_property_changed_event(
            &provider->simple, UIA_ValueValuePropertyId, empty_old, empty_new);
    }
    if ((widget->state & VG_STATE_FOCUSED) && api->raise_automation_event)
        api->raise_automation_event(&provider->simple, UIA_AutomationFocusChangedEventId);
    rt_uia_provider_release(provider);
}

/// @brief Synchronize the Win32 projection after layout; current providers resolve live state.
/// @details UI Automation providers read the retained widget tree on demand, so no eager rebuild
///          is required after layout.
/// @param window Borrowed ZannaGFX window associated with the tree.
/// @param root Borrowed semantic root.
void rt_gui_accessibility_platform_sync(vgfx_window_t window, vg_widget_t *root) {
    (void)window;
    (void)root;
}

/// @brief Project one live-region announcement through UIA notification events.
/// @param window Borrowed owning ZannaGFX window; may be NULL.
/// @param widget Borrowed announcement source widget; may be NULL or stale.
/// @param text UTF-8 announcement text borrowed for the call; may be NULL.
/// @param mode Live-region urgency (`VG_LIVE_REGION_POLITE` or `VG_LIVE_REGION_ASSERTIVE`).
void rt_gui_accessibility_platform_announce(vgfx_window_t window,
                                            vg_widget_t *widget,
                                            const char *text,
                                            vg_live_region_mode_t mode) {
    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window(window);
    rt_uia_api_t *api = rt_uia_api();
    if (!bridge || !bridge->root_provider || !api->raise_notification_event)
        return;
    if (api->clients_are_listening && !api->clients_are_listening())
        return;
    (void)widget;
    BSTR display = rt_uia_bstr(text);
    BSTR activity = SysAllocString(L"ZannaLiveRegion");
    if (!display || !activity) {
        SysFreeString(display);
        SysFreeString(activity);
        return;
    }
    api->raise_notification_event(&bridge->root_provider->simple,
                                  RT_UIA_NOTIFICATION_KIND_OTHER,
                                  mode == VG_LIVE_REGION_ASSERTIVE
                                      ? RT_UIA_NOTIFICATION_PROCESSING_IMPORTANT_ALL
                                      : RT_UIA_NOTIFICATION_PROCESSING_ALL,
                                  display,
                                  activity);
    SysFreeString(display);
    SysFreeString(activity);
}

//=============================================================================
// Test seam (headless vtable exercise without a UIA client)
//=============================================================================

/// @brief Create a caller-owned root provider for direct vtable tests.
/// @details Test-only entry point: installs a bridge keyed by an opaque
///          pointer (no live ZannaGFX window or uiautomationcore.dll is
///          required), so the COM plumbing (QueryInterface routing,
///          navigation, properties, patterns) is testable headlessly.
///          Release through the returned interface, then call
///          rt_gui_accessibility_win32_test_teardown with the same key.
///          Not part of any public runtime contract.
/// @param bridge_key Opaque non-NULL identity for the simulated window.
/// @param root Borrowed live semantic root widget.
/// @return IRawElementProviderSimple with one caller-owned reference, or NULL.
IRawElementProviderSimple *rt_gui_accessibility_win32_test_root(void *bridge_key,
                                                                vg_widget_t *root) {
    if (!bridge_key || !vg_widget_is_live(root))
        return NULL;
    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window((vgfx_window_t)bridge_key);
    if (!bridge) {
        for (int i = 0; i < RT_UIA_MAX_BRIDGES; ++i) {
            if (!g_rt_uia_bridges[i].window) {
                bridge = &g_rt_uia_bridges[i];
                break;
            }
        }
    }
    if (!bridge)
        return NULL;
    if (bridge->root_provider)
        rt_uia_provider_release(bridge->root_provider);
    rt_uia_bridge_reset(bridge);
    bridge->window = (vgfx_window_t)bridge_key;
    bridge->hwnd = NULL;
    bridge->root = root;
    bridge->root_id = root->id;
    bridge->root_provider = rt_uia_provider_create(bridge, root);
    if (!bridge->root_provider) {
        rt_uia_bridge_reset(bridge);
        return NULL;
    }
    rt_uia_provider_addref(bridge->root_provider);
    return &bridge->root_provider->simple;
}

/// @brief Release the bridge installed by rt_gui_accessibility_win32_test_root.
/// @param bridge_key Opaque identity previously passed to the test seam.
void rt_gui_accessibility_win32_test_teardown(void *bridge_key) {
    rt_uia_window_bridge_t *bridge = rt_uia_bridge_for_window((vgfx_window_t)bridge_key);
    if (!bridge)
        return;
    if (bridge->root_provider)
        rt_uia_provider_release(bridge->root_provider);
    rt_uia_bridge_reset(bridge);
}
