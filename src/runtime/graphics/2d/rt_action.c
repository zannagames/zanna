//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_action.c
/// @file
/// @brief Implements the global named-action registry, input polling, binding
///        mutation, and binding introspection.
// Purpose: Input action mapping core: named logical actions bound to keyboard, mouse,
//   gamepad, axis, and chord sources; per-frame polling and pressed/released/
//   held/axis queries. Owns the global action list and its lifecycle.
//
// Key invariants:
//   - Action names are unique and each action is permanently either button-
//     style or axis-style until removed.
//   - New actions and bindings are inserted at list heads, so enumeration and
//     first-match conflict queries use newest-first order.
//   - rt_action_update() rebuilds cached state from the current device snapshot;
//     registry and query entry points are restricted to the runtime's main thread.
//   - Finite axis sources accumulate with finite saturation. Axis() clamps the
//     cached sum while AxisRaw() exposes its finite, unclamped magnitude.
//
// Ownership/Lifetime:
//   - Action names and all Action/Binding nodes are private malloc-owned data.
//     Public rt_string names and sequence arguments are borrowed.
//   - List and string introspection APIs return owned runtime containers or
//     strings except for the immortal empty-string singleton on misses.
//
// Links: rt_action.h (public API), rt_action_internal.h (shared model),
//        rt_action_presets.c (built-in presets), rt_action_io.c (JSON save/load)
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_action_internal.h"
#include "rt_box.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_json_stream.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Validate the shallow Seq storage used by chord APIs before indexing.
static int8_t action_chord_seq_valid(void *keys) {
    if (!rt_seq_internal_is_valid(keys))
        return 0;
    const rt_seq_impl *seq = (const rt_seq_impl *)keys;
    return seq->len >= 2 && seq->len <= MAX_CHORD_KEYS && seq->cap >= seq->len && seq->cap > 0 &&
           seq->items && (uint64_t)seq->cap <= (uint64_t)SIZE_MAX / sizeof(*seq->items);
}

// Global action registry (declared extern in rt_action_internal.h).
/// @brief Head of the malloc-owned, newest-first global action list.
Action *g_actions = NULL;
/// @brief Nonzero after rt_action_init() and before rt_action_shutdown().
int8_t g_initialized = 0;

/// @brief Linear-scan the global action list by `rt_string` name.
///
/// Specialized to compare lengths first then bytes — avoids a
/// `strdup` round-trip versus calling `find_action(rt_string_cstr(...))`.
/// @param name Borrowed runtime string to match exactly.
/// @return Borrowed action node, or `NULL` when the name is null or absent.
static Action *find_action_str(rt_string name) {
    if (!name || !rt_string_is_handle(name))
        return NULL;
    int64_t name_len = rt_str_len(name);
    if (name_len <= 0 || (uint64_t)name_len > SIZE_MAX)
        return NULL;
    const char *name_data = name->data;

    Action *a = g_actions;
    int64_t visited = 0;
    while (a && visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return NULL;
        if (a->name_len == name_len && memcmp(a->name, name_data, (size_t)a->name_len) == 0)
            return a;
        a = a->next;
    }
    return NULL;
}

/// @brief Validate that an action name is nonempty, NUL-free, strict UTF-8.
/// @param name Borrowed runtime string.
/// @return Nonzero when the name can be represented losslessly in JSON and in
///         the registry's trailing-NUL C-string storage.
static int action_name_valid(rt_string name) {
    if (!name || !rt_string_is_handle(name))
        return 0;
    int64_t signed_len = rt_str_len(name);
    if (signed_len <= 0 || (uint64_t)signed_len > SIZE_MAX - 1u)
        return 0;

    const unsigned char *data = (const unsigned char *)name->data;
    size_t len = (size_t)signed_len;
    size_t i = 0;
    while (i < len) {
        unsigned char lead = data[i++];
        if (lead == 0)
            return 0;
        if (lead <= 0x7Fu)
            continue;

        size_t continuation_count;
        unsigned char second_min = 0x80u;
        unsigned char second_max = 0xBFu;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            continuation_count = 1;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            continuation_count = 2;
            if (lead == 0xE0u)
                second_min = 0xA0u;
            else if (lead == 0xEDu)
                second_max = 0x9Fu;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            continuation_count = 3;
            if (lead == 0xF0u)
                second_min = 0x90u;
            else if (lead == 0xF4u)
                second_max = 0x8Fu;
        } else {
            return 0;
        }

        if (continuation_count > len - i || data[i] < second_min || data[i] > second_max)
            return 0;
        ++i;
        for (size_t ci = 1; ci < continuation_count; ++ci, ++i) {
            if (data[i] < 0x80u || data[i] > 0xBFu)
                return 0;
        }
    }
    return 1;
}

/// @brief Heap-allocate a NUL-terminated C string from an `rt_string`.
///
/// Returns NULL on empty input or allocation failure. Used to capture
/// the action's name in our own buffer (the `rt_string` may go away).
/// @param s Borrowed runtime string to copy.
/// @return Owned null-terminated copy, or `NULL` for null/empty input or
///         allocation failure.
static char *strdup_rt_string(rt_string s) {
    if (!s || !rt_string_is_handle(s))
        return NULL;
    int64_t len = rt_str_len(s);
    if (len <= 0 || (uint64_t)len > SIZE_MAX - 1u)
        return NULL;
    char *result = (char *)malloc((size_t)len + 1);
    if (!result)
        return NULL;
    memcpy(result, s->data, (size_t)len);
    result[len] = '\0';
    return result;
}

/// @brief Remove the first binding matching `(type, code, pad_index)`.
///
/// Returns 1 if a binding was removed, 0 if none matched. Doesn't
/// remove duplicates beyond the first match — callers wanting to
/// strip all matching bindings need to loop.
/// @param action Borrowed action whose binding list is searched.
/// @param type Required physical-source type.
/// @param code Required key, button, or axis code.
/// @param pad_index Required controller index.
/// @return `1` after unlinking and freeing the first match; otherwise `0`.
static int8_t remove_binding(Action *action, BindingType type, int64_t code, int64_t pad_index) {
    if (!action_binding_list_valid(action))
        return 0;
    Binding **pp = &action->bindings;
    int64_t visited = 0;
    while (*pp && visited++ < action->binding_count) {
        Binding *b = *pp;
        if (!action_binding_valid(b, action->is_axis))
            return 0;
        if (b->type == type && b->code == code && b->pad_index == pad_index) {
            *pp = b->next;
            b->state_magic = 0;
            free(b);
            action->binding_count--;
            return 1;
        }
        pp = &b->next;
    }
    return 0;
}

/// @brief True if `key` is held down this frame.
/// @param key Runtime keyboard code to query.
/// @return Nonzero while the key is down.
static int8_t key_held(int64_t key) {
    return rt_keyboard_is_down(key);
}

/// @brief True if `key` was pressed (down-edge) this frame.
/// @param key Runtime keyboard code to query.
/// @return Nonzero on the key's current-frame down edge.
static int8_t key_pressed(int64_t key) {
    return rt_keyboard_was_pressed(key);
}

/// @brief True if `key` was released (up-edge) this frame.
/// @param key Runtime keyboard code to query.
/// @return Nonzero on the key's current-frame up edge.
static int8_t key_released(int64_t key) {
    return rt_keyboard_was_released(key);
}

/// @brief True if mouse `button` is held down this frame.
/// @param button Runtime mouse-button code to query.
/// @return Nonzero while the button is down.
static int8_t mouse_held(int64_t button) {
    return rt_mouse_is_down(button);
}

/// @brief True if mouse `button` was pressed (down-edge) this frame.
/// @param button Runtime mouse-button code to query.
/// @return Nonzero on the button's current-frame down edge.
static int8_t mouse_pressed(int64_t button) {
    return rt_mouse_was_pressed(button);
}

/// @brief True if mouse `button` was released (up-edge) this frame.
/// @param button Runtime mouse-button code to query.
/// @return Nonzero on the button's current-frame up edge.
static int8_t mouse_released(int64_t button) {
    return rt_mouse_was_released(button);
}

/// @brief Pad button held query, with `pad_index < 0` = any connected pad.
///
/// Loops over every supported pad when `pad_index` is negative, returning true on
/// the first connected pad with the button held.
/// @param pad_index Controller index, or any negative value for any connected
///        controller.
/// @param button Runtime gamepad-button code to query.
/// @return Nonzero when a matching controller has the button down.
static int8_t pad_held(int64_t pad_index, int64_t button) {
    if (pad_index < 0) {
        // Any controller
        for (int64_t i = 0; i < ZANNA_PAD_MAX; i++) {
            if (rt_pad_is_connected(i) && rt_pad_is_down(i, button))
                return 1;
        }
        return 0;
    }
    return rt_pad_is_down(pad_index, button);
}

/// @brief Pad button down-edge query (any-pad fallback for `pad_index < 0`).
/// @param pad_index Controller index, or a negative value for any controller.
/// @param button Runtime gamepad-button code to query.
/// @return Nonzero on a matching button's current-frame down edge.
static int8_t pad_pressed(int64_t pad_index, int64_t button) {
    if (pad_index < 0) {
        for (int64_t i = 0; i < ZANNA_PAD_MAX; i++) {
            if (rt_pad_is_connected(i) && rt_pad_was_pressed(i, button))
                return 1;
        }
        return 0;
    }
    return rt_pad_was_pressed(pad_index, button);
}

/// @brief Pad button up-edge query (any-pad fallback for `pad_index < 0`).
/// @param pad_index Controller index, or a negative value for any controller.
/// @param button Runtime gamepad-button code to query.
/// @return Nonzero on a matching button's current-frame up edge.
static int8_t pad_released(int64_t pad_index, int64_t button) {
    if (pad_index < 0) {
        for (int64_t i = 0; i < ZANNA_PAD_MAX; i++) {
            if (rt_pad_is_connected(i) && rt_pad_was_released(i, button))
                return 1;
        }
        return 0;
    }
    return rt_pad_was_released(pad_index, button);
}

/// @brief Read a gamepad axis value (-1..1 sticks, 0..1 triggers).
///
/// `axis` is one of `ZANNA_AXIS_*`. With `pad_index < 0`, returns the
/// largest-magnitude finite value across connected pads — useful when you want
/// "any controller's left stick" without binding to a specific index.
/// @param pad_index Controller index, or a negative value for any connected
///        controller.
/// @param axis One of the supported `ZANNA_AXIS_*` codes.
/// @return Raw device-axis value, or `0.0` for disconnected pads, unknown
///         axes, or no nonzero any-pad value.
static double pad_axis_value(int64_t pad_index, int64_t axis) {
    if (pad_index < 0) {
        double strongest = 0.0;
        for (int64_t i = 0; i < ZANNA_PAD_MAX; i++) {
            if (!rt_pad_is_connected(i))
                continue;
            double v = 0.0;
            switch (axis) {
                case ZANNA_AXIS_LEFT_X:
                    v = rt_pad_left_x(i);
                    break;
                case ZANNA_AXIS_LEFT_Y:
                    v = rt_pad_left_y(i);
                    break;
                case ZANNA_AXIS_RIGHT_X:
                    v = rt_pad_right_x(i);
                    break;
                case ZANNA_AXIS_RIGHT_Y:
                    v = rt_pad_right_y(i);
                    break;
                case ZANNA_AXIS_LEFT_TRIGGER:
                    v = rt_pad_left_trigger(i);
                    break;
                case ZANNA_AXIS_RIGHT_TRIGGER:
                    v = rt_pad_right_trigger(i);
                    break;
            }
            if (isfinite(v) && fabs(v) > fabs(strongest))
                strongest = v;
        }
        return strongest;
    }

    if (!rt_pad_is_connected(pad_index))
        return 0.0;

    switch (axis) {
        case ZANNA_AXIS_LEFT_X: {
            double value = rt_pad_left_x(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        case ZANNA_AXIS_LEFT_Y: {
            double value = rt_pad_left_y(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        case ZANNA_AXIS_RIGHT_X: {
            double value = rt_pad_right_x(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        case ZANNA_AXIS_RIGHT_Y: {
            double value = rt_pad_right_y(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        case ZANNA_AXIS_LEFT_TRIGGER: {
            double value = rt_pad_left_trigger(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        case ZANNA_AXIS_RIGHT_TRIGGER: {
            double value = rt_pad_right_trigger(pad_index);
            return isfinite(value) ? value : 0.0;
        }
        default:
            return 0.0;
    }
}

/// @brief Clamp `value` into `[-1, 1]` for axis output normalization.
/// @param value Accumulated axis value.
/// @return @p value constrained to -1..1; non-finite input becomes neutral.
static double clamp_axis(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < -1.0)
        return -1.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Add one finite contribution without allowing the raw sum to overflow.
/// @param total Borrowed accumulated raw-axis value.
/// @param contribution Candidate contribution from one binding.
static void action_axis_add(double *total, double contribution) {
    if (!total || !isfinite(contribution))
        return;
    if (contribution > 0.0 && *total > DBL_MAX - contribution) {
        *total = DBL_MAX;
    } else if (contribution < 0.0 && *total < -DBL_MAX - contribution) {
        *total = -DBL_MAX;
    } else {
        *total += contribution;
    }
}

/// @brief Multiply a source by a scale and add it with finite saturation.
static void action_axis_scale_add(double *total, double source, double scale) {
    if (!total || !isfinite(source) || !isfinite(scale) || source == 0.0 || scale == 0.0)
        return;
    if (fabs(scale) > DBL_MAX / fabs(source)) {
        action_axis_add(total, signbit(source) == signbit(scale) ? DBL_MAX : -DBL_MAX);
        return;
    }
    action_axis_add(total, source * scale);
}

/// @brief Initialize the global action mapping system.
/// @details The first call establishes an empty registry. Later calls are
///          idempotent and preserve existing actions. Public definition APIs
///          also initialize lazily.
void rt_action_init(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_initialized)
        return;
    g_actions = NULL;
    g_initialized = 1;
}

/// @brief Shutdown the action mapping system.
///
/// Clears every action and binding via `rt_action_clear`, then marks
/// the system uninitialized. Safe to call multiple times. Called
/// during runtime teardown.
void rt_action_shutdown(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_action_clear();
    g_initialized = 0;
}

/// @brief Update all action states for the current frame.
/// @details Polls each registered action's bindings against the current input
///   state (keyboard, mouse, gamepad). Computes pressed/released/held flags
///   and axis values. Must be called exactly once per frame, AFTER rt_canvas_poll()
///   has processed input events. Action.Pressed(), Action.Held(), and Action.Axis()
///   return values computed by this function. An uninitialized registry is
///   unchanged. Button release is also synthesized when an action was held in
///   the preceding update but no binding remains held in this update.
void rt_action_update(void) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_initialized)
        return;

    Action *a = g_actions;
    int64_t action_visited = 0;
    while (a && action_visited++ < ACTION_MAX_ACTIONS) {
        if (!action_node_shallow_valid(a))
            return;
        int8_t was_held = a->held;
        a->pressed = 0;
        a->released = 0;
        a->held = 0;
        a->axis_value = 0.0;

        Binding *b = a->bindings;
        int64_t binding_visited = 0;
        while (b && binding_visited++ < a->binding_count) {
            if (!action_binding_valid(b, a->is_axis)) {
                a->pressed = 0;
                a->released = was_held ? 1 : 0;
                a->held = 0;
                a->axis_value = 0.0;
                break;
            }
            switch (b->type) {
                case BIND_KEY:
                    if (a->is_axis) {
                        if (key_held(b->code))
                            action_axis_add(&a->axis_value, b->value);
                    } else {
                        if (key_pressed(b->code))
                            a->pressed = 1;
                        if (key_released(b->code))
                            a->released = 1;
                        if (key_held(b->code))
                            a->held = 1;
                    }
                    break;

                case BIND_MOUSE_BUTTON:
                    if (!a->is_axis) {
                        if (mouse_pressed(b->code))
                            a->pressed = 1;
                        if (mouse_released(b->code))
                            a->released = 1;
                        if (mouse_held(b->code))
                            a->held = 1;
                    }
                    break;

                case BIND_MOUSE_X:
                    if (a->is_axis)
                        action_axis_scale_add(&a->axis_value, (double)rt_mouse_delta_x(), b->value);
                    break;

                case BIND_MOUSE_Y:
                    if (a->is_axis)
                        action_axis_scale_add(&a->axis_value, (double)rt_mouse_delta_y(), b->value);
                    break;

                case BIND_SCROLL_X:
                    if (a->is_axis)
                        action_axis_scale_add(&a->axis_value, rt_mouse_wheel_xf(), b->value);
                    break;

                case BIND_SCROLL_Y:
                    if (a->is_axis)
                        action_axis_scale_add(&a->axis_value, rt_mouse_wheel_yf(), b->value);
                    break;

                case BIND_PAD_BUTTON:
                    if (!a->is_axis) {
                        if (pad_pressed(b->pad_index, b->code))
                            a->pressed = 1;
                        if (pad_released(b->pad_index, b->code))
                            a->released = 1;
                        if (pad_held(b->pad_index, b->code))
                            a->held = 1;
                    }
                    break;

                case BIND_PAD_AXIS:
                    if (a->is_axis)
                        action_axis_scale_add(
                            &a->axis_value, pad_axis_value(b->pad_index, b->code), b->value);
                    break;

                case BIND_PAD_BUTTON_AXIS:
                    if (a->is_axis) {
                        if (pad_held(b->pad_index, b->code))
                            action_axis_add(&a->axis_value, b->value);
                    }
                    break;

                case BIND_CHORD:
                    if (!a->is_axis && b->chord_len > 0) {
                        // All chord keys must be held
                        int8_t all_held = 1;
                        int8_t any_pressed = 0;
                        int32_t i;
                        for (i = 0; i < b->chord_len; i++) {
                            if (!key_held(b->chord_keys[i])) {
                                all_held = 0;
                                break;
                            }
                            if (key_pressed(b->chord_keys[i]))
                                any_pressed = 1;
                        }
                        if (all_held) {
                            a->held = 1;
                            // Chord is "pressed" when all keys held and at least one
                            // was newly pressed this frame
                            if (any_pressed)
                                a->pressed = 1;
                        }
                    }
                    break;

                default:
                    break;
            }
            b = b->next;
        }

        if (b || binding_visited != a->binding_count) {
            a->pressed = 0;
            a->released = was_held ? 1 : 0;
            a->held = 0;
            a->axis_value = 0.0;
            return;
        }

        if (was_held && !a->held)
            a->released = 1;

        a = a->next;
    }
}

/// @brief `Action.Clear` — destroy every action and binding.
///
/// Walks the global action list freeing each action (which in turn
/// frees its bindings). After clear the system is still initialized;
/// new actions can be defined immediately. Calling it before initialization
/// is also safe.
void rt_action_clear(void) {
    RT_ASSERT_MAIN_THREAD();
    action_free_list(g_actions);
    g_actions = NULL;
}

/// @brief Define one validated action kind without duplicating initialization.
static int8_t action_define_impl(rt_string name, int8_t is_axis) {
    if (!action_name_valid(name) || find_action_str(name))
        return 0;
    int64_t action_count = 0;
    for (Action *existing = g_actions; existing; existing = existing->next) {
        if (!action_binding_list_valid(existing) || ++action_count >= ACTION_MAX_ACTIONS)
            return 0;
    }

    Action *action = (Action *)calloc(1, sizeof(Action));
    if (!action)
        return 0;
    action->state_magic = ACTION_STATE_MAGIC;
    action->name = strdup_rt_string(name);
    if (!action->name) {
        free(action);
        return 0;
    }
    action->name_len = rt_str_len(name);
    action->is_axis = is_axis != 0;
    action->next = g_actions;
    g_actions = action;
    return 1;
}

/// @brief Register a new named action for input mapping.
/// @details Creates a button-style action (pressed/released/held). The name must
///   be unique. After defining, bind physical inputs with BindKey(), BindMouse(), etc.
///   The borrowed runtime string is copied into private storage.
/// @param name Nonempty action name string (for example, `"jump"` or `"fire"`).
/// @return 1 on success, 0 if name is empty, already exists, or allocation fails.
int8_t rt_action_define(rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_initialized)
        rt_action_init();

    return action_define_impl(name, 0);
}

/// @brief `Action.DefineAxis(name)` — register an axis-style action.
///
/// Axis actions accumulate continuous values (-1..1) from analog
/// sources (sticks, mouse delta) or button bindings (each button
/// contributes its `value` field). Use `Axis()` to read the latest
/// frame's accumulated value. The borrowed name is copied.
/// @param name Nonempty unique action name.
/// @return `1` on success; `0` for null/empty/duplicate names or allocation
///         failure.
int8_t rt_action_define_axis(rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_initialized)
        rt_action_init();

    return action_define_impl(name, 1);
}

/// @brief `Action.Exists(name)` — true if an action with that name is defined.
/// @param name Borrowed action name to search for.
/// @return `1` when an exact name match exists; otherwise `0`.
int8_t rt_action_exists(rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    return find_action_str(name) != NULL;
}

/// @brief `Action.IsAxis(name)` — true if the named action is an axis (vs. button).
/// @param name Borrowed action name to query.
/// @return `1` for an axis action; `0` for a button action or missing name.
int8_t rt_action_is_axis(rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(name);
    return a ? a->is_axis : 0;
}

/// @brief `Action.Remove(name)` — destroy a single action by name.
///
/// Walks the linked list with a back-pointer, unlinks on match, frees
/// the action + bindings. Returns 1 on success, 0 if not found.
/// @param name Borrowed action name to remove.
/// @return `1` when the action was removed; `0` for null or absent names.
int8_t rt_action_remove(rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    if (!name || !rt_string_is_handle(name))
        return 0;

    int64_t name_len = rt_str_len(name);
    if (name_len <= 0 || (uint64_t)name_len > SIZE_MAX)
        return 0;
    const char *name_data = name->data;

    Action **pp = &g_actions;
    int64_t visited = 0;
    while (*pp && visited++ < ACTION_MAX_ACTIONS) {
        Action *a = *pp;
        if (!action_binding_list_valid(a))
            return 0;
        if (a->name_len == name_len && memcmp(a->name, name_data, (size_t)a->name_len) == 0) {
            *pp = a->next;
            action_free_node(a);
            return 1;
        }
        pp = &a->next;
    }
    return 0;
}

/// @brief `Action.BindKey(action, key)` — add a button-style key binding.
///
/// `key` is a `ZANNA_KEY_*` constant. Fails if action doesn't exist,
/// is an axis action, or allocation fails. Duplicate bindings are permitted
/// and are evaluated independently.
/// @param action Borrowed button-action name.
/// @param key Runtime keyboard code to bind.
/// @return `1` when the binding is added; otherwise `0`.
int8_t rt_action_bind_key(rt_string action, int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || a->is_axis || !action_key_code_valid(key) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_KEY, key, 0, 1.0);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.BindKeyAxis(action, key, value)` — add a key→axis-contribution binding.
///
/// When `key` is held, `value` is added to the axis. Use opposite-
/// signed values for opposite directions (e.g., `Left` → -1.0,
/// `Right` → +1.0 for a horizontal-axis "MoveX" action).
/// @param action Borrowed axis-action name.
/// @param key Runtime keyboard code to bind.
/// @param value Finite contribution added on each update while held.
/// @return `1` when the binding is added; `0` if the action is missing,
///         button-style, or allocation fails.
int8_t rt_action_bind_key_axis(rt_string action, int64_t key, double value) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !action_key_code_valid(key) || !isfinite(value) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_KEY, key, 0, value);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.UnbindKey(action, key)` — remove a key binding.
/// @details Removes only the newest matching binding when duplicates exist.
/// @param action Borrowed action name; button and axis actions are accepted.
/// @param key Runtime keyboard code to unbind.
/// @return `1` when a binding was removed; otherwise `0`.
int8_t rt_action_unbind_key(rt_string action, int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !action_key_code_valid(key))
        return 0;
    return remove_binding(a, BIND_KEY, key, 0);
}

/// @brief `Action.BindChord(action, keys)` — bind a multi-key chord.
///
/// `keys` is a `seq<int>` of `ZANNA_KEY_*` codes. The action is
/// "pressed" the frame all keys in the chord are simultaneously held
/// AND at least one was newly pressed. Caps at `MAX_CHORD_KEYS` (8).
/// Useful for hotkey-style actions like Ctrl+Shift+S.
/// @param action Borrowed button-action name.
/// @param keys Borrowed runtime sequence containing two through eight boxed
///        integer key codes in chord order.
/// @return `1` when the chord is copied into a new binding; otherwise `0`.
int8_t rt_action_bind_chord(rt_string action, void *keys) {
    RT_ASSERT_MAIN_THREAD();
    int64_t len, i;
    int64_t chord_keys[MAX_CHORD_KEYS];
    Binding *b;
    Action *a = find_action_str(action);
    if (!a || a->is_axis || a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    if (!action_chord_seq_valid(keys))
        return 0;

    len = rt_seq_len(keys);
    if (len < 2 || len > MAX_CHORD_KEYS)
        return 0;

    for (i = 0; i < len; i++) {
        if (!rt_box_try_to_i64(rt_seq_get(keys, i), &chord_keys[i]) ||
            !action_key_code_valid(chord_keys[i]))
            return 0;
        for (int64_t previous = 0; previous < i; ++previous) {
            if (chord_keys[previous] == chord_keys[i])
                return 0;
        }
    }

    b = create_binding(BIND_CHORD, 0, 0, 1.0);
    if (!b)
        return 0;

    b->chord_len = (int32_t)len;
    memcpy(b->chord_keys, chord_keys, (size_t)len * sizeof(chord_keys[0]));

    add_binding(a, b);
    return 1;
}

/// @brief `Action.UnbindChord(action, keys)` — remove a chord binding.
///
/// Match is exact: same length, same keys in the same order. Returns
/// 1 on success, 0 if no chord matches.
/// @param action Borrowed action name.
/// @param keys Borrowed sequence of two through eight boxed key codes.
/// @return `1` after removing the newest exact match; otherwise `0`.
int8_t rt_action_unbind_chord(rt_string action, void *keys) {
    RT_ASSERT_MAIN_THREAD();
    int64_t len, i;
    int64_t chord_keys[MAX_CHORD_KEYS];
    Binding **pp;
    Action *a = find_action_str(action);
    if (!a || !action_chord_seq_valid(keys))
        return 0;

    len = rt_seq_len(keys);
    if (len < 2 || len > MAX_CHORD_KEYS)
        return 0;
    for (i = 0; i < len; ++i) {
        if (!rt_box_try_to_i64(rt_seq_get(keys, i), &chord_keys[i]) ||
            !action_key_code_valid(chord_keys[i]))
            return 0;
        for (int64_t previous = 0; previous < i; ++previous) {
            if (chord_keys[previous] == chord_keys[i])
                return 0;
        }
    }

    pp = &a->bindings;
    int64_t visited = 0;
    while (*pp && visited++ < a->binding_count) {
        Binding *b = *pp;
        if (!action_binding_valid(b, a->is_axis))
            return 0;
        if (b->type == BIND_CHORD && b->chord_len == (int32_t)len) {
            int8_t match = 1;
            for (i = 0; i < len; i++) {
                if (b->chord_keys[i] != chord_keys[i]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *pp = b->next;
                b->state_magic = 0;
                free(b);
                a->binding_count--;
                return 1;
            }
        }
        pp = &b->next;
    }
    return 0;
}

/// @brief `Action.ChordCount(action)` — number of chord bindings on this action.
/// @param action Borrowed action name.
/// @return Number of chord bindings, or `0` when the action is absent.
int64_t rt_action_chord_count(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    int64_t count = 0;
    Binding *b;
    Action *a = find_action_str(action);
    if (!a)
        return 0;

    b = a->bindings;
    int64_t visited = 0;
    while (b && visited++ < a->binding_count) {
        if (!action_binding_valid(b, a->is_axis))
            return 0;
        if (b->type == BIND_CHORD)
            count++;
        b = b->next;
    }
    if (b || visited != a->binding_count)
        return 0;
    return count;
}

/// @brief `Action.BindMouse(action, button)` — bind a mouse button to a button action.
/// @param action Borrowed button-action name.
/// @param button Runtime mouse-button code to bind.
/// @return `1` when added; `0` for a missing/axis action or allocation failure.
int8_t rt_action_bind_mouse(rt_string action, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || a->is_axis || !action_mouse_button_valid(button) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_MOUSE_BUTTON, button, 0, 1.0);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.UnbindMouse(action, button)` — remove a mouse-button binding.
/// @details Removes only the newest matching binding when duplicates exist.
/// @param action Borrowed action name.
/// @param button Runtime mouse-button code to unbind.
/// @return `1` when a binding was removed; otherwise `0`.
int8_t rt_action_unbind_mouse(rt_string action, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !action_mouse_button_valid(button))
        return 0;
    return remove_binding(a, BIND_MOUSE_BUTTON, button, 0);
}

/// @brief `Action.BindMouseX(action, sensitivity)` — bind mouse X-delta to an axis.
///
/// Per-frame mouse delta (in pixels) is multiplied by `sensitivity`
/// and added to the axis. Typical mouselook setup uses ~0.001-0.01.
/// @param action Borrowed axis-action name.
/// @param sensitivity Finite multiplier applied to horizontal pixel delta.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_mouse_x(rt_string action, double sensitivity) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !isfinite(sensitivity) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_MOUSE_X, 0, 0, sensitivity);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.BindMouseY(action, sensitivity)` — bind mouse Y-delta to an axis.
/// @param action Borrowed axis-action name.
/// @param sensitivity Finite multiplier applied to vertical pixel delta.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_mouse_y(rt_string action, double sensitivity) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !isfinite(sensitivity) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_MOUSE_Y, 0, 0, sensitivity);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.BindScrollX(action, sensitivity)` — bind horizontal scroll wheel to an axis.
/// @param action Borrowed axis-action name.
/// @param sensitivity Finite multiplier applied to horizontal wheel delta.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_scroll_x(rt_string action, double sensitivity) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !isfinite(sensitivity) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_SCROLL_X, 0, 0, sensitivity);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.BindScrollY(action, sensitivity)` — bind vertical scroll wheel to an axis.
/// @param action Borrowed axis-action name.
/// @param sensitivity Finite multiplier applied to vertical wheel delta.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_scroll_y(rt_string action, double sensitivity) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !isfinite(sensitivity) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_SCROLL_Y, 0, 0, sensitivity);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.BindPadButton(action, padIndex, button)` — bind a gamepad button to a button
/// action.
/// @param action Borrowed button-action name.
/// @param pad_index Controller index, conventionally 0..3, or `-1` for any.
/// @param button Runtime gamepad-button code to bind.
/// @return `1` when added; `0` for a missing/axis action or allocation failure.
int8_t rt_action_bind_pad_button(rt_string action, int64_t pad_index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || a->is_axis || !action_pad_index_valid(pad_index) ||
        !action_pad_button_valid(button) || a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_PAD_BUTTON, button, pad_index, 1.0);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.UnbindPadButton(action, padIndex, button)` — remove a pad-button binding.
/// @details The controller index must exactly match the stored binding.
/// @param action Borrowed action name.
/// @param pad_index Stored controller index to match.
/// @param button Runtime gamepad-button code to match.
/// @return `1` when the newest exact binding is removed; otherwise `0`.
int8_t rt_action_unbind_pad_button(rt_string action, int64_t pad_index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !action_pad_index_valid(pad_index) || !action_pad_button_valid(button))
        return 0;
    return remove_binding(a, BIND_PAD_BUTTON, button, pad_index);
}

/// @brief `Action.BindPadAxis(action, padIndex, axis, scale)` — bind a gamepad analog axis.
///
/// `axis` is `ZANNA_AXIS_*`. `scale` multiplies the raw axis value
/// (typically 1.0 or -1.0 to invert).
/// @param action Borrowed axis-action name.
/// @param pad_index Controller index, conventionally 0..3, or `-1` for any.
/// @param axis One of the `ZANNA_AXIS_*` source codes.
/// @param scale Finite multiplier applied to the raw device value.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_pad_axis(rt_string action, int64_t pad_index, int64_t axis, double scale) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !action_pad_index_valid(pad_index) || !action_pad_axis_valid(axis) ||
        !isfinite(scale) || a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_PAD_AXIS, axis, pad_index, scale);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief `Action.UnbindPadAxis(action, padIndex, axis)` — remove a pad-axis binding.
/// @param action Borrowed action name.
/// @param pad_index Stored controller index to match.
/// @param axis Stored `ZANNA_AXIS_*` code to match.
/// @return `1` when the newest exact binding is removed; otherwise `0`.
int8_t rt_action_unbind_pad_axis(rt_string action, int64_t pad_index, int64_t axis) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !action_pad_index_valid(pad_index) || !action_pad_axis_valid(axis))
        return 0;
    return remove_binding(a, BIND_PAD_AXIS, axis, pad_index);
}

/// @brief `Action.BindPadButtonAxis(action, padIndex, button, value)` — bind pad button as axis
/// contribution.
///
/// Like `BindKeyAxis` but for gamepad buttons — useful for D-pad
/// directions on an axis (D-pad-Left → -1.0, D-pad-Right → +1.0).
/// @param action Borrowed axis-action name.
/// @param pad_index Controller index, conventionally 0..3, or `-1` for any.
/// @param button Runtime gamepad-button code to bind.
/// @param value Finite contribution added while the button is held.
/// @return `1` when added; `0` for a missing/button action or allocation failure.
int8_t rt_action_bind_pad_button_axis(rt_string action,
                                      int64_t pad_index,
                                      int64_t button,
                                      double value) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a || !a->is_axis || !action_pad_index_valid(pad_index) ||
        !action_pad_button_valid(button) || !isfinite(value) ||
        a->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    Binding *b = create_binding(BIND_PAD_BUTTON_AXIS, button, pad_index, value);
    if (!b)
        return 0;
    add_binding(a, b);
    return 1;
}

/// @brief Check if an action was just pressed this frame (edge-triggered).
/// @details Set when any ordinary bound input reports a down edge, or when all
///          chord keys are held and at least one reports a down edge. Multiple
///          bindings mean a new edge can be reported while another binding
///          already holds the action.
/// @param action Borrowed action name.
/// @return `1` when the cached update observed a qualifying press; otherwise `0`.
int8_t rt_action_pressed(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? a->pressed : 0;
}

/// @brief Check if an action was just released this frame (edge-triggered).
/// @details Set when any ordinary bound input reports an up edge, or when the
///          overall action changes from held to unheld. It may therefore be
///          true while another binding continues to hold the action.
/// @param action Borrowed action name.
/// @return `1` when the cached update observed or synthesized a release;
///         otherwise `0`.
int8_t rt_action_released(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? a->released : 0;
}

/// @brief Check if an action is currently held down (level-triggered).
/// @details Returns 1 every frame the action is active. Use for movement,
///   charging, or any continuous action. Axis actions do not populate the
///   held flag.
/// @param action Borrowed action name.
/// @return `1` if any button binding or complete chord is held; otherwise `0`.
int8_t rt_action_held(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? a->held : 0;
}

/// @brief Get the strength of a button action (always digital: 0.0 or 1.0).
/// @details Returns 1.0 while any bound input is held and 0.0 otherwise. Button
///   actions have no trigger-axis binding, so no analog value is ever produced;
///   use axis actions for analog input.
/// @param action Borrowed action name.
/// @return `1.0` if held; `0.0` for unheld, axis, or missing actions.
double rt_action_strength(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a && a->held ? 1.0 : 0.0;
}

/// @brief Get the clamped axis value for an axis-type action.
/// @details Returns a value in [-1.0, 1.0] for axis inputs (gamepad sticks,
///   mouse movement). Button bindings contribute their configured `value` field
///   (typically ±1.0). The result is clamped to [-1.0, 1.0].
/// @param action Borrowed action name, normally defined with DefineAxis.
/// @return Finite axis value clamped to [-1.0, 1.0], or `0.0` for missing
///         actions.
double rt_action_axis(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? clamp_axis(a->axis_value) : 0.0;
}

/// @brief Get the raw (unclamped) axis value for an axis-type action.
/// @details Like rt_action_axis() but without clamping. Raw values can exceed
///   [-1.0, 1.0] when multiple bindings contribute simultaneously.
/// @param action Borrowed action name.
/// @return Raw accumulated axis value, or `0.0` if not found.
double rt_action_axis_raw(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? a->axis_value : 0.0;
}

/// @brief `Action.List` — return a `seq<str>` of every defined action's name.
///
/// Useful for binding-config UIs and serialization. Order matches
/// the internal linked-list order (newest-first). The sequence owns newly
/// copied runtime-string elements.
/// @return Owned runtime sequence of owned action-name strings.
void *rt_action_list(void) {
    RT_ASSERT_MAIN_THREAD();
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    /* Make the seq own its elements and drop our creation reference after each push
     * (the runtime seq<str> convention, cf. rt_dir_list.c) — otherwise every string
     * keeps an extra reference that nothing ever releases, leaking one per action. */
    rt_seq_set_owns_elements(seq, 1);
    Action *a = g_actions;
    int64_t visited = 0;
    while (a && visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return seq;
        rt_string name = rt_string_from_bytes(a->name, (size_t)a->name_len);
        if (!name)
            return seq;
        rt_seq_push(seq, (void *)name);
        rt_string_unref(name);
        a = a->next;
    }
    if (a) {
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        return NULL;
    }
    return seq;
}

/// @brief Append literal text to an action-binding description builder.
/// @details Wraps `rt_sb_append_cstr` with a boolean return so callers can share one error path.
///          A NULL literal is treated as an empty string, which keeps fallback names safe when
///          called from defensive branches.
/// @param sb Borrowed builder receiving the text.
/// @param text Borrowed NUL-terminated text to append; may be `NULL`.
/// @return `1` on success; `0` if the builder reports a failure.
static int action_bindings_append_cstr(rt_string_builder *sb, const char *text) {
    return rt_sb_append_cstr(sb, text ? text : "") == RT_SB_OK;
}

/// @brief Append the display name for a keyboard key to an action-binding description.
/// @details Uses `rt_keyboard_key_name` when it returns non-empty text. If the key code has no
///          known display name, appends @p fallback so callers never lose the binding entirely.
/// @param sb Borrowed builder receiving the key name.
/// @param key Runtime key code.
/// @param fallback Borrowed text used when no key name is available.
/// @return `1` on success; `0` if appending to the builder failed.
static int action_bindings_append_key_name(rt_string_builder *sb,
                                           int64_t key,
                                           const char *fallback) {
    rt_string key_name = rt_keyboard_key_name(key);
    int64_t key_len = key_name ? rt_str_len(key_name) : 0;
    int appended;
    if (key_len > 0)
        appended = rt_sb_append_bytes(sb, key_name->data, (size_t)key_len) == RT_SB_OK;
    else
        appended = action_bindings_append_cstr(sb, fallback ? fallback : "Key");
    if (key_name)
        rt_string_unref(key_name);
    return appended;
}

/// @brief Append a human-readable description for one action binding.
/// @details Covers keyboard, mouse, gamepad, axis, and chord bindings. Chord descriptions are
///          assembled directly into the dynamic builder, so long key names or many bindings no
///          longer truncate at a fixed stack-buffer boundary.
/// @param sb Borrowed builder receiving the binding text.
/// @param b Borrowed binding to describe; `NULL` emits `"Unknown"`.
/// @return `1` on success; `0` if appending to the builder failed.
static int action_bindings_append_desc(rt_string_builder *sb, const Binding *b) {
    if (!b || b->state_magic != ACTION_BINDING_STATE_MAGIC)
        return action_bindings_append_cstr(sb, "Unknown");

    switch (b->type) {
        case BIND_KEY:
            return action_bindings_append_key_name(sb, b->code, "Key");
        case BIND_MOUSE_BUTTON:
            switch (b->code) {
                case ZANNA_MOUSE_BUTTON_LEFT:
                    return action_bindings_append_cstr(sb, "Mouse Left");
                case ZANNA_MOUSE_BUTTON_RIGHT:
                    return action_bindings_append_cstr(sb, "Mouse Right");
                case ZANNA_MOUSE_BUTTON_MIDDLE:
                    return action_bindings_append_cstr(sb, "Mouse Middle");
                default:
                    return action_bindings_append_cstr(sb, "Mouse Button");
            }
        case BIND_MOUSE_X:
            return action_bindings_append_cstr(sb, "Mouse X");
        case BIND_MOUSE_Y:
            return action_bindings_append_cstr(sb, "Mouse Y");
        case BIND_SCROLL_X:
            return action_bindings_append_cstr(sb, "Scroll X");
        case BIND_SCROLL_Y:
            return action_bindings_append_cstr(sb, "Scroll Y");
        case BIND_PAD_BUTTON:
        case BIND_PAD_BUTTON_AXIS:
            switch (b->code) {
                case ZANNA_PAD_A:
                    return action_bindings_append_cstr(sb, "Pad A");
                case ZANNA_PAD_B:
                    return action_bindings_append_cstr(sb, "Pad B");
                case ZANNA_PAD_X:
                    return action_bindings_append_cstr(sb, "Pad X");
                case ZANNA_PAD_Y:
                    return action_bindings_append_cstr(sb, "Pad Y");
                case ZANNA_PAD_LB:
                    return action_bindings_append_cstr(sb, "Pad LB");
                case ZANNA_PAD_RB:
                    return action_bindings_append_cstr(sb, "Pad RB");
                case ZANNA_PAD_UP:
                    return action_bindings_append_cstr(sb, "Pad Up");
                case ZANNA_PAD_DOWN:
                    return action_bindings_append_cstr(sb, "Pad Down");
                case ZANNA_PAD_LEFT:
                    return action_bindings_append_cstr(sb, "Pad Left");
                case ZANNA_PAD_RIGHT:
                    return action_bindings_append_cstr(sb, "Pad Right");
                case ZANNA_PAD_START:
                    return action_bindings_append_cstr(sb, "Pad Start");
                case ZANNA_PAD_BACK:
                    return action_bindings_append_cstr(sb, "Pad Back");
                default:
                    return action_bindings_append_cstr(sb, "Pad Button");
            }
        case BIND_PAD_AXIS:
            switch (b->code) {
                case ZANNA_AXIS_LEFT_X:
                    return action_bindings_append_cstr(sb, "Left Stick X");
                case ZANNA_AXIS_LEFT_Y:
                    return action_bindings_append_cstr(sb, "Left Stick Y");
                case ZANNA_AXIS_RIGHT_X:
                    return action_bindings_append_cstr(sb, "Right Stick X");
                case ZANNA_AXIS_RIGHT_Y:
                    return action_bindings_append_cstr(sb, "Right Stick Y");
                case ZANNA_AXIS_LEFT_TRIGGER:
                    return action_bindings_append_cstr(sb, "Left Trigger");
                case ZANNA_AXIS_RIGHT_TRIGGER:
                    return action_bindings_append_cstr(sb, "Right Trigger");
                default:
                    return action_bindings_append_cstr(sb, "Pad Axis");
            }
        case BIND_CHORD: {
            int32_t chord_len = b->chord_len;
            if (chord_len <= 0)
                return action_bindings_append_cstr(sb, "Chord");
            if (chord_len > MAX_CHORD_KEYS)
                chord_len = MAX_CHORD_KEYS;
            for (int32_t ci = 0; ci < chord_len; ci++) {
                if (ci > 0 && !action_bindings_append_cstr(sb, "+"))
                    return 0;
                if (!action_bindings_append_key_name(sb, b->chord_keys[ci], "Key"))
                    return 0;
            }
            return 1;
        }
        default:
            return action_bindings_append_cstr(sb, "Unknown");
    }
}

/// @brief `Action.BindingsStr(action)` — human-readable description of all bindings.
///
/// Comma-separated list like "Space, Mouse Left, Pad A, Ctrl+S".
/// Useful for "press X to jump"-style on-screen prompts. The result is built
/// dynamically so long binding lists are returned in full unless allocation
/// fails. Binding order is newest-first.
/// @param action Borrowed action name.
/// @return Owned description string. Missing actions return the immortal empty
///         string; allocation failure traps and returns that same fallback.
rt_string rt_action_bindings_str(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    if (!a)
        return rt_str_empty();

    rt_string_builder sb;
    rt_sb_init(&sb);
    int first = 1;

    Binding *b = a->bindings;
    int64_t visited = 0;
    while (b && visited++ < a->binding_count) {
        if (!action_binding_valid(b, a->is_axis))
            goto failed;
        if (!first && !action_bindings_append_cstr(&sb, ", "))
            goto failed;
        first = 0;
        if (!action_bindings_append_desc(&sb, b))
            goto failed;
        b = b->next;
    }
    if (b || visited != a->binding_count)
        goto failed;

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    if (result)
        return result;
    rt_trap("Action.BindingsStr: result allocation failed");
    return rt_str_empty();

failed:
    rt_sb_free(&sb);
    rt_trap("Action.BindingsStr: binding description allocation failed");
    return rt_str_empty();
}

/// @brief `Action.BindingCount(action)` — total number of bindings on this action.
/// @param action Borrowed action name.
/// @return Total number of all binding types, or `0` if the action is absent.
int64_t rt_action_binding_count(rt_string action) {
    RT_ASSERT_MAIN_THREAD();
    Action *a = find_action_str(action);
    return a ? a->binding_count : 0;
}

/// @brief `Action.KeyBoundTo(key)` — name of the first action bound to `key`, or "".
/// @details Searches actions newest-first and includes both button and
///          key-to-axis bindings because they share `BIND_KEY`.
/// @param key Runtime keyboard code to search for.
/// @return Newly owned matching action name, or the immortal empty string.
rt_string rt_action_key_bound_to(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (!action_key_code_valid(key))
        return rt_str_empty();
    Action *a = g_actions;
    int64_t action_visited = 0;
    while (a && action_visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return rt_str_empty();
        Binding *b = a->bindings;
        int64_t binding_visited = 0;
        while (b && binding_visited++ < a->binding_count) {
            if (!action_binding_valid(b, a->is_axis))
                return rt_str_empty();
            if (b->type == BIND_KEY && b->code == key)
                return rt_string_from_bytes(a->name, (size_t)a->name_len);
            b = b->next;
        }
        if (b || binding_visited != a->binding_count)
            return rt_str_empty();
        a = a->next;
    }
    return rt_str_empty();
}

/// @brief `Action.MouseBoundTo(button)` — name of action bound to mouse button, or "".
/// @param button Runtime mouse-button code to search for.
/// @return Newly owned newest matching action name, or the immortal empty string.
rt_string rt_action_mouse_bound_to(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (!action_mouse_button_valid(button))
        return rt_str_empty();
    Action *a = g_actions;
    int64_t action_visited = 0;
    while (a && action_visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return rt_str_empty();
        Binding *b = a->bindings;
        int64_t binding_visited = 0;
        while (b && binding_visited++ < a->binding_count) {
            if (!action_binding_valid(b, a->is_axis))
                return rt_str_empty();
            if (b->type == BIND_MOUSE_BUTTON && b->code == button)
                return rt_string_from_bytes(a->name, (size_t)a->name_len);
            b = b->next;
        }
        if (b || binding_visited != a->binding_count)
            return rt_str_empty();
        a = a->next;
    }
    return rt_str_empty();
}

/// @brief `Action.PadButtonBoundTo(padIndex, button)` — name of action bound to a pad button.
///
/// Matches both regular pad-button bindings and pad-button-axis
/// bindings. A stored `pad_index = -1` wildcard matches every concrete query;
/// other stored indices must equal @p pad_index.
/// @param pad_index Controller index to search for.
/// @param button Runtime gamepad-button code to search for.
/// @return Newly owned newest matching action name, or the immortal empty string.
rt_string rt_action_pad_button_bound_to(int64_t pad_index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (!action_pad_index_valid(pad_index) || !action_pad_button_valid(button))
        return rt_str_empty();
    Action *a = g_actions;
    int64_t action_visited = 0;
    while (a && action_visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return rt_str_empty();
        Binding *b = a->bindings;
        int64_t binding_visited = 0;
        while (b && binding_visited++ < a->binding_count) {
            if (!action_binding_valid(b, a->is_axis))
                return rt_str_empty();
            if ((b->type == BIND_PAD_BUTTON || b->type == BIND_PAD_BUTTON_AXIS) &&
                b->code == button && (b->pad_index == pad_index || b->pad_index == -1))
                return rt_string_from_bytes(a->name, (size_t)a->name_len);
            b = b->next;
        }
        if (b || binding_visited != a->binding_count)
            return rt_str_empty();
        a = a->next;
    }
    return rt_str_empty();
}

/// @brief `Axis.LeftX` — gamepad left stick X-axis constant.
/// @return `ZANNA_AXIS_LEFT_X`.
int64_t rt_action_axis_left_x(void) {
    return ZANNA_AXIS_LEFT_X;
}

/// @brief `Axis.LeftY` — gamepad left stick Y-axis constant.
/// @return `ZANNA_AXIS_LEFT_Y`.
int64_t rt_action_axis_left_y(void) {
    return ZANNA_AXIS_LEFT_Y;
}

/// @brief `Axis.RightX` — gamepad right stick X-axis constant.
/// @return `ZANNA_AXIS_RIGHT_X`.
int64_t rt_action_axis_right_x(void) {
    return ZANNA_AXIS_RIGHT_X;
}

/// @brief `Axis.RightY` — gamepad right stick Y-axis constant.
/// @return `ZANNA_AXIS_RIGHT_Y`.
int64_t rt_action_axis_right_y(void) {
    return ZANNA_AXIS_RIGHT_Y;
}

/// @brief `Axis.LeftTrigger` — gamepad left analog trigger constant.
/// @return `ZANNA_AXIS_LEFT_TRIGGER`.
int64_t rt_action_axis_left_trigger(void) {
    return ZANNA_AXIS_LEFT_TRIGGER;
}

/// @brief `Axis.RightTrigger` — gamepad right analog trigger constant.
/// @return `ZANNA_AXIS_RIGHT_TRIGGER`.
int64_t rt_action_axis_right_trigger(void) {
    return ZANNA_AXIS_RIGHT_TRIGGER;
}
