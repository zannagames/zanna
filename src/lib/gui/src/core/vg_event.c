//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_event.c
// Purpose: GUI event dispatch system — routes keyboard, mouse, focus, and
//          custom widget events through the widget tree with hit-testing,
//          bubbling, capture, and per-root state slots.
// Key invariants:
//   - Per-root state (hover, focus, capture) is isolated per root widget.
//   - Root state storage grows as needed instead of sharing state on overflow.
//   - Setting event->handled = true stops bubbling immediately.
// Ownership/Lifetime:
//   - Events are stack-allocated value types; no heap allocation occurs here.
// Links: lib/gui/include/vg_event.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#include "../../include/vg_event.h"
#include "../../include/vg_widget.h"
#include "vgfx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements GUI event construction, platform translation, routing, and bubbling.
/// @details Interaction snapshots are retained independently for each live root widget.
/// Dispatch honors modal roots and input capture, maintains hover and focus, localizes screen
/// coordinates per receiver, and synthesizes bounded single/double/triple-click sequences.

#define VG_DOUBLE_CLICK_MAX_INTERVAL_MS 400
#define VG_DOUBLE_CLICK_MAX_DISTANCE_PX 5.0f
#define VG_EVENT_ROOT_STATE_INITIAL_CAPACITY 16

typedef struct vg_event_root_state_slot {
    vg_widget_t *root;
    uint64_t root_id;
    vg_widget_runtime_state_t state;
    bool initialized;
} vg_event_root_state_slot_t;

static vg_event_root_state_slot_t *g_root_state_slots = NULL;
static size_t g_root_state_slot_capacity = 0;
static vg_widget_t *g_active_event_root = NULL;
static uint64_t g_active_event_root_id = 0;

/// @brief Zero-initializes @p state and sets the last-click button sentinel to -1.
/// @param state Runtime-state record to reset; NULL is ignored.
static void event_init_empty_runtime_state(vg_widget_runtime_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->last_click_button = -1;
    state->last_click_count = 0;
}

/// @brief Returns true if @p ancestor is equal to or is an ancestor of @p widget in the widget
/// tree.
/// @param ancestor Candidate ancestor.
/// @param widget Candidate descendant.
/// @return true when walking from @p widget to the root encounters @p ancestor.
static bool event_widget_is_ancestor(const vg_widget_t *ancestor, const vg_widget_t *widget) {
    for (const vg_widget_t *current = widget; current; current = current->parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

/// @brief Validate a borrowed widget pointer together with the identity observed before a callback.
/// @param widget Candidate widget pointer.
/// @param id Widget generation captured while @p widget was known to be live.
/// @return true only when the same widget allocation remains live.
static bool event_widget_ref_is_live(const vg_widget_t *widget, uint64_t id) {
    return widget && id != 0 && vg_widget_is_live(widget) && widget->id == id;
}

/// @brief Nulls all per-root state references (hover, focus, capture, modal-root, click) that point
/// into @p subtree.
/// @param state Per-root runtime state to sanitize.
/// @param subtree Root of the tree being hidden, detached, or destroyed.
static void event_clear_state_widget(vg_widget_runtime_state_t *state, vg_widget_t *subtree) {
    if (!state || !subtree)
        return;

    if (state->focused_widget && event_widget_is_ancestor(subtree, state->focused_widget)) {
        state->focused_widget = NULL;
        state->focused_widget_id = 0;
    }
    if (state->input_capture_widget &&
        event_widget_is_ancestor(subtree, state->input_capture_widget)) {
        state->input_capture_widget = NULL;
        state->input_capture_widget_id = 0;
    }
    if (state->modal_root && event_widget_is_ancestor(subtree, state->modal_root)) {
        state->modal_root = NULL;
        state->modal_root_id = 0;
    }
    if (state->hovered_widget && event_widget_is_ancestor(subtree, state->hovered_widget)) {
        state->hovered_widget = NULL;
        state->hovered_widget_id = 0;
    }
    if (state->last_click_widget && event_widget_is_ancestor(subtree, state->last_click_widget)) {
        state->last_click_widget = NULL;
        state->last_click_widget_id = 0;
        state->last_click_time_ms = 0;
        state->last_click_button = -1;
        state->last_click_count = 0;
        state->last_click_screen_x = 0.0f;
        state->last_click_screen_y = 0.0f;
    }
    if (state->reported_click_widget &&
        event_widget_is_ancestor(subtree, state->reported_click_widget)) {
        state->reported_click_widget = NULL;
        state->reported_click_widget_id = 0;
        state->reported_click_time_ms = 0;
    }
}

/// @brief Returns true if @p widget is live, its stored ID matches, and it is a descendant of @p
/// root.
/// @param root Required containing root.
/// @param widget Stored widget pointer to validate.
/// @param id Stored live-id generation paired with @p widget.
/// @return true only when all liveness, identity, and ancestry checks pass.
static bool event_state_ref_is_in_root(vg_widget_t *root, vg_widget_t *widget, uint64_t id) {
    if (!root || !widget || !vg_widget_is_live(widget))
        return false;
    if (id != 0 && widget->id != id)
        return false;
    return event_widget_is_ancestor(root, widget);
}

/// @brief Strips any state references in @p state that are not within @p root's subtree, keeping
/// only in-root refs.
/// @param state Runtime-state record to filter.
/// @param root Root whose subtree defines valid references.
static void event_filter_state_to_root(vg_widget_runtime_state_t *state, vg_widget_t *root) {
    if (!state)
        return;

    if (!event_state_ref_is_in_root(root, state->focused_widget, state->focused_widget_id)) {
        state->focused_widget = NULL;
        state->focused_widget_id = 0;
    }
    if (!event_state_ref_is_in_root(
            root, state->input_capture_widget, state->input_capture_widget_id)) {
        state->input_capture_widget = NULL;
        state->input_capture_widget_id = 0;
    }
    if (!event_state_ref_is_in_root(root, state->modal_root, state->modal_root_id)) {
        state->modal_root = NULL;
        state->modal_root_id = 0;
    }
    if (!event_state_ref_is_in_root(root, state->hovered_widget, state->hovered_widget_id)) {
        state->hovered_widget = NULL;
        state->hovered_widget_id = 0;
    }
    if (!event_state_ref_is_in_root(root, state->last_click_widget, state->last_click_widget_id)) {
        state->last_click_widget = NULL;
        state->last_click_widget_id = 0;
        state->last_click_time_ms = 0;
        state->last_click_button = -1;
        state->last_click_count = 0;
        state->last_click_screen_x = 0.0f;
        state->last_click_screen_y = 0.0f;
    }
    if (!event_state_ref_is_in_root(
            root, state->reported_click_widget, state->reported_click_widget_id)) {
        state->reported_click_widget = NULL;
        state->reported_click_widget_id = 0;
        state->reported_click_time_ms = 0;
    }
}

/// @brief Scans all root-state slots and clears every reference pointing into the given subtree;
/// called before subtree destruction.
/// @copydoc vg_event_forget_widget_subtree
void vg_event_forget_widget_subtree(vg_widget_t *widget) {
    if (!widget)
        return;

    for (size_t i = 0; i < g_root_state_slot_capacity; i++) {
        vg_event_root_state_slot_t *slot = &g_root_state_slots[i];
        if (!slot->root)
            continue;
        if (!vg_widget_is_live(slot->root) || slot->root->id != slot->root_id) {
            if (g_active_event_root == slot->root) {
                g_active_event_root = NULL;
                g_active_event_root_id = 0;
            }
            memset(slot, 0, sizeof(*slot));
            continue;
        }
        if (event_widget_is_ancestor(widget, slot->root)) {
            if (g_active_event_root == slot->root) {
                g_active_event_root = NULL;
                g_active_event_root_id = 0;
            }
            memset(slot, 0, sizeof(*slot));
            continue;
        }
        event_clear_state_widget(&slot->state, widget);
    }
}

/// @brief Frees any root-state slots whose root widget has since been destroyed or recycled.
static void event_prune_dead_root_state_slots(void) {
    size_t live_count = 0;
    for (size_t i = 0; i < g_root_state_slot_capacity; i++) {
        if (!g_root_state_slots[i].root)
            continue;
        if (vg_widget_is_live(g_root_state_slots[i].root) &&
            g_root_state_slots[i].root->id == g_root_state_slots[i].root_id) {
            live_count++;
            continue;
        }
        if (g_active_event_root == g_root_state_slots[i].root)
            g_active_event_root = NULL;
        if (!g_active_event_root)
            g_active_event_root_id = 0;
        memset(&g_root_state_slots[i], 0, sizeof(g_root_state_slots[i]));
    }
    if (live_count == 0 && g_root_state_slots) {
        free(g_root_state_slots);
        g_root_state_slots = NULL;
        g_root_state_slot_capacity = 0;
    }
}

/// @brief Finds the per-root state slot for @p root; allocates a new one when @p create is true and
/// no slot exists.
/// @param root Live root widget used as the slot identity.
/// @param create true to allocate or reuse an empty slot when none exists.
/// @return Matching slot, newly initialized slot, or NULL on invalid input or allocation failure.
static vg_event_root_state_slot_t *event_find_root_state_slot(vg_widget_t *root, bool create) {
    if (!root)
        return NULL;

    event_prune_dead_root_state_slots();

    for (size_t i = 0; i < g_root_state_slot_capacity; i++) {
        if (g_root_state_slots[i].root == root && g_root_state_slots[i].root_id == root->id)
            return &g_root_state_slots[i];
    }

    if (!create)
        return NULL;

    for (size_t i = 0; i < g_root_state_slot_capacity; i++) {
        if (g_root_state_slots[i].root)
            continue;
        g_root_state_slots[i].root = root;
        g_root_state_slots[i].root_id = root->id;
        event_init_empty_runtime_state(&g_root_state_slots[i].state);
        g_root_state_slots[i].initialized = false;
        return &g_root_state_slots[i];
    }

    size_t old_capacity = g_root_state_slot_capacity;
    size_t new_capacity =
        old_capacity > 0 ? old_capacity * 2u : (size_t)VG_EVENT_ROOT_STATE_INITIAL_CAPACITY;
    if (new_capacity <= old_capacity)
        return NULL;
    if (new_capacity > SIZE_MAX / sizeof(vg_event_root_state_slot_t))
        return NULL;

    vg_event_root_state_slot_t *new_slots = (vg_event_root_state_slot_t *)realloc(
        g_root_state_slots, new_capacity * sizeof(*new_slots));
    if (!new_slots)
        return NULL;

    memset(new_slots + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*new_slots));
    g_root_state_slots = new_slots;
    g_root_state_slot_capacity = new_capacity;

    g_root_state_slots[old_capacity].root = root;
    g_root_state_slots[old_capacity].root_id = root->id;
    event_init_empty_runtime_state(&g_root_state_slots[old_capacity].state);
    g_root_state_slots[old_capacity].initialized = false;
    return &g_root_state_slots[old_capacity];
}

/// @brief Saves the current global event state into the previously-active root's slot, then
/// restores state for @p root.
/// @param root Live root whose isolated interaction state should become active.
static void event_activate_root_state(vg_widget_t *root) {
    if (!vg_widget_is_live(root))
        return;

    event_prune_dead_root_state_slots();
    if (g_active_event_root == root && g_active_event_root_id == root->id)
        return;

    vg_widget_runtime_state_t current_state;
    event_init_empty_runtime_state(&current_state);
    vg_widget_get_runtime_state(&current_state);

    if (g_active_event_root && vg_widget_is_live(g_active_event_root) &&
        g_active_event_root->id == g_active_event_root_id) {
        vg_event_root_state_slot_t *active_slot =
            event_find_root_state_slot(g_active_event_root, true);
        if (active_slot) {
            active_slot->state = current_state;
            event_filter_state_to_root(&active_slot->state, g_active_event_root);
            active_slot->initialized = true;
        }
    }

    vg_event_root_state_slot_t *slot = event_find_root_state_slot(root, true);
    if (slot && slot->initialized) {
        vg_widget_set_runtime_state(&slot->state);
    } else {
        event_filter_state_to_root(&current_state, root);
        vg_widget_set_runtime_state(&current_state);
        if (slot) {
            slot->state = current_state;
            slot->initialized = true;
        }
    }
    g_active_event_root = root;
    g_active_event_root_id = root->id;
}

/// @brief Returns true for event types that carry widget-local x/y; wheel events use their own
/// screen coords instead.
/// @param type Event type to classify.
/// @return true for movement, button, and synthesized click event types.
static bool event_has_widget_local_mouse_coords(vg_event_type_t type) {
    // Wheel events carry their own screen coordinates. They do not have
    // widget-local x/y because those slots are the scroll deltas.
    return type == VG_EVENT_MOUSE_MOVE || type == VG_EVENT_MOUSE_DOWN ||
           type == VG_EVENT_MOUSE_UP || type == VG_EVENT_CLICK || type == VG_EVENT_DOUBLE_CLICK ||
           type == VG_EVENT_TRIPLE_CLICK;
}

/// @brief Returns the screen-space X coordinate from a mouse or wheel event.
/// @param event Mouse or wheel event to inspect.
/// @return Stored screen x coordinate, or zero for NULL.
static float event_screen_x(const vg_event_t *event) {
    if (!event)
        return 0.0f;
    return event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_x : event->mouse.screen_x;
}

/// @brief Returns the screen-space Y coordinate from a mouse or wheel event.
/// @param event Mouse or wheel event to inspect.
/// @return Stored screen y coordinate, or zero for NULL.
static float event_screen_y(const vg_event_t *event) {
    if (!event)
        return 0.0f;
    return event->type == VG_EVENT_MOUSE_WHEEL ? event->wheel.screen_y : event->mouse.screen_y;
}

/// @brief Subtracts the widget's screen origin from the event's screen coordinates to produce
/// widget-local mouse x/y.
/// @param widget Current receiver defining the local coordinate system.
/// @param event Mouse event to update in place.
static void event_localize_mouse_to_widget(vg_widget_t *widget, vg_event_t *event) {
    if (!widget || !event || !event_has_widget_local_mouse_coords(event->type))
        return;

    float sx = 0.0f;
    float sy = 0.0f;
    vg_widget_get_screen_bounds(widget, &sx, &sy, NULL, NULL);
    event->mouse.x = event->mouse.screen_x - sx;
    event->mouse.y = event->mouse.screen_y - sy;
}

/// @brief Returns true if @p widget is equal to or a descendant of @p root.
/// @param root Candidate subtree root.
/// @param widget Candidate member of the subtree.
/// @return true when @p widget's parent chain reaches @p root.
static bool event_widget_is_in_subtree(vg_widget_t *root, vg_widget_t *widget) {
    for (vg_widget_t *w = widget; w; w = w->parent) {
        if (w == root)
            return true;
    }
    return false;
}

/// @brief Returns the captured widget only if it resides inside the active modal; releases and
/// returns NULL if outside.
/// @param out_id Receives the captured widget generation, or zero when none.
/// @return Live capture allowed by the active modal root, or NULL.
static vg_widget_t *event_modal_safe_capture(uint64_t *out_id) {
    if (out_id)
        *out_id = 0;
    vg_widget_t *capture = vg_widget_get_input_capture();
    if (capture && !vg_widget_is_live(capture)) {
        vg_widget_release_input_capture();
        return NULL;
    }
    vg_widget_t *modal = vg_widget_get_modal_root();
    if (modal && (!vg_widget_is_live(modal) || !modal->visible)) {
        vg_widget_set_modal_root(NULL);
        modal = NULL;
    }
    if (capture && modal && modal->visible && !event_widget_is_in_subtree(modal, capture)) {
        vg_widget_release_input_capture();
        return NULL;
    }
    if (capture && out_id)
        *out_id = capture->id;
    return capture;
}

/// @brief Returns the currently hovered widget from the active root's runtime state.
/// @param out_id Receives the stored widget generation, or zero when none.
/// @return Borrowed hovered widget pointer, or NULL.
static vg_widget_t *get_hovered_widget(uint64_t *out_id) {
    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    if (out_id)
        *out_id = state.hovered_widget_id;
    return state.hovered_widget;
}

/// @brief Updates the hovered widget field in the active root's runtime state.
/// @param widget New borrowed hovered widget, or NULL to clear hover.
static void set_hovered_widget(vg_widget_t *widget) {
    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    state.hovered_widget = widget;
    state.hovered_widget_id = widget ? widget->id : 0;
    vg_widget_set_runtime_state(&state);
}

/// @brief Transitions hover from the previous widget to @p widget, firing MOUSE_LEAVE and
/// MOUSE_ENTER events.
/// @param widget Newly hovered live widget, or NULL when the pointer left the tree.
static void update_hovered_widget(vg_widget_t *widget) {
    uint64_t widget_id = 0;
    if (widget && vg_widget_is_live(widget))
        widget_id = widget->id;
    else
        widget = NULL;

    uint64_t previous_id = 0;
    vg_widget_t *previous = get_hovered_widget(&previous_id);
    if (previous == widget && previous_id == widget_id)
        return;

    if (event_widget_ref_is_live(previous, previous_id)) {
        vg_event_t leave = {0};
        leave.type = VG_EVENT_MOUSE_LEAVE;
        leave.target = previous;
        vg_event_send(previous, &leave);
        if (!event_widget_ref_is_live(widget, widget_id)) {
            widget = NULL;
            widget_id = 0;
        }
    }

    set_hovered_widget(widget);

    if (event_widget_ref_is_live(widget, widget_id)) {
        vg_event_t enter = {0};
        enter.type = VG_EVENT_MOUSE_ENTER;
        enter.target = widget;
        vg_event_send(widget, &enter);
    }
}

/// @brief Resets all last-click tracking fields in the active root's runtime state.
static void clear_last_click_state(void) {
    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    state.last_click_widget = NULL;
    state.last_click_widget_id = 0;
    state.last_click_time_ms = 0;
    state.last_click_button = -1;
    state.last_click_count = 0;
    state.last_click_screen_x = 0.0f;
    state.last_click_screen_y = 0.0f;
    vg_widget_set_runtime_state(&state);
}

/// @brief Records a click on @p widget in the active root's runtime state.
/// @details Stores the latest click position, time, button, and rapid-click
///          count so the next mouse-up can synthesize double/triple-click
///          events without sharing state across root windows.
/// @param widget Widget that received the click.
/// @param widget_id Widget identity captured before dispatching callback code.
/// @param event Mouse-up event that produced the click.
/// @param click_count Count to store for the next rapid-click comparison.
static void remember_click(vg_widget_t *widget,
                           uint64_t widget_id,
                           const vg_event_t *event,
                           int click_count) {
    if (!event || !event_widget_ref_is_live(widget, widget_id))
        return;

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    state.last_click_widget = widget;
    state.last_click_widget_id = widget_id;
    state.last_click_time_ms = event->timestamp;
    state.last_click_button = (int32_t)event->mouse.button;
    state.last_click_count = click_count;
    state.last_click_screen_x = event->mouse.screen_x;
    state.last_click_screen_y = event->mouse.screen_y;
    vg_widget_set_runtime_state(&state);
}

/// @brief Return the rapid-click count represented by this mouse-up event.
/// @details A click continues the previous sequence only when the same live
///          widget receives the same button within the configured time and
///          distance thresholds. Otherwise the new click starts a fresh
///          sequence at count 1.
/// @param widget Widget receiving the mouse-up.
/// @param event Mouse-up event to classify.
/// @return 1 for a fresh click, 2 for a double-click, or 3 for a triple-click.
static int click_count_for_widget(vg_widget_t *widget, const vg_event_t *event) {
    if (!widget || !event || event->timestamp == 0)
        return 1;

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    if (state.last_click_widget != widget ||
        state.last_click_button != (int32_t)event->mouse.button || state.last_click_time_ms == 0 ||
        event->timestamp < state.last_click_time_ms) {
        return 1;
    }

    if (event->timestamp - state.last_click_time_ms > VG_DOUBLE_CLICK_MAX_INTERVAL_MS)
        return 1;

    float dx = event->mouse.screen_x - state.last_click_screen_x;
    float dy = event->mouse.screen_y - state.last_click_screen_y;
    float max_dist_sq = VG_DOUBLE_CLICK_MAX_DISTANCE_PX * VG_DOUBLE_CLICK_MAX_DISTANCE_PX;
    if (dx * dx + dy * dy > max_dist_sq)
        return 1;
    if (state.last_click_count >= 2)
        return 3;
    return 2;
}

//=============================================================================
// Event Creation Helpers
//=============================================================================

/// @brief Constructs a mouse event value with screen and local coordinates, button, and modifiers
/// pre-filled.
/// @copydoc vg_event_mouse
vg_event_t vg_event_mouse(
    vg_event_type_t type, float x, float y, vg_mouse_button_t button, uint32_t modifiers) {
    vg_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = type;
    event.modifiers = modifiers;
    event.mouse.x = x;
    event.mouse.y = y;
    event.mouse.screen_x = x;
    event.mouse.screen_y = y;
    event.mouse.button = button;
    event.mouse.click_count = 1;

    return event;
}

/// @brief Constructs a keyboard event value with key, codepoint, and modifiers set.
/// @copydoc vg_event_key
vg_event_t vg_event_key(vg_event_type_t type,
                        vg_key_t key,
                        uint32_t codepoint,
                        uint32_t modifiers) {
    vg_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = type;
    event.modifiers = modifiers;
    event.key.key = key;
    event.key.codepoint = codepoint;
    event.key.repeat = false;

    return event;
}

//=============================================================================
// Platform Event Translation
//=============================================================================

/// @brief Translates a VGFX platform key code to vg_key_t; passes printable ASCII through directly
/// and maps special keys via a switch table.
/// @copydoc vg_key_from_vgfx_key
vg_key_t vg_key_from_vgfx_key(int32_t vgfx_key) {
    if (vgfx_key <= 0)
        return VG_KEY_UNKNOWN;

    // Printable ASCII keys are the same
    if (vgfx_key >= ' ' && vgfx_key <= '~') {
        return (vg_key_t)vgfx_key;
    }

    // Special keys need translation (vgfx codes -> vg codes).
    switch (vgfx_key) {
        case VGFX_KEY_ESCAPE:
            return VG_KEY_ESCAPE;
        case VGFX_KEY_ENTER:
            return VG_KEY_ENTER;
        case VGFX_KEY_LEFT:
            return VG_KEY_LEFT;
        case VGFX_KEY_RIGHT:
            return VG_KEY_RIGHT;
        case VGFX_KEY_UP:
            return VG_KEY_UP;
        case VGFX_KEY_DOWN:
            return VG_KEY_DOWN;
        case VGFX_KEY_BACKSPACE:
            return VG_KEY_BACKSPACE;
        case VGFX_KEY_DELETE:
            return VG_KEY_DELETE;
        case VGFX_KEY_TAB:
            return VG_KEY_TAB;
        case VGFX_KEY_HOME:
            return VG_KEY_HOME;
        case VGFX_KEY_END:
            return VG_KEY_END;
        case VGFX_KEY_PAGE_UP:
            return VG_KEY_PAGE_UP;
        case VGFX_KEY_PAGE_DOWN:
            return VG_KEY_PAGE_DOWN;
        default:
            return (vg_key_t)vgfx_key;
    }
}

/// @brief Maps VGFX modifier bit-flags to the corresponding VG_MOD_* mask.
/// @param vgfx_modifiers Bitwise VGFX modifier mask.
/// @return Equivalent bitwise @ref vg_modifiers_t mask.
static uint32_t translate_vgfx_modifiers(int vgfx_modifiers) {
    uint32_t modifiers = VG_MOD_NONE;
    if (vgfx_modifiers & VGFX_MOD_SHIFT)
        modifiers |= VG_MOD_SHIFT;
    if (vgfx_modifiers & VGFX_MOD_CTRL)
        modifiers |= VG_MOD_CTRL;
    if (vgfx_modifiers & VGFX_MOD_ALT)
        modifiers |= VG_MOD_ALT;
    if (vgfx_modifiers & VGFX_MOD_CMD)
        modifiers |= VG_MOD_SUPER;
    return modifiers;
}

/// @brief Copy one bounded ZannaGFX composition payload into a toolkit event.
/// @details Both event layers use inline value storage. The defensive second clamp protects
///          against malformed embedding callers even though platform adapters already enforce the
///          ZannaGFX capacity. Selection/replacement offsets remain in codepoint units until the
///          focused text editor can convert them against its actual strings.
/// @param event Destination toolkit event whose type has already been selected.
/// @param platform_event Source ZannaGFX event carrying composition data.
static void translate_vgfx_composition(vg_event_t *event, const vgfx_event_t *platform_event) {
    if (!event || !platform_event)
        return;

    size_t text_length = platform_event->data.composition.text_length;
    if (text_length >= (size_t)VG_COMPOSITION_TEXT_CAPACITY) {
        text_length = (size_t)VG_COMPOSITION_TEXT_CAPACITY - 1u;
        event->composition.truncated = true;
    }
    memcpy(event->composition.text, platform_event->data.composition.text, text_length);
    event->composition.text[text_length] = '\0';
    event->composition.text_length = (uint32_t)text_length;
    event->composition.selection_start = platform_event->data.composition.selection_start;
    event->composition.selection_length = platform_event->data.composition.selection_length;
    event->composition.replacement_start = platform_event->data.composition.replacement_start;
    event->composition.replacement_length = platform_event->data.composition.replacement_length;
    event->composition.truncated =
        event->composition.truncated || platform_event->data.composition.truncated != 0;
    event->modifiers = translate_vgfx_modifiers(platform_event->data.composition.modifiers);
}

/// @brief Converts a vgfx_event_t platform event to a vg_event_t, mapping keyboard, mouse, scroll,
/// resize, and close events.
/// @copydoc vg_event_from_platform
vg_event_t vg_event_from_platform(void *platform_event) {
    vg_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = VG_EVENT_NONE;

    if (!platform_event)
        return event;

    vgfx_event_t *pe = (vgfx_event_t *)platform_event;
    event.timestamp = pe->time_ms;

    switch (pe->type) {
        case VGFX_EVENT_KEY_DOWN:
            event.type = VG_EVENT_KEY_DOWN;
            event.key.key = vg_key_from_vgfx_key(pe->data.key.key);
            event.key.repeat = pe->data.key.is_repeat != 0;
            event.modifiers = translate_vgfx_modifiers(pe->data.key.modifiers);
            break;

        case VGFX_EVENT_KEY_UP:
            event.type = VG_EVENT_KEY_UP;
            event.key.key = vg_key_from_vgfx_key(pe->data.key.key);
            event.modifiers = translate_vgfx_modifiers(pe->data.key.modifiers);
            break;

        case VGFX_EVENT_TEXT_INPUT:
            event.type = VG_EVENT_KEY_CHAR;
            event.key.key = VG_KEY_UNKNOWN;
            event.key.codepoint = pe->data.text.codepoint;
            event.modifiers = translate_vgfx_modifiers(pe->data.text.modifiers);
            break;

        case VGFX_EVENT_COMPOSITION_START:
            event.type = VG_EVENT_COMPOSITION_START;
            translate_vgfx_composition(&event, pe);
            break;

        case VGFX_EVENT_COMPOSITION_UPDATE:
            event.type = VG_EVENT_COMPOSITION_UPDATE;
            translate_vgfx_composition(&event, pe);
            break;

        case VGFX_EVENT_COMPOSITION_COMMIT:
            event.type = VG_EVENT_COMPOSITION_COMMIT;
            translate_vgfx_composition(&event, pe);
            break;

        case VGFX_EVENT_COMPOSITION_CANCEL:
            event.type = VG_EVENT_COMPOSITION_CANCEL;
            translate_vgfx_composition(&event, pe);
            break;

        case VGFX_EVENT_MOUSE_MOVE:
            event.type = VG_EVENT_MOUSE_MOVE;
            event.mouse.x = (float)pe->data.mouse_move.x;
            event.mouse.y = (float)pe->data.mouse_move.y;
            event.mouse.screen_x = (float)pe->data.mouse_move.x;
            event.mouse.screen_y = (float)pe->data.mouse_move.y;
            event.modifiers = translate_vgfx_modifiers(pe->data.mouse_move.modifiers);
            break;

        case VGFX_EVENT_MOUSE_DOWN:
            event.type = VG_EVENT_MOUSE_DOWN;
            event.mouse.x = (float)pe->data.mouse_button.x;
            event.mouse.y = (float)pe->data.mouse_button.y;
            event.mouse.screen_x = (float)pe->data.mouse_button.x;
            event.mouse.screen_y = (float)pe->data.mouse_button.y;
            event.mouse.button = (vg_mouse_button_t)pe->data.mouse_button.button;
            event.modifiers = translate_vgfx_modifiers(pe->data.mouse_button.modifiers);
            break;

        case VGFX_EVENT_MOUSE_UP:
            event.type = VG_EVENT_MOUSE_UP;
            event.mouse.x = (float)pe->data.mouse_button.x;
            event.mouse.y = (float)pe->data.mouse_button.y;
            event.mouse.screen_x = (float)pe->data.mouse_button.x;
            event.mouse.screen_y = (float)pe->data.mouse_button.y;
            event.mouse.button = (vg_mouse_button_t)pe->data.mouse_button.button;
            event.modifiers = translate_vgfx_modifiers(pe->data.mouse_button.modifiers);
            break;

        case VGFX_EVENT_SCROLL:
            event.type = VG_EVENT_MOUSE_WHEEL;
            event.wheel.delta_x = pe->data.scroll.delta_x;
            // ZannaGFX uses screen-space direction (positive Y is down), while
            // the GUI event contract follows conventional wheel direction
            // (positive Y is up / away from the user). Keep that conversion at
            // the subsystem boundary so every scrollable widget sees the same
            // cross-platform direction. Copying the value made a native
            // scroll-down at the top of a TreeView clamp back to zero.
            event.wheel.delta_y = -pe->data.scroll.delta_y;
            event.wheel.screen_x = (float)pe->data.scroll.x;
            event.wheel.screen_y = (float)pe->data.scroll.y;
            event.modifiers = translate_vgfx_modifiers(pe->data.scroll.modifiers);
            break;

        case VGFX_EVENT_RESIZE:
            event.type = VG_EVENT_RESIZE;
            event.resize.width = pe->data.resize.logical_width > 0 ? pe->data.resize.logical_width
                                                                   : pe->data.resize.width;
            event.resize.height = pe->data.resize.logical_height > 0
                                      ? pe->data.resize.logical_height
                                      : pe->data.resize.height;
            break;

        case VGFX_EVENT_CLOSE:
            event.type = VG_EVENT_CLOSE;
            break;

        case VGFX_EVENT_FOCUS_GAINED:
            event.type = VG_EVENT_FOCUS_IN;
            break;

        case VGFX_EVENT_FOCUS_LOST:
            event.type = VG_EVENT_FOCUS_OUT;
            break;

        default:
            event.type = VG_EVENT_NONE;
            break;
    }

    return event;
}

//=============================================================================
// Event Dispatch
//=============================================================================

/// @brief Routes an event from @p root to its target, applying hit-testing, input capture, modal
/// restriction, hover updates, and keyboard focus routing.
/// @copydoc vg_event_dispatch
bool vg_event_dispatch(vg_widget_t *root, vg_event_t *event) {
    if (!root || !event)
        return false;
    if (event->type == VG_EVENT_NONE)
        return false;
    if (!vg_widget_is_live(root))
        return false;
    const uint64_t root_id = root->id;

    event_activate_root_state(root);
    if (!event_widget_ref_is_live(root, root_id))
        return false;

    // Find target widget for mouse events
    if (event->type == VG_EVENT_MOUSE_MOVE || event->type == VG_EVENT_MOUSE_DOWN ||
        event->type == VG_EVENT_MOUSE_UP || event->type == VG_EVENT_CLICK ||
        event->type == VG_EVENT_DOUBLE_CLICK || event->type == VG_EVENT_TRIPLE_CLICK ||
        event->type == VG_EVENT_MOUSE_WHEEL) {
        // Check if a widget has captured input (e.g., open dropdown menu).
        // When capture is active, all mouse events route to the captured widget
        // regardless of hit testing. This allows dropdown menus to receive clicks
        // even though the dropdown renders outside the menubar's widget bounds.
        uint64_t capture_id = 0;
        vg_widget_t *capture = event_modal_safe_capture(&capture_id);
        if (capture) {
            update_hovered_widget(capture);
            if (!event_widget_ref_is_live(capture, capture_id) ||
                !event_widget_ref_is_live(root, root_id))
                return false;
            event->target = capture;

            // For MOUSE_UP, we need to synthesize a CLICK event because
            // vg_event_send()'s CLICK generation depends on contains_point(),
            // which fails for clicks landing in a popup rendered outside the
            // capturing widget's own bounds. This applies to both the combo-box
            // Dropdown's list popup and the MenuBar's open drop-down menu — both
            // capture input as themselves while painting an out-of-bounds popup.
            if (event->type == VG_EVENT_MOUSE_UP) {
                bool handled = vg_event_send(capture, event);
                if (!event_widget_ref_is_live(capture, capture_id) ||
                    !event_widget_ref_is_live(root, root_id))
                    return handled;
                if ((capture->type == VG_WIDGET_DROPDOWN || capture->type == VG_WIDGET_MENUBAR) &&
                    vg_widget_get_input_capture() == capture &&
                    !vg_widget_contains_point(
                        capture, event_screen_x(event), event_screen_y(event))) {
                    vg_event_t click_event = *event;
                    click_event.type = VG_EVENT_CLICK;
                    handled |= vg_event_send(capture, &click_event);
                }
                if (!event_widget_ref_is_live(root, root_id))
                    return handled;
                if (!event_widget_ref_is_live(capture, capture_id) ||
                    vg_widget_get_input_capture() != capture) {
                    vg_widget_t *modal = vg_widget_get_modal_root();
                    vg_widget_t *hit_root = (modal && modal->visible) ? modal : root;
                    vg_widget_t *target =
                        vg_widget_hit_test(hit_root, event_screen_x(event), event_screen_y(event));
                    update_hovered_widget(target);
                }
                return handled;
            }

            bool handled = vg_event_send(capture, event);
            if (!event_widget_ref_is_live(root, root_id))
                return handled;
            if (!event_widget_ref_is_live(capture, capture_id) ||
                vg_widget_get_input_capture() != capture) {
                vg_widget_t *modal = vg_widget_get_modal_root();
                vg_widget_t *hit_root = (modal && modal->visible) ? modal : root;
                vg_widget_t *target =
                    vg_widget_hit_test(hit_root, event_screen_x(event), event_screen_y(event));
                update_hovered_widget(target);
            }
            return handled;
        }

        // When a modal root is active, restrict hit-testing to its subtree so
        // that clicks on widgets behind the dialog are swallowed.
        vg_widget_t *modal = vg_widget_get_modal_root();
        const uint64_t modal_id = modal ? modal->id : 0;
        vg_widget_t *hit_root = (modal && modal->visible) ? modal : root;

        vg_widget_t *target =
            vg_widget_hit_test(hit_root, event_screen_x(event), event_screen_y(event));
        const uint64_t target_id = target ? target->id : 0;
        update_hovered_widget(target);
        if (!event_widget_ref_is_live(target, target_id))
            target = NULL;
        if (!event_widget_ref_is_live(root, root_id))
            return false;
        if (!event_widget_ref_is_live(modal, modal_id))
            modal = NULL;
        if (!target && modal && modal->visible) {
            // Click landed outside the modal dialog: swallow the event silently.
            return true;
        }
        if (target) {
            event->target = target;
            // Don't localize here — vg_event_send is the single owner of the
            // screen→widget-relative transform. Calling event_localize_mouse_to_widget
            // both here and inside vg_event_send caused mouse coordinates to be
            // offset by (widget_screen_x, widget_screen_y) twice, breaking
            // bounds-check-based mouse handlers like vg_codeeditor's wheel scroll.
            return vg_event_send(target, event);
        }
        return false;
    }

    // Keyboard events: route to captured widget first (for menu keyboard navigation),
    // then to focused widget, then to root.
    if (event->type == VG_EVENT_KEY_DOWN || event->type == VG_EVENT_KEY_UP ||
        event->type == VG_EVENT_KEY_CHAR || event->type == VG_EVENT_COMPOSITION_START ||
        event->type == VG_EVENT_COMPOSITION_UPDATE || event->type == VG_EVENT_COMPOSITION_COMMIT ||
        event->type == VG_EVENT_COMPOSITION_CANCEL) {
        uint64_t capture_id = 0;
        vg_widget_t *capture = event_modal_safe_capture(&capture_id);
        if (capture) {
            event->target = capture;
            if (vg_event_send(capture, event))
                return true;
            if (!event_widget_ref_is_live(root, root_id))
                return false;
            if (!event_widget_ref_is_live(capture, capture_id))
                event->target = NULL;
            // If captured widget didn't handle it, fall through to focused widget
        }

        vg_widget_t *focused = vg_widget_get_focused(root);
        uint64_t focused_id = focused ? focused->id : 0;

        // When a modal is active, redirect keyboard events to the modal if the
        // focused widget is outside the modal's subtree.
        vg_widget_t *modal_kb = vg_widget_get_modal_root();
        uint64_t modal_kb_id = modal_kb ? modal_kb->id : 0;
        if (modal_kb && modal_kb->visible) {
            if (!focused) {
                focused = modal_kb;
                focused_id = modal_kb_id;
            } else {
                bool inside = false;
                for (vg_widget_t *w = focused; w; w = w->parent) {
                    if (w == modal_kb) {
                        inside = true;
                        break;
                    }
                }
                if (!inside) {
                    focused = modal_kb;
                    focused_id = modal_kb_id;
                }
            }
        }

        vg_widget_t *focus_root = (modal_kb && modal_kb->visible) ? modal_kb : root;
        const uint64_t focus_root_id = focus_root == modal_kb ? modal_kb_id : root_id;

        if (event->type == VG_EVENT_KEY_DOWN && event->key.key == VG_KEY_TAB &&
            (event->modifiers & (VG_MOD_CTRL | VG_MOD_ALT | VG_MOD_SUPER)) == 0) {
            if (event_widget_ref_is_live(focused, focused_id)) {
                event->target = focused;
                if (vg_event_send(focused, event))
                    return true;
            }

            if (!event_widget_ref_is_live(focus_root, focus_root_id))
                return true;
            if (event->modifiers & VG_MOD_SHIFT)
                vg_widget_focus_prev(focus_root);
            else
                vg_widget_focus_next(focus_root);
            return true;
        }

        if (event_widget_ref_is_live(focused, focused_id)) {
            event->target = focused;
            return vg_event_send(focused, event);
        }

        // If no focused widget, try to dispatch to root
        event->target = root;
        return vg_event_send(root, event);
    }

    // Other events go directly to root
    event->target = root;
    return vg_event_send(root, event);
}

/// @brief Delivers an event to @p widget, bubbles it up the parent chain, and synthesizes
/// CLICK/DOUBLE_CLICK/TRIPLE_CLICK events on MOUSE_UP.
/// @copydoc vg_event_send
bool vg_event_send(vg_widget_t *widget, vg_event_t *event) {
    if (!widget || !event)
        return false;
    if (event->type == VG_EVENT_NONE)
        return false;
    if (!vg_widget_is_live(widget))
        return false;

    const uint64_t widget_id = widget->id;
    const bool restore_mouse = event_has_widget_local_mouse_coords(event->type);
    const float target_mouse_x = event->mouse.x;
    const float target_mouse_y = event->mouse.y;
    bool handled = event->handled;
    bool synthesize_click = false;
    bool synthesize_double_click = false;
    bool synthesize_triple_click = false;
    int synthesized_click_count = 1;
    vg_event_t click_source = {0};

    event_localize_mouse_to_widget(widget, event);
    if (event->type == VG_EVENT_CLICK)
        vg_widget_note_click(widget, event->timestamp);

    // Handle common state changes for mouse events
    if (event->type == VG_EVENT_MOUSE_ENTER) {
        widget->state |= VG_STATE_HOVERED;
        widget->needs_paint = true;
    } else if (event->type == VG_EVENT_MOUSE_LEAVE) {
        widget->state &= ~(VG_STATE_HOVERED | VG_STATE_PRESSED);
        widget->needs_paint = true;
    } else if (event->type == VG_EVENT_MOUSE_DOWN) {
        widget->state |= VG_STATE_PRESSED;
        widget->needs_paint = true;
        // Set focus on click if widget can accept focus
        if (widget->vtable && widget->vtable->can_focus)
            vg_widget_set_focus(widget);
    } else if (event->type == VG_EVENT_MOUSE_UP) {
        bool was_pressed = (widget->state & VG_STATE_PRESSED) != 0;
        widget->state &= ~VG_STATE_PRESSED;
        widget->needs_paint = true;

        if (was_pressed &&
            vg_widget_contains_point(widget, event->mouse.screen_x, event->mouse.screen_y)) {
            synthesize_click = true;
            synthesized_click_count = click_count_for_widget(widget, event);
            synthesize_double_click = synthesized_click_count == 2;
            synthesize_triple_click = synthesized_click_count == 3;
            click_source = *event;
        } else if (was_pressed) {
            clear_last_click_state();
        }
    }

    if (!event_widget_ref_is_live(widget, widget_id)) {
        if (restore_mouse) {
            event->mouse.x = target_mouse_x;
            event->mouse.y = target_mouse_y;
        }
        return event->handled || handled;
    }

    // Call widget's event handler
    if (widget->vtable && widget->vtable->handle_event) {
        if (widget->vtable->handle_event(widget, event)) {
            event->handled = true;
            handled = true;
        }
    }
    vg_widget_t *bubble_parent = NULL;
    if (!event_widget_ref_is_live(widget, widget_id)) {
        synthesize_click = false;
    } else {
        bubble_parent = widget->parent;
    }

    // Bubble up the parent chain iteratively (avoids stack overflow on deep trees)
    vg_widget_t *current = bubble_parent;
    while (!event->handled && current) {
        if (!vg_widget_is_live(current))
            break;
        const uint64_t current_id = current->id;
        event_localize_mouse_to_widget(current, event);
        if (current->vtable && current->vtable->handle_event) {
            if (current->vtable->handle_event(current, event)) {
                event->handled = true;
                handled = true;
                break;
            }
        }
        if (!event_widget_ref_is_live(current, current_id))
            break;
        vg_widget_t *next = current->parent;
        current = vg_widget_is_live(next) ? next : NULL;
    }

    if (event->handled)
        synthesize_click = false;

    if (synthesize_click && event_widget_ref_is_live(widget, widget_id)) {
        vg_event_t click_event = click_source;
        click_event.type = VG_EVENT_CLICK;
        click_event.handled = false;
        click_event.mouse.click_count = synthesized_click_count;
        handled |= vg_event_send(widget, &click_event);

        if (event_widget_ref_is_live(widget, widget_id) && synthesize_double_click) {
            vg_event_t double_click_event = click_source;
            double_click_event.type = VG_EVENT_DOUBLE_CLICK;
            double_click_event.handled = false;
            double_click_event.mouse.click_count = 2;
            handled |= vg_event_send(widget, &double_click_event);
            remember_click(widget, widget_id, &click_source, 2);
        } else if (event_widget_ref_is_live(widget, widget_id) && synthesize_triple_click) {
            vg_event_t triple_click_event = click_source;
            triple_click_event.type = VG_EVENT_TRIPLE_CLICK;
            triple_click_event.handled = false;
            triple_click_event.mouse.click_count = 3;
            handled |= vg_event_send(widget, &triple_click_event);
            if (event_widget_ref_is_live(widget, widget_id))
                clear_last_click_state();
        } else if (event_widget_ref_is_live(widget, widget_id)) {
            remember_click(widget, widget_id, &click_source, 1);
        }
    }

    if (handled)
        event->handled = true;
    if (restore_mouse) {
        event->mouse.x = target_mouse_x;
        event->mouse.y = target_mouse_y;
    }

    return event->handled || handled;
}
