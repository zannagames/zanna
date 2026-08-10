//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_action_internal.h
/// @file
/// @brief Defines the private Action/Binding records and allocation-free list
///        helpers shared by the action subsystem translation units.
// Purpose: Shared data model and state for the input-action subsystem, split
//   across rt_action.c (core/runtime), rt_action_presets.c (built-in preset
//   bindings), and rt_action_io.c (JSON save/load). Defines the Action /
//   Binding records, the global action list, and the small list helpers used
//   by all three translation units.
//
// Key invariants:
//   - Actions live in a single global singly-linked list (g_actions); names
//     are unique.
//   - g_initialized guards one-time setup; reset by the core teardown path.
//   - List helpers are static inline so each translation unit gets an
//     internal-linkage copy (no exported symbol, no source duplication).
//
// Ownership/Lifetime:
//   - Action and Binding nodes, plus each Action name, use private malloc-owned
//     storage and are freed by the core teardown functions; they are not
//     reference-counted runtime objects.
//   - Helper arguments are borrowed unless the helper explicitly creates and
//     returns a new owned node.
//
// Links: rt_action.c, rt_action_presets.c, rt_action_io.c, rt_action.h
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_action.h"
#include "rt_input.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Physical input-source kinds stored in a Binding node.
typedef enum {
    /// Sentinel used while parsing or representing no source.
    BIND_NONE = 0,
    /// Keyboard key used as a button or fixed axis contribution.
    BIND_KEY, // Keyboard key
    /// Mouse button used by a button action.
    BIND_MOUSE_BUTTON, // Mouse button
    /// Per-frame horizontal mouse delta.
    BIND_MOUSE_X, // Mouse X delta
    /// Per-frame vertical mouse delta.
    BIND_MOUSE_Y, // Mouse Y delta
    /// Per-frame horizontal scroll delta.
    BIND_SCROLL_X, // Mouse scroll X
    /// Per-frame vertical scroll delta.
    BIND_SCROLL_Y, // Mouse scroll Y
    /// Gamepad button used by a button action.
    BIND_PAD_BUTTON, // Gamepad button
    /// Analog gamepad axis.
    BIND_PAD_AXIS, // Gamepad axis
    /// Gamepad button used as a fixed axis contribution.
    BIND_PAD_BUTTON_AXIS, // Gamepad button as axis
    /// Ordered multi-key keyboard chord.
    BIND_CHORD // Multi-key chord (e.g., Ctrl+Shift+S)
} BindingType;

/// @brief Maximum key count stored inline in one chord binding.
#define MAX_CHORD_KEYS 8
/// @brief Maximum number of named actions retained by the global registry.
#define ACTION_MAX_ACTIONS 4096
/// @brief Maximum number of physical bindings retained by one action.
#define ACTION_MAX_BINDINGS_PER_ACTION 65536
/// @brief Maximum aggregate bindings accepted from one persistence document.
#define ACTION_LOAD_MAX_TOTAL_BINDINGS 262144
/// Private cookies for constructed malloc-owned registry nodes.
#define ACTION_STATE_MAGIC UINT64_C(0x5A414354494F4E32)
#define ACTION_BINDING_STATE_MAGIC UINT64_C(0x5A42494E44494E32)

/// @brief One malloc-owned physical-source binding in an Action's linked list.
typedef struct Binding {
    uint64_t state_magic; ///< Private initialized-node cookie.
    /// Source kind that determines how the remaining fields are interpreted.
    BindingType type;
    /// Key, mouse-button, gamepad-button, or gamepad-axis identifier.
    int64_t code; // Key/button/axis code
    /// Controller index, with `-1` conventionally selecting any controller.
    int64_t pad_index; // Controller index (-1 for any)
    /// Fixed digital contribution or multiplier for an analog/delta source.
    double value; // Axis value for key/button bindings, scale for analog
    // Chord data (only used when type == BIND_CHORD)
    /// Ordered key codes for a chord; only the first @ref chord_len entries apply.
    int64_t chord_keys[MAX_CHORD_KEYS];
    /// Number of valid entries in @ref chord_keys for BIND_CHORD.
    int32_t chord_len; // Number of keys in the chord
    /// Next binding in newest-first list order.
    struct Binding *next;
} Binding;

/// @brief One named logical action and its cached current-frame state.
typedef struct Action {
    uint64_t state_magic; ///< Private initialized-node cookie.
    /// Owned null-terminated action name.
    char *name;
    /// Exact byte length of @ref name, excluding its trailing NUL.
    int64_t name_len;
    /// Nonzero for an axis action; zero for a button action.
    int8_t is_axis;
    /// Head of the newest-first owned binding list.
    Binding *bindings;
    /// Number of nodes reachable from @ref bindings.
    int64_t binding_count;
    // Cached state (updated each frame)
    /// Nonzero when a qualifying input down edge occurred this frame.
    int8_t pressed;
    /// Nonzero when a qualifying input release occurred this frame.
    int8_t released;
    /// Nonzero while any button binding or complete chord is held.
    int8_t held;
    /// Unclamped sum of all active axis-binding contributions.
    double axis_value;
    /// Next action in newest-first global list order.
    struct Action *next;
} Action;

// Global state (defined in rt_action.c)
/// @brief Head of the global newest-first Action list.
extern Action *g_actions;
/// @brief Nonzero while the action subsystem is initialized.
extern int8_t g_initialized;

/// @brief Validate an Action and its exact bounded binding-list contents.
static inline int action_binding_list_valid(const Action *action);

/// @brief Validate one Action node without traversing its binding list.
static inline int action_node_shallow_valid(const Action *action) {
    return action && action->state_magic == ACTION_STATE_MAGIC && action->name &&
           action->name_len > 0 && (uint64_t)action->name_len <= SIZE_MAX - 1u &&
           action->name[action->name_len] == '\0' &&
           memchr(action->name, '\0', (size_t)action->name_len) == NULL &&
           (action->is_axis == 0 || action->is_axis == 1) &&
           (action->pressed == 0 || action->pressed == 1) &&
           (action->released == 0 || action->released == 1) &&
           (action->held == 0 || action->held == 1) && isfinite(action->axis_value) &&
           action->binding_count >= 0 && action->binding_count <= ACTION_MAX_BINDINGS_PER_ACTION &&
           ((action->binding_count == 0) == (action->bindings == NULL));
}

/// @brief Linear-scan the global action list by C-string name. NULL on miss.
/// @param name Borrowed null-terminated action name.
/// @return Borrowed matching Action, or `NULL` for null/absent names.
static inline Action *find_action(const char *name) {
    if (!name)
        return NULL;
    size_t name_len = strlen(name);
    if (name_len > INT64_MAX)
        return NULL;
    Action *a = g_actions;
    int64_t visited = 0;
    while (a && visited++ < ACTION_MAX_ACTIONS) {
        if (!action_binding_list_valid(a))
            return NULL;
        if (a->name_len == (int64_t)name_len && memcmp(a->name, name, name_len) == 0)
            return a;
        a = a->next;
    }
    return NULL;
}

/// @brief Allocate a new binding node populated with the given fields.
/// @details Chord storage is zero-initialized, so a detached non-chord binding
///          is always safe to inspect and serialize defensively.
/// @param type Physical source kind.
/// @param code Key, button, or axis identifier.
/// @param pad_index Controller index stored with the binding.
/// @param value Fixed contribution or source multiplier.
/// @return Owned detached binding node, or `NULL` if allocation fails.
static inline Binding *create_binding(BindingType type,
                                      int64_t code,
                                      int64_t pad_index,
                                      double value) {
    Binding *b = (Binding *)malloc(sizeof(Binding));
    if (!b)
        return NULL;
    b->state_magic = ACTION_BINDING_STATE_MAGIC;
    b->type = type;
    b->code = code;
    b->pad_index = pad_index;
    b->value = value;
    memset(b->chord_keys, 0, sizeof(b->chord_keys));
    b->chord_len = 0;
    b->next = NULL;
    return b;
}

/// @brief Push a binding onto the head of an action's binding list (LIFO).
/// @param action Borrowed destination Action; must be non-null.
/// @param binding Owned detached binding; must be non-null. Ownership transfers
///        to @p action.
static inline void add_binding(Action *action, Binding *binding) {
    if (!action || action->state_magic != ACTION_STATE_MAGIC || !binding ||
        binding->state_magic != ACTION_BINDING_STATE_MAGIC || action->binding_count < 0 ||
        action->binding_count >= ACTION_MAX_BINDINGS_PER_ACTION)
        return;
    binding->next = action->bindings;
    action->bindings = binding;
    action->binding_count++;
}

/// @brief Append a detached binding to a binding-list tail.
/// @param head Borrowed address of the list head.
/// @param tail Borrowed address of the cached list tail.
/// @param binding Owned detached binding; ownership transfers to the list.
static inline void append_binding(Binding **head, Binding **tail, Binding *binding) {
    if (!head || !tail || !binding || binding->state_magic != ACTION_BINDING_STATE_MAGIC)
        return;
    binding->next = NULL;
    if (*tail)
        (*tail)->next = binding;
    else
        *head = binding;
    *tail = binding;
}

/// @brief Destroy an owned binding list.
/// @param binding Owned list head; may be `NULL`.
static inline int action_binding_chain_shape_valid(const Binding *binding, int64_t count) {
    if (count < 0 || count > ACTION_MAX_BINDINGS_PER_ACTION)
        return 0;
    for (int64_t i = 0; i < count; ++i) {
        if (!binding || binding->state_magic != ACTION_BINDING_STATE_MAGIC)
            return 0;
        binding = binding->next;
    }
    return binding == NULL;
}

static inline void action_free_bindings(Binding *binding, int64_t count) {
    if (!action_binding_chain_shape_valid(binding, count))
        return;
    for (int64_t released = 0; released < count; ++released) {
        Binding *next = binding->next;
        binding->state_magic = 0;
        free(binding);
        binding = next;
    }
}

/// @brief Destroy one owned action node and all of its bindings.
/// @param action Owned action; may be `NULL`.
static inline void action_free_node(Action *action) {
    if (!action || action->state_magic != ACTION_STATE_MAGIC)
        return;
    int64_t binding_count = action->binding_count;
    action->state_magic = 0;
    free(action->name);
    action_free_bindings(action->bindings, binding_count);
    free(action);
}

/// @brief Destroy an owned action list.
/// @param action Owned list head; may be `NULL`.
static inline void action_free_list(Action *action) {
    const Action *cursor = action;
    int64_t validated = 0;
    while (cursor && validated++ < ACTION_MAX_ACTIONS) {
        if (cursor->state_magic != ACTION_STATE_MAGIC)
            return;
        cursor = cursor->next;
    }
    if (cursor)
        return;
    int64_t released = 0;
    while (action && released++ < ACTION_MAX_ACTIONS) {
        Action *next = action->next;
        action_free_node(action);
        action = next;
    }
}

/// @brief Test whether a binding has the same persisted identity and value.
/// @param lhs Borrowed first binding.
/// @param rhs Borrowed second binding.
/// @return Nonzero when every semantically relevant field matches exactly.
static inline int action_binding_equal(const Binding *lhs, const Binding *rhs) {
    if (!lhs || !rhs || lhs->state_magic != ACTION_BINDING_STATE_MAGIC ||
        rhs->state_magic != ACTION_BINDING_STATE_MAGIC || lhs->type != rhs->type ||
        lhs->code != rhs->code || lhs->pad_index != rhs->pad_index || lhs->value != rhs->value ||
        lhs->chord_len != rhs->chord_len)
        return 0;
    if (lhs->type != BIND_CHORD)
        return 1;
    if (lhs->chord_len < 0 || lhs->chord_len > MAX_CHORD_KEYS)
        return 0;
    return memcmp(lhs->chord_keys,
                  rhs->chord_keys,
                  (size_t)lhs->chord_len * sizeof(lhs->chord_keys[0])) == 0;
}

/// @brief Validate one runtime keyboard code.
static inline int action_key_code_valid(int64_t key) {
    return key > ZANNA_KEY_UNKNOWN && key < ZANNA_KEY_MAX;
}

/// @brief Validate one runtime mouse-button code.
static inline int action_mouse_button_valid(int64_t button) {
    return button >= 0 && button < ZANNA_MOUSE_BUTTON_MAX;
}

/// @brief Validate one concrete or wildcard controller index.
static inline int action_pad_index_valid(int64_t pad_index) {
    return pad_index == -1 || (pad_index >= 0 && pad_index < ZANNA_PAD_MAX);
}

/// @brief Validate one runtime gamepad-button code.
static inline int action_pad_button_valid(int64_t button) {
    return button >= 0 && button < ZANNA_PAD_BUTTON_MAX;
}

/// @brief Validate one runtime gamepad-axis code.
static inline int action_pad_axis_valid(int64_t axis) {
    return axis >= ZANNA_AXIS_LEFT_X && axis <= ZANNA_AXIS_RIGHT_TRIGGER;
}

/// @brief Validate a persisted binding against its enclosing action kind.
static inline int action_binding_valid(const Binding *binding, int8_t is_axis) {
    if (!binding || binding->state_magic != ACTION_BINDING_STATE_MAGIC ||
        (is_axis != 0 && is_axis != 1) || !isfinite(binding->value))
        return 0;
    switch (binding->type) {
        case BIND_KEY:
            return action_key_code_valid(binding->code);
        case BIND_MOUSE_BUTTON:
            return !is_axis && action_mouse_button_valid(binding->code);
        case BIND_MOUSE_X:
        case BIND_MOUSE_Y:
        case BIND_SCROLL_X:
        case BIND_SCROLL_Y:
            return is_axis != 0;
        case BIND_PAD_BUTTON:
            return !is_axis && action_pad_index_valid(binding->pad_index) &&
                   action_pad_button_valid(binding->code);
        case BIND_PAD_AXIS:
            return is_axis && action_pad_index_valid(binding->pad_index) &&
                   action_pad_axis_valid(binding->code);
        case BIND_PAD_BUTTON_AXIS:
            return is_axis && action_pad_index_valid(binding->pad_index) &&
                   action_pad_button_valid(binding->code);
        case BIND_CHORD:
            if (is_axis || binding->chord_len < 2 || binding->chord_len > MAX_CHORD_KEYS)
                return 0;
            for (int32_t i = 0; i < binding->chord_len; ++i) {
                if (!action_key_code_valid(binding->chord_keys[i]))
                    return 0;
                for (int32_t previous = 0; previous < i; ++previous) {
                    if (binding->chord_keys[previous] == binding->chord_keys[i])
                        return 0;
                }
            }
            return 1;
        default:
            return 0;
    }
}

/// @brief Validate an Action and the exact shape and contents of its binding list.
static inline int action_binding_list_valid(const Action *action) {
    if (!action_node_shallow_valid(action))
        return 0;
    const Binding *binding = action->bindings;
    for (int64_t i = 0; i < action->binding_count; ++i) {
        if (!action_binding_valid(binding, action->is_axis))
            return 0;
        binding = binding->next;
    }
    return binding == NULL;
}
