//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_input.c
// Purpose: Implements the provider-neutral Zanna.Services.ActionInput class on
//          top of the active provider's action input operation table, and the
//          ControllerType and GlyphSize constant classes.
// Key invariants:
//   - Every member checks the main thread first, then the argument rules that
//     hold for every provider (traps), then the provider's controller id
//     format (traps while that provider is active), then whether action input
//     is started (false with a diagnostic), and only then delegates.
//   - An empty controller id is expanded here into the connected controllers,
//     except for activation members, which pass it on so the provider can
//     reach controllers connected later.
// Ownership/Lifetime:
//   - Controller ids are copied from fixed buffers into caller-owned strings.
//   - The absolute manifest path is a temporary runtime string released
//     before Start returns.
// Links: src/runtime/services/rt_services_input.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0365-platform-services-action-input.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_input.c
 * @brief Implements Zanna.Services.ActionInput.
 */

#include "rt_services_input.h"

#include "rt_file_ext.h"
#include "rt_path.h"
#include "rt_services.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <string.h>

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's action input operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_action_input_ops *input_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->action_input : NULL;
}

/// @brief Validate the controller id and require started action input.
/// @details A malformed non-empty id traps while the active provider defines
///          its format. Action input that is not started records
///          "Services: <member> needs ActionInput.Start first".
/// @param member Class-qualified member name.
/// @param controller_id Controller id argument; NULL is the empty id.
/// @param out_id Receives the borrowed id bytes.
/// @return Operation table when the call may proceed, otherwise NULL.
static const rt_services_action_input_ops *input_enter(const char *member,
                                                       rt_string controller_id,
                                                       const char **out_id) {
    const rt_services_action_input_ops *ops = input_ops();
    const char *id = rt_services_internal_cstr(controller_id);
    *out_id = id;
    if (!ops)
        return NULL;
    if (id[0] && ops->check_controller_id && !ops->check_controller_id(member, id))
        return NULL;
    if (!ops->is_started || !ops->is_started()) {
        rt_services_provider_add_diagnostic("Services: %s needs ActionInput.Start first", member);
        return NULL;
    }
    return ops;
}

/// @brief Count the connected controllers.
/// @param ops Operation table.
/// @return Count, clamped to the documented maximum.
static int64_t input_controller_count(const rt_services_action_input_ops *ops) {
    if (!ops->controller_count || !ops->controller_id_at)
        return 0;
    int64_t count = ops->controller_count();
    if (count < 0)
        return 0;
    return count > RT_SERVICES_ACTION_INPUT_MAX_CONTROLLERS
               ? RT_SERVICES_ACTION_INPUT_MAX_CONTROLLERS
               : count;
}

/// @brief Read the id of a connected controller.
/// @param ops Operation table.
/// @param index Index in 0..count-1.
/// @param out Destination of RT_SERVICES_CONTROLLER_ID_CAPACITY bytes.
/// @return 1 when written, otherwise 0.
static int input_controller_at(const rt_services_action_input_ops *ops, int64_t index, char *out) {
    out[0] = '\0';
    return ops->controller_id_at(index, out, RT_SERVICES_CONTROLLER_ID_CAPACITY) && out[0];
}

/// @brief Resolve an empty controller id to the first connected controller.
/// @param ops Operation table.
/// @param id Validated id argument.
/// @param buffer Destination of RT_SERVICES_CONTROLLER_ID_CAPACITY bytes.
/// @return @p id when non-empty, @p buffer holding the first controller, or NULL when none.
static const char *input_first_or_id(const rt_services_action_input_ops *ops,
                                     const char *id,
                                     char *buffer) {
    if (id[0])
        return id;
    return (input_controller_count(ops) > 0 && input_controller_at(ops, 0, buffer)) ? buffer : NULL;
}

/// @brief Trap unless a value lies in a closed integer range.
/// @param member Class-qualified member name.
/// @param what Argument name.
/// @param value Candidate value.
/// @param low Lowest accepted value.
/// @param high Highest accepted value.
/// @return 1 when in range, 0 after reporting the trap.
static int input_require_range(
    const char *member, const char *what, int64_t value, int64_t low, int64_t high) {
    if (value >= low && value <= high)
        return 1;
    rt_services_internal_trap_argument(member,
                                       "%s must be in %lld..%lld (got %lld)",
                                       what,
                                       (long long)low,
                                       (long long)high,
                                       (long long)value);
    return 0;
}

/// @brief Trap unless a motor strength lies in 0..1.
/// @param member Class-qualified member name.
/// @param what Argument name.
/// @param value Candidate strength.
/// @return 1 when valid, 0 after reporting the trap.
static int input_require_strength(const char *member, const char *what, double value) {
    if (value >= 0.0 && value <= 1.0)
        return 1;
    rt_services_internal_trap_argument(member, "%s must be in 0..1 (got %g)", what, value);
    return 0;
}

/// @brief Read an analog action for one or every controller.
/// @param member Class-qualified member name.
/// @param controller_id Controller id argument.
/// @param action Action name argument.
/// @param out_x Receives the horizontal value.
/// @param out_y Receives the vertical value.
static void input_analog(
    const char *member, rt_string controller_id, rt_string action, double *out_x, double *out_y) {
    *out_x = 0.0;
    *out_y = 0.0;
    if (!rt_services_provider_require_main_thread(member))
        return;
    const char *name = rt_services_internal_require_name(action, member, "action name");
    if (!name)
        return;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->analog_action)
        return;
    double x = 0.0;
    double y = 0.0;
    int8_t active = 0;
    if (id[0]) {
        if (!ops->analog_action(id, name, &x, &y, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no analog action '%s' in the action manifest", member, name);
            return;
        }
        if (active) {
            *out_x = x;
            *out_y = y;
        }
        return;
    }
    const int64_t count = input_controller_count(ops);
    double longest = -1.0;
    for (int64_t i = 0; i < count; ++i) {
        char controller[RT_SERVICES_CONTROLLER_ID_CAPACITY];
        if (!input_controller_at(ops, i, controller))
            continue;
        if (!ops->analog_action(controller, name, &x, &y, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no analog action '%s' in the action manifest", member, name);
            return;
        }
        const double length = x * x + y * y;
        if (active && length > longest) {
            longest = length;
            *out_x = x;
            *out_y = y;
        }
    }
}

/// @brief Read whether a controller's digital or analog action is available.
/// @param ops Operation table.
/// @param controller Non-empty controller id.
/// @param name Action name.
/// @param out_active Receives 1 when available.
/// @return 1 when the action is defined, otherwise 0.
static int input_action_active(const rt_services_action_input_ops *ops,
                               const char *controller,
                               const char *name,
                               int8_t *out_active) {
    int8_t pressed = 0;
    double x = 0.0;
    double y = 0.0;
    *out_active = 0;
    if (ops->digital_action && ops->digital_action(controller, name, &pressed, out_active))
        return 1;
    return ops->analog_action && ops->analog_action(controller, name, &x, &y, out_active);
}

/// @brief Read the origins of an action for the resolved controller.
/// @param member Class-qualified member name.
/// @param controller_id Controller id argument.
/// @param action_set Action set argument.
/// @param action Action name argument.
/// @param out_origins Receives up to RT_SERVICES_ACTION_INPUT_MAX_ORIGINS origins.
/// @return Number of origins, or -1 after a trap.
static int64_t input_origins(const char *member,
                             rt_string controller_id,
                             rt_string action_set,
                             rt_string action,
                             int64_t *out_origins) {
    if (!rt_services_provider_require_main_thread(member))
        return -1;
    const char *name = rt_services_internal_require_name(action, member, "action name");
    if (!name)
        return -1;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->action_origins)
        return 0;
    char first[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    const char *controller = input_first_or_id(ops, id, first);
    if (!controller)
        return 0;
    int64_t count = ops->action_origins(controller,
                                        rt_services_internal_cstr(action_set),
                                        name,
                                        out_origins,
                                        RT_SERVICES_ACTION_INPUT_MAX_ORIGINS);
    if (count < 0)
        return 0;
    return count > RT_SERVICES_ACTION_INPUT_MAX_ORIGINS ? RT_SERVICES_ACTION_INPUT_MAX_ORIGINS
                                                        : count;
}

/// @brief Apply a light change to one or every controller.
/// @param member Class-qualified member name.
/// @param controller_id Controller id argument.
/// @param red Red component.
/// @param green Green component.
/// @param blue Blue component.
/// @param restore Nonzero to restore the player's color.
/// @return 1 when any controller received the change, otherwise 0.
static int8_t input_led(const char *member,
                        rt_string controller_id,
                        int64_t red,
                        int64_t green,
                        int64_t blue,
                        int8_t restore) {
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->set_led_color)
        return 0;
    if (id[0])
        return ops->set_led_color(id, red, green, blue, restore) ? 1 : 0;
    int8_t any = 0;
    const int64_t count = input_controller_count(ops);
    for (int64_t i = 0; i < count; ++i) {
        char controller[RT_SERVICES_CONTROLLER_ID_CAPACITY];
        if (input_controller_at(ops, i, controller) &&
            ops->set_led_color(controller, red, green, blue, restore))
            any = 1;
    }
    return any;
}

//===----------------------------------------------------------------------===//
// Lifecycle and controllers
//===----------------------------------------------------------------------===//

/// @brief Start action input.
/// @param manifest_path Manifest file, or "" for the platform's configuration.
/// @return 1 when started, otherwise 0.
int8_t rt_services_action_input_start(rt_string manifest_path) {
    static const char member[] = "ActionInput.Start";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const rt_services_action_input_ops *ops = input_ops();
    if (!ops || !ops->start)
        return 0;
    if (!rt_services_internal_cstr(manifest_path)[0])
        return ops->start("") ? 1 : 0;
    rt_string absolute = rt_path_abs(manifest_path);
    if (!absolute)
        return 0;
    int8_t started = 0;
    if (!rt_io_file_exists(absolute)) {
        rt_services_provider_add_diagnostic(
            "Services: ActionInput.Start found no action manifest at '%s'",
            rt_string_cstr(absolute));
    } else {
        started = ops->start(rt_string_cstr(absolute)) ? 1 : 0;
    }
    rt_string_unref(absolute);
    return started;
}

/// @brief Stop action input.
/// @return 1 when it was started, otherwise 0.
int8_t rt_services_action_input_stop(void) {
    if (!rt_services_provider_require_main_thread("ActionInput.Stop"))
        return 0;
    const rt_services_action_input_ops *ops = input_ops();
    return (ops && ops->stop && ops->stop()) ? 1 : 0;
}

/// @brief Report whether action input is started.
/// @return 1 when started, otherwise 0.
int8_t rt_services_action_input_get_is_started(void) {
    if (!rt_services_provider_require_main_thread("ActionInput.IsStarted"))
        return 0;
    const rt_services_action_input_ops *ops = input_ops();
    return (ops && ops->is_started && ops->is_started()) ? 1 : 0;
}

/// @brief Count connected controllers.
/// @return 0..16.
int64_t rt_services_action_input_get_controller_count(void) {
    if (!rt_services_provider_require_main_thread("ActionInput.ControllerCount"))
        return 0;
    const rt_services_action_input_ops *ops = input_ops();
    if (!ops || !ops->is_started || !ops->is_started())
        return 0;
    return input_controller_count(ops);
}

/// @brief Read the id of the connected controller at an index.
/// @param index Index.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_action_input_controller_id_at(int64_t index) {
    if (!rt_services_provider_require_main_thread("ActionInput.ControllerIdAt"))
        return rt_str_empty();
    const rt_services_action_input_ops *ops = input_ops();
    if (!ops || !ops->is_started || !ops->is_started() || index < 0 ||
        index >= input_controller_count(ops))
        return rt_str_empty();
    char id[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    return input_controller_at(ops, index, id) ? rt_services_internal_owned_text(id)
                                               : rt_str_empty();
}

/// @brief Read a controller's type.
/// @param controller_id Controller id, or "" for the first controller.
/// @return ControllerType value.
int64_t rt_services_action_input_controller_type(rt_string controller_id) {
    static const char member[] = "ActionInput.ControllerType";
    if (!rt_services_provider_require_main_thread(member))
        return RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->controller_type)
        return RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
    char first[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    const char *controller = input_first_or_id(ops, id, first);
    if (!controller)
        return RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
    const int64_t type = ops->controller_type(controller);
    return (type >= RT_SERVICES_CONTROLLER_TYPE_UNKNOWN &&
            type <= RT_SERVICES_CONTROLLER_TYPE_STEAM_FRAME_CONTROLLER_PAIR)
               ? type
               : RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
}

/// @brief Read the gamepad slot a controller emulates.
/// @param controller_id Controller id, or "" for the first controller.
/// @return Slot, or -1.
int64_t rt_services_action_input_gamepad_index(rt_string controller_id) {
    static const char member[] = "ActionInput.GamepadIndex";
    if (!rt_services_provider_require_main_thread(member))
        return -1;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->gamepad_index)
        return -1;
    char first[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    const char *controller = input_first_or_id(ops, id, first);
    if (!controller)
        return -1;
    const int64_t index = ops->gamepad_index(controller);
    return index < 0 ? -1 : index;
}

//===----------------------------------------------------------------------===//
// Action sets and layers
//===----------------------------------------------------------------------===//

/// @brief Activate an action set.
/// @param controller_id Controller id, or "" for every controller.
/// @param action_set Action set name.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_activate_action_set(rt_string controller_id, rt_string action_set) {
    static const char member[] = "ActionInput.ActivateActionSet";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *name = rt_services_internal_require_name(action_set, member, "action set name");
    if (!name)
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    return (ops && ops->activate_action_set && ops->activate_action_set(id, name)) ? 1 : 0;
}

/// @brief Activate or deactivate a layer.
/// @param member Class-qualified member name.
/// @param controller_id Controller id, or "" for every controller.
/// @param layer Layer name.
/// @param active Nonzero to activate.
/// @return 1 when passed to the platform, otherwise 0.
static int8_t input_set_layer(const char *member,
                              rt_string controller_id,
                              rt_string layer,
                              int8_t active) {
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *name = rt_services_internal_require_name(layer, member, "layer name");
    if (!name)
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    return (ops && ops->set_layer_active && ops->set_layer_active(id, name, active)) ? 1 : 0;
}

/// @brief Activate a layer.
/// @param controller_id Controller id, or "" for every controller.
/// @param layer Layer name.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_activate_layer(rt_string controller_id, rt_string layer) {
    return input_set_layer("ActionInput.ActivateLayer", controller_id, layer, 1);
}

/// @brief Deactivate a layer.
/// @param controller_id Controller id, or "" for every controller.
/// @param layer Layer name.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_deactivate_layer(rt_string controller_id, rt_string layer) {
    return input_set_layer("ActionInput.DeactivateLayer", controller_id, layer, 0);
}

/// @brief Deactivate every layer.
/// @param controller_id Controller id, or "" for every controller.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_deactivate_all_layers(rt_string controller_id) {
    static const char member[] = "ActionInput.DeactivateAllLayers";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    return (ops && ops->deactivate_all_layers && ops->deactivate_all_layers(id)) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Actions
//===----------------------------------------------------------------------===//

/// @brief Report whether a digital action is pressed.
/// @param controller_id Controller id, or "" for any controller.
/// @param action Digital action name.
/// @return 1 while pressed, otherwise 0.
int8_t rt_services_action_input_is_pressed(rt_string controller_id, rt_string action) {
    static const char member[] = "ActionInput.IsPressed";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *name = rt_services_internal_require_name(action, member, "action name");
    if (!name)
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->digital_action)
        return 0;
    int8_t pressed = 0;
    int8_t active = 0;
    if (id[0]) {
        if (!ops->digital_action(id, name, &pressed, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no digital action '%s' in the action manifest", member, name);
            return 0;
        }
        return (pressed && active) ? 1 : 0;
    }
    const int64_t count = input_controller_count(ops);
    for (int64_t i = 0; i < count; ++i) {
        char controller[RT_SERVICES_CONTROLLER_ID_CAPACITY];
        if (!input_controller_at(ops, i, controller))
            continue;
        if (!ops->digital_action(controller, name, &pressed, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no digital action '%s' in the action manifest", member, name);
            return 0;
        }
        if (pressed && active)
            return 1;
    }
    return 0;
}

/// @brief Read the horizontal value of an analog action.
/// @param controller_id Controller id, or "" for the longest vector.
/// @param action Analog action name.
/// @return Value, or 0.
double rt_services_action_input_analog_x(rt_string controller_id, rt_string action) {
    double x = 0.0;
    double y = 0.0;
    input_analog("ActionInput.AnalogX", controller_id, action, &x, &y);
    return x;
}

/// @brief Read the vertical value of an analog action.
/// @param controller_id Controller id, or "" for the longest vector.
/// @param action Analog action name.
/// @return Value, or 0.
double rt_services_action_input_analog_y(rt_string controller_id, rt_string action) {
    double x = 0.0;
    double y = 0.0;
    input_analog("ActionInput.AnalogY", controller_id, action, &x, &y);
    return y;
}

/// @brief Report whether an action is available in the active set.
/// @param controller_id Controller id, or "" for any controller.
/// @param action Action name.
/// @return 1 when available, otherwise 0.
int8_t rt_services_action_input_is_action_active(rt_string controller_id, rt_string action) {
    static const char member[] = "ActionInput.IsActionActive";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *name = rt_services_internal_require_name(action, member, "action name");
    if (!name)
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops)
        return 0;
    int8_t active = 0;
    if (id[0]) {
        if (!input_action_active(ops, id, name, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no action '%s' in the action manifest", member, name);
            return 0;
        }
        return active ? 1 : 0;
    }
    const int64_t count = input_controller_count(ops);
    for (int64_t i = 0; i < count; ++i) {
        char controller[RT_SERVICES_CONTROLLER_ID_CAPACITY];
        if (!input_controller_at(ops, i, controller))
            continue;
        if (!input_action_active(ops, controller, name, &active)) {
            rt_services_provider_add_diagnostic(
                "Services: %s found no action '%s' in the action manifest", member, name);
            return 0;
        }
        if (active)
            return 1;
    }
    return 0;
}

/// @brief Read an action's localized name.
/// @param action Action name.
/// @return Caller-owned name, or the empty string.
rt_string rt_services_action_input_action_label(rt_string action) {
    static const char member[] = "ActionInput.ActionLabel";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    const char *name = rt_services_internal_require_name(action, member, "action name");
    if (!name)
        return rt_str_empty();
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, NULL, &id);
    if (!ops || !ops->action_label)
        return rt_str_empty();
    return rt_services_internal_owned_or_empty(ops->action_label(name));
}

//===----------------------------------------------------------------------===//
// Origins
//===----------------------------------------------------------------------===//

/// @brief Count the physical inputs bound to an action.
/// @param controller_id Controller id, or "" for the first controller.
/// @param action_set Action set name, or "" for the current set.
/// @param action Action name.
/// @return 0..8.
int64_t rt_services_action_input_origin_count(rt_string controller_id,
                                              rt_string action_set,
                                              rt_string action) {
    int64_t origins[RT_SERVICES_ACTION_INPUT_MAX_ORIGINS];
    const int64_t count =
        input_origins("ActionInput.OriginCount", controller_id, action_set, action, origins);
    return count < 0 ? 0 : count;
}

/// @brief Read one physical input bound to an action.
/// @param controller_id Controller id, or "" for the first controller.
/// @param action_set Action set name, or "" for the current set.
/// @param action Action name.
/// @param index Origin index.
/// @return Origin id, or 0.
int64_t rt_services_action_input_origin_at(rt_string controller_id,
                                           rt_string action_set,
                                           rt_string action,
                                           int64_t index) {
    int64_t origins[RT_SERVICES_ACTION_INPUT_MAX_ORIGINS];
    const int64_t count =
        input_origins("ActionInput.OriginAt", controller_id, action_set, action, origins);
    return (index >= 0 && index < count) ? origins[index] : 0;
}

/// @brief Read an origin's localized name.
/// @param origin Non-negative origin id.
/// @return Caller-owned name, or the empty string.
rt_string rt_services_action_input_origin_label(int64_t origin) {
    static const char member[] = "ActionInput.OriginLabel";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    if (origin < 0) {
        rt_services_internal_trap_argument(
            member, "origin must not be negative (got %lld)", (long long)origin);
        return rt_str_empty();
    }
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, NULL, &id);
    if (!ops || !ops->origin_label)
        return rt_str_empty();
    return rt_services_internal_owned_or_empty(ops->origin_label(origin));
}

/// @brief Read the image file of an origin's glyph.
/// @param origin Non-negative origin id.
/// @param size GlyphSize value.
/// @return Caller-owned path, or the empty string.
rt_string rt_services_action_input_origin_glyph_path(int64_t origin, int64_t size) {
    static const char member[] = "ActionInput.OriginGlyphPath";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    if (origin < 0) {
        rt_services_internal_trap_argument(
            member, "origin must not be negative (got %lld)", (long long)origin);
        return rt_str_empty();
    }
    if (size < RT_SERVICES_GLYPH_SIZE_SMALL || size > RT_SERVICES_GLYPH_SIZE_LARGE) {
        rt_services_internal_trap_argument(
            member, "size must be a GlyphSize value (got %lld)", (long long)size);
        return rt_str_empty();
    }
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, NULL, &id);
    if (!ops || !ops->origin_glyph_path)
        return rt_str_empty();
    return rt_services_internal_owned_or_empty(ops->origin_glyph_path(origin, size));
}

//===----------------------------------------------------------------------===//
// Feedback and platform interface
//===----------------------------------------------------------------------===//

/// @brief Run a controller's rumble motors.
/// @param controller_id Controller id, or "" for every controller.
/// @param left Left motor strength in 0..1.
/// @param right Right motor strength in 0..1.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_vibrate(rt_string controller_id, double left, double right) {
    static const char member[] = "ActionInput.Vibrate";
    if (!rt_services_provider_require_main_thread(member) ||
        !input_require_strength(member, "left", left) ||
        !input_require_strength(member, "right", right))
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->vibrate)
        return 0;
    if (id[0])
        return ops->vibrate(id, left, right) ? 1 : 0;
    int8_t any = 0;
    const int64_t count = input_controller_count(ops);
    for (int64_t i = 0; i < count; ++i) {
        char controller[RT_SERVICES_CONTROLLER_ID_CAPACITY];
        if (input_controller_at(ops, i, controller) && ops->vibrate(controller, left, right))
            any = 1;
    }
    return any;
}

/// @brief Set a controller's light color.
/// @param controller_id Controller id, or "" for every controller.
/// @param red Red component in 0..255.
/// @param green Green component in 0..255.
/// @param blue Blue component in 0..255.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_set_led_color(rt_string controller_id,
                                              int64_t red,
                                              int64_t green,
                                              int64_t blue) {
    static const char member[] = "ActionInput.SetLedColor";
    if (!rt_services_provider_require_main_thread(member) ||
        !input_require_range(member, "red", red, 0, 255) ||
        !input_require_range(member, "green", green, 0, 255) ||
        !input_require_range(member, "blue", blue, 0, 255))
        return 0;
    return input_led(member, controller_id, red, green, blue, 0);
}

/// @brief Restore the light color the player chose.
/// @param controller_id Controller id, or "" for every controller.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_reset_led_color(rt_string controller_id) {
    static const char member[] = "ActionInput.ResetLedColor";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    return input_led(member, controller_id, 0, 0, 0, 1);
}

/// @brief Open the platform's binding panel.
/// @param controller_id Controller id, or "" for the first controller.
/// @return 1 when opened, otherwise 0.
int8_t rt_services_action_input_show_binding_panel(rt_string controller_id) {
    static const char member[] = "ActionInput.ShowBindingPanel";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = NULL;
    const rt_services_action_input_ops *ops = input_enter(member, controller_id, &id);
    if (!ops || !ops->show_binding_panel)
        return 0;
    char first[RT_SERVICES_CONTROLLER_ID_CAPACITY];
    const char *controller = input_first_or_id(ops, id, first);
    return (controller && ops->show_binding_panel(controller)) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return ControllerType.Unknown. @return 0.
int64_t rt_services_controller_type_unknown(void) {
    return RT_SERVICES_CONTROLLER_TYPE_UNKNOWN;
}

/// @brief Return ControllerType.SteamController. @return 1.
int64_t rt_services_controller_type_steam_controller(void) {
    return RT_SERVICES_CONTROLLER_TYPE_STEAM_CONTROLLER;
}

/// @brief Return ControllerType.Xbox360. @return 2.
int64_t rt_services_controller_type_xbox360(void) {
    return RT_SERVICES_CONTROLLER_TYPE_XBOX360;
}

/// @brief Return ControllerType.XboxOne. @return 3.
int64_t rt_services_controller_type_xbox_one(void) {
    return RT_SERVICES_CONTROLLER_TYPE_XBOX_ONE;
}

/// @brief Return ControllerType.GenericGamepad. @return 4.
int64_t rt_services_controller_type_generic_gamepad(void) {
    return RT_SERVICES_CONTROLLER_TYPE_GENERIC_GAMEPAD;
}

/// @brief Return ControllerType.PlayStation4. @return 5.
int64_t rt_services_controller_type_playstation4(void) {
    return RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION4;
}

/// @brief Return ControllerType.AppleMfi. @return 6.
int64_t rt_services_controller_type_apple_mfi(void) {
    return RT_SERVICES_CONTROLLER_TYPE_APPLE_MFI;
}

/// @brief Return ControllerType.Android. @return 7.
int64_t rt_services_controller_type_android(void) {
    return RT_SERVICES_CONTROLLER_TYPE_ANDROID;
}

/// @brief Return ControllerType.SwitchJoyConPair. @return 8.
int64_t rt_services_controller_type_switch_joy_con_pair(void) {
    return RT_SERVICES_CONTROLLER_TYPE_SWITCH_JOY_CON_PAIR;
}

/// @brief Return ControllerType.SwitchJoyConSingle. @return 9.
int64_t rt_services_controller_type_switch_joy_con_single(void) {
    return RT_SERVICES_CONTROLLER_TYPE_SWITCH_JOY_CON_SINGLE;
}

/// @brief Return ControllerType.SwitchPro. @return 10.
int64_t rt_services_controller_type_switch_pro(void) {
    return RT_SERVICES_CONTROLLER_TYPE_SWITCH_PRO;
}

/// @brief Return ControllerType.MobileTouch. @return 11.
int64_t rt_services_controller_type_mobile_touch(void) {
    return RT_SERVICES_CONTROLLER_TYPE_MOBILE_TOUCH;
}

/// @brief Return ControllerType.PlayStation3. @return 12.
int64_t rt_services_controller_type_playstation3(void) {
    return RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION3;
}

/// @brief Return ControllerType.PlayStation5. @return 13.
int64_t rt_services_controller_type_playstation5(void) {
    return RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION5;
}

/// @brief Return ControllerType.SteamDeck. @return 14.
int64_t rt_services_controller_type_steam_deck(void) {
    return RT_SERVICES_CONTROLLER_TYPE_STEAM_DECK;
}

/// @brief Return ControllerType.SteamOSHandheld. @return 15.
int64_t rt_services_controller_type_steamos_handheld(void) {
    return RT_SERVICES_CONTROLLER_TYPE_STEAMOS_HANDHELD;
}

/// @brief Return ControllerType.Switch2Pro. @return 16.
int64_t rt_services_controller_type_switch2_pro(void) {
    return RT_SERVICES_CONTROLLER_TYPE_SWITCH2_PRO;
}

/// @brief Return ControllerType.SteamController2026. @return 17.
int64_t rt_services_controller_type_steam_controller_2026(void) {
    return RT_SERVICES_CONTROLLER_TYPE_STEAM_CONTROLLER_2026;
}

/// @brief Return ControllerType.SteamFrameControllerPair. @return 18.
int64_t rt_services_controller_type_steam_frame_controller_pair(void) {
    return RT_SERVICES_CONTROLLER_TYPE_STEAM_FRAME_CONTROLLER_PAIR;
}

/// @brief Return GlyphSize.Small. @return 0.
int64_t rt_services_glyph_size_small(void) {
    return RT_SERVICES_GLYPH_SIZE_SMALL;
}

/// @brief Return GlyphSize.Medium. @return 1.
int64_t rt_services_glyph_size_medium(void) {
    return RT_SERVICES_GLYPH_SIZE_MEDIUM;
}

/// @brief Return GlyphSize.Large. @return 2.
int64_t rt_services_glyph_size_large(void) {
    return RT_SERVICES_GLYPH_SIZE_LARGE;
}
