//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_action_presets.c
/// @file
/// @brief Implements named collections of default keyboard, mouse, and
///        gamepad action bindings.
// Purpose: Built-in input-action presets (standard movement, menu navigation,
//   platformer, top-down, and 3D controls). Each preset defines a set of named
//   actions and their default bindings on top of the action core.
//
// Key invariants:
//   - Presets overlay the current global registry; they never clear it.
//   - A missing name is defined with the preset's expected button/axis kind.
//     Existing compatible actions and exact bindings are reused, making each
//     preset idempotent.
//   - Loading is transactional: allocation failure or a conflicting action
//     kind leaves the previous registry untouched.
//
// Ownership/Lifetime:
//   - Newly defined Action names and all Binding nodes are malloc-owned by the
//     global action registry. Input preset-name strings are borrowed.
//
// Links: rt_action.h (public API), rt_action_internal.h (shared model),
//        rt_action.c (action core)
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_action_internal.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_json_stream.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Extract an int64 value from a borrowed runtime box.
extern int64_t rt_unbox_i64(void *box);

/// @brief Sticky success flag for one main-thread-only preset transaction.
static int g_preset_build_ok = 1;

/// @brief Duplicate an action preset name with malloc-backed ownership.
/// @details Action structures free their `name` field with `free`, so this
///          helper avoids depending on platform-specific `strdup` declarations
///          while preserving the existing ownership contract.
/// @param text Source action name.
/// @return Owned copy, or `NULL` on invalid input, overflow, or allocation
///         failure.
static char *action_preset_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1u)
        return NULL;
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

/// @brief Deep-copy the action registry while preserving action/binding order.
/// @param source Borrowed registry head.
/// @return Owned clone, or `NULL` when the source is empty or allocation fails.
///         Callers distinguish those cases through @p ok.
static Action *clone_action_list(const Action *source, int *ok) {
    Action *head = NULL;
    Action *tail = NULL;
    *ok = 0;

    int64_t action_count = 0;
    while (source && action_count++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(source))
            goto failed;
        Action *copy = (Action *)calloc(1, sizeof(Action));
        if (!copy)
            goto failed;
        copy->state_magic = ACTION_STATE_MAGIC;
        copy->name = action_preset_strdup(source->name);
        if (!copy->name) {
            free(copy);
            goto failed;
        }
        copy->name_len = source->name_len;
        copy->is_axis = source->is_axis;
        copy->pressed = source->pressed;
        copy->released = source->released;
        copy->held = source->held;
        copy->axis_value = source->axis_value;
        Binding *binding_tail = NULL;
        int64_t binding_count = 0;
        for (const Binding *binding = source->bindings;
             binding && binding_count++ < source->binding_count;
             binding = binding->next) {
            if (!action_binding_valid(binding, source->is_axis)) {
                action_free_node(copy);
                goto failed;
            }
            Binding *binding_copy = (Binding *)malloc(sizeof(Binding));
            if (!binding_copy) {
                action_free_node(copy);
                goto failed;
            }
            *binding_copy = *binding;
            binding_copy->next = NULL;
            append_binding(&copy->bindings, &binding_tail, binding_copy);
            copy->binding_count++;
        }
        if (binding_count != source->binding_count || copy->binding_count != binding_count) {
            action_free_node(copy);
            goto failed;
        }

        if (tail)
            tail->next = copy;
        else
            head = copy;
        tail = copy;
        source = source->next;
    }
    if (source)
        goto failed;
    *ok = 1;
    return head;

failed:
    action_free_list(head);
    return NULL;
}

/// @brief Internal: define a button or axis action by C-string name.
/// @details Inserts new actions at the global list head, reuses compatible
///          existing actions, and marks kind conflicts as transaction failures.
/// @param name Borrowed null-terminated action name.
/// @param is_axis Nonzero to create an axis action; zero for a button action.
/// @return Borrowed pointer to the compatible registry node, or `NULL` for an
///         invalid/conflicting name or allocation failure.
static Action *define_action_cstr(const char *name, int8_t is_axis) {
    if (!name || !*name) {
        g_preset_build_ok = 0;
        return NULL;
    }
    Action *existing = find_action(name);
    if (existing) {
        if (existing->is_axis == (is_axis != 0))
            return existing;
        g_preset_build_ok = 0;
        return NULL;
    }
    int64_t action_count = 0;
    for (Action *action = g_actions; action; action = action->next) {
        if (!action_binding_list_valid(action) || ++action_count >= ACTION_MAX_ACTIONS) {
            g_preset_build_ok = 0;
            return NULL;
        }
    }

    Action *a = (Action *)calloc(1, sizeof(Action));
    if (!a) {
        g_preset_build_ok = 0;
        return NULL;
    }
    a->state_magic = ACTION_STATE_MAGIC;
    a->name = action_preset_strdup(name);
    if (!a->name) {
        free(a);
        g_preset_build_ok = 0;
        return NULL;
    }
    a->name_len = (int64_t)strlen(name);
    a->is_axis = is_axis != 0;
    a->next = g_actions;
    g_actions = a;
    action_rebuild_index();
    return a;
}

/// @brief Insert a binding unless an exact equivalent already exists.
static int add_unique_binding(Action *action, Binding *binding) {
    if (!action_node_shallow_valid(action) || !binding ||
        !action_binding_valid(binding, action ? action->is_axis : 0)) {
        if (binding)
            binding->state_magic = 0;
        free(binding);
        g_preset_build_ok = 0;
        return 0;
    }
    int64_t visited = 0;
    Binding *existing = action->bindings;
    for (; existing && visited++ < action->binding_count; existing = existing->next) {
        if (!action_binding_valid(existing, action->is_axis)) {
            binding->state_magic = 0;
            free(binding);
            g_preset_build_ok = 0;
            return 0;
        }
        if (action_binding_equal(existing, binding)) {
            binding->state_magic = 0;
            free(binding);
            return 1;
        }
    }
    if (existing || visited != action->binding_count ||
        action->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION) {
        binding->state_magic = 0;
        free(binding);
        g_preset_build_ok = 0;
        return 0;
    }
    add_binding(action, binding);
    return 1;
}

/// @brief Internal: bind a key to a button action by C-string name.
/// @details Missing or axis-style actions and allocation failures fail the
///          enclosing preset transaction.
/// @param name Borrowed action name.
/// @param key Runtime keyboard code to bind.
static int bind_key_to(const char *name, int64_t key) {
    Action *a = find_action(name);
    if (!a || a->is_axis || !action_key_code_valid(key)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_KEY, key, 0, 1.0);
    return add_unique_binding(a, b);
}

/// @brief Internal: bind a key to an axis action with the given contribution.
/// @details Missing or button-style actions and allocation failures fail the
///          enclosing preset transaction.
/// @param name Borrowed axis-action name.
/// @param key Runtime keyboard code to bind.
/// @param value Contribution added while the key is held.
static int bind_key_axis_to(const char *name, int64_t key, double value) {
    Action *a = find_action(name);
    if (!a || !a->is_axis || !action_key_code_valid(key)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_KEY, key, 0, value);
    return add_unique_binding(a, b);
}

/// @brief Internal: bind a mouse button to a button action.
static int bind_mouse_to(const char *name, int64_t button) {
    Action *a = find_action(name);
    if (!a || a->is_axis || !action_mouse_button_valid(button)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_MOUSE_BUTTON, button, 0, 1.0);
    return add_unique_binding(a, b);
}

/// @brief Internal: bind any-pad button to a button action.
/// @details Stores controller index `-1`; invalid state or allocation failure
///          fails the enclosing preset transaction.
/// @param name Borrowed button-action name.
/// @param button Runtime gamepad-button code to bind.
static int bind_pad_to(const char *name, int64_t button) {
    Action *a = find_action(name);
    if (!a || a->is_axis || !action_pad_button_valid(button)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_PAD_BUTTON, button, -1, 1.0);
    return add_unique_binding(a, b);
}

/// @brief Internal: bind any-pad analog axis to an axis action.
/// @details Stores controller index `-1`; invalid state or allocation failure
///          fails the enclosing preset transaction.
/// @param name Borrowed axis-action name.
/// @param axis One of the `ZANNA_AXIS_*` identifiers.
/// @param scale Multiplier applied to the raw device value.
static int bind_pad_axis_to(const char *name, int64_t axis, double scale) {
    Action *a = find_action(name);
    if (!a || !a->is_axis || !action_pad_axis_valid(axis)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_PAD_AXIS, axis, -1, scale);
    return add_unique_binding(a, b);
}

/// @brief Internal: bind any-pad button to an axis with a fixed contribution.
/// @details Stores controller index `-1`; invalid state or allocation failure
///          fails the enclosing preset transaction.
/// @param name Borrowed axis-action name.
/// @param button Runtime gamepad-button code to bind.
/// @param value Contribution added while the button is held.
static int bind_pad_button_axis_to(const char *name, int64_t button, double value) {
    Action *a = find_action(name);
    if (!a || !a->is_axis || !action_pad_button_valid(button)) {
        g_preset_build_ok = 0;
        return 0;
    }
    Binding *b = create_binding(BIND_PAD_BUTTON_AXIS, button, -1, value);
    return add_unique_binding(a, b);
}

/// @brief Preset: WASD/arrows/D-pad/left-stick → `move_*` actions.
///
/// Defines six actions: 4 button (`move_up/down/left/right`) and 2 axis
/// (`move_x`, `move_y`). Binds the standard physical inputs to all of
/// them so a script can use whichever style fits its needs. Existing
/// compatible actions receive the bindings even when definition is skipped.
static void load_preset_standard_movement(void) {
    /* Button actions */
    define_action_cstr("move_up", 0);
    define_action_cstr("move_down", 0);
    define_action_cstr("move_left", 0);
    define_action_cstr("move_right", 0);

    /* Axis actions */
    define_action_cstr("move_x", 1);
    define_action_cstr("move_y", 1);

    /* WASD */
    bind_key_to("move_up", ZANNA_KEY_W);
    bind_key_to("move_down", ZANNA_KEY_S);
    bind_key_to("move_left", ZANNA_KEY_A);
    bind_key_to("move_right", ZANNA_KEY_D);

    /* Arrow keys */
    bind_key_to("move_up", ZANNA_KEY_UP);
    bind_key_to("move_down", ZANNA_KEY_DOWN);
    bind_key_to("move_left", ZANNA_KEY_LEFT);
    bind_key_to("move_right", ZANNA_KEY_RIGHT);

    /* D-pad (any controller) */
    bind_pad_to("move_up", ZANNA_PAD_UP);
    bind_pad_to("move_down", ZANNA_PAD_DOWN);
    bind_pad_to("move_left", ZANNA_PAD_LEFT);
    bind_pad_to("move_right", ZANNA_PAD_RIGHT);

    /* Axis: WASD as digital */
    bind_key_axis_to("move_x", ZANNA_KEY_A, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_D, 1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_LEFT, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_RIGHT, 1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_W, -1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_S, 1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_UP, -1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_DOWN, 1.0);

    /* Axis: left stick */
    bind_pad_axis_to("move_x", ZANNA_AXIS_LEFT_X, 1.0);
    bind_pad_axis_to("move_y", ZANNA_AXIS_LEFT_Y, 1.0);

    /* Axis: D-pad as digital axis */
    bind_pad_button_axis_to("move_x", ZANNA_PAD_LEFT, -1.0);
    bind_pad_button_axis_to("move_x", ZANNA_PAD_RIGHT, 1.0);
    bind_pad_button_axis_to("move_y", ZANNA_PAD_UP, -1.0);
    bind_pad_button_axis_to("move_y", ZANNA_PAD_DOWN, 1.0);
}

/// @brief Preset: first/third-person 3D controls — WASD movement axes, jump,
///   sprint, crouch, interact, fire/aim, pause; left stick + face buttons on pad.
/// @details Adds mouse buttons for fire/aim, any-pad buttons for discrete
///          actions, and left-stick plus WASD movement axes.
static void load_preset_fps3d(void) {
    define_action_cstr("jump", 0);
    define_action_cstr("sprint", 0);
    define_action_cstr("crouch", 0);
    define_action_cstr("interact", 0);
    define_action_cstr("fire", 0);
    define_action_cstr("aim", 0);
    define_action_cstr("pause", 0);
    define_action_cstr("move_x", 1);
    define_action_cstr("move_y", 1);

    bind_key_to("jump", ZANNA_KEY_SPACE);
    bind_key_to("sprint", ZANNA_KEY_LSHIFT);
    bind_key_to("crouch", ZANNA_KEY_LCTRL);
    bind_key_to("interact", ZANNA_KEY_E);
    bind_key_to("pause", ZANNA_KEY_ESCAPE);
    bind_mouse_to("fire", ZANNA_MOUSE_BUTTON_LEFT);
    bind_mouse_to("aim", ZANNA_MOUSE_BUTTON_RIGHT);

    bind_pad_to("jump", ZANNA_PAD_A);
    bind_pad_to("sprint", ZANNA_PAD_LB);
    bind_pad_to("crouch", ZANNA_PAD_B);
    bind_pad_to("interact", ZANNA_PAD_X);
    bind_pad_to("pause", ZANNA_PAD_START);

    bind_key_axis_to("move_x", ZANNA_KEY_A, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_D, 1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_W, -1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_S, 1.0);
    bind_pad_axis_to("move_x", ZANNA_AXIS_LEFT_X, 1.0);
    bind_pad_axis_to("move_y", ZANNA_AXIS_LEFT_Y, 1.0);
}

/// @brief Preset: arrow/D-pad navigation + Enter/Space + Esc → `menu_*`/`confirm`/`back`.
/// @details Adds WASD aliases and any-pad D-pad/A/B bindings alongside the
///          keyboard navigation controls.
static void load_preset_menu_navigation(void) {
    define_action_cstr("menu_up", 0);
    define_action_cstr("menu_down", 0);
    define_action_cstr("menu_left", 0);
    define_action_cstr("menu_right", 0);
    define_action_cstr("confirm", 0);
    define_action_cstr("back", 0);

    /* Arrow keys */
    bind_key_to("menu_up", ZANNA_KEY_UP);
    bind_key_to("menu_down", ZANNA_KEY_DOWN);
    bind_key_to("menu_left", ZANNA_KEY_LEFT);
    bind_key_to("menu_right", ZANNA_KEY_RIGHT);

    /* WASD */
    bind_key_to("menu_up", ZANNA_KEY_W);
    bind_key_to("menu_down", ZANNA_KEY_S);
    bind_key_to("menu_left", ZANNA_KEY_A);
    bind_key_to("menu_right", ZANNA_KEY_D);

    /* Confirm / Back */
    bind_key_to("confirm", ZANNA_KEY_ENTER);
    bind_key_to("confirm", ZANNA_KEY_SPACE);
    bind_key_to("back", ZANNA_KEY_ESCAPE);

    /* D-pad (any controller) */
    bind_pad_to("menu_up", ZANNA_PAD_UP);
    bind_pad_to("menu_down", ZANNA_PAD_DOWN);
    bind_pad_to("menu_left", ZANNA_PAD_LEFT);
    bind_pad_to("menu_right", ZANNA_PAD_RIGHT);

    /* Gamepad confirm / back */
    bind_pad_to("confirm", ZANNA_PAD_A);
    bind_pad_to("back", ZANNA_PAD_B);
}

/// @brief Preset: 2D platformer controls — `move_left/right`, `jump`, `shoot`, `pause` + `move_x`
/// axis.
/// @details Combines keyboard, any-pad D-pad/face-button, and left-stick input.
static void load_preset_platformer(void) {
    define_action_cstr("move_left", 0);
    define_action_cstr("move_right", 0);
    define_action_cstr("jump", 0);
    define_action_cstr("shoot", 0);
    define_action_cstr("pause", 0);
    define_action_cstr("move_x", 1);

    /* Keys */
    bind_key_to("move_left", ZANNA_KEY_A);
    bind_key_to("move_left", ZANNA_KEY_LEFT);
    bind_key_to("move_right", ZANNA_KEY_D);
    bind_key_to("move_right", ZANNA_KEY_RIGHT);
    bind_key_to("jump", ZANNA_KEY_SPACE);
    bind_key_to("jump", ZANNA_KEY_W);
    bind_key_to("jump", ZANNA_KEY_UP);
    bind_key_to("shoot", ZANNA_KEY_J);
    bind_key_to("shoot", ZANNA_KEY_X);
    bind_key_to("pause", ZANNA_KEY_ESCAPE);

    /* Gamepad */
    bind_pad_to("move_left", ZANNA_PAD_LEFT);
    bind_pad_to("move_right", ZANNA_PAD_RIGHT);
    bind_pad_to("jump", ZANNA_PAD_A);
    bind_pad_to("shoot", ZANNA_PAD_X);
    bind_pad_to("pause", ZANNA_PAD_START);

    /* Axis: left/right */
    bind_key_axis_to("move_x", ZANNA_KEY_A, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_D, 1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_LEFT, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_RIGHT, 1.0);
    bind_pad_axis_to("move_x", ZANNA_AXIS_LEFT_X, 1.0);
    bind_pad_button_axis_to("move_x", ZANNA_PAD_LEFT, -1.0);
    bind_pad_button_axis_to("move_x", ZANNA_PAD_RIGHT, 1.0);
}

/// @brief Preset: top-down shooter controls — 4-directional movement + `fire`/`pause`.
/// @details Provides both discrete movement actions and two movement axes
///          sourced from keyboard, D-pad, and left stick.
static void load_preset_topdown(void) {
    define_action_cstr("move_up", 0);
    define_action_cstr("move_down", 0);
    define_action_cstr("move_left", 0);
    define_action_cstr("move_right", 0);
    define_action_cstr("fire", 0);
    define_action_cstr("pause", 0);
    define_action_cstr("move_x", 1);
    define_action_cstr("move_y", 1);

    /* WASD */
    bind_key_to("move_up", ZANNA_KEY_W);
    bind_key_to("move_down", ZANNA_KEY_S);
    bind_key_to("move_left", ZANNA_KEY_A);
    bind_key_to("move_right", ZANNA_KEY_D);

    /* Arrow keys */
    bind_key_to("move_up", ZANNA_KEY_UP);
    bind_key_to("move_down", ZANNA_KEY_DOWN);
    bind_key_to("move_left", ZANNA_KEY_LEFT);
    bind_key_to("move_right", ZANNA_KEY_RIGHT);

    /* Fire / Pause */
    bind_key_to("fire", ZANNA_KEY_SPACE);
    bind_key_to("fire", ZANNA_KEY_J);
    bind_key_to("pause", ZANNA_KEY_ESCAPE);

    /* D-pad (any controller) */
    bind_pad_to("move_up", ZANNA_PAD_UP);
    bind_pad_to("move_down", ZANNA_PAD_DOWN);
    bind_pad_to("move_left", ZANNA_PAD_LEFT);
    bind_pad_to("move_right", ZANNA_PAD_RIGHT);
    bind_pad_to("fire", ZANNA_PAD_A);
    bind_pad_to("pause", ZANNA_PAD_START);

    /* Axis: WASD + arrows as digital */
    bind_key_axis_to("move_x", ZANNA_KEY_A, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_D, 1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_LEFT, -1.0);
    bind_key_axis_to("move_x", ZANNA_KEY_RIGHT, 1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_W, -1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_S, 1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_UP, -1.0);
    bind_key_axis_to("move_y", ZANNA_KEY_DOWN, 1.0);

    /* Axis: left stick */
    bind_pad_axis_to("move_x", ZANNA_AXIS_LEFT_X, 1.0);
    bind_pad_axis_to("move_y", ZANNA_AXIS_LEFT_Y, 1.0);

    /* Axis: D-pad as digital axis */
    bind_pad_button_axis_to("move_x", ZANNA_PAD_LEFT, -1.0);
    bind_pad_button_axis_to("move_x", ZANNA_PAD_RIGHT, 1.0);
    bind_pad_button_axis_to("move_y", ZANNA_PAD_UP, -1.0);
    bind_pad_button_axis_to("move_y", ZANNA_PAD_DOWN, 1.0);
}

/// @brief `Action.LoadPreset(name)` — apply a pre-configured action set.
///
/// Defines actions and binds standard keyboard + gamepad inputs in one
/// call. Available presets:
///   - `"standard_movement"` — 4-directional move + axis variant.
///   - `"menu_navigation"` — menu_up/down/left/right + confirm + back.
///   - `"platformer"` — left/right + jump + shoot + pause + axis.
///   - `"topdown"` — 4-directional + fire + pause + axis variant.
///   - `"fps3d"` — 3D first/third-person: move axes, jump/sprint/crouch,
///     interact, mouse fire/aim, pause; pad face buttons + left stick.
/// Auto-initializes the action system if not already done. Returns
/// 0 if `name` doesn't match any preset. A recognized preset also returns 0
/// if a conflicting action kind or allocation failure prevents the complete
/// overlay; in that case the registry is unchanged.
/// @param preset_name Borrowed exact preset identifier.
/// @return `1` for a recognized identifier; `0` for null, empty, or unknown
///         names.
int8_t rt_action_load_preset(rt_string preset_name) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_initialized)
        rt_action_init();

    if (!preset_name || !rt_string_is_handle(preset_name) || rt_str_len(preset_name) == 0)
        return 0;

    int64_t len = rt_str_len(preset_name);
    const char *data = preset_name->data;
    void (*loader)(void) = NULL;

    if (len == 17 && memcmp(data, "standard_movement", 17) == 0) {
        loader = load_preset_standard_movement;
    } else if (len == 15 && memcmp(data, "menu_navigation", 15) == 0) {
        loader = load_preset_menu_navigation;
    } else if (len == 10 && memcmp(data, "platformer", 10) == 0) {
        loader = load_preset_platformer;
    } else if (len == 7 && memcmp(data, "topdown", 7) == 0) {
        loader = load_preset_topdown;
    } else if (len == 5 && memcmp(data, "fps3d", 5) == 0) {
        loader = load_preset_fps3d;
    }
    if (!loader)
        return 0;

    int clone_ok = 0;
    Action *saved_actions = g_actions;
    Action *working_actions = clone_action_list(saved_actions, &clone_ok);
    if (!clone_ok)
        return 0;

    g_actions = working_actions;
    action_rebuild_index();
    g_preset_build_ok = 1;
    loader();
    if (g_preset_build_ok) {
        action_free_list(saved_actions);
        action_rebuild_index();
        return 1;
    }

    action_free_list(g_actions);
    g_actions = saved_actions;
    action_rebuild_index();
    return 0;
}
