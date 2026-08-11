//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_events.inc  (#included from rt_physics3d.c;
// not a standalone translation unit) Purpose: Physics3D collision-event queries and hit lists
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_events.inc
 * @brief Implements collision-event, contact-point, physics-hit, and hit-list accessors.
 *
 * Accessors validate runtime classes and bounded indices, return safe defaults
 * for invalid input, preserve borrowed body/collider ownership, lazily box
 * packed hit records, and expose committed world contact transitions.
 */

/*==========================================================================
 * Collision event queries
 *=========================================================================*/

// Collision-query accessors — expose the latest frame's contact array
// to Zia. All return safe defaults (0 / NULL / origin Vec3) for NULL
// receivers and out-of-range indices so call sites can iterate without
// extra guards.

/// @brief `World3D.CollisionCount` — number of contacts this frame.
/// @param obj World3D handle to inspect.
/// @return Safely validated current contact count.
int64_t rt_world3d_get_collision_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity) : 0;
}

/// @brief `World3D.CollisionBodyA(i)` — borrowed reference to body A in contact `i`.
/// @param obj World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Borrowed first Body3D handle, or NULL when unavailable.
void *rt_world3d_get_collision_body_a(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    if (index < 0 || index >= contact_count)
        return NULL;
    return ph3d_body_ref_or_null(w->contacts[index].body_a);
}

/// @brief `World3D.CollisionBodyB(i)` — borrowed reference to body B in contact `i`.
/// @param obj World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Borrowed second Body3D handle, or NULL when unavailable.
void *rt_world3d_get_collision_body_b(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    if (index < 0 || index >= contact_count)
        return NULL;
    return ph3d_body_ref_or_null(w->contacts[index].body_b);
}

/// @brief `World3D.CollisionNormal(i)` — fresh `Vec3` of the contact normal.
/// @param obj World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Newly allocated normalized contact Vec3, or the zero vector when unavailable.
void *rt_world3d_get_collision_normal(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    double normal[3];
    if (!w)
        return rt_vec3_new(0, 0, 0);
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    if (index < 0 || index >= contact_count)
        return rt_vec3_new(0, 0, 0);
    ph3d_vec3_copy_normal_or_up(normal, w->contacts[index].normal);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `World3D.CollisionDepth(i)` — penetration depth of contact `i`.
///
/// Returned as a positive value (the internal `separation` is stored
/// negative; this flips it back).
/// @param obj World3D handle to inspect.
/// @param index Zero-based current contact index.
/// @return Non-negative penetration depth, or zero when unavailable.
double rt_world3d_get_collision_depth(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return 0;
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    if (index < 0 || index >= contact_count)
        return 0;
    return ph3d_clamp_nonnegative_finite(-w->contacts[index].separation, 0.0);
}

/// @brief `World3D.CollisionEventCount` — number of collision events this frame.
///
/// Currently aliased to `contact_count` (one event per contact); kept
/// as a separate accessor in case the engine ever splits "live contacts"
/// from "events emitted this step".
/// @param obj World3D handle to inspect.
/// @return Safely validated current collision-event count.
int64_t rt_world3d_get_collision_event_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity) : 0;
}

/// @brief `World3D.CollisionEvent(i)` — boxed `CollisionEvent3D` for contact `i`.
///
/// Returns NULL on out-of-range. The returned event is owned by the
/// caller (unlike the borrowed-reference body accessors).
/// @param obj World3D handle to inspect.
/// @param index Zero-based current event index.
/// @return Retained pooled CollisionEvent3D handle, or NULL when unavailable.
void *rt_world3d_get_collision_event(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t contact_count =
        world3d_contact_count_safe(w->contacts, w->contact_count, w->contact_capacity);
    if (index < 0 || index >= contact_count)
        return NULL;
    return world3d_cached_event_from_contact(&w->collision_event_objects,
                                             &w->collision_event_object_capacity,
                                             index,
                                             &w->contacts[index]);
}

// Enter/Stay/Exit event accessors — populated by the contact-diff in
// `world3d_build_event_buffers`. These mirror the "live contacts"
// accessors above but only fire on transitions (enter, exit) or
// continuations (stay) — useful for trigger events like OnPlayerEnter.

/// @brief `World3D.EnterEventCount` — contacts that began this frame.
/// @param obj World3D handle to inspect.
/// @return Safely validated enter-event count.
int64_t rt_world3d_get_enter_event_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? world3d_contact_count_safe(
                   w->enter_events, w->enter_event_count, w->enter_event_capacity)
             : 0;
}

/// @brief `World3D.EnterEvent(i)` — boxed event for the i-th new contact.
/// @param obj World3D handle to inspect.
/// @param index Zero-based enter-event index.
/// @return Retained pooled CollisionEvent3D handle, or NULL when unavailable.
void *rt_world3d_get_enter_event(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t event_count =
        world3d_contact_count_safe(w->enter_events, w->enter_event_count, w->enter_event_capacity);
    if (index < 0 || index >= event_count)
        return NULL;
    return world3d_cached_event_from_contact(
        &w->enter_event_objects, &w->enter_event_object_capacity, index, &w->enter_events[index]);
}

/// @brief `World3D.StayEventCount` — contacts that persisted from the previous frame.
/// @param obj World3D handle to inspect.
/// @return Safely validated stay-event count.
int64_t rt_world3d_get_stay_event_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? world3d_contact_count_safe(
                   w->stay_events, w->stay_event_count, w->stay_event_capacity)
             : 0;
}

/// @brief `World3D.StayEvent(i)` — boxed event for the i-th continuing contact.
/// @param obj World3D handle to inspect.
/// @param index Zero-based stay-event index.
/// @return Retained pooled CollisionEvent3D handle, or NULL when unavailable.
void *rt_world3d_get_stay_event(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t event_count =
        world3d_contact_count_safe(w->stay_events, w->stay_event_count, w->stay_event_capacity);
    if (index < 0 || index >= event_count)
        return NULL;
    return world3d_cached_event_from_contact(
        &w->stay_event_objects, &w->stay_event_object_capacity, index, &w->stay_events[index]);
}

/// @brief `World3D.ExitEventCount` — contacts that ended this frame.
/// @param obj World3D handle to inspect.
/// @return Safely validated exit-event count.
int64_t rt_world3d_get_exit_event_count(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    return w ? world3d_contact_count_safe(
                   w->exit_events, w->exit_event_count, w->exit_event_capacity)
             : 0;
}

/// @brief `World3D.ExitEvent(i)` — boxed event for the i-th newly-ended contact.
/// @param obj World3D handle to inspect.
/// @param index Zero-based exit-event index.
/// @return Retained pooled CollisionEvent3D handle, or NULL when unavailable.
void *rt_world3d_get_exit_event(void *obj, int64_t index) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return NULL;
    int32_t event_count =
        world3d_contact_count_safe(w->exit_events, w->exit_event_count, w->exit_event_capacity);
    if (index < 0 || index >= event_count)
        return NULL;
    return world3d_cached_event_from_contact(
        &w->exit_event_objects, &w->exit_event_object_capacity, index, &w->exit_events[index]);
}

/// @brief Clear all collision buffers currently visible to event/query APIs.
///
/// This is intentionally stronger than only zeroing this frame's contacts:
/// callers use it after handling events, so the next `Step` should not report
/// stale `stay` or `exit` transitions from the cleared frame.
/// @param obj World3D handle whose visible and historical event state is cleared.
void rt_world3d_clear_collision_events(void *obj) {
    rt_world3d *w = world3d_checked(obj);
    if (!w)
        return;
    world3d_clear_visible_collision_events(w);
}

// Boxed `CollisionEvent3D` field accessors — exposed to Zia as
// properties on the event object. Borrowed references for body /
// collider (the event already holds a retained ref).

/// @brief `CollisionEvent3D.BodyA` — first body in the collision pair.
/// @param obj CollisionEvent3D handle to inspect.
/// @return Borrowed first Body3D handle, or NULL when unavailable.
void *rt_collision_event3d_get_body_a(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_body_ref_or_null(event->body_a) : NULL;
}

/// @brief `CollisionEvent3D.BodyB` — second body in the collision pair.
/// @param obj CollisionEvent3D handle to inspect.
/// @return Borrowed second Body3D handle, or NULL when unavailable.
void *rt_collision_event3d_get_body_b(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_body_ref_or_null(event->body_b) : NULL;
}

/// @brief `CollisionEvent3D.ColliderA` — leaf collider on body A (for compounds).
/// @param obj CollisionEvent3D handle to inspect.
/// @return Borrowed first leaf Collider3D, or NULL when unavailable.
void *rt_collision_event3d_get_collider_a(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_collider_ref_or_null(event->collider_a) : NULL;
}

/// @brief `CollisionEvent3D.ColliderB` — leaf collider on body B (for compounds).
/// @param obj CollisionEvent3D handle to inspect.
/// @return Borrowed second leaf Collider3D, or NULL when unavailable.
void *rt_collision_event3d_get_collider_b(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_collider_ref_or_null(event->collider_b) : NULL;
}

/// @brief `CollisionEvent3D.SurfaceTypeA` — surface tag of body A's leaf collider.
/// @param obj CollisionEvent3D handle to inspect.
/// @return First leaf's surface registry identifier, or zero when unavailable.
int64_t rt_collision_event3d_get_surface_type_a(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event && event->collider_a ? rt_collider3d_get_surface_type(event->collider_a) : 0;
}

/// @brief `CollisionEvent3D.SurfaceTypeB` — surface tag of body B's leaf collider.
/// @param obj CollisionEvent3D handle to inspect.
/// @return Second leaf's surface registry identifier, or zero when unavailable.
int64_t rt_collision_event3d_get_surface_type_b(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event && event->collider_b ? rt_collider3d_get_surface_type(event->collider_b) : 0;
}

/// @brief `CollisionEvent3D.IsTrigger` — whether either party is a trigger.
/// @param obj CollisionEvent3D handle to inspect.
/// @return One for a trigger overlap event, otherwise zero.
int8_t rt_collision_event3d_get_is_trigger(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return (event && event->is_trigger) ? 1 : 0;
}

/// @brief `CollisionEvent3D.ContactCount` — number of points in this contact manifold.
/// @param obj CollisionEvent3D handle to inspect.
/// @return Safely bounded manifold point count.
int64_t rt_collision_event3d_get_contact_count(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return collision_event3d_contact_count_safe(event);
}

/// @brief `CollisionEvent3D.RelativeSpeed` — closing speed along the contact normal.
/// @param obj CollisionEvent3D handle to inspect.
/// @return Non-negative relative normal speed, or zero when unavailable.
double rt_collision_event3d_get_relative_speed(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_clamp_nonnegative_finite(event->relative_speed, 0.0) : 0.0;
}

/// @brief `CollisionEvent3D.NormalImpulse` — magnitude of the resolved normal impulse.
///
/// Useful for damage models or audio: louder/heavier sound on bigger
/// impulses. Always 0 for trigger events (no impulse is applied).
/// @param obj CollisionEvent3D handle to inspect.
/// @return Non-negative accumulated normal impulse, or zero when unavailable.
double rt_collision_event3d_get_normal_impulse(void *obj) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    return event ? ph3d_clamp_nonnegative_finite(event->normal_impulse, 0.0) : 0.0;
}

/// @brief `CollisionEvent3D.Contact(i)` — boxed contact-point sub-object.
///
/// Reconstructs a transient `rt_contact3d` from the event's stored manifold
/// point and boxes it via `contact_point3d_new_from_contact`.
/// @param obj CollisionEvent3D handle to inspect.
/// @param index Zero-based manifold point index.
/// @return Newly allocated ContactPoint3D handle, or NULL when unavailable.
void *rt_collision_event3d_get_contact(void *obj, int64_t index) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    rt_contact3d contact;
    int32_t contact_count = collision_event3d_contact_count_safe(event);
    if (!event || index < 0 || index >= contact_count)
        return NULL;
    memset(&contact, 0, sizeof(contact));
    contact.body_a = event->body_a;
    contact.body_b = event->body_b;
    contact.collider_a = event->collider_a;
    contact.collider_b = event->collider_b;
    ph3d_vec3_copy_state(contact.point, event->points[index]);
    ph3d_vec3_copy_normal_or_up(contact.normal, event->normals[index]);
    contact.separation = ph3d_sanitize_contact_scalar(event->separations[index]);
    contact3d_init_single_point(&contact, contact.point, contact.normal, contact.separation);
    return contact_point3d_new_from_contact(&contact);
}

/// @brief `CollisionEvent3D.ContactPoint(i)` — fresh `Vec3` for the contact point.
/// @param obj CollisionEvent3D handle to inspect.
/// @param index Zero-based manifold point index.
/// @return Newly allocated world point Vec3, or the origin when unavailable.
void *rt_collision_event3d_get_contact_point(void *obj, int64_t index) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    double point[3];
    int32_t contact_count = collision_event3d_contact_count_safe(event);
    if (!event || index < 0 || index >= contact_count)
        return rt_vec3_new(0.0, 0.0, 0.0);
    ph3d_vec3_copy_state(point, event->points[index]);
    return rt_vec3_new(point[0], point[1], point[2]);
}

/// @brief `CollisionEvent3D.ContactNormal(i)` — fresh `Vec3` for the contact normal.
/// @param obj CollisionEvent3D handle to inspect.
/// @param index Zero-based manifold point index.
/// @return Newly allocated unit-normal Vec3, or positive Y when unavailable.
void *rt_collision_event3d_get_contact_normal(void *obj, int64_t index) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    double normal[3];
    int32_t contact_count = collision_event3d_contact_count_safe(event);
    if (!event || index < 0 || index >= contact_count)
        return rt_vec3_new(0.0, 1.0, 0.0);
    ph3d_vec3_copy_normal_or_up(normal, event->normals[index]);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `CollisionEvent3D.ContactSeparation(i)` — signed separation distance.
///
/// Negative means penetration; zero or positive means touching/just-separated.
/// @param obj CollisionEvent3D handle to inspect.
/// @param index Zero-based manifold point index.
/// @return Finite signed separation, or zero when unavailable.
double rt_collision_event3d_get_contact_separation(void *obj, int64_t index) {
    rt_collision_event3d_obj *event = collision_event3d_checked(obj);
    int32_t contact_count = collision_event3d_contact_count_safe(event);
    if (!event || index < 0 || index >= contact_count)
        return 0.0;
    return ph3d_sanitize_contact_scalar(event->separations[index]);
}

/// @brief `ContactPoint3D.Point` — world-space contact location.
/// @param obj ContactPoint3D handle to inspect.
/// @return Newly allocated world-position Vec3, or the origin when invalid.
void *rt_contact_point3d_get_point(void *obj) {
    rt_contact_point3d_obj *contact = contact_point3d_checked(obj);
    double point[3];
    if (!contact)
        return rt_vec3_new(0.0, 0.0, 0.0);
    ph3d_vec3_copy_state(point, contact->point);
    return rt_vec3_new(point[0], point[1], point[2]);
}

/// @brief `ContactPoint3D.Normal` — world-space contact normal (A→B).
/// @param obj ContactPoint3D handle to inspect.
/// @return Newly allocated unit-normal Vec3, or positive Y when invalid.
void *rt_contact_point3d_get_normal(void *obj) {
    rt_contact_point3d_obj *contact = contact_point3d_checked(obj);
    double normal[3];
    if (!contact)
        return rt_vec3_new(0.0, 1.0, 0.0);
    ph3d_vec3_copy_normal_or_up(normal, contact->normal);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `ContactPoint3D.Separation` — signed gap (negative = penetrating).
/// @param obj ContactPoint3D handle to inspect.
/// @return Finite signed separation, or zero when invalid.
double rt_contact_point3d_get_separation(void *obj) {
    rt_contact_point3d_obj *contact = contact_point3d_checked(obj);
    return contact ? ph3d_sanitize_contact_scalar(contact->separation) : 0.0;
}

// PhysicsHit3D field accessors — exposed as Zia properties on the
// boxed query result. All borrowed references / safe defaults on NULL.

/// @brief `PhysicsHit3D.Distance` — world-space distance from query origin.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Non-negative hit distance, or zero when invalid.
double rt_physics_hit3d_get_distance(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return hit ? ph3d_clamp_nonnegative_finite(hit->distance, 0.0) : 0.0;
}

/// @brief `PhysicsHit3D.Body` — the body that was hit.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Borrowed hit Body3D handle, or NULL when unavailable.
void *rt_physics_hit3d_get_body(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return hit ? ph3d_body_ref_or_null(hit->body) : NULL;
}

/// @brief `PhysicsHit3D.Collider` — the leaf collider that was hit.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Borrowed hit leaf Collider3D, or NULL when unavailable.
void *rt_physics_hit3d_get_collider(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return hit ? ph3d_collider_ref_or_null(hit->collider) : NULL;
}

/// @brief `PhysicsHit3D.SurfaceType` — surface tag of the hit (leaf) collider.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Hit leaf's surface registry identifier, or zero when unavailable.
int64_t rt_physics_hit3d_get_surface_type(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return hit && hit->collider ? rt_collider3d_get_surface_type(hit->collider) : 0;
}

/// @brief `PhysicsHit3D.Point` — world-space hit location.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Newly allocated world-position Vec3, or the origin when invalid.
void *rt_physics_hit3d_get_point(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    double point[3];
    if (!hit)
        return rt_vec3_new(0.0, 0.0, 0.0);
    ph3d_vec3_copy_state(point, hit->point);
    return rt_vec3_new(point[0], point[1], point[2]);
}

/// @brief `PhysicsHit3D.Normal` — world-space surface normal at the hit.
/// @param obj PhysicsHit3D handle to inspect.
/// @return Newly allocated unit-normal Vec3, or positive Y when invalid.
void *rt_physics_hit3d_get_normal(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    double normal[3];
    if (!hit)
        return rt_vec3_new(0.0, 1.0, 0.0);
    ph3d_vec3_copy_normal_or_up(normal, hit->normal);
    return rt_vec3_new(normal[0], normal[1], normal[2]);
}

/// @brief `PhysicsHit3D.Fraction` — t along the swept path (0..1).
/// @param obj PhysicsHit3D handle to inspect.
/// @return Sanitized parametric fraction in the inclusive range zero to one.
double rt_physics_hit3d_get_fraction(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return hit ? ph3d_sanitize_fraction_value(hit->fraction) : 0.0;
}

/// @brief `PhysicsHit3D.StartedPenetrating` — true if the query started inside the collider.
/// @param obj PhysicsHit3D handle to inspect.
/// @return One for initial penetration, otherwise zero.
int8_t rt_physics_hit3d_get_started_penetrating(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return (hit && hit->started_penetrating) ? 1 : 0;
}

/// @brief `PhysicsHit3D.IsTrigger` — true if the hit collider belongs to a trigger body.
/// @param obj PhysicsHit3D handle to inspect.
/// @return One when the hit body is a trigger, otherwise zero.
int8_t rt_physics_hit3d_get_is_trigger(void *obj) {
    rt_physics_hit3d_obj *hit = physics_hit3d_checked(obj);
    return (hit && hit->is_trigger) ? 1 : 0;
}

/// @brief `PhysicsHitList3D.Count` — number of hits in the list.
/// @param obj PhysicsHitList3D handle to inspect.
/// @return Safely validated retained hit count.
int64_t rt_physics_hit_list3d_get_count(void *obj) {
    rt_physics_hit_list3d_obj *list = physics_hit_list3d_checked(obj);
    return physics_hit_list3d_count_safe(list);
}

/// @brief `PhysicsHitList3D.TotalCount` — full number of hits found before list truncation.
/// @param obj PhysicsHitList3D handle to inspect.
/// @return Non-negative total match count, never smaller than retained count.
int64_t rt_physics_hit_list3d_get_total_count(void *obj) {
    rt_physics_hit_list3d_obj *list = physics_hit_list3d_checked(obj);
    if (!list)
        return 0;
    int64_t count = physics_hit_list3d_count_safe(list);
    if (list->total_count < count)
        return count;
    return list->total_count > 0 ? list->total_count : 0;
}

/// @brief `PhysicsHitList3D.Truncated` — true when more hits matched than the list stores.
/// @param obj PhysicsHitList3D handle to inspect.
/// @return One when results were truncated, otherwise zero.
int8_t rt_physics_hit_list3d_get_truncated(void *obj) {
    rt_physics_hit_list3d_obj *list = physics_hit_list3d_checked(obj);
    return (list && list->truncated) ? 1 : 0;
}

/// @brief `PhysicsHitList3D[i]` — borrowed `PhysicsHit3D` reference.
/// @details The first access lazily boxes the packed raw hit and atomically
/// publishes the object so concurrent readers converge on a single retained slot.
/// @param obj PhysicsHitList3D handle to inspect.
/// @param index Zero-based retained hit index.
/// @return Borrowed lazily materialized PhysicsHit3D, or NULL when unavailable.
void *rt_physics_hit_list3d_get(void *obj, int64_t index) {
    rt_physics_hit_list3d_obj *list = physics_hit_list3d_checked(obj);
    int64_t count = physics_hit_list3d_count_safe(list);
    if (!list || index < 0 || index >= count)
        return NULL;
    void *item = rt_atomic_load_ptr((void *const volatile *)&list->items[index], __ATOMIC_ACQUIRE);
    if (item)
        return rt_g3d_checked_or_null(item, RT_G3D_PHYSICSHIT3D_CLASS_ID);
    if (!list->raw_hits)
        return NULL;
    void *created = physics_hit3d_new(&list->raw_hits[index]);
    if (!created)
        return NULL;
    void *expected = NULL;
    // The intermediate `void *` keeps -Wcast-qual quiet about adding
    // `volatile` below the first pointer level; the address is unchanged.
    if (!rt_atomic_compare_exchange_ptr((void *volatile *)(void *)&list->items[index],
                                        &expected,
                                        created,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE)) {
        if (rt_obj_release_check0(created))
            rt_obj_free(created);
        item = expected;
    } else {
        item = created;
    }
    return rt_g3d_checked_or_null(item, RT_G3D_PHYSICSHIT3D_CLASS_ID);
}
