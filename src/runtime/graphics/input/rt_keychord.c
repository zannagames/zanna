//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/input/rt_keychord.c
// Purpose: Key chord and sequential combo detection for the Zanna input system.
//   Chords require all specified keys held simultaneously; they trigger on the
//   frame the last key is pressed (edge detection). Combos require keys pressed
//   in order within a configurable frame window; they trigger when the final key
//   in the sequence is pressed within the window. Both types are named and
//   stored in a growable array within a GC-managed KeyChord object.
//
// Key invariants:
//   - Chord trigger fires on exactly one frame (the press frame of the last key);
//     it is cleared on the next rt_keychord_update() call.
//   - Combo progress resets if any key in the sequence is pressed out of order
//     or if the inter-key gap exceeds the configured window_frames.
//   - Entry names must be unique within a KeyChord instance; duplicate AddChord/
//     AddCombo calls with the same name replace the previous entry.
//   - KC_MAX_KEYS (16) is the maximum number of keys in a single chord or combo.
//   - rt_keychord_update() increments the internal frame counter; it must be
//     called once per frame before querying trigger state.
//
// Ownership/Lifetime:
//   - rt_keychord_impl is allocated via rt_obj_new_i64 (GC heap); the entries
//     array and per-entry name strings are malloc'd and freed in kc_finalizer,
//     which is registered as the GC finalizer at creation.
//
// Links: src/runtime/graphics/input/rt_keychord.h (public API),
//        src/runtime/graphics/input/rt_input.h (keyboard state queries),
//        src/runtime/graphics/2d/rt_action.c (action mapping uses BIND_CHORD)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements named keyboard chord and timed combo detection.
 *
 * @details GC-managed detectors store copied names and bounded key arrays,
 *          atomically replace definitions after fallible allocation, evaluate
 *          simultaneous held-key transitions, advance ordered sequences with
 *          timeout and wrong-order reset rules, and retain one-frame results.
 */

#include "rt_keychord.h"

#include "rt_input.h"
#include "rt_internal.h"
#include "rt_object.h"

/// @copydoc rt_unbox_i64()
extern int64_t rt_unbox_i64(void *box);
#include "rt_seq.h"
#include "rt_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Maximum number of public key codes in one chord or combo.
#define KC_MAX_KEYS 16
/// @brief Initial number of named-pattern slots allocated per detector.
#define KC_INITIAL_CAPACITY 8

/// @brief Discriminates simultaneous chords from ordered timed combos.
typedef enum { KC_TYPE_CHORD = 0, KC_TYPE_COMBO = 1 } kc_type;

/// @brief Owned definition and mutable detection state for one named pattern.
typedef struct {
    /// @brief Owned NUL-terminated unique pattern name.
    char *name;
    /// @brief Simultaneous or sequential pattern kind.
    kc_type type;
    /// @brief Ordered public key codes used by the pattern.
    int64_t keys[KC_MAX_KEYS];
    /// @brief Number of initialized entries in @ref keys.
    int64_t key_count;
    /// @brief Maximum inter-key gap for combos, in update frames.
    int64_t window_frames;
    /* Chord state */
    /// @brief Whether a chord was active during the preceding update.
    int8_t was_active;
    /// @brief Current chord-held or combo-completion-frame state.
    int8_t is_active;
    /// @brief One-frame activation/completion edge.
    int8_t triggered;
    /* Combo state */
    /// @brief Index of the next expected combo key.
    int64_t combo_index;
    /// @brief Detector frame on which the latest combo key matched.
    uint64_t last_match_frame;
} kc_entry;

/// @brief Private growable pattern table for one managed detector.
typedef struct {
    /// @brief Reserved runtime virtual-table slot.
    void *vptr;
    /// @brief Owned pattern allocation.
    kc_entry *entries;
    /// @brief Number of initialized pattern entries.
    int64_t count;
    /// @brief Allocated entry capacity.
    int64_t capacity;
    /// @brief Monotonic update counter used for combo timeouts.
    uint64_t frame_counter;
} rt_keychord_impl;

/// @brief Linear search for a keychord entry by name.
/// @details Used for both deduplication on add and for the public query API.
///   Entry counts are typically small (single digits to low tens) so linear
///   search is appropriate; the keychord object is not designed for hundreds
///   of named bindings.  Returns NULL when no match is found.
/// @param kc Live KeyChord instance to search.
/// @param name NUL-terminated entry name.
/// @return Borrowed matching entry, or NULL when absent.
static kc_entry *find_entry(rt_keychord_impl *kc, const char *name) {
    int64_t i;
    for (i = 0; i < kc->count; i++) {
        if (strcmp(kc->entries[i].name, name) == 0)
            return &kc->entries[i];
    }
    return NULL;
}

/// @brief GC finalizer — free all entry names and the entries array.
/// @details Called by the GC when the KeyChord object's reference count drops
///   to zero.  Each entry owns a malloc'd name string, so those are freed first
///   in order before the entries array itself is freed.  Pointers and count are
///   cleared to prevent double-free if the finalizer is somehow called twice.
/// @param obj Runtime-managed KeyChord object supplied by the object finalizer.
static void kc_finalizer(void *obj) {
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    if (kc) {
        int64_t i;
        for (i = 0; i < kc->count; i++) {
            free(kc->entries[i].name);
        }
        free(kc->entries);
        kc->entries = NULL;
        kc->count = 0;
    }
}

/// @brief Grow the entries array by 2x if it is full.
/// @details Called before every insertion so callers do not need to check.
///   Uses realloc for in-place growth where possible and guards both capacity
///   multiplication and byte-size conversion.
/// @param kc KeyChord instance whose entry storage may grow.
/// @return 1 when capacity is available, 0 on allocation overflow/failure.
static int ensure_capacity(rt_keychord_impl *kc) {
    if (!kc)
        return 0;
    if (kc->count < kc->capacity)
        return 1;
    if (kc->capacity <= 0 || kc->capacity > INT64_MAX / 2 ||
        (uint64_t)(kc->capacity * 2) > (uint64_t)SIZE_MAX / sizeof(kc_entry)) {
        rt_trap("KeyChord: too many entries");
        return 0;
    }
    int64_t new_cap = kc->capacity * 2;
    kc_entry *new_entries = (kc_entry *)realloc(kc->entries, (size_t)new_cap * sizeof(kc_entry));
    if (!new_entries) {
        rt_trap("KeyChord: memory allocation failed");
        return 0;
    }
    kc->entries = new_entries;
    kc->capacity = new_cap;
    return 1;
}

/// @brief Advance the KeyChord frame counter without signed overflow.
/// @details The counter is monotonic for practical runtimes. If it reaches the uint64 limit, active
///          combo progress is reset and the counter restarts from one so timeout arithmetic remains
///          well-defined.
/// @param kc KeyChord instance to advance.
static void keychord_advance_frame(rt_keychord_impl *kc) {
    if (!kc)
        return;
    if (kc->frame_counter == UINT64_MAX) {
        kc->frame_counter = 1;
        for (int64_t i = 0; i < kc->count; i++) {
            kc->entries[i].combo_index = 0;
            kc->entries[i].last_match_frame = 0;
        }
        return;
    }
    kc->frame_counter++;
}

/// @brief Add or replace a chord/combo entry in the keychord manager.
/// @details If an entry with the same name already exists it is removed first
///   (shift-delete) so the new definition replaces it.  Key codes are extracted
///   from the Zia Seq[Integer] object via rt_seq_get / rt_unbox_i64 and stored
///   in a fixed-size array capped at KC_MAX_KEYS (16).  The entry name is heap-
///   copied so the caller's string does not need to outlive the call. Invalid
///   key counts or allocation failures trap and leave the previous definition
///   unchanged unless replacement already succeeded.
/// @param kc Live KeyChord instance to mutate.
/// @param name NUL-terminated unique entry name.
/// @param type Chord or sequential-combo discriminator.
/// @param keys Runtime sequence of boxed public key codes.
/// @param window_frames Maximum inter-key gap for combos; ignored for chords.
static void add_entry(
    rt_keychord_impl *kc, const char *name, kc_type type, void *keys, int64_t window_frames) {
    int64_t key_count = rt_seq_len(keys);
    if (key_count <= 0 || key_count > KC_MAX_KEYS) {
        rt_trap("KeyChord: key count must be between 1 and 16");
        return;
    }

    char *name_copy = NULL;
    size_t name_len = strlen(name);
    name_copy = (char *)malloc(name_len + 1);
    if (!name_copy) {
        rt_trap("KeyChord: name allocation failed");
        return;
    }
    memcpy(name_copy, name, name_len + 1);

    /* Grow (and the name_copy malloc above) BEFORE removing any existing entry, so that every
     * fallible allocation completes before the table is mutated — an OOM here leaves the chord
     * list unchanged rather than having already dropped the old entry. When replacing an entry
     * while the table is full this grows by one slot the removal below then frees; that small
     * waste is the deliberate price of allocate-before-mutate atomicity and must NOT be
     * "optimised away" by moving the removal before ensure_capacity. */
    if (!ensure_capacity(kc)) {
        free(name_copy);
        return;
    }

    /* Remove existing entry with same name after all failure-prone allocation succeeds. */
    kc_entry *existing = find_entry(kc, name);
    if (existing) {
        free(existing->name);
        /* Shift entries to remove gap */
        int64_t idx = (int64_t)(existing - kc->entries);
        int64_t i;
        for (i = idx; i < kc->count - 1; i++) {
            kc->entries[i] = kc->entries[i + 1];
        }
        kc->count--;
        memset(&kc->entries[kc->count], 0, sizeof(kc->entries[kc->count]));
    }

    kc_entry *e = &kc->entries[kc->count];
    memset(e, 0, sizeof(kc_entry));
    e->name = name_copy;

    e->type = type;
    e->key_count = key_count;
    e->window_frames = window_frames;

    {
        int64_t i;
        for (i = 0; i < key_count; i++) {
            void *item = rt_seq_get(keys, i);
            e->keys[i] = rt_unbox_i64(item);
        }
    }

    kc->count++;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Create a new key chord manager for detecting multi-key combinations and combos.
/// @return New GC-managed KeyChord handle, or NULL after a recoverable allocation trap.
void *rt_keychord_new(void) {
    rt_keychord_impl *kc = (rt_keychord_impl *)rt_obj_new_i64(0, (int64_t)sizeof(rt_keychord_impl));
    if (!kc) {
        rt_trap("KeyChord: memory allocation failed");
        return NULL;
    }
    kc->vptr = NULL;
    kc->count = 0;
    kc->capacity = KC_INITIAL_CAPACITY;
    kc->frame_counter = 0;
    kc->entries = (kc_entry *)calloc((size_t)KC_INITIAL_CAPACITY, sizeof(kc_entry));
    if (!kc->entries) {
        rt_trap("KeyChord: memory allocation failed");
        if (rt_obj_release_check0(kc))
            rt_obj_free(kc);
        return NULL;
    }
    rt_obj_set_finalizer(kc, kc_finalizer);
    return kc;
}

/// @brief Define a chord — all keys must be held simultaneously to activate.
/// @details An existing entry with the same name is replaced atomically after fallible allocation.
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @param keys Sequence of one to 16 boxed public key codes.
void rt_keychord_define(void *obj, rt_string name, void *keys) {
    if (!obj || !keys)
        return;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return;
    add_entry(kc, cstr, KC_TYPE_CHORD, keys, 0);
}

/// @brief Define a combo — keys must be pressed in sequence within the time window.
/// @details Non-positive windows normalize to 15 frames. Existing names are replaced.
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @param keys Ordered sequence of one to 16 boxed public key codes.
/// @param window_frames Maximum frames permitted between successive matches.
void rt_keychord_define_combo(void *obj, rt_string name, void *keys, int64_t window_frames) {
    if (!obj || !keys)
        return;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return;
    if (window_frames <= 0)
        window_frames = 15; /* default ~250ms at 60fps */
    add_entry(kc, cstr, KC_TYPE_COMBO, keys, window_frames);
}

/// @brief Update the keychord state (called per frame/tick).
/// @details Clears prior trigger edges, evaluates simultaneous chords, advances sequential combos,
///          and resets combo progress on timeout or an out-of-order member-key press.
/// @param obj KeyChord handle; NULL is ignored.
void rt_keychord_update(void *obj) {
    if (!obj)
        return;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    int64_t i;

    keychord_advance_frame(kc);

    for (i = 0; i < kc->count; i++) {
        kc_entry *e = &kc->entries[i];
        e->triggered = 0;

        if (e->type == KC_TYPE_CHORD) {
            /* Check if all chord keys are currently held */
            int8_t all_down = 1;
            int8_t any_just_pressed = 0;
            int64_t k;
            for (k = 0; k < e->key_count; k++) {
                if (!rt_keyboard_is_down(e->keys[k])) {
                    all_down = 0;
                    break;
                }
                if (rt_keyboard_was_pressed(e->keys[k]))
                    any_just_pressed = 1;
            }

            e->was_active = e->is_active;
            e->is_active = all_down;

            /* Trigger on the frame the chord becomes active */
            if (all_down && !e->was_active && any_just_pressed)
                e->triggered = 1;
        } else /* KC_TYPE_COMBO */
        {
            int8_t wrong_order_pressed = 0;
            e->was_active = e->is_active;
            e->is_active = 0;

            /* Check for timeout */
            if (e->combo_index > 0) {
                uint64_t elapsed = kc->frame_counter >= e->last_match_frame
                                       ? kc->frame_counter - e->last_match_frame
                                       : UINT64_MAX;
                if (elapsed > (uint64_t)e->window_frames)
                    e->combo_index = 0;
            }

            if (e->combo_index < e->key_count) {
                int64_t expected_key = e->keys[e->combo_index];
                int64_t k;
                for (k = 0; k < e->key_count; k++) {
                    int64_t key = e->keys[k];
                    if (key != expected_key && rt_keyboard_was_pressed(key)) {
                        e->combo_index = 0;
                        wrong_order_pressed = 1;
                        break;
                    }
                }
            }

            /* Check if the next expected key was pressed this frame */
            if (!wrong_order_pressed && e->combo_index < e->key_count) {
                int64_t expected_key = e->keys[e->combo_index];
                if (rt_keyboard_was_pressed(expected_key)) {
                    e->combo_index++;
                    e->last_match_frame = kc->frame_counter;

                    if (e->combo_index >= e->key_count) {
                        e->triggered = 1;
                        e->is_active = 1;
                        e->combo_index = 0;
                    }
                }
            }
        }
    }
}

/// @brief Check whether a named chord/combo is currently active (all keys held or combo complete).
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @return One while a chord is held or during a combo's completion frame, otherwise zero.
int8_t rt_keychord_active(void *obj, rt_string name) {
    if (!obj)
        return 0;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    kc_entry *e = find_entry(kc, cstr);
    if (!e)
        return 0;
    return e->is_active;
}

/// @brief Check whether a chord/combo was triggered this frame (edge-triggered).
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @return One when the named entry triggered during the latest update, otherwise zero.
int8_t rt_keychord_triggered(void *obj, rt_string name) {
    if (!obj)
        return 0;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    kc_entry *e = find_entry(kc, cstr);
    if (!e)
        return 0;
    return e->triggered;
}

/// @brief Get the combo progress (0 = not started, count = steps completed so far).
/// @details Active chords report their complete key count; inactive chords report zero.
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @return Matched step count, or zero for missing/invalid input.
int64_t rt_keychord_progress(void *obj, rt_string name) {
    if (!obj)
        return 0;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    kc_entry *e = find_entry(kc, cstr);
    if (!e)
        return 0;
    if (e->type == KC_TYPE_CHORD)
        return e->is_active ? e->key_count : 0;
    return e->combo_index;
}

/// @brief Remove an entry from the keychord.
/// @param obj KeyChord handle.
/// @param name Runtime entry name.
/// @return One when a matching entry was removed, otherwise zero.
int8_t rt_keychord_remove(void *obj, rt_string name) {
    if (!obj)
        return 0;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    const char *cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    kc_entry *e = find_entry(kc, cstr);
    if (!e)
        return 0;

    free(e->name);
    int64_t idx = (int64_t)(e - kc->entries);
    int64_t j;
    for (j = idx; j < kc->count - 1; j++) {
        kc->entries[j] = kc->entries[j + 1];
    }
    kc->count--;
    return 1;
}

/// @brief Remove all entries from the keychord.
/// @param obj KeyChord handle; NULL is ignored.
void rt_keychord_clear(void *obj) {
    if (!obj)
        return;
    rt_keychord_impl *kc = (rt_keychord_impl *)obj;
    int64_t i;
    for (i = 0; i < kc->count; i++) {
        free(kc->entries[i].name);
    }
    kc->count = 0;
}

/// @brief Return the count of elements in the keychord.
/// @param obj KeyChord handle.
/// @return Current named-entry count, or zero for NULL.
int64_t rt_keychord_count(void *obj) {
    if (!obj)
        return 0;
    return ((rt_keychord_impl *)obj)->count;
}
