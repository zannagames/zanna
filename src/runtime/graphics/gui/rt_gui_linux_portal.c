//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_linux_portal.c
// Purpose: Query org.freedesktop.portal.Settings without a build-time GIO dependency.
// Key invariants:
//   - Portal calls have a short finite timeout and validate the returned variant type.
//   - Loader initialization is immutable after its first process-local attempt.
// Ownership/Lifetime: See rt_gui_linux_portal.h.
// Links: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html
//
//===----------------------------------------------------------------------===//

#include "rt_gui_linux_portal.h"

#include <dlfcn.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

typedef struct GDBusConnection GDBusConnection;
typedef struct GVariant GVariant;
typedef struct GError GError;

typedef struct rt_gui_gio_api {
    void *library;
    GDBusConnection *(*bus_get_sync)(int bus_type, void *cancellable, GError **error);
    GVariant *(*variant_new)(const char *format, ...);
    GVariant *(*connection_call_sync)(GDBusConnection *connection,
                                      const char *bus_name,
                                      const char *object_path,
                                      const char *interface_name,
                                      const char *method_name,
                                      GVariant *parameters,
                                      const void *reply_type,
                                      int flags,
                                      int timeout_msec,
                                      void *cancellable,
                                      GError **error);
    GVariant *(*variant_get_child_value)(GVariant *value, size_t index);
    GVariant *(*variant_get_variant)(GVariant *value);
    const char *(*variant_get_type_string)(GVariant *value);
    uint32_t (*variant_get_uint32)(GVariant *value);
    int (*variant_get_boolean)(GVariant *value);
    void (*variant_unref)(GVariant *value);
    void (*object_unref)(void *object);
    void (*error_free)(GError *error);
} rt_gui_gio_api_t;

static rt_gui_gio_api_t g_rt_gui_gio;
static atomic_int g_rt_gui_gio_state;

/// @brief Initialize the process-wide late-bound GIO function table exactly once.
/// @details One calling thread atomically claims initialization, tries the versioned and unversioned
///          Linux GIO sonames, and resolves every symbol required by the Settings portal request.
///          Contending callers yield while initialization is in progress. State is then published
///          with release semantics as permanently ready or unavailable; a partially resolved table
///          is closed and cleared before failure is published. A successful library handle remains
///          open for process lifetime so the cached function pointers cannot become stale.
/// @return One when the complete immutable GIO table is available, otherwise zero.
static int rt_gui_linux_portal_load(void) {
    int state = atomic_load_explicit(&g_rt_gui_gio_state, memory_order_acquire);
    if (state >= 2)
        return state == 2;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_rt_gui_gio_state,
                                                 &expected,
                                                 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        while ((state = atomic_load_explicit(&g_rt_gui_gio_state, memory_order_acquire)) == 1) {
            sched_yield();
        }
        return state == 2;
    }
    const char *names[] = {"libgio-2.0.so.0", "libgio-2.0.so"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_rt_gui_gio.library; ++i)
        g_rt_gui_gio.library = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!g_rt_gui_gio.library) {
        atomic_store_explicit(&g_rt_gui_gio_state, 3, memory_order_release);
        return 0;
    }

#define RT_GUI_GIO_LOAD(field, symbol)                                                            \
    do {                                                                                           \
        *(void **)(&g_rt_gui_gio.field) = dlsym(g_rt_gui_gio.library, symbol);                     \
        if (!g_rt_gui_gio.field) {                                                                 \
            dlclose(g_rt_gui_gio.library);                                                         \
            memset(&g_rt_gui_gio, 0, sizeof(g_rt_gui_gio));                                       \
            atomic_store_explicit(&g_rt_gui_gio_state, 3, memory_order_release);                    \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
    RT_GUI_GIO_LOAD(bus_get_sync, "g_bus_get_sync");
    RT_GUI_GIO_LOAD(variant_new, "g_variant_new");
    RT_GUI_GIO_LOAD(connection_call_sync, "g_dbus_connection_call_sync");
    RT_GUI_GIO_LOAD(variant_get_child_value, "g_variant_get_child_value");
    RT_GUI_GIO_LOAD(variant_get_variant, "g_variant_get_variant");
    RT_GUI_GIO_LOAD(variant_get_type_string, "g_variant_get_type_string");
    RT_GUI_GIO_LOAD(variant_get_uint32, "g_variant_get_uint32");
    RT_GUI_GIO_LOAD(variant_get_boolean, "g_variant_get_boolean");
    RT_GUI_GIO_LOAD(variant_unref, "g_variant_unref");
    RT_GUI_GIO_LOAD(object_unref, "g_object_unref");
    RT_GUI_GIO_LOAD(error_free, "g_error_free");
#undef RT_GUI_GIO_LOAD
    atomic_store_explicit(&g_rt_gui_gio_state, 2, memory_order_release);
    return 1;
}

/// @brief Read one integer or Boolean value from the desktop Settings portal.
/// @details Late-loads GIO, connects to the session bus, and synchronously invokes
///          `org.freedesktop.portal.Settings.Read` with a 250-millisecond timeout. The reply must
///          contain a variant whose concrete type is unsigned 32-bit integer or Boolean; unsigned
///          values above @c INT32_MAX are rejected. Every connection, result, wrapper, value, and
///          error object acquired by the call is released before return. No build-time GIO symbols
///          are referenced, allowing portal support to remain optional on Linux installations.
/// @param name_space Borrowed non-NULL NUL-terminated portal namespace such as
///                   `org.freedesktop.appearance`.
/// @param key Borrowed non-NULL NUL-terminated setting key.
/// @param[out] out_value Receives the decoded signed value only after a valid supported reply;
///                       remains unchanged on failure.
/// @return One when @p out_value was written, otherwise zero for invalid arguments, unavailable
///         GIO/bus/portal service, timeout, malformed reply, unsupported type, or range failure.
int rt_gui_linux_portal_read(const char *name_space, const char *key, int32_t *out_value) {
    if (!name_space || !key || !out_value || !rt_gui_linux_portal_load())
        return 0;
    GError *error = NULL;
    GDBusConnection *connection = g_rt_gui_gio.bus_get_sync(2, NULL, &error);
    if (!connection) {
        if (error)
            g_rt_gui_gio.error_free(error);
        return 0;
    }
    GVariant *result = g_rt_gui_gio.connection_call_sync(
        connection,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings",
        "Read",
        g_rt_gui_gio.variant_new("(ss)", name_space, key),
        NULL,
        0,
        250,
        NULL,
        &error);
    g_rt_gui_gio.object_unref(connection);
    if (!result) {
        if (error)
            g_rt_gui_gio.error_free(error);
        return 0;
    }
    GVariant *wrapper = g_rt_gui_gio.variant_get_child_value(result, 0);
    GVariant *value = wrapper ? g_rt_gui_gio.variant_get_variant(wrapper) : NULL;
    int found = 0;
    if (value) {
        const char *type = g_rt_gui_gio.variant_get_type_string(value);
        if (type && strcmp(type, "u") == 0) {
            uint32_t decoded = g_rt_gui_gio.variant_get_uint32(value);
            if (decoded <= INT32_MAX) {
                *out_value = (int32_t)decoded;
                found = 1;
            }
        } else if (type && strcmp(type, "b") == 0) {
            *out_value = g_rt_gui_gio.variant_get_boolean(value) ? 1 : 0;
            found = 1;
        }
        g_rt_gui_gio.variant_unref(value);
    }
    if (wrapper)
        g_rt_gui_gio.variant_unref(wrapper);
    g_rt_gui_gio.variant_unref(result);
    return found;
}
