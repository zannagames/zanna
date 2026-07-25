//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_animcontroller3d_internals.inc  (#included from
// rt_animcontroller3d.c; not a standalone translation unit) Purpose: AnimController3D
// state/transition/event/layer tables and repair helpers
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines AnimController3D private storage and state-machine helpers.
/// @details This fragment is included into `rt_animcontroller3d.c`; it is not a
///          standalone translation unit. It centralizes bounded storage,
///          defensive invariant repair, state lookup and event queuing, layer
///          selection/masking, matrix and quaternion utilities, and direct
///          animation-channel sampling used by the other controller fragments.

/// Number of animation layers, including the base layer at index zero.
#define RT_ANIM_CONTROLLER3D_MAX_LAYERS 4
/// Bytes per queued event-name slot, including its NUL terminator.
#define RT_ANIM_CONTROLLER3D_EVENT_NAME_MAX 64
/// Bytes per stored state name, including its NUL terminator.
#define RT_ANIM_CONTROLLER3D_STATE_NAME_MAX 64
/// Maximum number of pending event names retained by the ring buffer.
#define RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX 64
/// Largest finite distance metadata accepted by animation LOD configuration.
#define RT_ANIM_CONTROLLER3D_LOD_DISTANCE_MAX 1000000.0
/// Largest animation evaluation frequency accepted by the LOD gate.
#define RT_ANIM_CONTROLLER3D_LOD_RATE_MAX 240.0
/// Safety bound on loop cycles compensated during one root-motion update.
#define RT_ANIM_CONTROLLER3D_ROOT_MOTION_CYCLE_MAX 1024

/// @brief One named controller state and its retained animation clip.
typedef struct {
    /// Canonical bounded lookup name.
    char name[RT_ANIM_CONTROLLER3D_STATE_NAME_MAX];
    /// Retained Animation3D played when this state is selected.
    rt_animation3d *animation;
    /// Signed per-state playback multiplier.
    float speed;
    /// Nonzero when playback wraps at the clip duration.
    int8_t looping;
} anim_controller3d_state_t;

/// @brief Directional state transition with a default blend duration.
typedef struct {
    /// Source index in the controller's state table.
    int32_t from_state;
    /// Destination index in the controller's state table.
    int32_t to_state;
    /// Sanitized nonnegative blend duration in seconds.
    float blend_seconds;
} anim_controller3d_transition_t;

/// @brief Scheduled named event on one state's playback timeline.
typedef struct {
    /// Owning index in the controller's state table.
    int32_t state_index;
    /// Clamped event timestamp in seconds.
    float time_seconds;
    /// Bounded event name copied into pending queue slots when fired.
    char name[RT_ANIM_CONTROLLER3D_EVENT_NAME_MAX];
} anim_controller3d_event_t;

/// @brief Playback, transition, weight, and bone-mask state for one layer.
typedef struct {
    /// Retained AnimPlayer3D dedicated to this layer.
    rt_anim_player3d *player;
    /// Selected state-table index, or `-1`.
    int32_t current_state;
    /// Prior state-table index used during transition reporting, or `-1`.
    int32_t previous_state;
    /// Elapsed time within the current transition.
    float transition_time;
    /// Total transition duration in seconds.
    float transition_duration;
    /// Nonzero while transition time is below duration.
    int8_t transitioning;
    /// Nonzero to composite a bind-pose-relative delta instead of replacement.
    int8_t additive;
    /// Layer contribution in `[0,1]`; the base is always `1`.
    float weight;
    /// Root of the included skeleton subtree, or `-1` for every bone.
    int32_t mask_root_bone;
    /// Bone count for which @ref mask_bits was last built.
    int32_t mask_bone_count_seen;
    /// One inclusion flag per supported skeleton bone.
    uint8_t mask_bits[VGFX3D_MAX_SKELETON_BONES];
} anim_controller3d_layer_t;

/// @brief Private GC-managed AnimController3D object representation.
typedef struct {
    /// Runtime object header / virtual table slot.
    void *vptr;
    /// Retained Skeleton3D shared by states, layers, blend tree, and IK.
    rt_skeleton3d *skeleton;

    /// Geometrically grown array of registered states.
    anim_controller3d_state_t *states;
    /// Logical number of initialized state entries.
    int32_t state_count;
    /// Allocated state-array capacity.
    int32_t state_capacity;
    /// Open-addressing hash slots for large state tables.
    uint64_t *state_name_hashes;
    /// State indexes corresponding to @ref state_name_hashes.
    int32_t *state_name_indices;
    /// Power-of-two slot count in each name-index array.
    int32_t state_name_index_capacity;
    /// Nonzero when the name index must be rebuilt before use.
    int8_t state_name_index_dirty;

    /// Geometrically grown array of directional transitions.
    anim_controller3d_transition_t *transitions;
    /// Logical number of initialized transitions.
    int32_t transition_count;
    /// Allocated transition-array capacity.
    int32_t transition_capacity;

    /// Geometrically grown array of scheduled events.
    anim_controller3d_event_t *events;
    /// Logical number of initialized event entries.
    int32_t event_count;
    /// Allocated event-array capacity.
    int32_t event_capacity;

    /// Fixed base-plus-overlay layer storage.
    anim_controller3d_layer_t layers[RT_ANIM_CONTROLLER3D_MAX_LAYERS];
    /// Optional retained BlendTree3D pose source.
    void *blend_tree;
    /// Optional retained IKSolver3D post-process.
    void *ik_solver;

    /// Current skinning matrices (`global * inverse_bind`), 16 floats per bone.
    float *final_palette;
    /// Current model-space global matrices, 16 floats per bone.
    float *final_globals;
    /// Previous update's skinning matrices for temporal rendering.
    float *prev_final_palette;
    /// Nonzero once @ref prev_final_palette contains a valid snapshot.
    int8_t has_prev_final_palette;
    /// Accumulated model-space root translation since the last consume.
    double root_motion_delta[3];
    /// Accumulated normalized root rotation since the last consume.
    double root_motion_rotation[4];
    /// Caller-provided distance metadata associated with update-rate LOD.
    double animation_lod_distance;
    /// Enabled batched evaluation frequency, or zero.
    double animation_lod_rate_hz;
    /// Unconsumed time carried by the animation LOD gate.
    double animation_lod_accum;
    /// Positive leading-bone evaluation count, or zero to disable bone LOD.
    int32_t animation_lod_max_bones; /* >0 freezes bones at/after this index to bind-local (LOD) */
    /// Root-motion source bone, or `-1` when extraction is disabled.
    int32_t root_motion_bone;

    /// Fixed FIFO ring storage for fired event names.
    char event_queue[RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX][RT_ANIM_CONTROLLER3D_EVENT_NAME_MAX];
    /// Index of the oldest pending event slot.
    int32_t event_queue_head;
    /// Number of valid pending event slots.
    int32_t event_queue_count;
    /// Lifetime count of pending events dropped on queue overflow.
    int64_t events_dropped;
    /// Reusable scratch for controller_compute_final_palette's per-call local-matrix buffer,
    /// sized like the palettes (VGFX3D_MAX_BONES*16 floats). Owned per-controller (one updater
    /// at a time) so it replaces a ~16 KB stack array on the parallel-animator worker threads.
    /// Appended at the struct's end so white-box test layout mirrors of the earlier fields keep
    /// their offsets.
    float *blend_locals_scratch;
    /// Lazily grown scratch for controller_sample_state_global_matrix (bone-socket /
    /// root-motion queries, which run per frame for socketed nodes): holds
    /// sample_scratch_bones*32 floats (locals + globals) so repeated samples stop
    /// paying a calloc/free each call. Appended at the struct's end (see above).
    float *sample_scratch;
    /// Bone capacity currently available in @ref sample_scratch.
    int32_t sample_scratch_bones;
} rt_anim_controller3d;

/// @brief Validate that @p obj is a live AnimController3D handle, returning NULL otherwise.
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed controller pointer, or `NULL` on class/liveness
///         mismatch.
static rt_anim_controller3d *anim_controller3d_checked(void *obj) {
    return (rt_anim_controller3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
}

/// @brief Validate that @p obj is a live Skeleton3D handle, returning NULL otherwise.
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed skeleton pointer, or `NULL` on class/liveness
///         mismatch.
static rt_skeleton3d *skeleton3d_checked(void *obj) {
    return (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
}

/// @brief Validate that @p obj is a live Animation3D handle, returning NULL otherwise.
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed animation pointer, or `NULL` on class/liveness
///         mismatch.
static rt_animation3d *animation3d_checked(void *obj) {
    return (rt_animation3d *)rt_g3d_checked_or_null(obj, RT_G3D_ANIMATION3D_CLASS_ID);
}

/// @brief Narrow a double to float, returning `fallback` for non-finite input and
///   saturating to ±FLT_MAX on overflow (keeps NaN/Inf out of the float pipeline).
/// @param[in] value Double-precision value to narrow.
/// @param[in] fallback Finite replacement used for NaN or infinity.
/// @return Narrowed finite float, the fallback, or the appropriate signed
///         `FLT_MAX` saturation value.
static float controller_clamp_to_float(double value, float fallback) {
    if (!isfinite(value))
        return fallback;
    if (value > (double)FLT_MAX)
        return FLT_MAX;
    if (value < -(double)FLT_MAX)
        return -FLT_MAX;
    return (float)value;
}

/// @brief Narrow a double to a non-negative float; non-finite or ≤0 yields 0, overflow
///   saturates to FLT_MAX. For durations/speeds that must stay ≥ 0.
/// @param[in] value Value to normalize and narrow.
/// @return Finite float in `[0,FLT_MAX]`.
static float controller_clamp_nonnegative_float(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0f;
    if (value > (double)FLT_MAX)
        return FLT_MAX;
    return (float)value;
}

/// @brief Drop one reference from a slot, free if it was the last, then NULL out the slot.
/// @details Used by the finalizer and slot-replacement paths so the same
///          retain-then-release discipline is shared across every animated-
///          object pointer the controller owns. NULL slot or NULL contents
///          are no-ops.
/// @param[in,out] slot Address of a retained object slot to release and clear.
static void controller_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained controller-owned object only when its class still matches.
/// @details A non-null object of the wrong class is treated as an unowned
///          corrupt slot and cleared without releasing through the expected
///          class path.
/// @param[in,out] slot Address of the retained object slot.
/// @param[in] class_id Expected graphics-runtime class identifier.
static void controller_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    controller_release_ref(slot);
}

/// @brief Borrow the controller skeleton only if the private slot still has the right class.
/// @details A stale or wrong-class slot is cleared defensively.
/// @param[in,out] controller Controller whose skeleton reference to validate.
/// @return Borrowed typed skeleton pointer, or `NULL`.
static rt_skeleton3d *controller_skeleton_ref(rt_anim_controller3d *controller) {
    if (!controller || !controller->skeleton)
        return NULL;
    if (!rt_g3d_has_class(controller->skeleton, RT_G3D_SKELETON3D_CLASS_ID)) {
        controller->skeleton = NULL;
        return NULL;
    }
    return controller->skeleton;
}

/// @brief Return a C string only for a valid runtime string handle.
/// @param[in] name Borrowed runtime string handle.
/// @return Borrowed NUL-terminated byte view, or `NULL` for a null or invalid
///         string handle.
static const char *controller_string_cstr_or_null(rt_string name) {
    if (!name || !rt_string_is_handle(name))
        return NULL;
    return rt_string_cstr(name);
}

/// @brief Copy an `rt_string` into a fixed-size character buffer, NUL-terminating.
/// @details Truncates if the source exceeds `cap - 1` bytes; produces an
///          empty string when `name` is NULL or the buffer is zero-sized.
///          Used to snapshot state and event names into the fixed-size
///          arrays in `anim_controller3d_state_t` / `_event_t`.
/// @param[out] dst Destination character buffer; may be `NULL`.
/// @param[in] cap Destination capacity in bytes, including the terminator.
/// @param[in] name Borrowed runtime string to copy.
static void controller_copy_name(char *dst, size_t cap, rt_string name) {
    const char *src = controller_string_cstr_or_null(name);
    size_t len = src ? strlen(src) : 0;
    if (!dst || cap == 0) {
        return;
    }
    if (len >= cap)
        len = cap - 1;
    if (len > 0)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

/// @brief Geometric-grow a heap buffer to hold at least `need` elements.
/// @details Doubles capacity (or jumps to `need`, whichever is larger) and
///          `realloc`s in place. Used by the states / transitions / events
///          arrays — capacity starts at 0, jumps to 4, then doubles.
/// @param[in,out] buffer Address of the heap-buffer pointer updated by
///                       successful `realloc`.
/// @param[in,out] capacity Current element capacity and success output.
/// @param[in] need Minimum required element count.
/// @param[in] elem_size Size of one element in bytes.
/// @return 1 on success (buffer + capacity updated); 0 on allocation failure
///         (buffer + capacity left untouched).
static int controller_grow_array(void **buffer, int32_t *capacity, int32_t need, size_t elem_size) {
    int32_t new_capacity;
    void *grown;
    if (!buffer || !capacity || need <= 0 || elem_size == 0)
        return 0;
    if (*capacity < 0)
        return 0;
    if (*capacity >= need)
        return 1;
    if (*capacity > INT32_MAX / 2)
        new_capacity = need;
    else
        new_capacity = *capacity > 0 ? (*capacity * 2) : 4;
    if (new_capacity < need)
        new_capacity = need;
    if ((size_t)new_capacity > SIZE_MAX / elem_size)
        return 0;
    grown = realloc(*buffer, (size_t)new_capacity * elem_size);
    if (!grown)
        return 0;
    *buffer = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Clamp a (count, capacity) pair to a safe element count: 0 when the array or sizes
///   are invalid, otherwise min(count, capacity).
/// @param[in] items Array pointer whose presence gates readability.
/// @param[in] count Claimed logical count.
/// @param[in] capacity Claimed allocation capacity.
/// @return Zero for invalid storage metadata; otherwise `min(count,capacity)`.
static int32_t controller_clamped_array_count(const void *items, int32_t count, int32_t capacity) {
    if (!items || count <= 0 || capacity <= 0)
        return 0;
    if (count > capacity)
        return capacity;
    return count;
}

/// @brief Number of animation states safe to read directly (live count clamped to capacity).
/// @param[in] controller Controller to inspect.
/// @return Safe readable state count, or zero.
static int32_t controller_safe_state_count(const rt_anim_controller3d *controller) {
    return controller ? controller_clamped_array_count(
                            controller->states, controller->state_count, controller->state_capacity)
                      : 0;
}

/// @brief Number of transitions safe to read directly (live count clamped to capacity).
/// @param[in] controller Controller to inspect.
/// @return Safe readable transition count, or zero.
static int32_t controller_safe_transition_count(const rt_anim_controller3d *controller) {
    return controller ? controller_clamped_array_count(controller->transitions,
                                                       controller->transition_count,
                                                       controller->transition_capacity)
                      : 0;
}

/// @brief Number of events safe to read directly (live count clamped to capacity).
/// @param[in] controller Controller to inspect.
/// @return Safe readable scheduled-event count, or zero.
static int32_t controller_safe_event_count(const rt_anim_controller3d *controller) {
    return controller ? controller_clamped_array_count(
                            controller->events, controller->event_count, controller->event_capacity)
                      : 0;
}

/// @brief Normalize the state-table count/capacity invariants: reset both when the array is
///   absent, clamp a negative capacity, and re-clamp the count (marks the name index dirty).
/// @param[in,out] controller Controller whose state metadata to repair.
static void controller_repair_state_table(rt_anim_controller3d *controller) {
    if (!controller)
        return;
    if (!controller->states) {
        controller->state_count = 0;
        controller->state_capacity = 0;
        controller->state_name_index_dirty = 1;
        return;
    }
    if (controller->state_capacity < 0)
        controller->state_capacity = 0;
    controller->state_count = controller_safe_state_count(controller);
}

/// @brief Normalize the transition-table count/capacity invariants (defensive clamp).
/// @param[in,out] controller Controller whose transition metadata to repair.
static void controller_repair_transition_table(rt_anim_controller3d *controller) {
    if (!controller)
        return;
    if (!controller->transitions) {
        controller->transition_count = 0;
        controller->transition_capacity = 0;
        return;
    }
    if (controller->transition_capacity < 0)
        controller->transition_capacity = 0;
    controller->transition_count = controller_safe_transition_count(controller);
}

/// @brief Normalize the event-table count/capacity invariants (defensive clamp).
/// @param[in,out] controller Controller whose scheduled-event metadata to
///                           repair.
static void controller_repair_event_table(rt_anim_controller3d *controller) {
    if (!controller)
        return;
    if (!controller->events) {
        controller->event_count = 0;
        controller->event_capacity = 0;
        return;
    }
    if (controller->event_capacity < 0)
        controller->event_capacity = 0;
    controller->event_count = controller_safe_event_count(controller);
}

/// @brief Clamp every animation layer's current/previous state indices, transition timers,
///   and blend weight into valid ranges (layer 0 is forced to full weight).
/// @param[in,out] controller Controller whose fixed layer records to sanitize.
static void controller_repair_layer_state_indices(rt_anim_controller3d *controller) {
    int32_t state_count;
    if (!controller)
        return;
    state_count = controller_safe_state_count(controller);
    for (int32_t layer_index = 0; layer_index < RT_ANIM_CONTROLLER3D_MAX_LAYERS; layer_index++) {
        anim_controller3d_layer_t *layer = &controller->layers[layer_index];
        if (layer->current_state < 0 || layer->current_state >= state_count) {
            layer->current_state = -1;
            layer->previous_state = -1;
            layer->transitioning = 0;
            layer->transition_time = 0.0f;
            layer->transition_duration = 0.0f;
        } else if (layer->previous_state < 0 || layer->previous_state >= state_count) {
            layer->previous_state = -1;
        }
        if (!isfinite((double)layer->transition_time) || layer->transition_time < 0.0f)
            layer->transition_time = 0.0f;
        if (!isfinite((double)layer->transition_duration) || layer->transition_duration < 0.0f)
            layer->transition_duration = 0.0f;
        if (layer->transition_duration <= 0.0f) {
            layer->transitioning = 0;
            layer->transition_time = 0.0f;
        } else if (layer->transition_time >= layer->transition_duration) {
            layer->transitioning = 0;
            layer->transition_time = layer->transition_duration;
        }
        if (!isfinite((double)layer->weight) || layer->weight < 0.0f)
            layer->weight = layer_index == 0 ? 1.0f : 0.0f;
        else if (layer->weight > 1.0f)
            layer->weight = 1.0f;
        if (layer_index == 0)
            layer->weight = 1.0f;
    }
}

/// @brief True if @p value is a positive power of two.
/// @param[in] value Signed integer to classify.
/// @return Nonzero only for positive powers of two.
static int controller_is_power_of_two_i32(int32_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

/// @brief Number of skeleton bones safe to read (delegates to skeleton3d_safe_bone_count).
/// @param[in] skeleton Skeleton to inspect.
/// @return Sanitized readable bone count.
static int32_t controller_safe_bone_count(const rt_skeleton3d *skeleton) {
    return skeleton3d_safe_bone_count(skeleton);
}

/// @brief Clamp the event ring-buffer's head/tail/size indices back into range after any
///   potential corruption, keeping the queue traversable.
/// @param[in,out] controller Controller whose queue metadata to sanitize.
static void controller_sanitize_event_queue(rt_anim_controller3d *controller) {
    if (!controller)
        return;
    if (controller->event_queue_head < 0 ||
        controller->event_queue_head >= RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX)
        controller->event_queue_head = 0;
    if (controller->event_queue_count < 0)
        controller->event_queue_count = 0;
    if (controller->event_queue_count > RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX)
        controller->event_queue_count = RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX;
}

/// @brief FNV-1a 64-bit hash of a state name, used as the key in the open-addressing
///   name index. Returns 0 for a NULL name; otherwise a non-zero hash (a hash that would
///   be 0 is remapped to 1 so 0 stays reserved as the empty-slot sentinel).
/// @param[in] name Borrowed NUL-terminated byte string.
/// @return Zero for `NULL`; otherwise a nonzero FNV-1a hash.
static uint64_t controller_hash_name_cstr(const char *name) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!name)
        return 0u;
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1u;
}

/// @brief Rebuild the open-addressing (linear-probe) name index that maps state-name
///   hashes to state-array slots, growing to a power-of-two capacity sized to keep the
///   load factor below 50%, and clear the dirty flag.
/// @param[in,out] controller Controller whose auxiliary index to rebuild.
/// @return 1 on success, 0 if `controller` is NULL or an index allocation fails.
static int controller_rebuild_state_name_index(rt_anim_controller3d *controller) {
    int32_t capacity = 16;
    int32_t state_count;
    uint64_t *hashes;
    int32_t *indices;
    if (!controller)
        return 0;
    controller_repair_state_table(controller);
    state_count = controller->state_count;
    while (capacity < state_count * 2) {
        if (capacity > INT32_MAX / 2)
            return 0;
        capacity *= 2;
    }
    if (capacity != controller->state_name_index_capacity) {
        hashes = (uint64_t *)calloc((size_t)capacity, sizeof(*hashes));
        indices = (int32_t *)malloc((size_t)capacity * sizeof(*indices));
        if (!hashes || !indices) {
            free(hashes);
            free(indices);
            return 0;
        }
        free(controller->state_name_hashes);
        free(controller->state_name_indices);
        controller->state_name_hashes = hashes;
        controller->state_name_indices = indices;
        controller->state_name_index_capacity = capacity;
    } else {
        memset(controller->state_name_hashes,
               0,
               (size_t)capacity * sizeof(*controller->state_name_hashes));
    }
    for (int32_t i = 0; i < capacity; ++i)
        controller->state_name_indices[i] = -1;
    for (int32_t i = 0; i < state_count; ++i) {
        uint64_t hash = controller_hash_name_cstr(controller->states[i].name);
        int32_t mask = capacity - 1;
        for (int32_t probe = 0; probe < capacity; ++probe) {
            int32_t slot = (int32_t)((hash + (uint64_t)probe) & (uint64_t)mask);
            if (controller->state_name_hashes[slot] == 0u) {
                controller->state_name_hashes[slot] = hash;
                controller->state_name_indices[slot] = i;
                break;
            }
        }
    }
    controller->state_name_index_dirty = 0;
    return 1;
}

/// @brief Find a state by NUL-terminated name (the allocation-free core lookup).
/// @details Shared by the rt_string entry point and C-internal callers (e.g. the
///   per-frame combat window check) that already hold a plain C string, so hot
///   paths need not allocate a transient rt_string per query.
/// @param[in,out] controller Controller whose repaired state table and optional
///                           hash index to search.
/// @param[in] target Borrowed exact canonical NUL-terminated name.
/// @return State index, or -1 if `target` is NULL/empty or no match.
static int32_t controller_find_state_cstr(rt_anim_controller3d *controller, const char *target) {
    int32_t state_count;
    if (!controller || !target || target[0] == '\0')
        return -1;
    controller_repair_state_table(controller);
    state_count = controller->state_count;
    if (state_count >= 16) {
        uint64_t hash = controller_hash_name_cstr(target);
        if ((controller->state_name_index_dirty || controller->state_name_index_capacity <= 0 ||
             !controller_is_power_of_two_i32(controller->state_name_index_capacity) ||
             !controller->state_name_hashes || !controller->state_name_indices) &&
            !controller_rebuild_state_name_index(controller)) {
            goto linear_scan;
        }
        if (controller->state_name_hashes && controller->state_name_indices) {
            int32_t mask = controller->state_name_index_capacity - 1;
            for (int32_t probe = 0; probe < controller->state_name_index_capacity; ++probe) {
                int32_t slot = (int32_t)((hash + (uint64_t)probe) & (uint64_t)mask);
                int32_t index;
                if (controller->state_name_hashes[slot] == 0u)
                    return -1;
                index = controller->state_name_indices[slot];
                if (controller->state_name_hashes[slot] == hash && index >= 0 &&
                    index < state_count && strcmp(controller->states[index].name, target) == 0)
                    return index;
            }
            return -1;
        }
    }
linear_scan:
    for (int32_t i = 0; i < state_count; i++) {
        if (strcmp(controller->states[i].name, target) == 0)
            return i;
    }
    return -1;
}

/// @brief Find a state by name, using an open-addressed name index for larger controllers.
/// @details Copies and truncates the runtime string through the same
///          canonicalization used when states are inserted.
/// @param[in,out] controller Controller whose state table to search.
/// @param[in] name Borrowed runtime state name.
/// @return State index, or -1 if `name` is NULL/empty or no match.
static int32_t controller_find_state(rt_anim_controller3d *controller, rt_string name) {
    char target_buf[RT_ANIM_CONTROLLER3D_STATE_NAME_MAX];
    if (!controller || !name)
        return -1;
    controller_copy_name(target_buf, sizeof(target_buf), name);
    return controller_find_state_cstr(controller, target_buf);
}

/// @brief Linear-scan the transition table for an exact `(from, to)` pair.
/// @param[in] controller Controller whose sanitized transition range to scan.
/// @param[in] from_state Source state index.
/// @param[in] to_state Destination state index.
/// @return Transition index, or -1 if no match is registered.
static int32_t controller_find_transition(const rt_anim_controller3d *controller,
                                          int32_t from_state,
                                          int32_t to_state) {
    if (!controller)
        return -1;
    for (int32_t i = 0, count = controller_safe_transition_count(controller); i < count; i++) {
        if (controller->transitions[i].from_state == from_state &&
            controller->transitions[i].to_state == to_state)
            return i;
    }
    return -1;
}

/// @brief Push an event name onto the event-queue ring buffer.
/// @details The queue is bounded at `RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX`.
///          When full, the oldest entry is dropped (head advances) before
///          the new one is appended — the rationale is "lose the stalest
///          event, never silently drop the freshest one." NULL or empty
///          event names are silently ignored. Names longer than the
///          fixed-size slot are truncated (NUL-terminated within `slot`).
/// @param[in,out] controller Controller whose pending-event ring to extend.
/// @param[in] event_name Borrowed nonempty NUL-terminated name.
static void controller_enqueue_event(rt_anim_controller3d *controller, const char *event_name) {
    int32_t slot;
    size_t len;
    if (!controller || !event_name || event_name[0] == '\0')
        return;
    controller_sanitize_event_queue(controller);
    if (controller->event_queue_count == RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX) {
        controller->event_queue_head =
            (controller->event_queue_head + 1) % RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX;
        controller->event_queue_count--;
        controller->events_dropped++;
        rt_game3d_diag_record_anim_events_dropped(1);
    }
    slot = (controller->event_queue_head + controller->event_queue_count) %
           RT_ANIM_CONTROLLER3D_EVENT_QUEUE_MAX;
    len = strlen(event_name);
    if (len >= RT_ANIM_CONTROLLER3D_EVENT_NAME_MAX)
        len = RT_ANIM_CONTROLLER3D_EVENT_NAME_MAX - 1;
    memcpy(controller->event_queue[slot], event_name, len);
    controller->event_queue[slot][len] = '\0';
    controller->event_queue_count++;
}

/// @brief Enqueue every event registered at time ≈ 0 for a state, on entry to that state.
/// @details Some events are pinned to the start of an animation (e.g., a
///          weapon-pull anim that should fire its sound on frame 0). These
///          would otherwise be missed by the prev_time→curr_time window
///          tracking in `controller_process_events` because the player
///          hasn't ticked yet. Threshold is 1µs.
/// @param[in,out] controller Controller whose scheduled events to inspect and
///                           pending queue to extend.
/// @param[in] state_index Valid selected state-table index.
static void controller_fire_entry_events(rt_anim_controller3d *controller, int32_t state_index) {
    int32_t event_count;
    if (!controller || state_index < 0 || state_index >= controller_safe_state_count(controller))
        return;
    event_count = controller_safe_event_count(controller);
    for (int32_t i = 0; i < event_count; i++) {
        if (controller->events[i].state_index == state_index &&
            controller->events[i].time_seconds <= 1e-6f) {
            controller_enqueue_event(controller, controller->events[i].name);
        }
    }
}

/// @brief Fire state events crossed by the last playback step.
/// @details Zero-duration clips have no traversable interval and are ignored
///          here; their time-zero events are handled on state entry. For clips
///          with duration, two cases are distinguished:
///          - **Non-looping**: half-open forward/reverse windows fire only
///            events crossed by the clamped step.
///          - **Looping**: forward and reverse wrap split the window at the
///            loop boundary. A step that spans at least one full duration
///            fires each registered event once.
///          Lower-bound exclusivity prevents an event exactly at `prev_time`
///          from firing twice on consecutive ticks.
/// @param[in,out] controller Controller owning the schedule and pending queue.
/// @param[in] state_index State whose events are eligible.
/// @param[in] prev_time Player time before the update.
/// @param[in] curr_time Player time after clamping or wrapping.
/// @param[in] duration Positive clip duration in seconds.
/// @param[in] looping Nonzero when the state timeline wraps.
/// @param[in] elapsed_time Signed unclamped clip-time step after speed.
static void controller_process_events(rt_anim_controller3d *controller,
                                      int32_t state_index,
                                      double prev_time,
                                      double curr_time,
                                      double duration,
                                      int8_t looping,
                                      double elapsed_time) {
    int32_t event_count;
    if (!controller || state_index < 0 || state_index >= controller_safe_state_count(controller))
        return;
    if (duration <= 0.0) {
        return;
    }
    event_count = controller_safe_event_count(controller);
    for (int32_t i = 0; i < event_count; i++) {
        const anim_controller3d_event_t *event = &controller->events[i];
        double t;
        if (event->state_index != state_index)
            continue;
        t = (double)event->time_seconds;
        if (!looping) {
            if (elapsed_time >= 0.0) {
                if (t > prev_time && t <= curr_time)
                    controller_enqueue_event(controller, event->name);
            } else if (t < prev_time && t >= curr_time) {
                controller_enqueue_event(controller, event->name);
            }
            continue;
        }
        if (isfinite(elapsed_time) && fabs(elapsed_time) >= duration) {
            controller_enqueue_event(controller, event->name);
            continue;
        }
        if (elapsed_time < 0.0) {
            if (curr_time <= prev_time) {
                if (t < prev_time && t >= curr_time)
                    controller_enqueue_event(controller, event->name);
            } else {
                if (t < prev_time || t >= curr_time)
                    controller_enqueue_event(controller, event->name);
            }
            continue;
        }
        if (curr_time >= prev_time) {
            if (t > prev_time && t <= curr_time)
                controller_enqueue_event(controller, event->name);
        } else {
            if (t > prev_time || t <= curr_time)
                controller_enqueue_event(controller, event->name);
        }
    }
}

/// @brief Mark every bone (up to `bone_count`) as included in this layer's mask.
/// @details Used for the base layer (layer 0) which always covers the full
///          skeleton, and as the reset state when an overlay layer's
///          `mask_root_bone` is invalid or out of range.
/// @param[in,out] layer Layer whose fixed bit array to rebuild.
/// @param[in] bone_count Requested included prefix, clamped to the supported
///                       skeleton maximum.
static void controller_set_all_mask_bits(anim_controller3d_layer_t *layer, int32_t bone_count) {
    if (!layer)
        return;
    if (bone_count < 0)
        bone_count = 0;
    if (bone_count > VGFX3D_MAX_SKELETON_BONES)
        bone_count = VGFX3D_MAX_SKELETON_BONES;
    memset(layer->mask_bits, 0, sizeof(layer->mask_bits));
    for (int32_t bone = 0; bone < bone_count; bone++)
        layer->mask_bits[bone] = 1;
    layer->mask_bone_count_seen = bone_count;
}

/// @brief Recompute a layer's mask bits from its `mask_root_bone` setting.
/// @details For layer 0 or invalid root, sets all bits (covers the whole
///          skeleton). Otherwise, walks each bone's parent chain and marks
///          the bone as masked-in if `mask_root_bone` is an ancestor.
///          This is the standard "this layer animates an arm subtree only"
///          construction — the upper-body layer of a character that
///          composites a wave / aim / reload pose over a running base.
/// @param[in,out] controller Controller providing the skeleton and layer.
/// @param[in] layer_index Zero-based valid layer index to rebuild.
static void controller_rebuild_layer_mask(rt_anim_controller3d *controller, int32_t layer_index) {
    anim_controller3d_layer_t *layer;
    int32_t bone_count;
    if (!controller || layer_index < 0 || layer_index >= RT_ANIM_CONTROLLER3D_MAX_LAYERS)
        return;
    layer = &controller->layers[layer_index];
    bone_count = controller_safe_bone_count(controller->skeleton);
    if (layer_index == 0 || layer->mask_root_bone < 0 || layer->mask_root_bone >= bone_count) {
        controller_set_all_mask_bits(layer, bone_count);
        return;
    }
    memset(layer->mask_bits, 0, sizeof(layer->mask_bits));
    for (int32_t bone = 0; bone < bone_count; bone++) {
        int32_t current = bone;
        int32_t depth = 0;
        while (current >= 0 && current < bone_count && depth++ < bone_count) {
            if (current == layer->mask_root_bone) {
                layer->mask_bits[bone] = 1;
                break;
            }
            current = controller->skeleton->bones[current].parent_index;
        }
    }
    layer->mask_bone_count_seen = bone_count;
}

/// @brief Push a state's per-state speed and looping settings into a player.
/// @details Always enables the player's loop-override so the controller's
///          per-state looping decision wins over whatever the underlying
///          Animation3D defaults to.
/// @param[in,out] player Layer player to configure.
/// @param[in] state State providing speed and looping policy.
static void controller_apply_state_settings(rt_anim_player3d *player,
                                            const anim_controller3d_state_t *state) {
    if (!player || !state)
        return;
    player->loop_override_enabled = 1;
    player->loop_override_value = state->looping ? 1 : 0;
    rt_anim_player3d_set_speed(player, state->speed);
}

/// @brief Evaluate and composite the controller's final globals and skinning
///        palette.
/// @param[in,out] controller Controller whose active pose sources and output
///                           buffers to evaluate.
static void controller_compute_final_palette(rt_anim_controller3d *controller);

/// @brief Switch a layer to a new state, optionally crossfading from the current state.
/// @details Crossfade is used when `blend_seconds > 0`, the layer already has
///          a different active state, and the existing player is mid-play.
///          Otherwise the switch is instantaneous (a hard cut). After
///          switching, the player is ticked by 0 seconds to push initial
///          pose data into the bone palette so the next render sees the
///          new state without a one-frame lag, and time-zero entry events
///          for the new state are fired.
/// @param[in,out] controller Controller whose layer selection to change.
/// @param[in] layer_index Zero-based layer index.
/// @param[in] state_index Valid registered state index.
/// @param[in] blend_seconds Requested nonnegative fade duration; invalid
///                          values normalize to zero.
/// @return 1 on success, 0 on bad layer index, missing state, or missing
///         underlying player/animation.
static int8_t controller_set_layer_state(rt_anim_controller3d *controller,
                                         int32_t layer_index,
                                         int32_t state_index,
                                         double blend_seconds) {
    anim_controller3d_layer_t *layer;
    const anim_controller3d_state_t *state;
    int use_crossfade;
    if (!controller || layer_index < 0 || layer_index >= RT_ANIM_CONTROLLER3D_MAX_LAYERS)
        return 0;
    blend_seconds = (double)controller_clamp_nonnegative_float(blend_seconds);
    if (state_index < 0 || state_index >= controller_safe_state_count(controller))
        return 0;
    layer = &controller->layers[layer_index];
    state = &controller->states[state_index];
    if (!layer->player || !state->animation)
        return 0;

    use_crossfade = blend_seconds > 0.0 && layer->current_state >= 0 &&
                    layer->current_state != state_index &&
                    rt_anim_player3d_is_playing(layer->player);

    layer->previous_state = layer->current_state;
    layer->current_state = state_index;
    if (use_crossfade) {
        rt_anim_player3d_crossfade(layer->player, state->animation, blend_seconds);
        controller_apply_state_settings(layer->player, state);
        layer->transitioning = 1;
        layer->transition_time = 0.0f;
        layer->transition_duration = controller_clamp_nonnegative_float(blend_seconds);
    } else {
        controller_apply_state_settings(layer->player, state);
        rt_anim_player3d_play(layer->player, state->animation);
        layer->transitioning = 0;
        layer->transition_time = 0.0f;
        layer->transition_duration = 0.0f;
    }
    rt_anim_player3d_update(layer->player, 0.0);
    controller_compute_final_palette(controller);
    controller_fire_entry_events(controller, state_index);
    return 1;
}

/// @brief Multiply two row-major 4x4 float matrices.
/// @param[in] a Borrowed left operand of 16 floats.
/// @param[in] b Borrowed right operand of 16 floats.
/// @param[out] out Writable 16-float result; must not alias either input.
static void controller_mat4f_mul(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

/// @brief Reset final globals to bind pose and final palette to skinning identity.
/// @details Computes globals from a temporary heap copy of bind-local matrices,
///          then multiplies each global by its inverse bind. Missing storage,
///          an empty skeleton, or scratch allocation failure leaves outputs
///          unchanged.
/// @param[in,out] controller Controller whose output buffers to initialize.
static void controller_bind_pose_palette(rt_anim_controller3d *controller) {
    int32_t bone_count;
    float *bind_locals;
    if (!controller || !controller->final_palette || !controller->final_globals ||
        !controller->skeleton)
        return;
    bone_count = controller_safe_bone_count(controller->skeleton);
    if (bone_count <= 0)
        return;
    /* Heap-allocated: a 1024-bone rig would need 64 KB of stack here. */
    bind_locals = (float *)malloc((size_t)bone_count * 16u * sizeof(float));
    if (!bind_locals)
        return;
    for (int32_t bone = 0; bone < bone_count; bone++)
        memcpy(&bind_locals[bone * 16],
               controller->skeleton->bones[bone].bind_pose_local,
               16 * sizeof(float));
    skeleton3d_compute_globals_from_locals(
        controller->skeleton, bind_locals, controller->final_globals, bone_count);
    for (int32_t bone = 0; bone < bone_count; bone++) {
        controller_mat4f_mul(&controller->final_globals[bone * 16],
                             controller->skeleton->bones[bone].inverse_bind,
                             &controller->final_palette[bone * 16]);
    }
    free(bind_locals);
}

/// @brief Convert a 3-by-3 rotation basis to a normalized quaternion.
/// @param[in] m00 Row-zero, column-zero basis lane.
/// @param[in] m01 Row-zero, column-one basis lane.
/// @param[in] m02 Row-zero, column-two basis lane.
/// @param[in] m10 Row-one, column-zero basis lane.
/// @param[in] m11 Row-one, column-one basis lane.
/// @param[in] m12 Row-one, column-two basis lane.
/// @param[in] m20 Row-two, column-zero basis lane.
/// @param[in] m21 Row-two, column-one basis lane.
/// @param[in] m22 Row-two, column-two basis lane.
/// @param[out] out Writable four-double quaternion in `(x,y,z,w)` order.
static void controller_quat_from_matrix_rows(double m00,
                                             double m01,
                                             double m02,
                                             double m10,
                                             double m11,
                                             double m12,
                                             double m20,
                                             double m21,
                                             double m22,
                                             double *out);

/// @brief Spherical-linear interpolate two unit quaternions (float lanes).
/// @details Flips `b` when the dot product is negative so the shorter arc is
///          taken, then falls back to a plain normalized lerp for nearly
///          parallel inputs (dot > 0.9995) where the `sin(theta)` divisor would
///          approach zero. The result is renormalized, with an identity-quaternion
///          guard for the degenerate zero-length case.
/// @param[in] a Borrowed four-float unit quaternion.
/// @param[in] b Borrowed four-float unit quaternion.
/// @param[in] t Interpolation weight clamped to `[0,1]`; non-finite becomes
///              zero.
/// @param[out] out Writable normalized four-float result.
static void controller_quat_slerp_float(const float *a, const float *b, float t, float *out) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float nb[4] = {b[0], b[1], b[2], b[3]};
    if (!isfinite(t) || t <= 0.0f)
        t = 0.0f;
    else if (t >= 1.0f)
        t = 1.0f;
    if (dot < 0.0f) {
        dot = -dot;
        nb[0] = -nb[0];
        nb[1] = -nb[1];
        nb[2] = -nb[2];
        nb[3] = -nb[3];
    }
    if (!isfinite(dot)) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    if (dot > 1.0f)
        dot = 1.0f;
    if (dot < -1.0f)
        dot = -1.0f;
    if (dot > 0.9995f) {
        for (int32_t i = 0; i < 4; i++)
            out[i] = a[i] + t * (nb[i] - a[i]);
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / sin_theta;
        float wb = sinf(t * theta) / sin_theta;
        for (int32_t i = 0; i < 4; i++)
            out[i] = wa * a[i] + wb * nb[i];
    }
    float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (!isfinite(len) || len <= 1e-8f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    for (int32_t i = 0; i < 4; i++)
        out[i] /= len;
}

/// @brief Quaternion conjugate (negated vector part) — the inverse for a unit quaternion.
/// @param[in] q Borrowed four-float quaternion.
/// @param[out] out Writable four-float conjugate; may alias @p q.
static void controller_quat_conjugate_float(const float *q, float *out) {
    if (!q || !out)
        return;
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

/// @brief Hamilton product out = a * b (apply b then a), normalized; identity on degenerate result.
/// @param[in] a Borrowed left quaternion operand.
/// @param[in] b Borrowed right quaternion operand.
/// @param[out] out Writable normalized product; must not alias either input.
static void controller_quat_mul_float(const float *a, const float *b, float *out) {
    float w;
    float x;
    float y;
    float z;
    float len;
    if (!a || !b || !out)
        return;
    w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (!isfinite(len) || len <= 1e-8f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    for (int32_t i = 0; i < 4; i++)
        out[i] /= len;
}

/// @brief Compose a row-major 4x4 TRS matrix from translation, rotation
///        (quaternion) and scale (all float lanes).
/// @details Standard quaternion-to-basis expansion with each basis column
///          pre-multiplied by its scale component and the translation written
///          into the 4th column.
/// @param[in] pos Borrowed three-float translation.
/// @param[in] quat Borrowed normalized four-float quaternion.
/// @param[in] scl Borrowed three-float scale.
/// @param[out] out Writable row-major matrix of 16 floats.
static void controller_build_trs_float(const float *pos,
                                       const float *quat,
                                       const float *scl,
                                       float *out) {
    float x = quat[0], y = quat[1], z = quat[2], w = quat[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    out[0] = (1.0f - (yy + zz)) * scl[0];
    out[1] = (xy - wz) * scl[1];
    out[2] = (xz + wy) * scl[2];
    out[3] = pos[0];
    out[4] = (xy + wz) * scl[0];
    out[5] = (1.0f - (xx + zz)) * scl[1];
    out[6] = (yz - wx) * scl[2];
    out[7] = pos[1];
    out[8] = (xz - wy) * scl[0];
    out[9] = (yz + wx) * scl[1];
    out[10] = (1.0f - (xx + yy)) * scl[2];
    out[11] = pos[2];
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

/// @brief Decompose a row-major 4x4 matrix into translation, rotation
///        (quaternion) and scale (all float lanes).
/// @details Scale per axis is the length of each basis column, guarded to 1.0
///          when near-zero or non-finite so the subsequent divide that
///          orthonormalizes the basis before quaternion extraction stays finite.
/// @param[in] m Borrowed row-major matrix of 16 floats.
/// @param[out] out_pos Writable three-float translation.
/// @param[out] out_rot Writable normalized four-float quaternion.
/// @param[out] out_scl Writable three-float scale.
static void controller_decompose_trs_float(const float *m,
                                           float *out_pos,
                                           float *out_rot,
                                           float *out_scl) {
    double rot[4];
    double sx = sqrt((double)m[0] * (double)m[0] + (double)m[4] * (double)m[4] +
                     (double)m[8] * (double)m[8]);
    double sy = sqrt((double)m[1] * (double)m[1] + (double)m[5] * (double)m[5] +
                     (double)m[9] * (double)m[9]);
    double sz = sqrt((double)m[2] * (double)m[2] + (double)m[6] * (double)m[6] +
                     (double)m[10] * (double)m[10]);
    out_pos[0] = isfinite(m[3]) ? m[3] : 0.0f;
    out_pos[1] = isfinite(m[7]) ? m[7] : 0.0f;
    out_pos[2] = isfinite(m[11]) ? m[11] : 0.0f;
    out_scl[0] = isfinite(sx) && sx > 1e-6 && sx <= (double)FLT_MAX ? (float)sx : 1.0f;
    out_scl[1] = isfinite(sy) && sy > 1e-6 && sy <= (double)FLT_MAX ? (float)sy : 1.0f;
    out_scl[2] = isfinite(sz) && sz > 1e-6 && sz <= (double)FLT_MAX ? (float)sz : 1.0f;
    controller_quat_from_matrix_rows((double)m[0] / out_scl[0],
                                     (double)m[1] / out_scl[1],
                                     (double)m[2] / out_scl[2],
                                     (double)m[4] / out_scl[0],
                                     (double)m[5] / out_scl[1],
                                     (double)m[6] / out_scl[2],
                                     (double)m[8] / out_scl[0],
                                     (double)m[9] / out_scl[1],
                                     (double)m[10] / out_scl[2],
                                     rot);
    out_rot[0] = (float)rot[0];
    out_rot[1] = (float)rot[1];
    out_rot[2] = (float)rot[2];
    out_rot[3] = (float)rot[3];
}

/// @brief Find the animation channel targeting @p bone_index, or NULL if the clip has
///   no channel for that bone.
/// @param[in] anim Animation3D whose sanitized channel range to scan.
/// @param[in] bone_index Nonnegative target skeleton-bone index.
/// @return Borrowed matching channel pointer, or `NULL`.
static const vgfx3d_anim_channel_t *controller_find_animation_channel(const rt_animation3d *anim,
                                                                      int32_t bone_index) {
    int32_t channel_count;
    if (!anim || bone_index < 0)
        return NULL;
    channel_count = animation3d_safe_channel_count(anim);
    for (int32_t i = 0; i < channel_count; i++) {
        if (anim->channels[i].bone_index == bone_index)
            return &anim->channels[i];
    }
    return NULL;
}

/// @brief Return @p value when finite, else @p fallback (float scalar sanitizer).
/// @param[in] value Candidate float value.
/// @param[in] fallback Replacement for NaN or infinity.
/// @return @p value when finite; otherwise @p fallback.
static float controller_finite_float_or(float value, float fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Normalize a float quaternion in place, falling back to @p fallback (then to identity)
///   when its length is non-finite or ~zero.
/// @param[in,out] q Writable four-float quaternion; `NULL` is ignored.
/// @param[in] fallback Optional borrowed four-float replacement, itself
///                     normalized before use.
static void controller_normalize_quat_float_or(float *q, const float *fallback) {
    float len;
    if (!q)
        return;
    len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!isfinite(len) || len <= 1e-8f) {
        if (fallback) {
            memcpy(q, fallback, 4 * sizeof(float));
            len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        }
        if (!fallback || !isfinite(len) || len <= 1e-8f) {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
            return;
        }
    }
    for (int32_t i = 0; i < 4; i++)
        q[i] /= len;
}

/// @brief Sanitize a TRS sample in place: finite-guard each position/scale lane (using the
///   supplied fallbacks) and re-normalize the rotation quaternion.
/// @param[in,out] pos Writable three-float translation.
/// @param[in,out] rot Writable four-float quaternion.
/// @param[in,out] scl Writable three-float scale.
/// @param[in] fallback_pos Optional three-float translation fallback.
/// @param[in] fallback_rot Optional four-float quaternion fallback.
/// @param[in] fallback_scl Optional three-float nonzero scale fallback.
static void controller_sanitize_trs_sample(float *pos,
                                           float *rot,
                                           float *scl,
                                           const float *fallback_pos,
                                           const float *fallback_rot,
                                           const float *fallback_scl) {
    if (!pos || !rot || !scl)
        return;
    for (int32_t i = 0; i < 3; i++) {
        pos[i] = controller_finite_float_or(pos[i], fallback_pos ? fallback_pos[i] : 0.0f);
        scl[i] = controller_finite_float_or(scl[i], fallback_scl ? fallback_scl[i] : 1.0f);
        if (fabsf(scl[i]) <= 1e-8f)
            scl[i] = fallback_scl && isfinite(fallback_scl[i]) && fabsf(fallback_scl[i]) > 1e-8f
                         ? fallback_scl[i]
                         : 1.0f;
    }
    for (int32_t i = 0; i < 4; i++)
        rot[i] = controller_finite_float_or(
            rot[i], fallback_rot ? fallback_rot[i] : (i == 3 ? 1.0f : 0.0f));
    controller_normalize_quat_float_or(rot, fallback_rot);
}

/// @brief Resolve a keyframe's translation/rotation/scale, substituting the bind-pose
///   fallback for any channel component the keyframe's presence masks leave unset
///   (rotation is all-or-nothing: only used when all four quaternion lanes are present).
/// @param[in] key Keyframe whose masked values to resolve.
/// @param[in] fallback_pos Borrowed three-float bind translation.
/// @param[in] fallback_rot Borrowed four-float bind quaternion.
/// @param[in] fallback_scl Borrowed three-float bind scale.
/// @param[out] out_pos Writable resolved translation.
/// @param[out] out_rot Writable resolved normalized quaternion.
/// @param[out] out_scl Writable resolved nondegenerate scale.
static void controller_keyframe_effective_trs(const vgfx3d_keyframe_t *key,
                                              const float *fallback_pos,
                                              const float *fallback_rot,
                                              const float *fallback_scl,
                                              float *out_pos,
                                              float *out_rot,
                                              float *out_scl) {
    if (!key)
        return;
    for (int32_t i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        out_pos[i] = (key->position_mask & bit) ? key->position[i] : fallback_pos[i];
        out_scl[i] = (key->scale_mask & bit) ? key->scale_xyz[i] : fallback_scl[i];
    }
    if ((key->rotation_mask & 0x0Fu) == 0x0Fu)
        memcpy(out_rot, key->rotation, 4 * sizeof(float));
    else
        memcpy(out_rot, fallback_rot, 4 * sizeof(float));
    controller_sanitize_trs_sample(
        out_pos, out_rot, out_scl, fallback_pos, fallback_rot, fallback_scl);
}

/// @brief Sample a channel's TRS at @p time, interpolating between the surrounding
///   keyframes (linear for position/scale, SLERP for rotation). Empty channels return
///   the fallback bind-pose; single-keyframe channels return that key directly.
/// @param[in] channel Optional animation channel with time-ordered keyframes.
/// @param[in] time Sample time; non-finite input becomes zero and out-of-range
///                 input clamps to the first or last keyframe.
/// @param[in] fallback_pos Borrowed three-float bind translation.
/// @param[in] fallback_rot Borrowed four-float bind quaternion.
/// @param[in] fallback_scl Borrowed three-float bind scale.
/// @param[out] out_pos Writable sampled translation.
/// @param[out] out_rot Writable sampled normalized quaternion.
/// @param[out] out_scl Writable sampled scale.
static void controller_sample_channel_trs(const vgfx3d_anim_channel_t *channel,
                                          float time,
                                          const float *fallback_pos,
                                          const float *fallback_rot,
                                          const float *fallback_scl,
                                          float *out_pos,
                                          float *out_rot,
                                          float *out_scl) {
    int32_t keyframe_count;
    if (!isfinite(time))
        time = 0.0f;
    keyframe_count = animation3d_safe_keyframe_count(channel);
    if (!channel || keyframe_count <= 0 || !channel->keyframes) {
        memcpy(out_pos, fallback_pos, 3 * sizeof(float));
        memcpy(out_rot, fallback_rot, 4 * sizeof(float));
        memcpy(out_scl, fallback_scl, 3 * sizeof(float));
        return;
    }
    if (keyframe_count == 1) {
        controller_keyframe_effective_trs(&channel->keyframes[0],
                                          fallback_pos,
                                          fallback_rot,
                                          fallback_scl,
                                          out_pos,
                                          out_rot,
                                          out_scl);
        return;
    }

    if (time <= channel->keyframes[0].time) {
        controller_keyframe_effective_trs(&channel->keyframes[0],
                                          fallback_pos,
                                          fallback_rot,
                                          fallback_scl,
                                          out_pos,
                                          out_rot,
                                          out_scl);
        return;
    }
    if (time >= channel->keyframes[keyframe_count - 1].time) {
        controller_keyframe_effective_trs(&channel->keyframes[keyframe_count - 1],
                                          fallback_pos,
                                          fallback_rot,
                                          fallback_scl,
                                          out_pos,
                                          out_rot,
                                          out_scl);
        return;
    }

    int32_t k0 = 0;
    int32_t k1 = keyframe_count - 1;
    while (k1 - k0 > 1) {
        int32_t mid = k0 + (k1 - k0) / 2;
        if (channel->keyframes[mid].time <= time)
            k0 = mid;
        else
            k1 = mid;
    }
    double t0 = channel->keyframes[k0].time;
    double t1 = channel->keyframes[k1].time;
    float alpha = (float)((t1 > t0) ? ((double)time - t0) / (t1 - t0) : 0.0);
    if (!isfinite(alpha) || alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;

    float pos0[3], rot0[4], scl0[3];
    float pos1[3], rot1[4], scl1[3];
    controller_keyframe_effective_trs(
        &channel->keyframes[k0], fallback_pos, fallback_rot, fallback_scl, pos0, rot0, scl0);
    controller_keyframe_effective_trs(
        &channel->keyframes[k1], fallback_pos, fallback_rot, fallback_scl, pos1, rot1, scl1);
    for (int32_t i = 0; i < 3; i++) {
        out_pos[i] = pos0[i] + (pos1[i] - pos0[i]) * alpha;
        out_scl[i] = scl0[i] + (scl1[i] - scl0[i]) * alpha;
    }
    controller_quat_slerp_float(rot0, rot1, alpha, out_rot);
    controller_sanitize_trs_sample(
        out_pos, out_rot, out_scl, fallback_pos, fallback_rot, fallback_scl);
}

/// @brief Compute one bone's local transform matrix for @p animation at @p time: sample
///   the bone's channel (falling back to its bind pose) and recompose into @p out_local.
/// @param[in] skeleton Skeleton providing the indexed bone's bind-local matrix.
/// @param[in] animation Animation clip whose matching channel to sample.
/// @param[in] bone_index Valid skeleton bone index.
/// @param[in] time Clip sample time.
/// @param[out] out_local Writable row-major local matrix of 16 floats.
static void controller_sample_animation_local_matrix(const rt_skeleton3d *skeleton,
                                                     const rt_animation3d *animation,
                                                     int32_t bone_index,
                                                     float time,
                                                     float *out_local) {
    float fallback_pos[3], fallback_rot[4], fallback_scl[3];
    float pos[3], rot[4], scl[3];
    const float *bind_local = skeleton->bones[bone_index].bind_pose_local;
    controller_decompose_trs_float(bind_local, fallback_pos, fallback_rot, fallback_scl);
    controller_sample_channel_trs(controller_find_animation_channel(animation, bone_index),
                                  time,
                                  fallback_pos,
                                  fallback_rot,
                                  fallback_scl,
                                  pos,
                                  rot,
                                  scl);
    controller_build_trs_float(pos, rot, scl, out_local);
}
