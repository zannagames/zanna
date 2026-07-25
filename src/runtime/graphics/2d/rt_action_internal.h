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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Physical input-source kinds stored in a Binding node.
typedef enum {
    /// Sentinel used while parsing or representing no source.
    BIND_NONE = 0,
    /// Keyboard key used as a button or fixed axis contribution.
    BIND_KEY,             // Keyboard key
    /// Mouse button used by a button action.
    BIND_MOUSE_BUTTON,    // Mouse button
    /// Per-frame horizontal mouse delta.
    BIND_MOUSE_X,         // Mouse X delta
    /// Per-frame vertical mouse delta.
    BIND_MOUSE_Y,         // Mouse Y delta
    /// Per-frame horizontal scroll delta.
    BIND_SCROLL_X,        // Mouse scroll X
    /// Per-frame vertical scroll delta.
    BIND_SCROLL_Y,        // Mouse scroll Y
    /// Gamepad button used by a button action.
    BIND_PAD_BUTTON,      // Gamepad button
    /// Analog gamepad axis.
    BIND_PAD_AXIS,        // Gamepad axis
    /// Gamepad button used as a fixed axis contribution.
    BIND_PAD_BUTTON_AXIS, // Gamepad button as axis
    /// Ordered multi-key keyboard chord.
    BIND_CHORD            // Multi-key chord (e.g., Ctrl+Shift+S)
} BindingType;

/// @brief Maximum key count stored inline in one chord binding.
#define MAX_CHORD_KEYS 8

/// @brief One malloc-owned physical-source binding in an Action's linked list.
typedef struct Binding {
    /// Source kind that determines how the remaining fields are interpreted.
    BindingType type;
    /// Key, mouse-button, gamepad-button, or gamepad-axis identifier.
    int64_t code;      // Key/button/axis code
    /// Controller index, with `-1` conventionally selecting any controller.
    int64_t pad_index; // Controller index (-1 for any)
    /// Fixed digital contribution or multiplier for an analog/delta source.
    double value;      // Axis value for key/button bindings, scale for analog
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
    /// Owned null-terminated action name.
    char *name;
    /// Nonzero for an axis action; zero for a button action.
    int8_t is_axis;
    /// Head of the newest-first owned binding list.
    Binding *bindings;
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

/// @brief Linear-scan the global action list by C-string name. NULL on miss.
/// @param name Borrowed null-terminated action name.
/// @return Borrowed matching Action, or `NULL` for null/absent names.
static inline Action *find_action(const char *name) {
    if (!name)
        return NULL;
    Action *a = g_actions;
    while (a) {
        if (strcmp(a->name, name) == 0)
            return a;
        a = a->next;
    }
    return NULL;
}

/// @brief Allocate a new binding node populated with the given fields.
/// @details `chord_keys` and `chord_len` remain uninitialized; callers creating
///          a BIND_CHORD must populate them before inserting the node.
/// @param type Physical source kind.
/// @param code Key, button, or axis identifier.
/// @param pad_index Controller index stored with the binding.
/// @param value Fixed contribution or source multiplier.
/// @return Owned detached binding node, or `NULL` if allocation fails.
static inline Binding *create_binding(BindingType type, int64_t code, int64_t pad_index,
                                      double value) {
    Binding *b = (Binding *)malloc(sizeof(Binding));
    if (!b)
        return NULL;
    b->type = type;
    b->code = code;
    b->pad_index = pad_index;
    b->value = value;
    b->next = NULL;
    return b;
}

/// @brief Push a binding onto the head of an action's binding list (LIFO).
/// @param action Borrowed destination Action; must be non-null.
/// @param binding Owned detached binding; must be non-null. Ownership transfers
///        to @p action.
static inline void add_binding(Action *action, Binding *binding) {
    binding->next = action->bindings;
    action->bindings = binding;
}
