//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_interact.c
// Purpose: Focus-and-use interaction — Interactable3D components on entities
//   plus an Interactor3D scanner (distance + view-cone + optional line of
//   sight + hysteresis) ticked in the world step, with polled focus/interact
//   state.
// Key invariants:
//   - Scanning walks the world entity list (no physics query): deterministic
//     candidate order, stale/despawned entities fail closed.
//   - Line-of-sight probes are allocation-free and ignore the scanner owner's
//     body; retained focus is revalidated before every public action/query.
//   - The current focus receives a sign-safe 10% score boost to reduce focus churn.
// Ownership/Lifetime:
//   - Components hold zeroing weak owner-entity slots; the interactor retains
//     its focused interactable without extending the target entity's lifetime.
// Links: misc/plans/thirdpersonupgrade/21-interaction-system.md, ADR 0093.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic focus selection and polled interactions for Game3D.
/// @details Interactable3D describes a prompt, kind, range, priority, and enabled
///          state on a candidate entity. Interactor3D scans the authoritative
///          world registry in stable order, filters candidates by distance,
///          view cone, and optional physics line of sight, then retains the
///          highest-scoring target with a current-focus hysteresis multiplier.
///          Focus changes and successful interactions are exposed as polled state.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Per-entity interaction target metadata.
typedef struct rt_game3d_interactable {
    void *vptr;
    void *entity; /* zeroing weak owner Entity3D slot */
    rt_string prompt;
    int64_t kind;
    double radius;
    double focus_priority;
    int8_t enabled;
} rt_game3d_interactable;

/// @brief Per-entity focus scanner and interaction telemetry.
typedef struct rt_game3d_interactor {
    void *vptr;
    void *entity; /* zeroing weak owner Entity3D slot */
    double cone_degrees;
    int64_t los_mask;
    int8_t require_los;
    void *focused; /* retained Interactable3D or NULL */
    int8_t focus_changed;
    int8_t interact_requested;
    int64_t interact_count;
    void *last_interacted; /* retained Interactable3D from the last interact */
} rt_game3d_interactor;

/*==========================================================================
 * Interactable3D
 *=========================================================================*/

/// @brief Clear the entity back-reference and release the retained prompt.
/// @param obj Interactable3D storage being finalized; NULL is ignored.
static void game3d_interactable_finalize(void *obj) {
    rt_game3d_interactable *item = (rt_game3d_interactable *)obj;
    if (!item)
        return;
    rt_weak_store(&item->entity, NULL);
    game3d_release_ref((void **)&item->prompt);
}

/// @brief Create and install an enabled interaction target on an entity.
/// @details The entity retains the new component and clears the weak back-reference
///          of any previous target. The returned creation reference remains owned
///          by the caller.
/// @param entity_obj Entity3D that receives the component.
/// @return A newly allocated Interactable3D, or NULL after validation or allocation failure.
void *rt_game3d_interactable_new(void *entity_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Interactable3D.New: invalid entity");
    if (!entity) {
        return NULL;
    }
    rt_game3d_interactable *item = (rt_game3d_interactable *)rt_obj_new_i64(
        RT_G3D_GAME3D_INTERACTABLE_CLASS_ID, (int64_t)sizeof(rt_game3d_interactable));
    if (!item) {
        rt_trap("Game3D.Interactable3D.New: allocation failed");
        return NULL;
    }
    memset(item, 0, sizeof(*item));
    rt_obj_set_finalizer(item, game3d_interactable_finalize);
    rt_weak_store(&item->entity, entity);
    if (!item->entity) {
        game3d_release_ref((void **)&item);
        return NULL;
    }
    item->prompt = rt_const_cstr("Use");
    item->radius = 2.0;
    item->enabled = 1;
    {
        rt_game3d_interactable *previous = (rt_game3d_interactable *)rt_g3d_checked_or_null(
            entity->interactable, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID);
        if (previous && previous != item)
            rt_weak_store(&previous->entity, NULL);
        game3d_assign_typed_ref(&entity->interactable, item, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID);
    }
    return item;
}

/// @brief Validate a runtime handle as Interactable3D.
/// @param obj Candidate runtime handle.
/// @param method Trap message used when validation fails.
/// @return The typed component pointer, or NULL after trapping.
static rt_game3d_interactable *game3d_interactable_checked(void *obj, const char *method) {
    rt_game3d_interactable *item =
        (rt_game3d_interactable *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID);
    if (!item)
        rt_trap(method);
    return item;
}

/// @brief Retain a new display prompt for an interaction target.
/// @param obj Interactable3D runtime handle.
/// @param prompt Runtime string to retain; NULL leaves the existing prompt unchanged.
/// @return @p obj for fluent chaining.
void *rt_game3d_interactable_with_prompt(void *obj, rt_string prompt) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.withPrompt: invalid component");
    if (item && prompt)
        game3d_assign_ref((void **)&item->prompt, prompt);
    return obj;
}

/// @brief Return an owned reference to the interaction prompt.
/// @param obj Interactable3D runtime handle.
/// @return A retained runtime string, or the canonical empty string when unavailable.
rt_string rt_game3d_interactable_get_prompt(void *obj) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.get_Prompt: invalid component");
    if (!item || !item->prompt)
        return rt_str_empty();
    return rt_string_ref(item->prompt);
}

/// @brief Store the application-defined interaction kind.
/// @param obj Interactable3D runtime handle.
/// @param kind Opaque scalar kind value.
/// @return @p obj for fluent chaining.
void *rt_game3d_interactable_with_kind(void *obj, int64_t kind) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.withKind: invalid component");
    if (item)
        item->kind = kind;
    return obj;
}

/// @brief Return the application-defined interaction kind.
/// @param obj Interactable3D runtime handle.
/// @return The stored kind, or zero when invalid.
int64_t rt_game3d_interactable_get_kind(void *obj) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.get_Kind: invalid component");
    return item ? item->kind : 0;
}

/// @brief Configure the maximum focus distance.
/// @param obj Interactable3D runtime handle.
/// @param radius Finite positive world-space radius, capped at 64; invalid
///               values leave the existing radius unchanged.
/// @return @p obj for fluent chaining.
void *rt_game3d_interactable_with_radius(void *obj, double radius) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.withRadius: invalid component");
    if (item && isfinite(radius) && radius > 0.0)
        item->radius = radius > 64.0 ? 64.0 : radius;
    return obj;
}

/// @brief Return the maximum focus distance.
/// @param obj Interactable3D runtime handle.
/// @return The stored world-space radius, or zero when invalid.
double rt_game3d_interactable_get_radius(void *obj) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.get_Radius: invalid component");
    return item ? item->radius : 0.0;
}

/// @brief Enable or disable participation in focus scans.
/// @param obj Interactable3D runtime handle.
/// @param enabled Non-zero to make the target eligible.
void rt_game3d_interactable_set_enabled(void *obj, int8_t enabled) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.set_Enabled: invalid component");
    if (item)
        item->enabled = enabled ? 1 : 0;
}

/// @brief Report whether the target participates in focus scans.
/// @param obj Interactable3D runtime handle.
/// @return Non-zero when enabled, otherwise zero.
int8_t rt_game3d_interactable_get_enabled(void *obj) {
    rt_game3d_interactable *item =
        game3d_interactable_checked(obj, "Game3D.Interactable3D.get_Enabled: invalid component");
    return item ? item->enabled : 0;
}

/// @brief Set the additive application-defined focus score bias.
/// @param obj Interactable3D runtime handle.
/// @param priority Finite score bias; non-finite values are ignored.
void rt_game3d_interactable_set_focus_priority(void *obj, double priority) {
    rt_game3d_interactable *item = game3d_interactable_checked(
        obj, "Game3D.Interactable3D.set_FocusPriority: invalid component");
    if (item && isfinite(priority))
        item->focus_priority =
            game3d_clamp(priority, -RT_GAME3D_COORD_ABS_MAX, RT_GAME3D_COORD_ABS_MAX);
}

/// @brief Return the additive focus score bias.
/// @param obj Interactable3D runtime handle.
/// @return The stored bias, or zero when invalid.
double rt_game3d_interactable_get_focus_priority(void *obj) {
    rt_game3d_interactable *item = game3d_interactable_checked(
        obj, "Game3D.Interactable3D.get_FocusPriority: invalid component");
    return item ? item->focus_priority : 0.0;
}

/*==========================================================================
 * Interactor3D
 *=========================================================================*/

/// @brief Clear the owner back-reference and release focused-target history.
/// @param obj Interactor3D storage being finalized; NULL is ignored.
static void game3d_interactor_finalize(void *obj) {
    rt_game3d_interactor *scanner = (rt_game3d_interactor *)obj;
    if (!scanner)
        return;
    rt_weak_store(&scanner->entity, NULL);
    game3d_release_ref(&scanner->focused);
    game3d_release_ref(&scanner->last_interacted);
}

/// @brief Create and install a focus scanner on an entity.
/// @details The entity retains the new component and clears the weak back-reference
///          of any previous scanner. The returned creation reference remains owned
///          by the caller.
/// @param entity_obj Entity3D that owns the scanner origin and forward direction.
/// @return A newly allocated Interactor3D, or NULL after validation or allocation failure.
void *rt_game3d_interactor_new(void *entity_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Interactor3D.New: invalid entity");
    if (!entity) {
        return NULL;
    }
    rt_game3d_interactor *scanner = (rt_game3d_interactor *)rt_obj_new_i64(
        RT_G3D_GAME3D_INTERACTOR_CLASS_ID, (int64_t)sizeof(rt_game3d_interactor));
    if (!scanner) {
        rt_trap("Game3D.Interactor3D.New: allocation failed");
        return NULL;
    }
    memset(scanner, 0, sizeof(*scanner));
    rt_obj_set_finalizer(scanner, game3d_interactor_finalize);
    rt_weak_store(&scanner->entity, entity);
    if (!scanner->entity) {
        game3d_release_ref((void **)&scanner);
        return NULL;
    }
    scanner->cone_degrees = 70.0;
    scanner->los_mask = -1;
    scanner->require_los = 1;
    {
        rt_game3d_interactor *previous = (rt_game3d_interactor *)rt_g3d_checked_or_null(
            entity->interactor, RT_G3D_GAME3D_INTERACTOR_CLASS_ID);
        if (previous && previous != scanner)
            rt_weak_store(&previous->entity, NULL);
        game3d_assign_typed_ref(&entity->interactor, scanner, RT_G3D_GAME3D_INTERACTOR_CLASS_ID);
    }
    return scanner;
}

/// @brief Validate a runtime handle as Interactor3D.
/// @param obj Candidate runtime handle.
/// @param method Trap message used when validation fails.
/// @return The typed scanner pointer, or NULL after trapping.
static rt_game3d_interactor *game3d_interactor_checked(void *obj, const char *method) {
    rt_game3d_interactor *scanner =
        (rt_game3d_interactor *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_INTERACTOR_CLASS_ID);
    if (!scanner)
        rt_trap(method);
    return scanner;
}

/// @brief Revalidate and, when stale, clear the scanner's retained focus.
/// @details A target becomes stale when disabled, destroyed, or replaced in its
///          entity's one-component slot. Clearing here prevents an interaction
///          between world ticks from acting on a detached component.
/// @param scanner Scanner whose retained focus is inspected.
/// @return Borrowed live enabled Interactable3D, or NULL.
static rt_game3d_interactable *game3d_interactor_focus_ref(rt_game3d_interactor *scanner) {
    rt_game3d_interactable *item = scanner
                                       ? (rt_game3d_interactable *)rt_g3d_checked_or_null(
                                             scanner->focused, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID)
                                       : NULL;
    rt_game3d_entity *entity = item ? (rt_game3d_entity *)rt_weak_load(&item->entity) : NULL;
    int valid = item && item->enabled && entity && entity->alive && !entity->destroyed &&
                entity->interactable == (void *)item;
    game3d_release_ref((void **)&entity);
    if (valid)
        return item;
    if (scanner && scanner->focused) {
        game3d_release_ref(&scanner->focused);
        scanner->focus_changed = 1;
    }
    return NULL;
}

/// @brief Return a fail-closed world entity count for interaction scans.
/// @param world World whose dense entity array is inspected.
/// @return Safe logical count, or zero for missing/corrupt storage.
static int32_t game3d_interactor_world_count(const rt_game3d_world *world) {
    if (!world || !world->entities || world->entity_count <= 0 || world->entity_capacity <= 0)
        return 0;
    return world->entity_count > world->entity_capacity ? world->entity_capacity
                                                        : world->entity_count;
}

/// @brief Configure the full horizontal/vertical focus cone angle.
/// @param obj Interactor3D runtime handle.
/// @param degrees Finite angle greater than one degree, capped at 180;
///                invalid values leave the current cone unchanged.
void rt_game3d_interactor_set_cone_degrees(void *obj, double degrees) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.set_ConeDegrees: invalid scanner");
    if (scanner && isfinite(degrees) && degrees > 1.0)
        scanner->cone_degrees = degrees > 180.0 ? 180.0 : degrees;
}

/// @brief Return the full focus cone angle.
/// @param obj Interactor3D runtime handle.
/// @return The stored angle in degrees, or zero when invalid.
double rt_game3d_interactor_get_cone_degrees(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.get_ConeDegrees: invalid scanner");
    return scanner ? scanner->cone_degrees : 0.0;
}

/// @brief Configure whether physics line of sight gates focus.
/// @param obj Interactor3D runtime handle.
/// @param required Non-zero to reject candidates occluded by a different body.
void rt_game3d_interactor_set_require_los(void *obj, int8_t required) {
    rt_game3d_interactor *scanner = game3d_interactor_checked(
        obj, "Game3D.Interactor3D.set_RequireLineOfSight: invalid scanner");
    if (scanner)
        scanner->require_los = required ? 1 : 0;
}

/// @brief Report whether physics line of sight is required.
/// @param obj Interactor3D runtime handle.
/// @return Non-zero when line-of-sight filtering is enabled.
int8_t rt_game3d_interactor_get_require_los(void *obj) {
    rt_game3d_interactor *scanner = game3d_interactor_checked(
        obj, "Game3D.Interactor3D.get_RequireLineOfSight: invalid scanner");
    return scanner ? scanner->require_los : 0;
}

/// @brief Set the collision mask used for occlusion rays.
/// @param obj Interactor3D runtime handle.
/// @param mask Physics collision-mask bits; -1 selects all layers by convention.
void rt_game3d_interactor_set_los_mask(void *obj, int64_t mask) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.set_LosMask: invalid scanner");
    if (scanner)
        scanner->los_mask = mask;
}

/// @brief Return the collision mask used for occlusion rays.
/// @param obj Interactor3D runtime handle.
/// @return The stored mask bits, or zero when invalid.
int64_t rt_game3d_interactor_get_los_mask(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.get_LosMask: invalid scanner");
    return scanner ? scanner->los_mask : 0;
}

/// @brief Return an owned reference to the currently focused target.
/// @param obj Interactor3D runtime handle.
/// @return A retained Interactable3D pointer, or NULL when no target is focused.
void *rt_game3d_interactor_get_focused(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.get_Focused: invalid scanner");
    rt_game3d_interactable *focused = game3d_interactor_focus_ref(scanner);
    if (!focused)
        return NULL;
    rt_obj_retain_maybe(focused);
    return focused;
}

/// @brief One-shot: true when the focused target changed since the last call.
/// @param obj Interactor3D runtime handle.
/// @return Non-zero once after a focus transition, otherwise zero.
/// @post Any pending focus-change notification is cleared.
int8_t rt_game3d_interactor_focus_changed(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.FocusChanged: invalid scanner");
    if (!scanner)
        return 0;
    int8_t changed = scanner->focus_changed;
    scanner->focus_changed = 0;
    return changed;
}

/// @brief Record an interaction immediately against the current focus.
/// @details Successful calls increment telemetry and retain the focused target
///          as last-interacted state; no callback or deferred tick is involved.
/// @param obj Interactor3D runtime handle.
/// @return 1 when a target is currently focused.
int8_t rt_game3d_interactor_interact(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.Interact: invalid scanner");
    rt_game3d_interactable *focused = game3d_interactor_focus_ref(scanner);
    if (!focused)
        return 0;
    if (scanner->interact_count < INT64_MAX)
        scanner->interact_count++;
    game3d_assign_ref(&scanner->last_interacted, focused);
    return 1;
}

/// @brief Return the number of successful interaction polls.
/// @param obj Interactor3D runtime handle.
/// @return The accumulated interaction count, or zero when invalid.
int64_t rt_game3d_interactor_get_interact_count(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.get_InteractCount: invalid scanner");
    return scanner ? scanner->interact_count : 0;
}

/// @brief Return an owned reference to the most recently interacted target.
/// @param obj Interactor3D runtime handle.
/// @return A retained Interactable3D pointer, or NULL before any interaction.
void *rt_game3d_interactor_get_last_interacted(void *obj) {
    rt_game3d_interactor *scanner =
        game3d_interactor_checked(obj, "Game3D.Interactor3D.get_LastInteracted: invalid scanner");
    if (!scanner || !scanner->last_interacted)
        return NULL;
    rt_obj_retain_maybe(scanner->last_interacted);
    return scanner->last_interacted;
}

/// @brief Owner-forward vector: the entity node's world rotation applied to -Z.
/// @param entity Entity3D supplying the scene-node world rotation.
/// @param[out] out_fwd Three-element destination initialized to normalized -Z
///                     and replaced with the normalized rotated vector when available.
static void game3d_interactor_forward(rt_game3d_entity *entity, double out_fwd[3]) {
    out_fwd[0] = 0.0;
    out_fwd[1] = 0.0;
    out_fwd[2] = -1.0;
    if (!entity || !entity->node)
        return;
    double qx, qy, qz, qw;
    if (!rt_scene_node3d_get_world_rotation_components(entity->node, &qx, &qy, &qz, &qw))
        return;
    /* Rotate (0,0,-1) by the quaternion. */
    double x = 0.0, y = 0.0, z = -1.0;
    double tx = 2.0 * (qy * z - qz * y);
    double ty = 2.0 * (qz * x - qx * z);
    double tz = 2.0 * (qx * y - qy * x);
    out_fwd[0] = x + qw * tx + (qy * tz - qz * ty);
    out_fwd[1] = y + qw * ty + (qz * tx - qx * tz);
    out_fwd[2] = z + qw * tz + (qx * ty - qy * tx);
    double len = sqrt(out_fwd[0] * out_fwd[0] + out_fwd[1] * out_fwd[1] + out_fwd[2] * out_fwd[2]);
    if (isfinite(len) && len > 1e-9) {
        out_fwd[0] /= len;
        out_fwd[1] /= len;
        out_fwd[2] /= len;
    } else {
        out_fwd[0] = 0.0;
        out_fwd[1] = 0.0;
        out_fwd[2] = -1.0;
    }
}

/// @brief Per-step scan: pick the best focused interactable (hysteresis-stable).
/// @details Candidates are visited in registry order and must be alive, enabled,
///          within their radius, and inside the cone. Optional rays ignore a
///          hit on the candidate's own body; near-touching candidates skip the
///          ray. Strictly greater scores replace the current best, preserving
///          deterministic first-candidate tie behavior.
/// @param world World3D supplying candidate order and optional physics.
/// @param owner Entity3D supplying scanner state, origin, and orientation.
/// @param dt Simulation step in seconds; unused because scanning is instantaneous.
void game3d_interactor_tick(rt_game3d_world *world, rt_game3d_entity *owner, double dt) {
    (void)dt;
    rt_game3d_interactor *scanner = (rt_game3d_interactor *)rt_g3d_checked_or_null(
        owner->interactor, RT_G3D_GAME3D_INTERACTOR_CLASS_ID);
    if (!scanner)
        return;
    double origin[3];
    if (!game3d_entity_world_position_components(owner, origin))
        return;
    double forward[3];
    game3d_interactor_forward(owner, forward);
    double cone_cos = cos(scanner->cone_degrees * 0.5 * (3.14159265358979323846 / 180.0));

    void *best = NULL;
    double best_score = -DBL_MAX;
    int32_t count = game3d_interactor_world_count(world);
    void *owner_body = game3d_entity_body_ref(owner);
    for (int32_t i = 0; i < count; ++i) {
        rt_game3d_entity *candidate = world->entities ? world->entities[i] : NULL;
        if (!candidate || !candidate->alive || candidate == owner || !candidate->interactable)
            continue;
        rt_game3d_interactable *item = (rt_game3d_interactable *)rt_g3d_checked_or_null(
            candidate->interactable, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID);
        if (!item || !item->enabled)
            continue;
        double target[3];
        if (!game3d_entity_world_position_components(candidate, target))
            continue;
        double to[3] = {target[0] - origin[0], target[1] - origin[1], target[2] - origin[2]};
        double dist = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (!isfinite(dist) || dist > item->radius)
            continue;
        double align = 1.0;
        if (dist > 1e-6) {
            align = (to[0] * forward[0] + to[1] * forward[1] + to[2] * forward[2]) / dist;
            if (align < cone_cos)
                continue;
        }
        /* Near-touching candidates (dist <= 0.05) skip the occlusion ray: the
         * epsilon pull-back would drive the ray length negative. */
        if (scanner->require_los && world->physics && dist > 0.05) {
            void *candidate_body = game3d_entity_body_ref(candidate);
            void *hit_body = rt_world3d_raycast_closest_body_raw(world->physics,
                                                                 origin[0],
                                                                 origin[1],
                                                                 origin[2],
                                                                 to[0] / dist,
                                                                 to[1] / dist,
                                                                 to[2] / dist,
                                                                 dist - 0.05,
                                                                 scanner->los_mask,
                                                                 owner_body,
                                                                 NULL);
            if (hit_body && hit_body != candidate_body)
                continue;
        }
        double score = (1.0 - dist / item->radius) +
                       0.5 * ((align - cone_cos) / (1.0 - cone_cos + 1e-9)) + item->focus_priority;
        if ((void *)item == scanner->focused)
            score *= score >= 0.0 ? 1.10 : 0.90; /* sign-safe hysteresis boost */
        if (score > best_score) {
            best_score = score;
            best = item;
        }
    }
    if (best != scanner->focused) {
        game3d_assign_ref(&scanner->focused, best);
        scanner->focus_changed = 1;
    }
}

#else
typedef int rt_game3d_interact_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
