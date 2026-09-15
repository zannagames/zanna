//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_input.c
// Purpose: Binds ISteamInput (Steam Input) for the Steam provider's
//          Zanna.Services.ActionInput operations: action manifests, the
//          connected controller snapshot, action sets and layers, digital and
//          analog action data, origins with labels and glyphs, rumble,
//          controller lights, the binding panel, and device callbacks.
// Key invariants:
//   - The group is usable only when its accessor and every export resolved;
//     otherwise one diagnostic names what is missing.
//   - Steam Input starts only through ActionInput.Start (Init with explicit
//     frames); each provider pump then runs one input frame and refreshes the
//     controller snapshot before dispatching callbacks.
//   - Controller ids are decimal InputHandle_t values; a malformed id traps
//     while Steam is the active provider. The empty id reaches Steam as
//     STEAM_INPUT_HANDLE_ALL_CONTROLLERS.
//   - Handles are looked up by name once and cached, unknown names included;
//     the caches are cleared on Start, on Stop, and whenever a controller
//     connects or a configuration loads.
//   - Steam accepts an action manifest only once it holds a controller mapping
//     for the app; until then SetInputActionManifestFilePath times out and
//     returns false. A refused manifest leaves Steam Input running and is
//     retried whenever a controller connects, before the connection event is
//     reported.
//   - Glyph paths are returned with '/' separators outside Windows; Steam for
//     macOS joins part of the path with backslashes.
// Ownership/Lifetime:
//   - Cache names and the pending manifest path are heap copies freed when the
//     caches are cleared, the manifest is accepted, or Steam Input stops.
//   - Strings Steam returns are copied into caller-owned runtime strings.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_input.h,
//        docs/adr/0365-platform-services-action-input.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_input.c
 * @brief Implements Steam Input for Zanna.Services.ActionInput.
 */

#include "rt_platform.h"
#include "rt_services.h"
#include "rt_services_input.h"
#include "rt_services_provider.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Binding
//===----------------------------------------------------------------------===//

/// @brief Resolve one export into a typed field, remembering the first missing name.
#define STEAM_BIND(field, type, name, missing)                                                     \
    do {                                                                                           \
        void *symbol_ = rt_services_steam_symbol(name);                                            \
        if (!symbol_ && !(missing))                                                                \
            (missing) = (name);                                                                    \
        (field) = symbol_ ? RT_FN_PTR_CAST((type)symbol_) : NULL;                                  \
    } while (0)

/// @brief Open and bind ISteamInput.
void rt_services_steam_bind_input(void) {
    static const char *const accessors[] = {RT_STEAM_SYMBOL_INPUT_V007, RT_STEAM_SYMBOL_INPUT_V006};
    steam_input_api api;
    memset(&api, 0, sizeof(api));
    for (size_t i = 0; i < sizeof(accessors) / sizeof(accessors[0]) && !api.self; ++i) {
        void *accessor = rt_services_steam_symbol(accessors[i]);
        if (accessor)
            api.self = (RT_FN_PTR_CAST((rt_steam_accessor_fn)accessor))();
    }
    if (!api.self) {
        rt_services_steam_report_missing(
            RT_STEAM_SYMBOL_INPUT_V007 " or " RT_STEAM_SYMBOL_INPUT_V006, "action input");
        return;
    }
    const char *missing = NULL;
    STEAM_BIND(api.init, rt_steam_input_init_fn, RT_STEAM_SYMBOL_INPUT_INIT, missing);
    STEAM_BIND(api.shutdown, rt_steam_self_bool_fn, RT_STEAM_SYMBOL_INPUT_SHUTDOWN, missing);
    STEAM_BIND(
        api.set_manifest, rt_steam_self_str_bool_fn, RT_STEAM_SYMBOL_INPUT_SET_MANIFEST, missing);
    STEAM_BIND(
        api.run_frame, rt_steam_input_run_frame_fn, RT_STEAM_SYMBOL_INPUT_RUN_FRAME, missing);
    STEAM_BIND(
        api.connected, rt_steam_input_controllers_fn, RT_STEAM_SYMBOL_INPUT_CONNECTED, missing);
    STEAM_BIND(api.device_callbacks,
               rt_steam_self_void_fn,
               RT_STEAM_SYMBOL_INPUT_DEVICE_CALLBACKS,
               missing);
    STEAM_BIND(api.action_set_handle,
               rt_steam_self_str_u64_fn,
               RT_STEAM_SYMBOL_INPUT_ACTION_SET_HANDLE,
               missing);
    STEAM_BIND(api.activate_set,
               rt_steam_self_u64_u64_void_fn,
               RT_STEAM_SYMBOL_INPUT_ACTIVATE_SET,
               missing);
    STEAM_BIND(
        api.current_set, rt_steam_self_u64_u64_fn, RT_STEAM_SYMBOL_INPUT_CURRENT_SET, missing);
    STEAM_BIND(api.activate_layer,
               rt_steam_self_u64_u64_void_fn,
               RT_STEAM_SYMBOL_INPUT_ACTIVATE_LAYER,
               missing);
    STEAM_BIND(api.deactivate_layer,
               rt_steam_self_u64_u64_void_fn,
               RT_STEAM_SYMBOL_INPUT_DEACTIVATE_LAYER,
               missing);
    STEAM_BIND(api.deactivate_all_layers,
               rt_steam_self_u64_void_fn,
               RT_STEAM_SYMBOL_INPUT_DEACTIVATE_ALL_LAYERS,
               missing);
    STEAM_BIND(api.digital_handle,
               rt_steam_self_str_u64_fn,
               RT_STEAM_SYMBOL_INPUT_DIGITAL_HANDLE,
               missing);
    STEAM_BIND(api.digital_data,
               rt_steam_input_digital_data_fn,
               RT_STEAM_SYMBOL_INPUT_DIGITAL_DATA,
               missing);
    STEAM_BIND(api.digital_origins,
               rt_steam_input_origins_fn,
               RT_STEAM_SYMBOL_INPUT_DIGITAL_ORIGINS,
               missing);
    STEAM_BIND(
        api.digital_name, rt_steam_self_u64_cstr_fn, RT_STEAM_SYMBOL_INPUT_DIGITAL_NAME, missing);
    STEAM_BIND(
        api.analog_handle, rt_steam_self_str_u64_fn, RT_STEAM_SYMBOL_INPUT_ANALOG_HANDLE, missing);
    STEAM_BIND(
        api.analog_data, rt_steam_input_analog_data_fn, RT_STEAM_SYMBOL_INPUT_ANALOG_DATA, missing);
    STEAM_BIND(api.analog_origins,
               rt_steam_input_origins_fn,
               RT_STEAM_SYMBOL_INPUT_ANALOG_ORIGINS,
               missing);
    STEAM_BIND(
        api.analog_name, rt_steam_self_u64_cstr_fn, RT_STEAM_SYMBOL_INPUT_ANALOG_NAME, missing);
    STEAM_BIND(
        api.glyph_png, rt_steam_input_glyph_png_fn, RT_STEAM_SYMBOL_INPUT_GLYPH_PNG, missing);
    STEAM_BIND(
        api.origin_name, rt_steam_self_int_cstr_fn, RT_STEAM_SYMBOL_INPUT_ORIGIN_NAME, missing);
    STEAM_BIND(api.vibrate, rt_steam_input_vibration_fn, RT_STEAM_SYMBOL_INPUT_VIBRATION, missing);
    STEAM_BIND(api.led_color, rt_steam_input_led_fn, RT_STEAM_SYMBOL_INPUT_LED_COLOR, missing);
    STEAM_BIND(
        api.binding_panel, rt_steam_self_u64_bool_fn, RT_STEAM_SYMBOL_INPUT_BINDING_PANEL, missing);
    STEAM_BIND(api.input_type, rt_steam_self_u64_int_fn, RT_STEAM_SYMBOL_INPUT_TYPE, missing);
    STEAM_BIND(
        api.gamepad_index, rt_steam_self_u64_int_fn, RT_STEAM_SYMBOL_INPUT_GAMEPAD_INDEX, missing);
    if (missing) {
        rt_services_steam_report_missing(missing, "action input");
        return;
    }
    api.ready = 1;
    g_steam.input = api;
}

#undef STEAM_BIND

//===----------------------------------------------------------------------===//
// Handle caches
//===----------------------------------------------------------------------===//

/// @brief Free one handle cache.
/// @param cache Cache to clear.
static void steam_input_clear_cache(steam_input_handle_cache *cache) {
    for (int i = 0; i < cache->count; ++i)
        free(cache->entries[i].name);
    free(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

/// @brief Free every handle cache.
static void steam_input_clear_caches(void) {
    steam_input_clear_cache(&g_steam.input.action_sets);
    steam_input_clear_cache(&g_steam.input.digital_actions);
    steam_input_clear_cache(&g_steam.input.analog_actions);
}

/// @brief Look a name up through a cache.
/// @param cache Cache for the handle kind.
/// @param lookup Steam lookup for the handle kind.
/// @param name Non-empty manifest name.
/// @return Handle, or 0 when Steam does not know the name.
static uint64_t steam_input_handle(steam_input_handle_cache *cache,
                                   rt_steam_self_str_u64_fn lookup,
                                   const char *name) {
    for (int i = 0; i < cache->count; ++i) {
        if (strcmp(cache->entries[i].name, name) == 0)
            return cache->entries[i].handle;
    }
    const uint64_t handle = lookup(g_steam.input.self, name);
    if (cache->count == cache->capacity) {
        const int capacity = cache->capacity ? cache->capacity * 2 : 16;
        steam_input_handle_entry *grown = (steam_input_handle_entry *)realloc(
            cache->entries, (size_t)capacity * sizeof(steam_input_handle_entry));
        if (!grown)
            return handle;
        cache->entries = grown;
        cache->capacity = capacity;
    }
    const size_t length = strlen(name);
    char *copy = (char *)malloc(length + 1);
    if (!copy)
        return handle;
    memcpy(copy, name, length + 1);
    cache->entries[cache->count].name = copy;
    cache->entries[cache->count].handle = handle;
    cache->count++;
    return handle;
}

/// @brief Look up an action set or layer handle.
/// @param name Non-empty set or layer name.
/// @return Handle, or 0.
static uint64_t steam_input_set_handle(const char *name) {
    return steam_input_handle(&g_steam.input.action_sets, g_steam.input.action_set_handle, name);
}

/// @brief Look up a digital action handle.
/// @param name Non-empty action name.
/// @return Handle, or 0.
static uint64_t steam_input_digital_handle(const char *name) {
    return steam_input_handle(&g_steam.input.digital_actions, g_steam.input.digital_handle, name);
}

/// @brief Look up an analog action handle.
/// @param name Non-empty action name.
/// @return Handle, or 0.
static uint64_t steam_input_analog_handle(const char *name) {
    return steam_input_handle(&g_steam.input.analog_actions, g_steam.input.analog_handle, name);
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Report whether Steam Input is bound and started.
/// @return 1 when calls may reach Steam, otherwise 0.
static int steam_input_ready(void) {
    return g_steam.started && g_steam.input.ready && g_steam.input.started;
}

/// @brief Convert a controller id to an InputHandle_t.
/// @param controller_id Decimal handle, or "" for every controller.
/// @param out Receives the handle.
/// @return 1 when converted, otherwise 0.
static int steam_input_controller(const char *controller_id, uint64_t *out) {
    if (!controller_id[0]) {
        *out = RT_STEAM_INPUT_HANDLE_ALL_CONTROLLERS;
        return 1;
    }
    return rt_services_steam_parse_u64(controller_id, out) &&
           *out != RT_STEAM_INPUT_HANDLE_ALL_CONTROLLERS;
}

/// @brief Copy borrowed Steam text into a runtime string.
/// @param text Borrowed text, or NULL.
/// @return Owned copy, or NULL when @p text is NULL or empty.
static rt_string steam_input_text(const char *text) {
    return (text && *text) ? rt_const_cstr(text) : NULL;
}

/// @brief Forget a manifest Steam has not accepted.
static void steam_input_clear_pending_manifest(void) {
    free(g_steam.input.pending_manifest);
    g_steam.input.pending_manifest = NULL;
}

/// @brief Shut Steam Input down and forget controllers, handles, and the pending manifest.
static void steam_input_shutdown(void) {
    steam_input_api *input = &g_steam.input;
    if (input->started && input->self && input->shutdown)
        input->shutdown(input->self);
    input->started = 0;
    input->controller_count = 0;
    steam_input_clear_caches();
    steam_input_clear_pending_manifest();
}

/// @brief Hand a manifest to Steam, remembering it for a retry when Steam refuses it.
/// @param manifest_path Absolute manifest path.
/// @return 1 when Steam accepted the manifest, otherwise 0.
static int steam_input_apply_manifest(const char *manifest_path) {
    steam_input_api *input = &g_steam.input;
    if (input->set_manifest(input->self, manifest_path)) {
        // manifest_path may be the pending copy itself; it is not used after this.
        steam_input_clear_pending_manifest();
        steam_input_clear_caches();
        return 1;
    }
    if (input->pending_manifest != manifest_path) {
        steam_input_clear_pending_manifest();
        const size_t length = strlen(manifest_path);
        input->pending_manifest = (char *)malloc(length + 1);
        if (input->pending_manifest)
            memcpy(input->pending_manifest, manifest_path, length + 1);
    }
    return 0;
}

//===----------------------------------------------------------------------===//
// Lifecycle and controllers
//===----------------------------------------------------------------------===//

/// @brief Run a Steam Input frame and refresh the connected controllers.
void rt_services_steam_input_frame(void) {
    steam_input_api *input = &g_steam.input;
    if (!steam_input_ready())
        return;
    input->run_frame(input->self, false);
    uint64_t handles[RT_STEAM_INPUT_MAX_COUNT];
    memset(handles, 0, sizeof(handles));
    int count = input->connected(input->self, handles);
    if (count < 0)
        count = 0;
    if (count > RT_STEAM_INPUT_MAX_COUNT)
        count = RT_STEAM_INPUT_MAX_COUNT;
    memcpy(input->controllers, handles, (size_t)count * sizeof(handles[0]));
    input->controller_count = count;
}

/// @brief Shut Steam Input down before SteamAPI_Shutdown.
void rt_services_steam_stop_input(void) {
    steam_input_shutdown();
}

/// @brief Start Steam Input.
/// @param manifest_path Absolute manifest path, or "" for the Steamworks configuration.
/// @return 1 when started, otherwise 0.
static int8_t steam_input_start(const char *manifest_path) {
    steam_input_api *input = &g_steam.input;
    if (!g_steam.started || !input->ready)
        return 0;
    if (!input->started) {
        if (!input->init(input->self, true)) {
            rt_services_provider_add_diagnostic("Steam: ISteamInput Init failed");
            return 0;
        }
        input->started = 1;
        input->device_callbacks(input->self);
    }
    steam_input_clear_caches();
    int8_t accepted = 1;
    if (manifest_path[0]) {
        accepted = steam_input_apply_manifest(manifest_path) ? 1 : 0;
        if (!accepted) {
            rt_services_provider_add_diagnostic(
                "Steam: SetInputActionManifestFilePath('%s') failed; Steam accepts a manifest once "
                "it has a controller mapping for the app, so the manifest is retried when a "
                "controller connects (also check that the file is a Steam Input action manifest)",
                manifest_path);
        }
    } else {
        steam_input_clear_pending_manifest();
    }
    rt_services_steam_input_frame();
    return accepted;
}

/// @brief Stop Steam Input.
/// @return 1 when it was started, otherwise 0.
static int8_t steam_input_stop(void) {
    if (!g_steam.input.started)
        return 0;
    steam_input_shutdown();
    return 1;
}

/// @brief Report whether Steam Input is started.
/// @return 1 when started, otherwise 0.
static int8_t steam_input_is_started(void) {
    return steam_input_ready() ? 1 : 0;
}

/// @brief Trap unless a controller id is a decimal InputHandle_t.
/// @param member Class-qualified member name for the trap.
/// @param controller_id Non-empty candidate id.
/// @return 1 when well formed, 0 after reporting the trap.
static int8_t steam_input_check_controller_id(const char *member, const char *controller_id) {
    uint64_t handle = 0;
    if (steam_input_controller(controller_id, &handle))
        return 1;
    char message[320];
    snprintf(message,
             sizeof(message),
             "Services.%s: Steam controller id '%s' must be an integer in "
             "1..18446744073709551614",
             member,
             controller_id);
    rt_trap(message);
    return 0;
}

/// @brief Count connected controllers.
/// @return Controllers at the last pump.
static int64_t steam_input_controller_count(void) {
    return steam_input_ready() ? g_steam.input.controller_count : 0;
}

/// @brief Write the id of a connected controller.
/// @param index Non-negative index.
/// @param out_id Destination buffer.
/// @param id_capacity Size of @p out_id in bytes.
/// @return 1 when written, otherwise 0.
static int8_t steam_input_controller_id_at(int64_t index, char *out_id, size_t id_capacity) {
    if (!steam_input_ready() || index < 0 || index >= g_steam.input.controller_count)
        return 0;
    snprintf(out_id, id_capacity, "%llu", (unsigned long long)g_steam.input.controllers[index]);
    return 1;
}

/// @brief Read a controller's type.
/// @param controller_id Decimal handle.
/// @return ControllerType value (same ordinals as ESteamInputType).
static int64_t steam_input_controller_type(const char *controller_id) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
    const int type = g_steam.input.input_type(g_steam.input.self, controller);
    return (type >= 0 && type <= RT_SERVICES_CONTROLLER_TYPE_STEAM_FRAME_CONTROLLER_PAIR)
               ? (int64_t)type
               : RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
}

/// @brief Read the gamepad slot a controller emulates.
/// @param controller_id Decimal handle.
/// @return Slot, or -1.
static int64_t steam_input_gamepad_index(const char *controller_id) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return -1;
    const int index = g_steam.input.gamepad_index(g_steam.input.self, controller);
    return index < 0 ? -1 : (int64_t)index;
}

//===----------------------------------------------------------------------===//
// Action sets and layers
//===----------------------------------------------------------------------===//

/// @brief Activate an action set.
/// @param controller_id Decimal handle, or "" for every controller.
/// @param action_set Action set name.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_input_activate_action_set(const char *controller_id, const char *action_set) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !steam_input_controller(controller_id, &controller))
        return 0;
    const uint64_t set = steam_input_set_handle(action_set);
    if (set == 0) {
        rt_services_provider_add_diagnostic("Steam: action set '%s' is not in the action manifest",
                                            action_set);
        return 0;
    }
    g_steam.input.activate_set(g_steam.input.self, controller, set);
    return 1;
}

/// @brief Activate or deactivate an action set layer.
/// @param controller_id Decimal handle, or "" for every controller.
/// @param layer Layer name.
/// @param active Nonzero to activate.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_input_set_layer_active(const char *controller_id,
                                           const char *layer,
                                           int8_t active) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !steam_input_controller(controller_id, &controller))
        return 0;
    const uint64_t handle = steam_input_set_handle(layer);
    if (handle == 0) {
        rt_services_provider_add_diagnostic(
            "Steam: action set layer '%s' is not in the action manifest", layer);
        return 0;
    }
    if (active)
        g_steam.input.activate_layer(g_steam.input.self, controller, handle);
    else
        g_steam.input.deactivate_layer(g_steam.input.self, controller, handle);
    return 1;
}

/// @brief Deactivate every layer.
/// @param controller_id Decimal handle, or "" for every controller.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_input_deactivate_all_layers(const char *controller_id) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !steam_input_controller(controller_id, &controller))
        return 0;
    g_steam.input.deactivate_all_layers(g_steam.input.self, controller);
    return 1;
}

//===----------------------------------------------------------------------===//
// Actions
//===----------------------------------------------------------------------===//

/// @brief Read a digital action.
/// @param controller_id Decimal handle.
/// @param action Action name.
/// @param out_pressed Receives 1 while pressed.
/// @param out_active Receives 1 when available.
/// @return 1 when the action is defined, otherwise 0.
static int8_t steam_input_digital_action(const char *controller_id,
                                         const char *action,
                                         int8_t *out_pressed,
                                         int8_t *out_active) {
    uint64_t controller = 0;
    *out_pressed = 0;
    *out_active = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    const uint64_t handle = steam_input_digital_handle(action);
    if (handle == 0)
        return 0;
    const rt_steam_input_digital_data data =
        g_steam.input.digital_data(g_steam.input.self, controller, handle);
    *out_pressed = data.state ? 1 : 0;
    *out_active = data.active ? 1 : 0;
    return 1;
}

/// @brief Read an analog action.
/// @param controller_id Decimal handle.
/// @param action Action name.
/// @param out_x Receives the horizontal value.
/// @param out_y Receives the vertical value.
/// @param out_active Receives 1 when available.
/// @return 1 when the action is defined, otherwise 0.
static int8_t steam_input_analog_action(const char *controller_id,
                                        const char *action,
                                        double *out_x,
                                        double *out_y,
                                        int8_t *out_active) {
    uint64_t controller = 0;
    *out_x = 0.0;
    *out_y = 0.0;
    *out_active = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    const uint64_t handle = steam_input_analog_handle(action);
    if (handle == 0)
        return 0;
    const rt_steam_input_analog_data data =
        g_steam.input.analog_data(g_steam.input.self, controller, handle);
    *out_x = isfinite(data.x) ? (double)data.x : 0.0;
    *out_y = isfinite(data.y) ? (double)data.y : 0.0;
    *out_active = data.active ? 1 : 0;
    return 1;
}

/// @brief Read an action's localized name.
/// @param action Action name.
/// @return Owned name, or NULL.
static rt_string steam_input_action_label(const char *action) {
    if (!steam_input_ready())
        return NULL;
    const uint64_t digital = steam_input_digital_handle(action);
    if (digital != 0)
        return steam_input_text(g_steam.input.digital_name(g_steam.input.self, digital));
    const uint64_t analog = steam_input_analog_handle(action);
    return analog != 0 ? steam_input_text(g_steam.input.analog_name(g_steam.input.self, analog))
                       : NULL;
}

/// @brief Read the physical inputs bound to an action.
/// @param controller_id Decimal handle.
/// @param action_set Action set name, or "" for the controller's current set.
/// @param action Action name.
/// @param out_origins Receives origin ids.
/// @param capacity Entries available in @p out_origins.
/// @return Number of origins written.
static int64_t steam_input_action_origins(const char *controller_id,
                                          const char *action_set,
                                          const char *action,
                                          int64_t *out_origins,
                                          int64_t capacity) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    uint64_t set = 0;
    if (action_set[0]) {
        set = steam_input_set_handle(action_set);
        if (set == 0) {
            rt_services_provider_add_diagnostic(
                "Steam: action set '%s' is not in the action manifest", action_set);
            return 0;
        }
    } else {
        set = g_steam.input.current_set(g_steam.input.self, controller);
        if (set == 0)
            return 0;
    }
    int32_t origins[RT_STEAM_INPUT_MAX_ORIGINS];
    memset(origins, 0, sizeof(origins));
    int count = 0;
    const uint64_t digital = steam_input_digital_handle(action);
    if (digital != 0) {
        count =
            g_steam.input.digital_origins(g_steam.input.self, controller, set, digital, origins);
    } else {
        const uint64_t analog = steam_input_analog_handle(action);
        if (analog == 0)
            return 0;
        count = g_steam.input.analog_origins(g_steam.input.self, controller, set, analog, origins);
    }
    if (count < 0)
        count = 0;
    if (count > RT_STEAM_INPUT_MAX_ORIGINS)
        count = RT_STEAM_INPUT_MAX_ORIGINS;
    if (count > capacity)
        count = (int)capacity;
    for (int i = 0; i < count; ++i)
        out_origins[i] = (int64_t)origins[i];
    return count;
}

/// @brief Read an origin's localized name.
/// @param origin Origin id.
/// @return Owned name, or NULL.
static rt_string steam_input_origin_label(int64_t origin) {
    if (!steam_input_ready() || origin <= 0 || origin > RT_STEAM_INPUT_MAX_ORIGIN)
        return NULL;
    return steam_input_text(g_steam.input.origin_name(g_steam.input.self, (int)origin));
}

/// @brief Read the image file of an origin's glyph.
/// @param origin Origin id.
/// @param size GlyphSize value (same ordinals as ESteamInputGlyphSize).
/// @return Owned path, or NULL.
static rt_string steam_input_origin_glyph_path(int64_t origin, int64_t size) {
    if (!steam_input_ready() || origin <= 0 || origin > RT_STEAM_INPUT_MAX_ORIGIN)
        return NULL;
    rt_string path =
        steam_input_text(g_steam.input.glyph_png(g_steam.input.self, (int)origin, (int)size, 0u));
#if !RT_PLATFORM_WINDOWS
    if (path) {
        const char *text = rt_string_cstr(path);
        if (strchr(text, '\\')) {
            const size_t length = strlen(text);
            char *copy = (char *)malloc(length + 1);
            if (copy) {
                for (size_t i = 0; i <= length; ++i)
                    copy[i] = text[i] == '\\' ? '/' : text[i];
                rt_string_unref(path);
                path = rt_const_cstr(copy);
                free(copy);
            }
        }
    }
#endif
    return path;
}

//===----------------------------------------------------------------------===//
// Feedback and platform interface
//===----------------------------------------------------------------------===//

/// @brief Run a controller's rumble motors.
/// @param controller_id Decimal handle.
/// @param left Left strength in 0..1.
/// @param right Right strength in 0..1.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_input_vibrate(const char *controller_id, double left, double right) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    g_steam.input.vibrate(g_steam.input.self,
                          controller,
                          (uint16_t)lround(left * 65535.0),
                          (uint16_t)lround(right * 65535.0));
    return 1;
}

/// @brief Set or restore a controller's light color.
/// @param controller_id Decimal handle.
/// @param red Red component in 0..255.
/// @param green Green component in 0..255.
/// @param blue Blue component in 0..255.
/// @param restore Nonzero to restore the player's color.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_input_set_led_color(
    const char *controller_id, int64_t red, int64_t green, int64_t blue, int8_t restore) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    g_steam.input.led_color(g_steam.input.self,
                            controller,
                            (uint8_t)red,
                            (uint8_t)green,
                            (uint8_t)blue,
                            restore ? RT_STEAM_INPUT_LED_RESTORE_USER_DEFAULT
                                    : RT_STEAM_INPUT_LED_SET_COLOR);
    return 1;
}

/// @brief Open the binding panel for a controller.
/// @param controller_id Decimal handle.
/// @return 1 when opened, otherwise 0.
static int8_t steam_input_show_binding_panel(const char *controller_id) {
    uint64_t controller = 0;
    if (!steam_input_ready() || !controller_id[0] ||
        !steam_input_controller(controller_id, &controller))
        return 0;
    if (g_steam.input.binding_panel(g_steam.input.self, controller))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: ShowBindingPanel failed; the Steam overlay must be available or Steam must be in "
        "Big Picture mode");
    return 0;
}

/// @brief Steam action input operations.
const rt_services_action_input_ops rt_services_steam_action_input_ops = {
    .start = steam_input_start,
    .stop = steam_input_stop,
    .is_started = steam_input_is_started,
    .check_controller_id = steam_input_check_controller_id,
    .controller_count = steam_input_controller_count,
    .controller_id_at = steam_input_controller_id_at,
    .controller_type = steam_input_controller_type,
    .gamepad_index = steam_input_gamepad_index,
    .activate_action_set = steam_input_activate_action_set,
    .set_layer_active = steam_input_set_layer_active,
    .deactivate_all_layers = steam_input_deactivate_all_layers,
    .digital_action = steam_input_digital_action,
    .analog_action = steam_input_analog_action,
    .action_label = steam_input_action_label,
    .action_origins = steam_input_action_origins,
    .origin_label = steam_input_origin_label,
    .origin_glyph_path = steam_input_origin_glyph_path,
    .vibrate = steam_input_vibrate,
    .set_led_color = steam_input_set_led_color,
    .show_binding_panel = steam_input_show_binding_panel,
};

//===----------------------------------------------------------------------===//
// Callbacks
//===----------------------------------------------------------------------===//

/// @brief Decode Steam Input device and configuration callbacks.
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a Steam Input callback, otherwise 0.
int rt_services_steam_input_callback(const rt_steam_callback_msg *msg) {
    char id[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    switch (msg->callback_id) {
        case RT_STEAM_CB_INPUT_DEVICE_CONNECTED:
        case RT_STEAM_CB_INPUT_DEVICE_DISCONNECTED: {
            rt_steam_input_device payload;
            if (!g_steam.input.started || !rt_services_steam_payload_matches(msg, sizeof(payload)))
                return 1;
            memcpy(&payload, msg->param, sizeof(payload));
            snprintf(id, sizeof(id), "%llu", (unsigned long long)payload.device);
            const int connected = msg->callback_id == RT_STEAM_CB_INPUT_DEVICE_CONNECTED;
            if (connected) {
                steam_input_clear_caches();
                // Steam gains a controller mapping for the app with a connection, so a
                // refused manifest may be accepted now; apply it before games react.
                if (g_steam.input.pending_manifest)
                    (void)steam_input_apply_manifest(g_steam.input.pending_manifest);
            }
            rt_services_provider_emit_event(connected ? RT_SERVICES_EVENT_CONTROLLER_CONNECTED
                                                      : RT_SERVICES_EVENT_CONTROLLER_DISCONNECTED,
                                            0,
                                            0,
                                            0,
                                            id);
            return 1;
        }
        case RT_STEAM_CB_INPUT_CONFIGURATION_LOADED: {
            rt_steam_input_configuration_loaded payload;
            if (!g_steam.input.started || !rt_services_steam_payload_matches(msg, sizeof(payload)))
                return 1;
            memcpy(&payload, msg->param, sizeof(payload));
            snprintf(id, sizeof(id), "%llu", (unsigned long long)payload.device);
            steam_input_clear_caches();
            rt_services_provider_emit_event(RT_SERVICES_EVENT_CONTROLLER_CONFIGURED,
                                            0,
                                            (int64_t)payload.major_revision,
                                            payload.uses_input_api ? 1 : 0,
                                            id);
            return 1;
        }
        default:
            return 0;
    }
}
