//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_dialogue.c
// Purpose: Zanna.Game3D.Dialogue3D — 3D conversation surface: a typewriter
//   line queue drawn over the world overlay (bottom panel or speaker-anchored
//   bubble via Camera3D.WorldToScreen), localization-keyed text through a
//   bound MessageBundle, optional per-line voice clips, and blocking choice
//   prompts with polled results.
// Key invariants:
//   - One shown conversation per world (show() installs; hide()/end releases).
//   - Key resolution rule: a bound bundle that has the string as a key
//     substitutes the localized text; otherwise the literal is used.
//   - Two-stage skip convention: the first skip completes the reveal, the
//     next advance() moves to the following line.
//   - Choice prompts block line advance until confirmed; results are polled
//     (choiceMade one-shot + lastChoice), never callbacks.
//   - Localized lookup temporaries and queued voice clips have balanced
//     ownership; empty and choice-only conversations cannot wedge playback.
// Ownership/Lifetime:
//   - GC-managed; finalizer releases world/bundle/entity/clip references. The
//     world retains the shown dialogue until hidden or finished.
// Links: rt_game3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements localized, typewriter-driven dialogue overlays for Game3D.
/// @details Dialogue3D stores a bounded line queue, optional voice clips, and
///          one blocking choice prompt. A shown dialogue occupies its world's
///          single active slot; the world tick advances reveal/hold state and
///          the overlay hook renders either a bottom panel or a projected
///          speaker bubble. Text is resolved when queued, so subsequent locale
///          changes affect only newly added lines and choices.

#include "rt_audio.h"
#include "rt_canvas3d.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_message_bundle.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <string.h>

#define GAME3D_DLG_DEFAULT_REVEAL_SPEED 40.0
#define GAME3D_DLG_AUTO_HOLD_SECONDS 1.2

/// @brief Clamp the dialogue line count to its fixed backing-array extent.
/// @param dialogue Dialogue whose private count is inspected.
/// @return Safe line count in `[0, RT_GAME3D_DLG_MAX_LINES]`.
static int32_t game3d_dialogue_line_count_safe(const rt_game3d_dialogue *dialogue) {
    if (!dialogue || dialogue->line_count <= 0)
        return 0;
    return dialogue->line_count > RT_GAME3D_DLG_MAX_LINES ? RT_GAME3D_DLG_MAX_LINES
                                                          : dialogue->line_count;
}

/// @brief Clamp the choice count to its fixed backing-array extent.
/// @param dialogue Dialogue whose private count is inspected.
/// @return Safe choice count in `[0, RT_GAME3D_DLG_MAX_CHOICES]`.
static int32_t game3d_dialogue_choice_count_safe(const rt_game3d_dialogue *dialogue) {
    if (!dialogue || dialogue->choice_count <= 0)
        return 0;
    return dialogue->choice_count > RT_GAME3D_DLG_MAX_CHOICES ? RT_GAME3D_DLG_MAX_CHOICES
                                                              : dialogue->choice_count;
}

/// @brief Return the retained speaker only while it remains a live Entity3D.
/// @details A destroyed speaker is released in place so a long-lived dialogue
///          cannot retain a dead entity indefinitely.
/// @param dialogue Dialogue whose optional anchor is inspected.
/// @return Borrowed live speaker entity, or `NULL` after clearing stale state.
static rt_game3d_entity *game3d_dialogue_speaker_ref(rt_game3d_dialogue *dialogue) {
    if (!dialogue || !dialogue->speaker_entity)
        return NULL;
    rt_game3d_entity *speaker = (rt_game3d_entity *)rt_g3d_checked_or_null(
        dialogue->speaker_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    if (game3d_entity_alive_or_record(speaker))
        return speaker;
    game3d_release_ref(&dialogue->speaker_entity);
    return NULL;
}

//=========================================================================
// Lifecycle
//=========================================================================

/// @brief GC finalizer: release retained references (clips per line included).
/// @param obj Dialogue3D storage being finalized; NULL is ignored.
static void game3d_dialogue_finalize(void *obj) {
    rt_game3d_dialogue *dialogue = (rt_game3d_dialogue *)obj;
    if (!dialogue)
        return;
    for (int32_t i = 0; i < RT_GAME3D_DLG_MAX_LINES; ++i)
        game3d_release_ref(&dialogue->lines[i].voice_clip);
    game3d_release_ref(&dialogue->world);
    game3d_release_ref(&dialogue->bundle);
    game3d_release_ref(&dialogue->speaker_entity);
}

/// @brief Create a conversation bound to @p world (shown via show()).
/// @param world_obj World3D that owns the active-dialogue slot, audio engine,
///                  camera, and overlay canvas used during playback.
/// @return A newly allocated Dialogue3D retaining @p world_obj, or NULL after
///         validation or allocation failure.
void *rt_game3d_dialogue_new(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.Dialogue3D.New: invalid world");
    if (!world)
        return NULL;
    rt_game3d_dialogue *dialogue = (rt_game3d_dialogue *)rt_obj_new_i64(
        RT_G3D_GAME3D_DIALOGUE_CLASS_ID, (int64_t)sizeof(*dialogue));
    if (!dialogue) {
        rt_trap("Game3D.Dialogue3D.New: allocation failed");
        return NULL;
    }
    memset(dialogue, 0, sizeof(*dialogue));
    rt_obj_set_finalizer(dialogue, game3d_dialogue_finalize);
    game3d_assign_ref(&dialogue->world, world);
    dialogue->reveal_speed = GAME3D_DLG_DEFAULT_REVEAL_SPEED;
    dialogue->panel_alpha = 0.65;
    dialogue->name_color = 0xFFD75A;
    dialogue->last_choice = -1;
    return dialogue;
}

/// @brief Resolve text through the bound bundle (key hit) or keep the literal.
/// @param dialogue Dialogue whose optional MessageBundle supplies translations.
/// @param text Localization key or literal runtime string; NULL resolves empty.
/// @param[out] dst Destination byte buffer.
/// @param dst_size Capacity of @p dst including the terminator; must be positive.
static void game3d_dialogue_resolve_text(rt_game3d_dialogue *dialogue,
                                         rt_string text,
                                         char *dst,
                                         size_t dst_size) {
    dst[0] = '\0';
    if (!text)
        return;
    rt_string resolved = text;
    int owns_resolved = 0;
    if (dialogue->bundle && rt_message_bundle_has(dialogue->bundle, text)) {
        resolved = rt_message_bundle_get(dialogue->bundle, text);
        owns_resolved = 1;
    }
    const char *cstr = resolved ? rt_string_cstr(resolved) : NULL;
    if (cstr)
        (void)game3d_utf8_copy_bounded(dst, dst_size, cstr);
    if (owns_resolved && resolved)
        rt_string_unref(resolved);
}

//=========================================================================
// Line queue
//=========================================================================

/// @brief Shared line-append body.
/// @param obj Dialogue3D runtime handle.
/// @param speaker Optional speaker label copied into the bounded line record.
/// @param text Localization key or literal text resolved and copied immediately.
/// @param voice_clip Optional clip retained until the dialogue is finalized.
/// @param api_name Validation message identifying the public caller.
/// @return @p obj for fluent chaining, including validation and capacity failures.
static void *game3d_dialogue_say_impl(
    void *obj, rt_string speaker, rt_string text, void *voice_clip, const char *api_name) {
    rt_game3d_dialogue *dialogue = game3d_dialogue_checked(obj, api_name);
    if (!dialogue)
        return obj;
    if (voice_clip && !rt_sound_is_handle(voice_clip)) {
        rt_trap("Game3D.Dialogue3D.sayVoiced: clip must be Sound or null");
        return obj;
    }
    dialogue->line_count = game3d_dialogue_line_count_safe(dialogue);
    if (dialogue->line_count >= RT_GAME3D_DLG_MAX_LINES) {
        rt_trap("Game3D.Dialogue3D.say: line queue limit reached (32)");
        return obj;
    }
    rt_game3d_dlg_line *line = &dialogue->lines[dialogue->line_count];
    game3d_release_ref(&line->voice_clip);
    memset(line, 0, sizeof(*line));
    const char *speaker_cstr = speaker ? rt_string_cstr(speaker) : NULL;
    if (speaker_cstr)
        (void)game3d_utf8_copy_bounded(line->speaker, RT_GAME3D_DLG_NAME_MAX, speaker_cstr);
    game3d_dialogue_resolve_text(dialogue, text, line->text, RT_GAME3D_TL_TEXT_MAX);
    game3d_assign_ref(&line->voice_clip, voice_clip);
    dialogue->line_count += 1;
    return obj;
}

/// @brief Fluent: queue a line (text may be a localization key).
/// @param obj Dialogue3D runtime handle.
/// @param speaker Optional speaker label.
/// @param text Localization key or literal line text.
/// @return @p obj for fluent chaining.
void *rt_game3d_dialogue_say(void *obj, rt_string speaker, rt_string text) {
    return game3d_dialogue_say_impl(
        obj, speaker, text, NULL, "Game3D.Dialogue3D.say: invalid dialogue");
}

/// @brief Fluent: queue a voiced line (clip plays when the line starts).
/// @param obj Dialogue3D runtime handle.
/// @param speaker Optional speaker label.
/// @param text Localization key or literal line text.
/// @param clip Optional audio clip retained by the queued line.
/// @return @p obj for fluent chaining.
void *rt_game3d_dialogue_say_voiced(void *obj, rt_string speaker, rt_string text, void *clip) {
    return game3d_dialogue_say_impl(
        obj, speaker, text, clip, "Game3D.Dialogue3D.sayVoiced: invalid dialogue");
}

/// @brief Fluent: queue a blocking choice prompt after the current lines.
/// @param obj Dialogue3D runtime handle.
/// @param options_seq Sequence containing one through eight runtime strings.
///                    Each option is localized and copied immediately.
/// @return @p obj for fluent chaining; invalid option counts trap without
///         replacing the prior prompt.
void *rt_game3d_dialogue_ask_choice(void *obj, void *options_seq) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.askChoice: invalid dialogue");
    if (!dialogue)
        return obj;
    int64_t count = options_seq ? rt_seq_len(options_seq) : 0;
    if (count <= 0 || count > RT_GAME3D_DLG_MAX_CHOICES) {
        rt_trap("Game3D.Dialogue3D.askChoice: choices must hold 1..8 options");
        return obj;
    }
    dialogue->choice_count = (int32_t)count;
    for (int32_t i = 0; i < dialogue->choice_count; ++i) {
        rt_string option = rt_seq_get_str(options_seq, i);
        game3d_dialogue_resolve_text(dialogue, option, dialogue->choices[i], RT_GAME3D_TL_TEXT_MAX);
    }
    dialogue->choice_selected = 0;
    dialogue->choice_active = 0; /* armed when the queue reaches its end */
    return obj;
}

//=========================================================================
// Playback control
//=========================================================================

/// @brief Show the conversation: install as the world's active dialogue.
/// @details Any different dialogue already installed in the world is marked
///          inactive before the world retains this one. Playback restarts from
///          the first queued line and clears transient reveal/choice notification state.
/// @param obj Dialogue3D runtime handle.
void rt_game3d_dialogue_show(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.show: invalid dialogue");
    if (!dialogue)
        return;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(dialogue->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world)
        return;
    dialogue->line_count = game3d_dialogue_line_count_safe(dialogue);
    dialogue->choice_count = game3d_dialogue_choice_count_safe(dialogue);
    if (dialogue->line_count == 0 && dialogue->choice_count == 0) {
        dialogue->active = 0;
        dialogue->choice_active = 0;
        if (world->active_dialogue == (void *)dialogue)
            game3d_release_typed_ref(&world->active_dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
        return;
    }
    rt_game3d_dialogue *previous = (rt_game3d_dialogue *)rt_g3d_checked_or_null(
        world->active_dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
    if (previous && previous != dialogue)
        previous->active = 0;
    game3d_assign_typed_ref(&world->active_dialogue, dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
    dialogue->active = 1;
    dialogue->line_index = 0;
    dialogue->reveal_chars = 0.0;
    dialogue->hold_remaining = 0.0;
    dialogue->line_started = 0;
    dialogue->choice_made = 0;
    dialogue->choice_active = dialogue->line_count == 0 && dialogue->choice_count > 0 ? 1 : 0;
    if (dialogue->choice_selected < 0 || dialogue->choice_selected >= dialogue->choice_count)
        dialogue->choice_selected = 0;
}

/// @brief Hide the conversation (releases the world slot).
/// @param obj Dialogue3D runtime handle.
/// @post The dialogue is inactive; its world slot is released only if it still
///       points to this dialogue.
void rt_game3d_dialogue_hide(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.hide: invalid dialogue");
    if (!dialogue)
        return;
    dialogue->active = 0;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(dialogue->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (world && world->active_dialogue == (void *)dialogue)
        game3d_release_typed_ref(&world->active_dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
}

/// @brief Current line codepoint length (revealed cap).
/// @param dialogue Dialogue whose current queue position is inspected.
/// @return The codepoint length of the current resolved line, or zero outside the queue.
static size_t game3d_dialogue_line_len(const rt_game3d_dialogue *dialogue) {
    int32_t line_count = game3d_dialogue_line_count_safe(dialogue);
    if (!dialogue || dialogue->line_index < 0 || dialogue->line_index >= line_count)
        return 0;
    const char *text = dialogue->lines[dialogue->line_index].text;
    return game3d_utf8_codepoint_count(text, strlen(text));
}

/// @brief Advance to the next line (or arm the pending choice / finish).
/// @details A partially revealed line is completed without changing the queue
///          position. A second advance moves forward; reaching the queue end
///          activates an unanswered choice or hides the dialogue.
/// @param obj Dialogue3D runtime handle.
void rt_game3d_dialogue_advance(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.advance: invalid dialogue");
    if (!dialogue || !dialogue->active || dialogue->choice_active)
        return;
    int32_t line_count = game3d_dialogue_line_count_safe(dialogue);
    int32_t choice_count = game3d_dialogue_choice_count_safe(dialogue);
    dialogue->line_count = line_count;
    dialogue->choice_count = choice_count;
    if (dialogue->line_index < 0 || dialogue->line_index >= line_count) {
        if (choice_count > 0 && !dialogue->choice_made)
            dialogue->choice_active = 1;
        else
            rt_game3d_dialogue_hide(obj);
        return;
    }
    size_t len = game3d_dialogue_line_len(dialogue);
    if (dialogue->reveal_chars < (double)len) {
        /* Two-stage skip: first press completes the reveal. */
        dialogue->reveal_chars = (double)len;
        return;
    }
    dialogue->line_index =
        dialogue->line_index < line_count - 1 ? dialogue->line_index + 1 : line_count;
    dialogue->reveal_chars = 0.0;
    dialogue->hold_remaining = 0.0;
    dialogue->line_started = 0;
    if (dialogue->line_index >= line_count) {
        if (choice_count > 0 && !dialogue->choice_made) {
            dialogue->choice_active = 1; /* block until confirmed */
        } else {
            rt_game3d_dialogue_hide(obj);
        }
    }
}

/// @brief Complete the current line's reveal instantly.
/// @param obj Dialogue3D runtime handle; inactive dialogues are unchanged.
void rt_game3d_dialogue_skip_reveal(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.skipReveal: invalid dialogue");
    if (dialogue && dialogue->active)
        dialogue->reveal_chars = (double)game3d_dialogue_line_len(dialogue);
}

/// @brief Move the choice highlight by @p delta (clamped).
/// @param obj Dialogue3D runtime handle.
/// @param delta Signed selection displacement. The result is clamped rather
///              than wrapped at the first and last option.
void rt_game3d_dialogue_move_choice(void *obj, int64_t delta) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.moveChoice: invalid dialogue");
    if (!dialogue || !dialogue->choice_active)
        return;
    int32_t choice_count = game3d_dialogue_choice_count_safe(dialogue);
    if (choice_count <= 0) {
        dialogue->choice_active = 0;
        return;
    }
    int64_t current = dialogue->choice_selected;
    if (current < 0)
        current = 0;
    else if (current >= choice_count)
        current = choice_count - 1;
    int64_t next;
    if (delta > 0 && delta >= (int64_t)choice_count - current)
        next = choice_count - 1;
    else if (delta < 0 && delta <= -current)
        next = 0;
    else
        next = current + delta;
    dialogue->choice_selected = (int32_t)next;
}

/// @brief Confirm the highlighted choice: latches choiceMade + lastChoice.
/// @param obj Dialogue3D runtime handle.
/// @post The selected zero-based index remains available through lastChoice,
///       choiceMade is armed once, and the dialogue is hidden.
void rt_game3d_dialogue_confirm_choice(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.confirmChoice: invalid dialogue");
    if (!dialogue || !dialogue->choice_active)
        return;
    int32_t choice_count = game3d_dialogue_choice_count_safe(dialogue);
    if (choice_count <= 0) {
        dialogue->choice_active = 0;
        dialogue->choice_count = 0;
        return;
    }
    dialogue->choice_count = choice_count;
    if (dialogue->choice_selected < 0)
        dialogue->choice_selected = 0;
    else if (dialogue->choice_selected >= choice_count)
        dialogue->choice_selected = choice_count - 1;
    dialogue->last_choice = dialogue->choice_selected;
    dialogue->choice_made = 1;
    dialogue->choice_active = 0;
    dialogue->choice_count = 0;
    rt_game3d_dialogue_hide(obj);
}

//=========================================================================
// Properties
//=========================================================================

/// @brief Report whether the dialogue is currently shown.
/// @param obj Dialogue3D runtime handle.
/// @return Non-zero when active, otherwise zero.
int8_t rt_game3d_dialogue_get_active(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.get_active: invalid dialogue");
    return dialogue ? dialogue->active : 0;
}

/// @brief Return the number of queued dialogue lines.
/// @param obj Dialogue3D runtime handle.
/// @return The bounded queue length, or zero for an invalid dialogue.
int64_t rt_game3d_dialogue_get_line_count(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.get_lineCount: invalid dialogue");
    return game3d_dialogue_line_count_safe(dialogue);
}

/// @brief Report whether playback is blocked on a visible choice prompt.
/// @param obj Dialogue3D runtime handle.
/// @return Non-zero while a choice is active, otherwise zero.
int8_t rt_game3d_dialogue_get_choice_pending(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.choicePending: invalid dialogue");
    return dialogue && dialogue->choice_active && game3d_dialogue_choice_count_safe(dialogue) > 0;
}

/// @brief One-shot: a choice was confirmed since the last query.
/// @param obj Dialogue3D runtime handle.
/// @return Non-zero once after confirmation, otherwise zero.
/// @post A non-zero pending notification is cleared by this query.
int8_t rt_game3d_dialogue_choice_made(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.choiceMade: invalid dialogue");
    if (!dialogue)
        return 0;
    int8_t made = dialogue->choice_made;
    dialogue->choice_made = 0;
    return made;
}

/// @brief Return the most recently confirmed zero-based choice index.
/// @param obj Dialogue3D runtime handle.
/// @return The latched index, or -1 before confirmation or for an invalid dialogue.
int64_t rt_game3d_dialogue_last_choice(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.lastChoice: invalid dialogue");
    return dialogue ? dialogue->last_choice : -1;
}

/// @brief Currently displayed (revealed) text of the active line.
/// @param obj Dialogue3D runtime handle.
/// @return A runtime string containing only the currently revealed codepoint prefix;
///         inactive or exhausted dialogues return an empty string.
rt_string rt_game3d_dialogue_current_text(void *obj) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.currentText: invalid dialogue");
    /* Stack buffer, not TU-static: the result is copied into a runtime string
     * before return, so a shared static only invited multi-instance/threading
     * hazards (the overlay path already uses a stack buffer). */
    char revealed[RT_GAME3D_TL_TEXT_MAX];
    revealed[0] = '\0';
    int32_t line_count = game3d_dialogue_line_count_safe(dialogue);
    if (dialogue && dialogue->active && dialogue->line_index >= 0 &&
        dialogue->line_index < line_count) {
        const char *full = dialogue->lines[dialogue->line_index].text;
        size_t len = strlen(full);
        size_t shown_codepoints = 0;
        if (isfinite(dialogue->reveal_chars) && dialogue->reveal_chars > 0.0)
            shown_codepoints = (size_t)dialogue->reveal_chars;
        size_t shown_bytes = game3d_utf8_prefix_bytes(full, len, shown_codepoints);
        memcpy(revealed, full, shown_bytes);
        revealed[shown_bytes] = '\0';
    }
    return rt_const_cstr(revealed);
}

/// @brief Select the entity used to anchor speaker-bubble presentation.
/// @param obj Dialogue3D runtime handle.
/// @param entity Entity3D to retain, or NULL to clear the anchor. Other runtime
///               classes trap and leave the prior reference unchanged.
void rt_game3d_dialogue_set_speaker_entity(void *obj, void *entity) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setSpeakerEntity: invalid dialogue");
    if (!dialogue)
        return;
    if (!entity) {
        game3d_release_ref(&dialogue->speaker_entity);
        return;
    }
    rt_game3d_entity *speaker = game3d_entity_checked(
        entity, "Game3D.Dialogue3D.setSpeakerEntity: value must be a live Entity3D");
    if (speaker)
        game3d_assign_ref(&dialogue->speaker_entity, speaker);
}

/// @brief Enable or disable speaker-anchored bubble presentation.
/// @param obj Dialogue3D runtime handle.
/// @param anchored Non-zero to try projecting above the speaker entity. Failed
///                 projection falls back to the bottom panel.
void rt_game3d_dialogue_set_anchored(void *obj, int8_t anchored) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setAnchored: invalid dialogue");
    if (dialogue)
        dialogue->anchored = anchored ? 1 : 0;
}

/// @brief Configure automatic advancement after each line is fully revealed.
/// @param obj Dialogue3D runtime handle.
/// @param enabled Non-zero to advance after the fixed hold interval.
void rt_game3d_dialogue_set_auto_advance(void *obj, int8_t enabled) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setAutoAdvance: invalid dialogue");
    if (dialogue)
        dialogue->auto_advance = enabled ? 1 : 0;
}

/// @brief Configure the typewriter reveal rate.
/// @param obj Dialogue3D runtime handle.
/// @param chars_per_second Requested positive Unicode codepoint reveal rate. Invalid values
///                         select the default and the result is capped at 10,000.
void rt_game3d_dialogue_set_reveal_speed(void *obj, double chars_per_second) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setRevealSpeed: invalid dialogue");
    if (dialogue)
        dialogue->reveal_speed =
            game3d_positive_clamped_or(chars_per_second, GAME3D_DLG_DEFAULT_REVEAL_SPEED, 10000.0);
}

/// @brief Bind a MessageBundle for key resolution (NULL unbinds).
/// @details Resolution occurs while lines or choices are queued; existing copied
///          text is not retranslated when this binding changes.
/// @param obj Dialogue3D runtime handle.
/// @param bundle MessageBundle to retain, or NULL to restore literal-only behavior.
void rt_game3d_dialogue_set_locale(void *obj, void *bundle) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setLocale: invalid dialogue");
    if (dialogue)
        game3d_assign_ref(&dialogue->bundle, bundle);
}

/// @brief Configure panel opacity and speaker-name color.
/// @param obj Dialogue3D runtime handle.
/// @param panel_alpha Requested opacity, finite-clamped to [0, 1] with 0.65 fallback.
/// @param name_color Packed color passed to Canvas3D for speaker labels.
void rt_game3d_dialogue_set_style(void *obj, double panel_alpha, int64_t name_color) {
    rt_game3d_dialogue *dialogue =
        game3d_dialogue_checked(obj, "Game3D.Dialogue3D.setStyle: invalid dialogue");
    if (dialogue) {
        dialogue->panel_alpha = game3d_clamp(game3d_finite_or(panel_alpha, 0.65), 0.0, 1.0);
        dialogue->name_color = name_color;
    }
}

//=========================================================================
// World hooks (tick + overlay)
//=========================================================================

/// @brief Typewriter tick + voice fire + auto-advance. See internal header.
/// @details The voice clip starts exactly once when a line first ticks. Reveal
///          progress accumulates at the configured rate; when complete, automatic
///          mode counts down a fixed hold before invoking advance.
/// @param world World3D whose active dialogue is advanced; NULL is ignored.
/// @param dt Frame duration in seconds, sanitized before reveal and hold updates.
void game3d_world_dialogue_tick(rt_game3d_world *world, double dt) {
    if (!world)
        return;
    rt_game3d_dialogue *dialogue = (rt_game3d_dialogue *)rt_g3d_checked_or_null(
        world->active_dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
    if (!dialogue || !dialogue->active)
        return;
    dt = game3d_clamp_dt(dt);
    int32_t line_count = game3d_dialogue_line_count_safe(dialogue);
    int32_t choice_count = game3d_dialogue_choice_count_safe(dialogue);
    dialogue->line_count = line_count;
    dialogue->choice_count = choice_count;
    (void)game3d_dialogue_speaker_ref(dialogue);
    if (dialogue->line_index < 0 || dialogue->line_index >= line_count) {
        if (choice_count > 0 && !dialogue->choice_made)
            dialogue->choice_active = 1;
        else
            rt_game3d_dialogue_hide(dialogue);
        return;
    }
    rt_game3d_dlg_line *line = &dialogue->lines[dialogue->line_index];
    if (!dialogue->line_started) {
        dialogue->line_started = 1;
        if (line->voice_clip && world->audio)
            (void)rt_game3d_audio_play2d(world->audio, line->voice_clip);
    }
    size_t len = game3d_utf8_codepoint_count(line->text, strlen(line->text));
    dialogue->reveal_speed = game3d_positive_clamped_or(
        dialogue->reveal_speed, GAME3D_DLG_DEFAULT_REVEAL_SPEED, 10000.0);
    if (!isfinite(dialogue->reveal_chars) || dialogue->reveal_chars < 0.0)
        dialogue->reveal_chars = 0.0;
    else if (dialogue->reveal_chars > (double)len)
        dialogue->reveal_chars = (double)len;
    if (dialogue->reveal_chars < (double)len) {
        dialogue->reveal_chars += dialogue->reveal_speed * dt;
        if (dialogue->reveal_chars >= (double)len) {
            dialogue->reveal_chars = (double)len;
            dialogue->hold_remaining = GAME3D_DLG_AUTO_HOLD_SECONDS;
        }
    } else if (dialogue->auto_advance) {
        if (!isfinite(dialogue->hold_remaining))
            dialogue->hold_remaining = 0.0;
        dialogue->hold_remaining -= dt;
        if (dialogue->hold_remaining <= 0.0)
            rt_game3d_dialogue_advance(dialogue);
    }
}

/// @brief Draw borrowed C text through Canvas3D without leaking the temporary runtime string.
/// @param canvas Canvas3D receiving the overlay text.
/// @param x Horizontal pixel coordinate.
/// @param y Vertical pixel coordinate.
/// @param text Borrowed NUL-terminated text; NULL is treated as empty.
/// @param color Packed text color.
static void game3d_dialogue_draw_text_cstr(
    void *canvas, int64_t x, int64_t y, const char *text, int64_t color) {
    rt_string runtime_text = rt_const_cstr(text ? text : "");
    if (!runtime_text)
        return;
    rt_canvas3d_draw_text2d(canvas, x, y, runtime_text, color);
    rt_string_unref(runtime_text);
}

/// @brief Overlay draw: bottom panel or anchored bubble + choices. See header.
/// @details Active choices use a centered bottom list. Normal lines first try a
///          clamped bubble above the projected speaker and fall back to a bottom
///          panel when anchoring is disabled, invalid, or behind the camera.
/// @param world World3D whose canvas, camera, dimensions, and active dialogue
///              supply the overlay; incomplete worlds are ignored.
void game3d_world_dialogue_overlay(rt_game3d_world *world) {
    if (!world || !world->canvas)
        return;
    rt_game3d_dialogue *dialogue = (rt_game3d_dialogue *)rt_g3d_checked_or_null(
        world->active_dialogue, RT_G3D_GAME3D_DIALOGUE_CLASS_ID);
    if (!dialogue || !dialogue->active)
        return;
    int64_t width = world->width;
    int64_t height = world->height;
    if (width <= 0 || height <= 0)
        return;

    if (dialogue->choice_active) {
        /* Choice prompt: centered list with a highlight marker. */
        int32_t choice_count = game3d_dialogue_choice_count_safe(dialogue);
        if (choice_count <= 0)
            return;
        int64_t panel_h = (int64_t)(choice_count + 2) * 14;
        int64_t top = height - panel_h - 16;
        rt_canvas3d_draw_rect2d_alpha(
            world->canvas, 12, top, width - 24, panel_h, 0x101418, dialogue->panel_alpha);
        for (int32_t i = 0; i < choice_count; ++i) {
            char row[RT_GAME3D_TL_TEXT_MAX + 4];
            row[0] = i == dialogue->choice_selected ? '>' : ' ';
            row[1] = ' ';
            (void)game3d_utf8_copy_bounded(row + 2, sizeof(row) - 2, dialogue->choices[i]);
            game3d_dialogue_draw_text_cstr(world->canvas,
                                           24,
                                           top + 10 + (int64_t)i * 14,
                                           row,
                                           i == dialogue->choice_selected ? 0xFFFFFF : 0xB0B8C0);
        }
        return;
    }
    int32_t line_count = game3d_dialogue_line_count_safe(dialogue);
    if (dialogue->line_index < 0 || dialogue->line_index >= line_count)
        return;

    rt_game3d_dlg_line *line = &dialogue->lines[dialogue->line_index];
    char revealed[RT_GAME3D_TL_TEXT_MAX];
    size_t len = strlen(line->text);
    size_t shown_codepoints = 0;
    if (isfinite(dialogue->reveal_chars) && dialogue->reveal_chars > 0.0)
        shown_codepoints = (size_t)dialogue->reveal_chars;
    size_t shown_bytes = game3d_utf8_prefix_bytes(line->text, len, shown_codepoints);
    memcpy(revealed, line->text, shown_bytes);
    revealed[shown_bytes] = '\0';

    /* Anchored bubble above the speaker entity when projectable. */
    if (dialogue->anchored) {
        rt_game3d_entity *speaker = game3d_dialogue_speaker_ref(dialogue);
        double pos[3];
        if (speaker && world->camera && game3d_entity_world_position_components(speaker, pos)) {
            double sx = 0.0;
            double sy = 0.0;
            if (rt_camera3d_world_to_screen(
                    world->camera, pos[0], pos[1] + 1.9, pos[2], width, height, &sx, &sy)) {
                size_t speaker_codepoints =
                    game3d_utf8_codepoint_count(line->speaker, strlen(line->speaker));
                size_t bubble_codepoints = game3d_utf8_codepoint_count(revealed, shown_bytes);
                if (speaker_codepoints > bubble_codepoints)
                    bubble_codepoints = speaker_codepoints;
                if (bubble_codepoints < 8)
                    bubble_codepoints = 8;
                int64_t bubble_w = (int64_t)bubble_codepoints * 8 + 16;
                int64_t margin_x = width >= 8 ? 4 : 0;
                int64_t max_bubble_w = width - margin_x * 2;
                if (bubble_w > max_bubble_w)
                    bubble_w = max_bubble_w;
                int64_t bx = (int64_t)sx - bubble_w / 2;
                if (bx < margin_x)
                    bx = margin_x;
                if (bx + bubble_w > width - margin_x)
                    bx = width - margin_x - bubble_w;
                const int64_t bubble_h = 28;
                int64_t margin_y = height >= bubble_h + 8 ? 4 : 0;
                int64_t by = (int64_t)sy - 34;
                if (by < margin_y)
                    by = margin_y;
                if (by + bubble_h > height - margin_y)
                    by = height - margin_y - bubble_h;
                if (by < 0)
                    by = 0;
                rt_canvas3d_draw_rect2d_alpha(
                    world->canvas, bx, by, bubble_w, bubble_h, 0x101418, dialogue->panel_alpha);
                if (line->speaker[0])
                    game3d_dialogue_draw_text_cstr(
                        world->canvas, bx + 8, by + 4, line->speaker, dialogue->name_color);
                game3d_dialogue_draw_text_cstr(world->canvas, bx + 8, by + 16, revealed, 0xFFFFFF);
                return;
            }
        }
        /* Behind camera / no speaker: fall through to the bottom panel. */
    }

    int64_t panel_h = height / 4;
    if (panel_h < 44)
        panel_h = 44;
    int64_t top = height - panel_h - 8;
    rt_canvas3d_draw_rect2d_alpha(
        world->canvas, 12, top, width - 24, panel_h, 0x101418, dialogue->panel_alpha);
    if (line->speaker[0])
        game3d_dialogue_draw_text_cstr(
            world->canvas, 24, top + 8, line->speaker, dialogue->name_color);
    game3d_dialogue_draw_text_cstr(world->canvas, 24, top + 24, revealed, 0xFFFFFF);
}
