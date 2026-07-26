//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/rt_game3d_events.inc  (#included from rt_game3d.c; not a standalone translation unit)
// Purpose: Game3D collision-event wrapping and the public CollisionEvent3D accessor surface
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Game3D collision-event enumeration and wrapper accessors.
/// @details This include fragment maps the physics world's enter/stay/exit
///          buffers into Game3D Collision3DEvent objects. Wrappers retain the
///          raw event and any entities resolved through the world's body index;
///          unresolved bodies remain representable through the raw event.

/// @brief Count collision events for the given phase (ANY sums enter+stay+exit). See header.
/// @param obj World3D runtime handle.
/// @param phase Collision phase to count; unsupported values are treated as ANY.
/// @return The number of events in the selected buffer or combined buffers, or
///         zero when the world has no physics simulation.
int64_t rt_game3d_world_collision_event_count(void *obj, int64_t phase) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.collisionEventCount: invalid world");
    if (!world || !world->physics)
        return 0;
    switch (phase) {
        case RT_GAME3D_COLLISION_ENTER:
            return rt_world3d_get_enter_event_count(world->physics);
        case RT_GAME3D_COLLISION_STAY:
            return rt_world3d_get_stay_event_count(world->physics);
        case RT_GAME3D_COLLISION_EXIT:
            return rt_world3d_get_exit_event_count(world->physics);
        case RT_GAME3D_COLLISION_ANY:
        default:
            return rt_world3d_get_enter_event_count(world->physics) +
                   rt_world3d_get_stay_event_count(world->physics) +
                   rt_world3d_get_exit_event_count(world->physics);
    }
}

/// @brief GC finalizer for a collision event: release the raw event and both entities.
/// @param obj Collision3DEvent storage being finalized; NULL is ignored.
static void game3d_collision_event_finalize(void *obj) {
    rt_game3d_collision_event *event = (rt_game3d_collision_event *)obj;
    if (!event)
        return;
    game3d_release_typed_ref(&event->raw, RT_G3D_COLLISIONEVENT3D_CLASS_ID);
    game3d_release_typed_ref(&event->b, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_release_typed_ref(&event->a, RT_G3D_GAME3D_ENTITY_CLASS_ID);
}

/// @brief Normalize a collision phase to a supported public value.
/// @param phase Candidate collision-phase identifier.
/// @return @p phase when it is ENTER, STAY, EXIT, or ANY; otherwise ANY.
static int64_t game3d_collision_event_phase_or_any(int64_t phase) {
    switch (phase) {
        case RT_GAME3D_COLLISION_ENTER:
        case RT_GAME3D_COLLISION_STAY:
        case RT_GAME3D_COLLISION_EXIT:
        case RT_GAME3D_COLLISION_ANY:
            return phase;
        default:
            return RT_GAME3D_COLLISION_ANY;
    }
}

/// @brief Validate a retained collision participant as an Entity3D.
/// @param ref Candidate entity reference; may be NULL.
/// @return The validated Entity3D pointer, or NULL.
static void *game3d_collision_event_entity_ref(void *ref) {
    return rt_g3d_checked_or_null(ref, RT_G3D_GAME3D_ENTITY_CLASS_ID);
}

/// @brief Validate a wrapper's retained low-level collision event.
/// @param event Collision3DEvent wrapper; may be NULL.
/// @return The validated CollisionEvent3D pointer, or NULL.
static void *game3d_collision_event_raw_ref(const rt_game3d_collision_event *event) {
    return event ? rt_g3d_checked_or_null(event->raw, RT_G3D_COLLISIONEVENT3D_CLASS_ID) : NULL;
}

/// @brief Wrap a raw physics collision event into a Game3D Collision3DEvent, resolving
///   each body back to its owning entity; consumes `raw_event` and traps on OOM.
/// @param world World3D whose body index resolves participants.
/// @param phase Resolved Game3D collision phase stored on the wrapper.
/// @param raw_event Owned CollisionEvent3D reference transferred to the wrapper;
///                  NULL produces no wrapper.
/// @return A newly allocated Collision3DEvent, or NULL. Allocation failure
///         releases @p raw_event before trapping.
static void *game3d_collision_event_wrap(rt_game3d_world *world, int64_t phase, void *raw_event) {
    rt_game3d_collision_event *event;
    if (!raw_event)
        return NULL;
    event = (rt_game3d_collision_event *)rt_obj_new_i64(RT_G3D_GAME3D_COLLISION_EVENT_CLASS_ID,
                                                        (int64_t)sizeof(*event));
    if (!event) {
        game3d_release_ref(&raw_event);
        rt_trap("Game3D.Collision3DEvent: allocation failed");
        return NULL;
    }
    memset(event, 0, sizeof(*event));
    rt_obj_set_finalizer(event, game3d_collision_event_finalize);
    event->phase = phase;
    event->raw = raw_event;
    void *body_a = rt_collision_event3d_get_body_a(raw_event);
    void *body_b = rt_collision_event3d_get_body_b(raw_event);
    game3d_assign_ref(&event->a, game3d_world_find_entity_by_body(world, body_a));
    game3d_assign_ref(&event->b, game3d_world_find_entity_by_body(world, body_b));
    return event;
}

/// @brief Fetch the raw physics collision event for (phase, index); for the ANY phase it
///   walks enter→stay→exit ranges and reports the resolved phase via `actual_phase`.
/// @param world World3D whose physics buffers are queried.
/// @param phase Requested collision phase; unsupported values use ANY ordering.
/// @param index Zero-based index within the selected or combined phase range.
/// @param[out] actual_phase Optional destination for the specific returned phase.
/// @return The low-level CollisionEvent3D reference, or NULL for invalid state
///         or an out-of-range index.
static void *game3d_world_raw_collision_event(rt_game3d_world *world,
                                              int64_t phase,
                                              int64_t index,
                                              int64_t *actual_phase) {
    if (!world || !world->physics || index < 0)
        return NULL;
    if (actual_phase)
        *actual_phase = phase;
    switch (phase) {
        case RT_GAME3D_COLLISION_ENTER:
            return rt_world3d_get_enter_event(world->physics, index);
        case RT_GAME3D_COLLISION_STAY:
            return rt_world3d_get_stay_event(world->physics, index);
        case RT_GAME3D_COLLISION_EXIT:
            return rt_world3d_get_exit_event(world->physics, index);
        case RT_GAME3D_COLLISION_ANY:
        default: {
            int64_t enter_count = rt_world3d_get_enter_event_count(world->physics);
            if (index < enter_count) {
                if (actual_phase)
                    *actual_phase = RT_GAME3D_COLLISION_ENTER;
                return rt_world3d_get_enter_event(world->physics, index);
            }
            index -= enter_count;
            int64_t stay_count = rt_world3d_get_stay_event_count(world->physics);
            if (index < stay_count) {
                if (actual_phase)
                    *actual_phase = RT_GAME3D_COLLISION_STAY;
                return rt_world3d_get_stay_event(world->physics, index);
            }
            index -= stay_count;
            if (actual_phase)
                *actual_phase = RT_GAME3D_COLLISION_EXIT;
            return rt_world3d_get_exit_event(world->physics, index);
        }
    }
}

/// @brief Fetch and wrap the i-th collision event for the phase (NULL if out of range). See header.
/// @param obj World3D runtime handle.
/// @param phase Requested collision phase; unsupported values use ANY ordering.
/// @param index Zero-based event index.
/// @return A newly allocated Game3D Collision3DEvent wrapper, or NULL.
void *rt_game3d_world_collision_event(void *obj, int64_t phase, int64_t index) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.collisionEvent: invalid world");
    if (!world || !world->physics)
        return NULL;
    int64_t actual_phase = phase;
    void *raw_event = game3d_world_raw_collision_event(world, phase, index, &actual_phase);
    return game3d_collision_event_wrap(world, actual_phase, raw_event);
}

/// @brief Clear the physics world's recorded collision-event buffers. See header.
/// @param obj World3D runtime handle; worlds without physics are unchanged.
void rt_game3d_world_clear_collision_events(void *obj) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.clearCollisionEvents: invalid world");
    if (world && world->physics)
        rt_world3d_clear_collision_events(world->physics);
}

/// @brief Get the event phase (defaults to ANY if invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return ENTER, STAY, EXIT, or ANY; corrupt and invalid state normalizes to ANY.
int64_t rt_game3d_collision_event_get_phase(void *obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.get_phase: invalid event");
    return event ? game3d_collision_event_phase_or_any(event->phase) : RT_GAME3D_COLLISION_ANY;
}

/// @brief Get the first participating entity (NULL if unresolved/invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The validated first Entity3D participant, or NULL.
void *rt_game3d_collision_event_get_a(void *obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.get_a: invalid event");
    return event ? game3d_collision_event_entity_ref(event->a) : NULL;
}

/// @brief Get the second participating entity (NULL if unresolved/invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The validated second Entity3D participant, or NULL.
void *rt_game3d_collision_event_get_b(void *obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.get_b: invalid event");
    return event ? game3d_collision_event_entity_ref(event->b) : NULL;
}

/// @brief Get the underlying low-level physics collision event (NULL if invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The validated retained CollisionEvent3D pointer, or NULL.
void *rt_game3d_collision_event_get_raw(void *obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.get_raw: invalid event");
    return game3d_collision_event_raw_ref(event);
}

/// @brief True if either body in the contact is a trigger. See header.
/// @param obj Collision3DEvent runtime handle.
/// @return Non-zero when the raw event is trigger-only, otherwise zero.
int8_t rt_game3d_collision_event_get_is_trigger(void *obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.get_isTrigger: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_is_trigger(raw) : 0;
}

/// @brief Get the relative approach speed at contact (0 if invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The raw event's relative speed, or zero when unavailable.
double rt_game3d_collision_event_get_relative_speed(void *obj) {
    rt_game3d_collision_event *event = game3d_collision_event_checked(
        obj, "Game3D.Collision3DEvent.get_relativeSpeed: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_relative_speed(raw) : 0.0;
}

/// @brief Get the resolution normal impulse magnitude (0 if invalid). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The raw event's normal impulse magnitude, or zero when unavailable.
double rt_game3d_collision_event_get_normal_impulse(void *obj) {
    rt_game3d_collision_event *event = game3d_collision_event_checked(
        obj, "Game3D.Collision3DEvent.get_normalImpulse: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_normal_impulse(raw) : 0.0;
}

/// @brief Get the number of contacts on the wrapped raw event. See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The raw event's contact count, or zero when unavailable.
int64_t rt_game3d_collision_event_get_contact_count(void *obj) {
    rt_game3d_collision_event *event = game3d_collision_event_checked(
        obj, "Game3D.Collision3DEvent.get_contactCount: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_contact_count(raw) : 0;
}

/// @brief Get the first contact point as a Vec3 (origin fallback). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The first contact-point Vec3 or a newly allocated origin fallback.
void *rt_game3d_collision_event_point(void *obj) {
    return rt_game3d_collision_event_contact_point(obj, 0);
}

/// @brief Get the first contact normal as a Vec3 (+Y fallback). See header.
/// @param obj Collision3DEvent runtime handle.
/// @return The first contact-normal Vec3 or a newly allocated +Y fallback.
void *rt_game3d_collision_event_normal(void *obj) {
    return rt_game3d_collision_event_contact_normal(obj, 0);
}

/// @brief Get indexed contact point as a Vec3 (origin fallback). See header.
/// @param obj Collision3DEvent runtime handle.
/// @param index Zero-based contact index forwarded to the raw event.
/// @return The indexed point Vec3 or a newly allocated origin fallback.
void *rt_game3d_collision_event_contact_point(void *obj, int64_t index) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.contactPoint: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_contact_point(raw, index) : rt_vec3_new(0.0, 0.0, 0.0);
}

/// @brief Get indexed contact normal as a Vec3 (+Y fallback). See header.
/// @param obj Collision3DEvent runtime handle.
/// @param index Zero-based contact index forwarded to the raw event.
/// @return The indexed normal Vec3 or a newly allocated +Y fallback.
void *rt_game3d_collision_event_contact_normal(void *obj, int64_t index) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.contactNormal: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_contact_normal(raw, index) : rt_vec3_new(0.0, 1.0, 0.0);
}

/// @brief Get indexed contact separation (0 fallback). See header.
/// @param obj Collision3DEvent runtime handle.
/// @param index Zero-based contact index forwarded to the raw event.
/// @return The indexed signed separation, or zero when the raw event is unavailable.
double rt_game3d_collision_event_contact_separation(void *obj, int64_t index) {
    rt_game3d_collision_event *event = game3d_collision_event_checked(
        obj, "Game3D.Collision3DEvent.contactSeparation: invalid event");
    void *raw = game3d_collision_event_raw_ref(event);
    return raw ? rt_collision_event3d_get_contact_separation(raw, index) : 0.0;
}

/// @brief Given one participant, return the other entity in the contact (NULL if `entity`
///   is not a participant). See header.
/// @param obj Collision3DEvent runtime handle.
/// @param entity_obj Entity3D expected to match one retained participant.
/// @return The opposite validated participant, or NULL when the entity is
///         absent, unresolved, or invalid.
void *rt_game3d_collision_event_other(void *obj, void *entity_obj) {
    rt_game3d_collision_event *event =
        game3d_collision_event_checked(obj, "Game3D.Collision3DEvent.other: invalid event");
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Collision3DEvent.other: entity must be Entity3D");
    void *a = event ? game3d_collision_event_entity_ref(event->a) : NULL;
    void *b = event ? game3d_collision_event_entity_ref(event->b) : NULL;
    if (!event || !entity)
        return NULL;
    if (a == entity)
        return b;
    if (b == entity)
        return a;
    return NULL;
}
