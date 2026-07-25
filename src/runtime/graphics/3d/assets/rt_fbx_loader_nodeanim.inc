//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_nodeanim.inc
// Purpose: Import non-skeletal FBX Model transform animation into NodeAnimation3D clips.
// Key invariants:
//   - Channel targets use the Model record's stable ordinal in Objects, matching SceneNode3D.
//   - Full FBX transform stacks and TransformInheritType are sampled through the static composer.
//   - One transactional NodeAnimation3D is emitted per AnimationStack that drives scene models.
// Ownership/Lifetime:
//   - Builder/sample arrays are native scratch allocations freed before return.
//   - Successfully emitted clips are owned by the caller's reference array.
// Links: rt_fbx_loader.c, rt_fbx_loader_anim.inc, rt_scene3d_nodeanim.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_nodeanim.inc
 * @brief Imports FBX model transforms, camera properties, and morph weights into
 *        runtime NodeAnimation3D clips.
 *
 * Each AnimationStack is sampled through the same layered transform and constraint
 * machinery used by skeletal animation. The importer emits channels targeted by the
 * stable `Objects` ordinal used by the runtime scene hierarchy, includes inherited and
 * constraint-dependent source times, derives canonical camera projection values, and
 * converts progressive BlendShapeChannel percentages into weight vectors.
 *
 * Builder tables, sample arrays, and evaluated poses are load-local. Successfully
 * published clips transfer to the caller-provided reference array.
 */

/// @brief One non-skeletal Model participating in an FBX object-animation stack.
typedef struct {
    /// FBX Model object identifier.
    int64_t id;
    /// Identifier of the connected parent Model, or zero for a root.
    int64_t parent_id;
    /// Index of the parent in the builder table, or -1.
    int32_t parent_index;
    /// Stable ordinal of the Model in the FBX `Objects` child array.
    int32_t import_index;
    /// Non-zero when the Model represents a skeletal node.
    int is_skeleton;
    /// Borrowed source Model node.
    fbx_node_t *source;
    /// Base transform plus curves/layer-stack sampling state.
    fbx_anim_bone_builder_t animation;
} fbx_node_anim_model_builder_t;

/// @brief Evaluate one progressive FBX BlendShapeChannel lane for a local shape.
/// @details Before the first FullWeights threshold the first shape ramps from the base. Between
///          thresholds the adjacent shapes cross-fade, and values beyond the final threshold hold
///          the final shape. Negative channel values extrapolate the first shape down to -1.
/// @param binding Imported channel metadata containing ordered shape thresholds.
/// @param percent Sampled FBX `DeformPercent` value.
/// @param local_shape Zero-based shape within the channel.
/// @return The shape's normalized weight, clamped to the progressive interval semantics,
///         or zero for invalid input or a shape outside the active interval.
static double fbx_morph_progressive_shape_weight(const fbx_morph_channel_binding_t *binding,
                                                 double percent,
                                                 int32_t local_shape) {
    if (!binding || !binding->full_weights || binding->shape_count <= 0 || local_shape < 0 ||
        local_shape >= binding->shape_count || !isfinite(percent)) {
        return 0.0;
    }
    if (percent <= binding->full_weights[0]) {
        if (local_shape != 0 || binding->full_weights[0] <= 0.0)
            return 0.0;
        return fbx_clamp_double(percent / binding->full_weights[0], -1.0, 1.0, 0.0);
    }
    for (int32_t upper = 1; upper < binding->shape_count; ++upper) {
        double lower_weight = binding->full_weights[upper - 1];
        double upper_weight = binding->full_weights[upper];
        if (percent <= upper_weight) {
            double alpha = (percent - lower_weight) / (upper_weight - lower_weight);
            alpha = fbx_clamp_double(alpha, 0.0, 1.0, 0.0);
            if (local_shape == upper - 1)
                return 1.0 - alpha;
            if (local_shape == upper)
                return alpha;
            return 0.0;
        }
    }
    return local_shape == binding->shape_count - 1 ? 1.0 : 0.0;
}

/// @brief Populate a validated borrowed curve view from a typed AnimationCurve node.
/// @param curve Parsed FBX `AnimationCurve` node.
/// @param[out] out_view Receives pointers into the node's key arrays.
/// @return Non-zero when time/value arrays and optional interpolation metadata form a
///         valid curve; otherwise zero.
static int fbx_node_anim_read_curve_view(fbx_node_t *curve, fbx_anim_curve_view_t *out_view) {
    uint32_t time_count = 0;
    uint32_t value_count = 0;
    uint32_t attr_flag_count = 0;
    uint32_t attr_data_count = 0;
    fbx_anim_curve_view_t candidate;
    const int64_t *times;
    const double *values64;
    const float *values32 = NULL;
    if (!curve || !out_view || strcmp(curve->name, "AnimationCurve") != 0)
        return 0;
    times = fbx_get_i64_array(curve, "KeyTime", &time_count);
    values64 = fbx_get_f64_array(curve, "KeyValueFloat", &value_count);
    if (!values64)
        values32 = fbx_get_f32_array(curve, "KeyValueFloat", &value_count);
    if (!times || (!values64 && !values32) || time_count == 0 || time_count != value_count)
        return 0;
    memset(&candidate, 0, sizeof(candidate));
    candidate.times = times;
    candidate.values64 = values64;
    candidate.values32 = values32;
    candidate.attr_flags = fbx_get_i32_array(curve, "KeyAttrFlags", &attr_flag_count);
    candidate.attr_data64 = fbx_get_f64_array(curve, "KeyAttrDataFloat", &attr_data_count);
    if (!candidate.attr_data64)
        candidate.attr_data32 = fbx_get_f32_array(curve, "KeyAttrDataFloat", &attr_data_count);
    candidate.count = time_count;
    candidate.attr_flag_count = candidate.attr_flags ? attr_flag_count : 0;
    candidate.attr_data_count =
        (candidate.attr_data64 || candidate.attr_data32) ? attr_data_count : 0;
    if (!fbx_anim_curve_view_valid(&candidate))
        return 0;
    *out_view = candidate;
    return 1;
}

/// @brief Allocate and initialize the stable Model table used by node animation.
/// @details Includes skeletal Models so parent transforms and constraints can be sampled,
///          while emission later skips skeletal targets. A sorted ID table resolves parent
///          indices without changing the stable `Objects` ordinals.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Connection table used to resolve Model parents.
/// @param[out] out_models Receives the newly allocated builder array.
/// @param[out] out_count Receives the number of Model builders.
/// @param[out] out_ids Receives a newly allocated, ID-sorted lookup table.
/// @param[in,out] context Loader context charged for both arrays.
/// @return Non-zero on success, including an empty table, or zero for invalid input,
///         overflow, budget rejection, or allocation failure.
static int fbx_node_anim_make_model_table(fbx_node_t *objects,
                                          const fbx_conn_table_t *ct,
                                          fbx_node_anim_model_builder_t **out_models,
                                          int32_t *out_count,
                                          fbx_model_id_entry_t **out_ids,
                                          fbx_load_context_t *context) {
    fbx_node_anim_model_builder_t *models = NULL;
    fbx_model_id_entry_t *ids = NULL;
    int32_t count = 0;
    if (!objects || !ct || !out_models || !out_count || !out_ids || !context)
        return 0;
    *out_models = NULL;
    *out_count = 0;
    *out_ids = NULL;
    for (int32_t i = 0; i < objects->child_count; ++i) {
        fbx_node_t *object = &objects->children[i];
        if (strcmp(object->name, "Model") == 0 && object->prop_count >= 3) {
            if (count == INT32_MAX)
                return 0;
            ++count;
        }
    }
    if (count == 0)
        return 1;
    if ((size_t)count > SIZE_MAX / sizeof(*models) || (size_t)count > SIZE_MAX / sizeof(*ids) ||
        !fbx_budget_charge(context, (uint64_t)count, sizeof(*models)) ||
        !fbx_budget_charge(context, (uint64_t)count, sizeof(*ids))) {
        return 0;
    }
    models = (fbx_node_anim_model_builder_t *)calloc((size_t)count, sizeof(*models));
    ids = (fbx_model_id_entry_t *)malloc((size_t)count * sizeof(*ids));
    if (!models || !ids) {
        free(models);
        free(ids);
        return 0;
    }
    count = 0;
    for (int32_t i = 0; i < objects->child_count; ++i) {
        fbx_node_t *object = &objects->children[i];
        if (strcmp(object->name, "Model") != 0 || object->prop_count < 3) {
            continue;
        }
        models[count].id = fbx_prop_i64(object, 0);
        models[count].parent_id = fbx_find_parent(ct, models[count].id);
        models[count].parent_index = -1;
        models[count].import_index = i;
        models[count].is_skeleton = fbx_model_type_is_skeleton(fbx_prop_str(object, 2));
        models[count].source = object;
        models[count].animation.target_id = models[count].id;
        fbx_read_model_transform_components(object, &models[count].animation.base_transform);
        models[count].animation.initialized = 1;
        ids[count].id = models[count].id;
        ids[count].index = count;
        ++count;
    }
    qsort(ids, (size_t)count, sizeof(*ids), fbx_model_id_entry_cmp);
    for (int32_t i = 0; i < count; ++i) {
        if (models[i].parent_id != 0)
            models[i].parent_index = fbx_model_index_for_id(ids, count, models[i].parent_id);
    }
    *out_models = models;
    *out_count = count;
    *out_ids = ids;
    return 1;
}

/// @brief Collect an animation stack's local T/R/S curve graph into the Model table.
/// @details When @p layer_stack is available it becomes the authoritative layered sampler
///          for every model. The legacy graph walk is retained for callers without one.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Connection table linking stacks, layers, curve nodes, curves, and Models.
/// @param stack_id AnimationStack object identifier.
/// @param layer_stack Optional parsed layered-animation stack.
/// @param[in,out] models Model builders receiving curve views or the layered stack.
/// @param model_count Number of entries in @p models.
/// @param ids Sorted Model-ID lookup table parallel to @p models.
static void fbx_node_anim_collect_transform_curves(fbx_node_t *objects,
                                                   const fbx_conn_table_t *ct,
                                                   int64_t stack_id,
                                                   const fbx_anim_layer_stack_t *layer_stack,
                                                   fbx_node_anim_model_builder_t *models,
                                                   int32_t model_count,
                                                   const fbx_model_id_entry_t *ids) {
    if (!objects || !ct || !models || model_count <= 0 || !ids)
        return;
    if (layer_stack) {
        for (int32_t i = 0; i < model_count; ++i)
            models[i].animation.layer_stack = layer_stack;
        return;
    }
    for (int32_t layer_connection = fbx_connections_first_for_parent(ct, stack_id);
         layer_connection >= 0;
         layer_connection = fbx_connections_next_for_parent(ct, layer_connection)) {
        fbx_node_t *layer = fbx_find_object_by_id(objects, ct->entries[layer_connection].child_id);
        if (!layer || strcmp(layer->name, "AnimationLayer") != 0)
            continue;
        for (int32_t node_connection = fbx_connections_first_for_parent(ct, fbx_prop_i64(layer, 0));
             node_connection >= 0;
             node_connection = fbx_connections_next_for_parent(ct, node_connection)) {
            int64_t curve_node_id = ct->entries[node_connection].child_id;
            fbx_node_t *curve_node = fbx_find_object_by_id(objects, curve_node_id);
            int32_t model_index = -1;
            int32_t trs = -1;
            if (!curve_node || strcmp(curve_node->name, "AnimationCurveNode") != 0)
                continue;
            for (int32_t target_connection = fbx_connections_first_for_child(ct, curve_node_id);
                 target_connection >= 0;
                 target_connection = fbx_connections_next_for_child(ct, target_connection)) {
                int64_t target_id = ct->entries[target_connection].parent_id;
                fbx_node_t *target = fbx_find_object_by_id(objects, target_id);
                const char *property = ct->entries[target_connection].prop;
                if (!target || strcmp(target->name, "Model") != 0 || !property)
                    continue;
                if (strcmp(property, "Lcl Translation") == 0)
                    trs = 0;
                else if (strcmp(property, "Lcl Rotation") == 0)
                    trs = 1;
                else if (strcmp(property, "Lcl Scaling") == 0)
                    trs = 2;
                if (trs >= 0) {
                    model_index = fbx_model_index_for_id(ids, model_count, target_id);
                    break;
                }
            }
            if (model_index < 0 || trs < 0)
                continue;
            for (int32_t curve_connection = fbx_connections_first_for_parent(ct, curve_node_id);
                 curve_connection >= 0;
                 curve_connection = fbx_connections_next_for_parent(ct, curve_connection)) {
                int32_t component =
                    fbx_anim_component_from_connection_prop(ct->entries[curve_connection].prop);
                fbx_node_t *curve;
                if (component < 0 ||
                    fbx_anim_curve_has_data(&models[model_index].animation.curves[trs][component]))
                    continue;
                curve = fbx_find_object_by_id(objects, ct->entries[curve_connection].child_id);
                (void)fbx_node_anim_read_curve_view(
                    curve, &models[model_index].animation.curves[trs][component]);
            }
        }
    }
}

/// @brief Collect one `DeformPercent` curve per imported BlendShapeChannel for a stack.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Connection table linking stacks through layers and curve nodes to channels.
/// @param stack_id AnimationStack object identifier.
/// @param bindings Imported morph-channel binding array.
/// @param binding_count Number of entries in @p bindings and @p curves.
/// @param[out] curves Zero-initialized curve-view array populated by binding index.
static void fbx_node_anim_collect_morph_curves(fbx_node_t *objects,
                                               const fbx_conn_table_t *ct,
                                               int64_t stack_id,
                                               const fbx_morph_channel_binding_t *bindings,
                                               int32_t binding_count,
                                               fbx_anim_curve_view_t *curves) {
    if (!objects || !ct || !bindings || binding_count <= 0 || !curves)
        return;
    for (int32_t layer_connection = fbx_connections_first_for_parent(ct, stack_id);
         layer_connection >= 0;
         layer_connection = fbx_connections_next_for_parent(ct, layer_connection)) {
        fbx_node_t *layer = fbx_find_object_by_id(objects, ct->entries[layer_connection].child_id);
        if (!layer || strcmp(layer->name, "AnimationLayer") != 0 || layer->prop_count < 1)
            continue;
        for (int32_t node_connection = fbx_connections_first_for_parent(ct, fbx_prop_i64(layer, 0));
             node_connection >= 0;
             node_connection = fbx_connections_next_for_parent(ct, node_connection)) {
            int64_t curve_node_id = ct->entries[node_connection].child_id;
            fbx_node_t *curve_node = fbx_find_object_by_id(objects, curve_node_id);
            int32_t binding_index = -1;
            if (!curve_node || strcmp(curve_node->name, "AnimationCurveNode") != 0)
                continue;
            for (int32_t target_connection = fbx_connections_first_for_child(ct, curve_node_id);
                 target_connection >= 0;
                 target_connection = fbx_connections_next_for_child(ct, target_connection)) {
                int64_t target_id = ct->entries[target_connection].parent_id;
                const char *property = ct->entries[target_connection].prop;
                if (property && !fbx_ascii_contains_i(property, "DeformPercent"))
                    continue;
                for (int32_t i = 0; i < binding_count; ++i) {
                    if (bindings[i].channel_id == target_id) {
                        binding_index = i;
                        break;
                    }
                }
                if (binding_index >= 0)
                    break;
            }
            if (binding_index < 0 || fbx_anim_curve_has_data(&curves[binding_index]))
                continue;
            fbx_node_t *fallback_curve = NULL;
            for (int32_t curve_connection = fbx_connections_first_for_parent(ct, curve_node_id);
                 curve_connection >= 0;
                 curve_connection = fbx_connections_next_for_parent(ct, curve_connection)) {
                fbx_node_t *curve =
                    fbx_find_object_by_id(objects, ct->entries[curve_connection].child_id);
                const char *property = ct->entries[curve_connection].prop;
                if (!curve || strcmp(curve->name, "AnimationCurve") != 0)
                    continue;
                if (!fallback_curve)
                    fallback_curve = curve;
                if (!property || fbx_ascii_contains_i(property, "DeformPercent") ||
                    fbx_anim_component_from_connection_prop(property) == 0) {
                    if (fbx_node_anim_read_curve_view(curve, &curves[binding_index]))
                        break;
                }
            }
            if (!fbx_anim_curve_has_data(&curves[binding_index]) && fallback_curve)
                (void)fbx_node_anim_read_curve_view(fallback_curve, &curves[binding_index]);
        }
    }
}

/// @brief Compute source-time bounds across all transform animation in a Model table.
/// @param models Model builder array containing layered or legacy curve state.
/// @param model_count Number of entries in @p models.
/// @param[out] out_min Receives the earliest source tick.
/// @param[out] out_max Receives the latest source tick.
/// @return Non-zero when at least one transform curve contributes bounds; otherwise zero.
static int fbx_node_anim_transform_bounds(const fbx_node_anim_model_builder_t *models,
                                          int32_t model_count,
                                          int64_t *out_min,
                                          int64_t *out_max) {
    int found = 0;
    int64_t minimum = 0;
    int64_t maximum = 0;
    if (!models || model_count <= 0 || !out_min || !out_max)
        return 0;
    for (int32_t i = 0; i < model_count; ++i)
        if (models[i].animation.layer_stack)
            return fbx_anim_layer_stack_bounds(
                models[i].animation.layer_stack, 1, out_min, out_max);
    for (int32_t i = 0; i < model_count; ++i) {
        for (int trs = 0; trs < 3; ++trs) {
            for (int component = 0; component < 3; ++component) {
                const fbx_anim_curve_view_t *curve = &models[i].animation.curves[trs][component];
                if (!fbx_anim_curve_has_data(curve))
                    continue;
                if (!found || curve->times[0] < minimum)
                    minimum = curve->times[0];
                if (!found || curve->times[curve->count - 1] > maximum)
                    maximum = curve->times[curve->count - 1];
                found = 1;
            }
        }
    }
    if (!found)
        return 0;
    *out_min = minimum;
    *out_max = maximum;
    return 1;
}

/// @brief Expand source-time bounds to cover every animated morph channel.
/// @param curves Morph-channel curve views.
/// @param curve_count Number of entries in @p curves.
/// @param have_bounds Non-zero when @p minimum and @p maximum already contain bounds.
/// @param[in,out] minimum Earliest source tick, initialized when needed and widened in place.
/// @param[in,out] maximum Latest source tick, initialized when needed and widened in place.
/// @return Non-zero when existing or morph-derived bounds are available.
static int fbx_node_anim_include_morph_bounds(const fbx_anim_curve_view_t *curves,
                                              int32_t curve_count,
                                              int have_bounds,
                                              int64_t *minimum,
                                              int64_t *maximum) {
    if (!curves || curve_count <= 0 || !minimum || !maximum)
        return have_bounds;
    for (int32_t i = 0; i < curve_count; ++i) {
        if (!fbx_anim_curve_has_data(&curves[i]))
            continue;
        if (!have_bounds || curves[i].times[0] < *minimum)
            *minimum = curves[i].times[0];
        if (!have_bounds || curves[i].times[curves[i].count - 1] > *maximum)
            *maximum = curves[i].times[curves[i].count - 1];
        have_bounds = 1;
    }
    return have_bounds;
}

/// @brief Determine whether a constraint can write the evaluated transform at a Model index.
/// @details Ordinary constraints write their explicit target. A single-chain IK constraint
///          writes every joint from its effector ancestor chain through the target.
/// @param constraint Constraint record to inspect.
/// @param pose Pose whose ID and parent tables define Model indices.
/// @param model_index Candidate Model index in @p pose.
/// @return Non-zero when the active supported constraint writes that Model.
static int fbx_node_anim_constraint_writes_index(const fbx_constraint_t *constraint,
                                                 const fbx_constraint_pose_t *pose,
                                                 int32_t model_index) {
    if (!constraint || !pose || model_index < 0 || model_index >= pose->count ||
        !constraint->active || constraint->kind == FBX_CONSTRAINT_UNKNOWN)
        return 0;
    if (constraint->kind != FBX_CONSTRAINT_SINGLE_CHAIN_IK)
        return constraint->target_id == pose->ids[model_index];
    for (int32_t current = fbx_constraint_pose_index(pose, constraint->ik_end_id), guard = 0;
         current >= 0 && guard++ <= pose->count;
         current = pose->parent_indices[current]) {
        if (current == model_index)
            return 1;
        if (pose->ids[current] == constraint->target_id)
            break;
    }
    return 0;
}

/// @brief Append transform sample times from all hierarchy and constraint dependencies of a Model.
/// @details Computes a transitive dependency closure containing the target, its ancestors,
///          and every source/world-up/IK role that can affect a constraint writing one of
///          those Models. Times from all marked layered or legacy curves are appended.
/// @param[in,out] times Address of the heap-grown source-tick array.
/// @param[in,out] count Number of initialized ticks in @p times.
/// @param[in,out] capacity Allocated tick capacity.
/// @param models Complete Model builder table.
/// @param model_count Number of entries in @p models.
/// @param model_index Target Model whose dependencies are traced.
/// @param constraints Optional parsed constraint set.
/// @param[in,out] pose Pose providing dependency IDs, parents, and scratch flags.
/// @param[in,out] context Loader context charged when the tick array grows.
/// @return Non-zero on success, or zero for invalid topology, sampling metadata,
///         budget rejection, or allocation failure.
static int fbx_node_anim_append_inherited_times(int64_t **times,
                                                int32_t *count,
                                                int32_t *capacity,
                                                const fbx_node_anim_model_builder_t *models,
                                                int32_t model_count,
                                                int32_t model_index,
                                                const fbx_constraint_set_t *constraints,
                                                fbx_constraint_pose_t *pose,
                                                fbx_load_context_t *context) {
    uint8_t *marked;
    if (!models || model_count <= 0 || model_index < 0 || model_index >= model_count || !pose ||
        pose->count != model_count || !pose->evaluation_flags)
        return 0;
    marked = pose->evaluation_flags;
    memset(marked, 0, (size_t)model_count * sizeof(*marked));
    marked[model_index] = 1;
    for (int32_t pass = 0; pass < model_count + (constraints ? constraints->count : 0); ++pass) {
        int progress = 0;
        for (int32_t current = 0; current < model_count; ++current) {
            int32_t parent;
            if (!marked[current])
                continue;
            parent = models[current].parent_index;
            if (parent >= 0 && !marked[parent]) {
                marked[parent] = 1;
                progress = 1;
            }
            if (!constraints)
                continue;
            for (int32_t ci = 0; ci < constraints->count; ++ci) {
                const fbx_constraint_t *constraint = &constraints->entries[ci];
                if (!fbx_node_anim_constraint_writes_index(constraint, pose, current))
                    continue;
                for (int32_t source = 0; source < constraint->source_count; ++source) {
                    int32_t dependency =
                        fbx_constraint_pose_index(pose, constraint->sources[source].model_id);
                    if (dependency >= 0 && !marked[dependency]) {
                        marked[dependency] = 1;
                        progress = 1;
                    }
                }
                const int64_t role_ids[] = {
                    constraint->world_up_id, constraint->ik_effector_id, constraint->ik_pole_id};
                for (size_t role = 0; role < sizeof(role_ids) / sizeof(role_ids[0]); ++role) {
                    int32_t dependency = fbx_constraint_pose_index(pose, role_ids[role]);
                    if (dependency >= 0 && !marked[dependency]) {
                        marked[dependency] = 1;
                        progress = 1;
                    }
                }
            }
        }
        if (!progress)
            break;
    }
    for (int32_t current = 0; current < model_count; ++current) {
        if (!marked[current])
            continue;
        if (models[current].animation.layer_stack) {
            if (!fbx_anim_layer_stack_append_times(times,
                                                   count,
                                                   capacity,
                                                   models[current].animation.layer_stack,
                                                   models[current].id,
                                                   1,
                                                   NULL,
                                                   context))
                return 0;
            continue;
        }
        for (int trs = 0; trs < 3; ++trs)
            for (int component = 0; component < 3; ++component)
                if (!fbx_anim_append_curve_sample_times(
                        times,
                        count,
                        capacity,
                        &models[current].animation.curves[trs][component],
                        context))
                    return 0;
    }
    return 1;
}

/// @brief Sample the complete Model graph, apply constraints, and derive one runtime local TRS.
/// @details Rebuilds every authored local matrix at @p source_time, converts coordinate
///          systems, evaluates hierarchy globals and constraints, then removes the runtime
///          parent transform. Skeletal parents are intentionally omitted because skeletal
///          motion is represented by the skeleton animation rather than scene-node parenting.
/// @param models Complete Model builder table.
/// @param model_count Number of entries in @p models.
/// @param model_index Target Model to decompose.
/// @param source_time FBX source tick to sample.
/// @param z_up Non-zero to convert matrices into runtime Y-up coordinates.
/// @param[in,out] pose Reusable evaluated pose scratch storage.
/// @param constraints Optional constraints applied after hierarchy composition.
/// @param[in,out] context Loader context used by constraint evaluation.
/// @param[out] position Receives local translation XYZ.
/// @param[out] quaternion Receives local rotation quaternion XYZW.
/// @param[out] scale Receives local scale XYZ.
/// @return Non-zero on success, or zero for invalid input, curve/constraint failure,
///         or a non-invertible parent transform.
static int fbx_node_anim_sample_runtime_trs(const fbx_node_anim_model_builder_t *models,
                                            int32_t model_count,
                                            int32_t model_index,
                                            int64_t source_time,
                                            int z_up,
                                            fbx_constraint_pose_t *pose,
                                            fbx_constraint_set_t *constraints,
                                            fbx_load_context_t *context,
                                            double position[3],
                                            double quaternion[4],
                                            double scale[3]) {
    double runtime_local[16];
    int32_t runtime_parent;
    if (!models || model_count <= 0 || model_index < 0 || model_index >= model_count || !pose ||
        pose->count != model_count || !context)
        return 0;
    pose->z_up = z_up;
    memset(pose->fixed_globals, 0, (size_t)model_count * sizeof(*pose->fixed_globals));
    for (int32_t current = 0; current < model_count; ++current) {
        if (!fbx_anim_sample_components(
                &models[current].animation, source_time, &pose->components[current]))
            return 0;
        fbx_build_model_transform_matrix(&pose->components[current],
                                         fbx_constraint_pose_local(pose, current));
        if (z_up)
            fbx_mat4_apply_zup(fbx_constraint_pose_local(pose, current));
    }
    if (!fbx_constraint_pose_recompute(pose) ||
        (constraints && !fbx_constraint_apply_set(constraints, pose, context)))
        return 0;
    runtime_parent = models[model_index].parent_index;
    if (runtime_parent >= 0 && models[runtime_parent].is_skeleton)
        runtime_parent = -1;
    if (runtime_parent >= 0) {
        double inverse_parent[16];
        if (!fbx_mat4_inverse_affine(fbx_constraint_pose_global(pose, runtime_parent),
                                     inverse_parent))
            return 0;
        fbx_mat4_mul_local(
            inverse_parent, fbx_constraint_pose_global(pose, model_index), runtime_local);
    } else {
        memcpy(runtime_local, fbx_constraint_pose_global(pose, model_index), sizeof(runtime_local));
    }
    fbx_decompose_matrix_trs(runtime_local, position, quaternion, scale);
    return 1;
}

/// @brief Add the T/R/S channels for one Model affected by this animation stack.
/// @details Samples all inherited dependency ticks, preserves quaternion hemisphere
///          continuity, and publishes translation, rotation, and scale channels targeted
///          by the Model's stable import index.
/// @param[in,out] clip Runtime NodeAnimation3D receiving new channels.
/// @param models Complete Model builder table.
/// @param model_count Number of entries in @p models.
/// @param model_index Model to sample and target.
/// @param clip_start Earliest FBX source tick, mapped to runtime time zero.
/// @param z_up Non-zero to convert evaluated transforms to runtime Y-up space.
/// @param[in,out] pose Reusable evaluated pose storage.
/// @param constraints Optional constraint set applied at every sample.
/// @param[in,out] context Loader context charged for samples and runtime channels.
/// @return 1 when channels were emitted, 0 when unaffected, or -1 on hard failure.
static int fbx_node_anim_emit_model_channels(void *clip,
                                             const fbx_node_anim_model_builder_t *models,
                                             int32_t model_count,
                                             int32_t model_index,
                                             int64_t clip_start,
                                             int z_up,
                                             fbx_constraint_pose_t *pose,
                                             fbx_constraint_set_t *constraints,
                                             fbx_load_context_t *context) {
    int64_t *source_times = NULL;
    int32_t source_count = 0;
    int32_t source_capacity = 0;
    double *times = NULL;
    float *translations = NULL;
    float *rotations = NULL;
    float *scales = NULL;
    rt_string target_name = NULL;
    int64_t channel_indices[3] = {-1, -1, -1};
    if (!fbx_node_anim_append_inherited_times(&source_times,
                                              &source_count,
                                              &source_capacity,
                                              models,
                                              model_count,
                                              model_index,
                                              constraints,
                                              pose,
                                              context)) {
        free(source_times);
        return -1;
    }
    fbx_anim_sort_unique_times(source_times, &source_count);
    if (source_count == 0) {
        free(source_times);
        return 0;
    }
    if ((size_t)source_count > SIZE_MAX / sizeof(*times) ||
        (size_t)source_count > SIZE_MAX / (4u * sizeof(float)) ||
        !fbx_budget_charge(context, (uint64_t)source_count, sizeof(*times)) ||
        !fbx_budget_charge(context, (uint64_t)source_count, 10u * sizeof(float)) ||
        !fbx_budget_charge(context, 3u, sizeof(rt_node_anim_channel3d)) ||
        !fbx_budget_charge(context, (uint64_t)source_count, 10u * sizeof(float)) ||
        !fbx_budget_charge(context, (uint64_t)source_count, 3u * sizeof(double))) {
        free(source_times);
        return -1;
    }
    times = (double *)malloc((size_t)source_count * sizeof(*times));
    translations = (float *)malloc((size_t)source_count * 3u * sizeof(*translations));
    rotations = (float *)malloc((size_t)source_count * 4u * sizeof(*rotations));
    scales = (float *)malloc((size_t)source_count * 3u * sizeof(*scales));
    if (!times || !translations || !rotations || !scales)
        goto fail;
    for (int32_t i = 0; i < source_count; ++i) {
        double p[3];
        double q[4];
        double s[3];
        if (!fbx_node_anim_sample_runtime_trs(models,
                                              model_count,
                                              model_index,
                                              source_times[i],
                                              z_up,
                                              pose,
                                              constraints,
                                              context,
                                              p,
                                              q,
                                              s)) {
            goto fail;
        }
        times[i] = ((double)source_times[i] - (double)clip_start) / (double)FBX_TIME_SECOND;
        for (int component = 0; component < 3; ++component) {
            translations[(size_t)i * 3u + (size_t)component] = (float)p[component];
            scales[(size_t)i * 3u + (size_t)component] = (float)s[component];
        }
        for (int component = 0; component < 4; ++component)
            rotations[(size_t)i * 4u + (size_t)component] = (float)q[component];
        if (i > 0) {
            float *previous = &rotations[(size_t)(i - 1) * 4u];
            float *current = &rotations[(size_t)i * 4u];
            double dot = (double)previous[0] * current[0] + (double)previous[1] * current[1] +
                         (double)previous[2] * current[2] + (double)previous[3] * current[3];
            if (dot < 0.0)
                for (int component = 0; component < 4; ++component)
                    current[component] = -current[component];
        }
    }
    target_name = fbx_object_name_string(models[model_index].source, 1);
    if (!target_name || rt_str_is_empty(target_name)) {
        char generated[64];
        rt_string_unref(target_name);
        snprintf(generated, sizeof(generated), "Model_%d", models[model_index].import_index);
        target_name = rt_const_cstr(generated);
    }
    channel_indices[0] = rt_node_animation3d_add_channel(clip,
                                                         target_name,
                                                         RT_NODE_ANIM_PATH_TRANSLATION,
                                                         RT_NODE_ANIM_INTERP_LINEAR,
                                                         source_count,
                                                         3,
                                                         times,
                                                         translations);
    channel_indices[1] = rt_node_animation3d_add_channel(clip,
                                                         target_name,
                                                         RT_NODE_ANIM_PATH_ROTATION,
                                                         RT_NODE_ANIM_INTERP_LINEAR,
                                                         source_count,
                                                         4,
                                                         times,
                                                         rotations);
    channel_indices[2] = rt_node_animation3d_add_channel(clip,
                                                         target_name,
                                                         RT_NODE_ANIM_PATH_SCALE,
                                                         RT_NODE_ANIM_INTERP_LINEAR,
                                                         source_count,
                                                         3,
                                                         times,
                                                         scales);
    if (channel_indices[0] < 0 || channel_indices[1] < 0 || channel_indices[2] < 0)
        goto fail;
    for (int path = 0; path < 3; ++path)
        rt_node_animation3d_set_channel_target_node_index(
            clip, channel_indices[path], models[model_index].import_index);
    rt_string_unref(target_name);
    free(times);
    free(translations);
    free(rotations);
    free(scales);
    free(source_times);
    return 1;
fail:
    rt_string_unref(target_name);
    free(times);
    free(translations);
    free(rotations);
    free(scales);
    free(source_times);
    return -1;
}

/// @brief Bit flags for raw FBX camera properties that affect canonical runtime channels.
enum {
    /// Projection-mode enum.
    FBX_CAMERA_ANIM_PROJECTION = 1u << 0,
    /// Aperture-mode enum used while deriving vertical field of view.
    FBX_CAMERA_ANIM_APERTURE_MODE = 1u << 1,
    /// Generic field-of-view angle.
    FBX_CAMERA_ANIM_FOV = 1u << 2,
    /// Horizontal field-of-view angle.
    FBX_CAMERA_ANIM_FOV_X = 1u << 3,
    /// Vertical field-of-view angle.
    FBX_CAMERA_ANIM_FOV_Y = 1u << 4,
    /// Camera focal length.
    FBX_CAMERA_ANIM_FOCAL_LENGTH = 1u << 5,
    /// Film aperture width.
    FBX_CAMERA_ANIM_FILM_WIDTH = 1u << 6,
    /// Film aperture height.
    FBX_CAMERA_ANIM_FILM_HEIGHT = 1u << 7,
    /// Render aspect-width component.
    FBX_CAMERA_ANIM_ASPECT_WIDTH = 1u << 8,
    /// Render aspect-height component.
    FBX_CAMERA_ANIM_ASPECT_HEIGHT = 1u << 9,
    /// Near clipping plane.
    FBX_CAMERA_ANIM_NEAR = 1u << 10,
    /// Far clipping plane.
    FBX_CAMERA_ANIM_FAR = 1u << 11,
    /// Orthographic zoom/size.
    FBX_CAMERA_ANIM_ORTHO_ZOOM = 1u << 12
};

/// @brief Map a raw FBX camera property to its canonical animation dependency bit.
/// @param property FBX connection property name.
/// @return One @ref FBX_CAMERA_ANIM_PROJECTION-style bit, or zero when unrecognized.
static uint32_t fbx_node_anim_camera_property_flag(const char *property) {
    if (fbx_ascii_streq_ignore_case(property, "CameraProjectionType"))
        return FBX_CAMERA_ANIM_PROJECTION;
    if (fbx_ascii_streq_ignore_case(property, "ApertureMode"))
        return FBX_CAMERA_ANIM_APERTURE_MODE;
    if (fbx_ascii_streq_ignore_case(property, "FieldOfView"))
        return FBX_CAMERA_ANIM_FOV;
    if (fbx_ascii_streq_ignore_case(property, "FieldOfViewX"))
        return FBX_CAMERA_ANIM_FOV_X;
    if (fbx_ascii_streq_ignore_case(property, "FieldOfViewY"))
        return FBX_CAMERA_ANIM_FOV_Y;
    if (fbx_ascii_streq_ignore_case(property, "FocalLength"))
        return FBX_CAMERA_ANIM_FOCAL_LENGTH;
    if (fbx_ascii_streq_ignore_case(property, "FilmWidth"))
        return FBX_CAMERA_ANIM_FILM_WIDTH;
    if (fbx_ascii_streq_ignore_case(property, "FilmHeight"))
        return FBX_CAMERA_ANIM_FILM_HEIGHT;
    if (fbx_ascii_streq_ignore_case(property, "AspectWidth"))
        return FBX_CAMERA_ANIM_ASPECT_WIDTH;
    if (fbx_ascii_streq_ignore_case(property, "AspectHeight"))
        return FBX_CAMERA_ANIM_ASPECT_HEIGHT;
    if (fbx_ascii_streq_ignore_case(property, "NearPlane"))
        return FBX_CAMERA_ANIM_NEAR;
    if (fbx_ascii_streq_ignore_case(property, "FarPlane"))
        return FBX_CAMERA_ANIM_FAR;
    if (fbx_ascii_streq_ignore_case(property, "OrthoZoom") ||
        fbx_ascii_streq_ignore_case(property, "CameraOrthoZoom"))
        return FBX_CAMERA_ANIM_ORTHO_ZOOM;
    return 0;
}

/// @brief Append all active camera curve/layer-weight times and report affected properties.
/// @details Step-valued enum curves also contribute the tick immediately before each
///          transition so runtime STEP interpolation preserves the old value.
/// @param[in,out] times Address of the heap-grown source-tick array.
/// @param[in,out] count Number of initialized ticks.
/// @param[in,out] capacity Allocated tick capacity.
/// @param stack Parsed layered-animation stack.
/// @param attribute_id Camera NodeAttribute identifier, or zero.
/// @param model_id Owning Camera Model identifier.
/// @param[out] out_flags Receives the union of animated raw-property flags.
/// @param[in,out] context Loader context charged when the tick array grows.
/// @return Non-zero on success, or zero for invalid input, budget rejection,
///         or allocation failure.
static int fbx_node_anim_append_camera_times(int64_t **times,
                                             int32_t *count,
                                             int32_t *capacity,
                                             const fbx_anim_layer_stack_t *stack,
                                             int64_t attribute_id,
                                             int64_t model_id,
                                             uint32_t *out_flags,
                                             fbx_load_context_t *context) {
    uint32_t flags = 0;
    if (!times || !count || !capacity || !stack || !out_flags || !context)
        return 0;
    for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
        int layer_used = 0;
        if (!fbx_anim_layer_is_active(stack, layer_index))
            continue;
        for (int32_t channel_index = 0; channel_index < stack->channel_count; ++channel_index) {
            const fbx_anim_layer_channel_t *channel = &stack->channels[channel_index];
            uint32_t flag;
            if (channel->layer_index != layer_index || channel->transform_path >= 0 ||
                (channel->target_id != attribute_id && channel->target_id != model_id))
                continue;
            flag = fbx_node_anim_camera_property_flag(channel->property);
            if (flag == 0)
                continue;
            if (channel->curve_mask == 0 && channel->default_mask == 0)
                continue;
            flags |= flag;
            layer_used = 1;
            for (int component = 0; component < 3; ++component) {
                const fbx_anim_curve_view_t *curve = &channel->curves[component];
                if (!fbx_anim_append_curve_sample_times(times, count, capacity, curve, context))
                    return 0;
                if ((flag == FBX_CAMERA_ANIM_PROJECTION || flag == FBX_CAMERA_ANIM_APERTURE_MODE) &&
                    fbx_anim_curve_has_data(curve)) {
                    /* Enum properties are step-valued even when an exporter omits interpolation
                     * flags. Preserve the old value through the final representable source tick. */
                    for (uint32_t key = 1; key < curve->count; ++key)
                        if (curve->times[key] - 1 > curve->times[key - 1] &&
                            !fbx_anim_append_time(
                                times, count, capacity, curve->times[key] - 1, context))
                            return 0;
                }
            }
        }
        if (layer_used &&
            !fbx_anim_append_curve_sample_times(
                times, count, capacity, &stack->layers[layer_index].weight_curve, context))
            return 0;
    }
    *out_flags = flags;
    return 1;
}

/// @brief Find one canonical camera property channel on a layer, preferring NodeAttribute.
/// @param stack Parsed layered-animation stack.
/// @param layer_index Layer whose channels are searched.
/// @param attribute_id Camera NodeAttribute identifier, or zero.
/// @param model_id Owning Camera Model identifier.
/// @param property_flag Canonical raw-property bit to match.
/// @return Borrowed matching NodeAttribute channel, otherwise a Model-targeted fallback,
///         or NULL when the property is absent.
static const fbx_anim_layer_channel_t *fbx_node_anim_find_camera_channel(
    const fbx_anim_layer_stack_t *stack,
    int32_t layer_index,
    int64_t attribute_id,
    int64_t model_id,
    uint32_t property_flag) {
    const fbx_anim_layer_channel_t *model_channel = NULL;
    if (!stack)
        return NULL;
    for (int32_t channel_index = 0; channel_index < stack->channel_count; ++channel_index) {
        const fbx_anim_layer_channel_t *channel = &stack->channels[channel_index];
        if (channel->layer_index != layer_index || channel->transform_path >= 0 ||
            fbx_node_anim_camera_property_flag(channel->property) != property_flag)
            continue;
        if (attribute_id != 0 && channel->target_id == attribute_id)
            return channel;
        if (channel->target_id == model_id)
            model_channel = channel;
    }
    return model_channel;
}

/// @brief Compose one raw camera scalar across all active FBX animation layers.
/// @param stack Parsed layered-animation stack.
/// @param attribute_id Camera NodeAttribute identifier, or zero.
/// @param model_id Owning Camera Model identifier.
/// @param property_flag Canonical property bit to sample.
/// @param base Static property value used before layer composition.
/// @param source_time FBX source tick to evaluate.
/// @param[out] out Receives the finite composed scalar.
/// @return Non-zero on success, or zero when input or channel evaluation is invalid.
static int fbx_node_anim_sample_camera_scalar(const fbx_anim_layer_stack_t *stack,
                                              int64_t attribute_id,
                                              int64_t model_id,
                                              uint32_t property_flag,
                                              double base,
                                              int64_t source_time,
                                              double *out) {
    double value = base;
    if (!stack || !out)
        return 0;
    for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
        const fbx_anim_layer_channel_t *channel;
        double layer_value;
        double weight;
        int present;
        int blend_mode;
        if (!fbx_anim_layer_is_active(stack, layer_index))
            continue;
        channel = fbx_node_anim_find_camera_channel(
            stack, layer_index, attribute_id, model_id, property_flag);
        if (!channel)
            continue;
        weight = fbx_anim_layer_weight_at(&stack->layers[layer_index], source_time);
        if (weight <= 0.0)
            continue;
        blend_mode = layer_index == 0 ? 1 : stack->layers[layer_index].blend_mode;
        if (!fbx_anim_layer_channel_value(channel, 0, source_time, base, &layer_value, &present))
            return 0;
        if (!present && blend_mode != 1)
            continue;
        if (blend_mode == 0)
            value += layer_value * weight;
        else
            value += (layer_value - value) * weight;
    }
    if (!isfinite(value))
        return 0;
    *out = value;
    return 1;
}

/// @brief Evaluate all animated raw camera properties at one source tick.
/// @details Starts from the static property state, composes only flagged lanes,
///          canonicalizes blended enum values at half-integer boundaries, and validates
///          the resulting projection/aperture modes.
/// @param stack Parsed layered-animation stack.
/// @param attribute_id Camera NodeAttribute identifier, or zero.
/// @param model_id Owning Camera Model identifier.
/// @param flags Union of raw camera-property bits to sample.
/// @param base Valid static camera-property state.
/// @param source_time FBX source tick to evaluate.
/// @param[out] out Receives the composed and enum-normalized state.
/// @return Non-zero on success, or zero for invalid input, sampling failure,
///         non-finite enum values, or unsupported enum ranges.
static int fbx_node_anim_sample_camera_state(const fbx_anim_layer_stack_t *stack,
                                             int64_t attribute_id,
                                             int64_t model_id,
                                             uint32_t flags,
                                             const fbx_camera_property_state_t *base,
                                             int64_t source_time,
                                             fbx_camera_property_state_t *out) {
    if (!stack || !base || !out)
        return 0;
    *out = *base;
#define FBX_SAMPLE_CAMERA_PROPERTY(flag_, member_)                                                 \
    do {                                                                                           \
        if ((flags & (flag_)) != 0 && !fbx_node_anim_sample_camera_scalar(stack,                   \
                                                                          attribute_id,            \
                                                                          model_id,                \
                                                                          (flag_),                 \
                                                                          base->member_,           \
                                                                          source_time,             \
                                                                          &out->member_))          \
            return 0;                                                                              \
    } while (0)
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_PROJECTION, projection_type);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_APERTURE_MODE, aperture_mode);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FOV, field_of_view);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FOV_X, field_of_view_x);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FOV_Y, field_of_view_y);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FOCAL_LENGTH, focal_length);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FILM_WIDTH, film_width);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FILM_HEIGHT, film_height);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_ASPECT_WIDTH, aspect_width);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_ASPECT_HEIGHT, aspect_height);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_NEAR, near_plane);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_FAR, far_plane);
    FBX_SAMPLE_CAMERA_PROPERTY(FBX_CAMERA_ANIM_ORTHO_ZOOM, ortho_zoom);
#undef FBX_SAMPLE_CAMERA_PROPERTY
    if (flags & FBX_CAMERA_ANIM_FOV)
        out->has_field_of_view = 1;
    if (flags & FBX_CAMERA_ANIM_FOV_X)
        out->has_field_of_view_x = 1;
    if (flags & FBX_CAMERA_ANIM_FOV_Y)
        out->has_field_of_view_y = 1;
    if (flags & FBX_CAMERA_ANIM_FOCAL_LENGTH)
        out->has_focal_length = 1;
    if (out->projection_type < 0.0 || out->projection_type > 1.0 || out->aperture_mode < 0.0 ||
        out->aperture_mode > 3.0)
        return 0;
    /* Layer weights can blend enum lanes. Resolve the composed value at the halfway boundary,
     * then publish it through a STEP runtime channel. */
    out->projection_type = floor(out->projection_type + 0.5);
    out->aperture_mode = floor(out->aperture_mode + 0.5);
    return fbx_camera_state_validate_enums(out);
}

/// @brief Emit derived runtime camera projection channels for one FBX camera Model.
/// @details Samples raw FBX camera attributes at every contributing curve/weight time,
///          derives canonical projection mode, aspect, vertical field of view, clip
///          planes, and orthographic size, then publishes only affected runtime lanes.
/// @param[in,out] clip Runtime NodeAnimation3D receiving derived camera channels.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Connection table used to resolve the Camera NodeAttribute.
/// @param model Camera Model builder and stable target index.
/// @param stack Parsed layered-animation stack.
/// @param clip_start Earliest FBX source tick, mapped to runtime time zero.
/// @param[in,out] context Loader context charged for samples and runtime channels.
/// @return 1 when channels were emitted, 0 when the stack does not animate this camera, -1 on
///         allocation, validation, or channel-publication failure.
static int fbx_node_anim_emit_camera_channels(void *clip,
                                              fbx_node_t *objects,
                                              const fbx_conn_table_t *ct,
                                              const fbx_node_anim_model_builder_t *model,
                                              const fbx_anim_layer_stack_t *stack,
                                              int64_t clip_start,
                                              fbx_load_context_t *context) {
    enum {
        FBX_CAMERA_VALUE_PROJECTION = 0,
        FBX_CAMERA_VALUE_ASPECT = 1,
        FBX_CAMERA_VALUE_FOV = 2,
        FBX_CAMERA_VALUE_NEAR = 3,
        FBX_CAMERA_VALUE_FAR = 4,
        FBX_CAMERA_VALUE_ORTHO = 5,
        FBX_CAMERA_VALUE_LANES = 6
    };

    static const int64_t paths[FBX_CAMERA_VALUE_LANES] = {RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE,
                                                          RT_NODE_ANIM_PATH_CAMERA_ASPECT,
                                                          RT_NODE_ANIM_PATH_CAMERA_FOV,
                                                          RT_NODE_ANIM_PATH_CAMERA_NEAR,
                                                          RT_NODE_ANIM_PATH_CAMERA_FAR,
                                                          RT_NODE_ANIM_PATH_CAMERA_ORTHO_SIZE};
    fbx_node_t *attribute;
    fbx_camera_property_state_t base;
    int64_t attribute_id;
    int64_t *source_times = NULL;
    int32_t source_count = 0;
    int32_t source_capacity = 0;
    uint32_t raw_flags = 0;
    uint32_t output_flags = 0;
    double *times = NULL;
    float *values = NULL;
    rt_string target_name = NULL;
    int emitted = 0;
    if (!clip || !objects || !ct || !model || !stack || !context)
        return 0;
    attribute = fbx_find_model_node_attribute(objects, ct, model->id, "Camera");
    if (!attribute && !(model->source->prop_count >= 3 &&
                        fbx_ascii_equals_i(fbx_prop_str(model->source, 2), "Camera")))
        return 0;
    attribute_id = attribute && attribute->prop_count > 0 ? fbx_prop_i64(attribute, 0) : 0;
    if (!fbx_node_anim_append_camera_times(&source_times,
                                           &source_count,
                                           &source_capacity,
                                           stack,
                                           attribute_id,
                                           model->id,
                                           &raw_flags,
                                           context))
        goto fail;
    fbx_anim_sort_unique_times(source_times, &source_count);
    if (raw_flags == 0 || source_count == 0) {
        free(source_times);
        return 0;
    }
    fbx_read_camera_property_state(attribute, model->source, &base);
    if (!fbx_camera_state_validate_enums(&base))
        goto fail;
    if (raw_flags & FBX_CAMERA_ANIM_PROJECTION)
        output_flags |= 1u << FBX_CAMERA_VALUE_PROJECTION;
    if (raw_flags & (FBX_CAMERA_ANIM_ASPECT_WIDTH | FBX_CAMERA_ANIM_ASPECT_HEIGHT))
        output_flags |= 1u << FBX_CAMERA_VALUE_ASPECT;
    if (raw_flags & (FBX_CAMERA_ANIM_APERTURE_MODE | FBX_CAMERA_ANIM_FOV | FBX_CAMERA_ANIM_FOV_X |
                     FBX_CAMERA_ANIM_FOV_Y | FBX_CAMERA_ANIM_FOCAL_LENGTH |
                     FBX_CAMERA_ANIM_FILM_WIDTH | FBX_CAMERA_ANIM_FILM_HEIGHT |
                     FBX_CAMERA_ANIM_ASPECT_WIDTH | FBX_CAMERA_ANIM_ASPECT_HEIGHT))
        output_flags |= 1u << FBX_CAMERA_VALUE_FOV;
    if (raw_flags & FBX_CAMERA_ANIM_NEAR)
        output_flags |= 1u << FBX_CAMERA_VALUE_NEAR;
    if (raw_flags & FBX_CAMERA_ANIM_FAR)
        output_flags |= 1u << FBX_CAMERA_VALUE_FAR;
    if (raw_flags & FBX_CAMERA_ANIM_ORTHO_ZOOM)
        output_flags |= 1u << FBX_CAMERA_VALUE_ORTHO;
    if ((size_t)source_count > SIZE_MAX / sizeof(*times) ||
        (size_t)source_count > SIZE_MAX / (FBX_CAMERA_VALUE_LANES * sizeof(*values)) ||
        !fbx_budget_charge(context, (uint64_t)source_count, sizeof(*times)) ||
        !fbx_budget_charge(
            context, (uint64_t)source_count * FBX_CAMERA_VALUE_LANES, sizeof(*values)))
        goto fail;
    times = (double *)malloc((size_t)source_count * sizeof(*times));
    values = (float *)calloc((size_t)source_count * FBX_CAMERA_VALUE_LANES, sizeof(*values));
    if (!times || !values)
        goto fail;
    for (int32_t key = 0; key < source_count; ++key) {
        fbx_camera_property_state_t sampled;
        if (!fbx_node_anim_sample_camera_state(
                stack, attribute_id, model->id, raw_flags, &base, source_times[key], &sampled))
            goto fail;
        times[key] = ((double)source_times[key] - (double)clip_start) / (double)FBX_TIME_SECOND;
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_PROJECTION] =
            (float)sampled.projection_type;
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_ASPECT] =
            (float)fbx_camera_state_aspect(&sampled);
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_FOV] =
            (float)fbx_camera_state_vertical_fov(&sampled);
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_NEAR] =
            (float)sampled.near_plane;
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_FAR] =
            (float)sampled.far_plane;
        values[(size_t)key * FBX_CAMERA_VALUE_LANES + FBX_CAMERA_VALUE_ORTHO] =
            (float)fabs(sampled.ortho_zoom);
        for (int lane = 0; lane < FBX_CAMERA_VALUE_LANES; ++lane)
            if (!isfinite(values[(size_t)key * FBX_CAMERA_VALUE_LANES + (size_t)lane]))
                goto fail;
    }
    target_name = fbx_object_name_string(model->source, 1);
    if (!target_name || rt_str_is_empty(target_name)) {
        char generated[64];
        rt_string_unref(target_name);
        snprintf(generated, sizeof(generated), "Model_%d", model->import_index);
        target_name = rt_const_cstr(generated);
    }
    for (int lane = 0; lane < FBX_CAMERA_VALUE_LANES; ++lane) {
        float *lane_values;
        int64_t channel_index;
        int interpolation = lane == FBX_CAMERA_VALUE_PROJECTION ? RT_NODE_ANIM_INTERP_STEP
                                                                : RT_NODE_ANIM_INTERP_LINEAR;
        if ((output_flags & (1u << lane)) == 0)
            continue;
        if (!fbx_budget_charge(context, 1u, sizeof(rt_node_anim_channel3d)) ||
            !fbx_budget_charge(context, (uint64_t)source_count, sizeof(*times)) ||
            !fbx_budget_charge(context, (uint64_t)source_count, sizeof(*lane_values)))
            goto fail;
        lane_values = (float *)malloc((size_t)source_count * sizeof(*lane_values));
        if (!lane_values)
            goto fail;
        for (int32_t key = 0; key < source_count; ++key)
            lane_values[key] = values[(size_t)key * FBX_CAMERA_VALUE_LANES + (size_t)lane];
        channel_index = rt_node_animation3d_add_channel(
            clip, target_name, paths[lane], interpolation, source_count, 1, times, lane_values);
        free(lane_values);
        if (channel_index < 0)
            goto fail;
        rt_node_animation3d_set_channel_target_node_index(clip, channel_index, model->import_index);
        emitted = 1;
    }
    rt_string_unref(target_name);
    free(source_times);
    free(times);
    free(values);
    return emitted;
fail:
    rt_asset_error_set_if_empty(
        context && context->budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
        context && context->budget_exhausted
            ? "FBX.Load: camera animation exceeds the aggregate load budget"
            : "FBX.Load: invalid or incomplete camera projection animation");
    rt_string_unref(target_name);
    free(source_times);
    free(times);
    free(values);
    return -1;
}

/// @brief Test whether a Model has a standard FBX object connection to a Geometry.
/// @param ct Connection table containing Geometry-to-Model adjacency.
/// @param model_id Candidate Model identifier.
/// @param geometry_id Candidate Geometry identifier.
/// @return Non-zero when the Geometry is connected as a child of the Model.
static int fbx_node_anim_model_uses_geometry(const fbx_conn_table_t *ct,
                                             int64_t model_id,
                                             int64_t geometry_id) {
    if (!ct)
        return 0;
    for (int32_t connection = fbx_connections_first_for_child(ct, geometry_id); connection >= 0;
         connection = fbx_connections_next_for_child(ct, connection))
        if (ct->entries[connection].parent_id == model_id)
            return 1;
    return 0;
}

/// @brief Add source ticks where a DeformPercent curve crosses progressive-weight breakpoints.
/// @details FullWeights turns a scalar curve into a piecewise-linear vector. Authored/adaptive
///          scalar samples alone can straddle a breakpoint and would then interpolate the wrong
///          shape vector, so crossings are located by integer-tick bisection and inserted.
/// @param[in,out] times Address of the heap-grown, initially populated source-tick array.
/// @param[in,out] count Number of ticks, sorted/deduplicated before breakpoint analysis.
/// @param[in,out] capacity Allocated tick capacity.
/// @param binding Progressive channel metadata and ordered thresholds.
/// @param curve Legacy scalar curve used when @p layer_stack is NULL.
/// @param layer_stack Optional layered sampler for the channel.
/// @param[in,out] context Loader context charged when new crossing ticks are appended.
/// @return Non-zero on success or when no usable curve exists; zero on sampling,
///         budget, or allocation failure.
static int fbx_node_anim_append_morph_threshold_times(int64_t **times,
                                                      int32_t *count,
                                                      int32_t *capacity,
                                                      const fbx_morph_channel_binding_t *binding,
                                                      const fbx_anim_curve_view_t *curve,
                                                      const fbx_anim_layer_stack_t *layer_stack,
                                                      fbx_load_context_t *context) {
    int32_t base_count;
    if (!times || !count || !capacity || !binding || !curve || !context ||
        (!layer_stack && !fbx_anim_curve_has_data(curve)) || binding->shape_count <= 0 ||
        !binding->full_weights) {
        return 1;
    }
    fbx_anim_sort_unique_times(*times, count);
    base_count = *count;
    for (int32_t interval = 0; interval + 1 < base_count; ++interval) {
        int64_t interval_start = (*times)[interval];
        int64_t interval_end = (*times)[interval + 1];
        for (int32_t threshold_index = -1; threshold_index < binding->shape_count;
             ++threshold_index) {
            double threshold = threshold_index < 0 ? -binding->full_weights[0]
                                                   : binding->full_weights[threshold_index];
            int64_t lo = interval_start;
            int64_t hi = interval_end;
            double lo_percent;
            double hi_percent;
            double lo_delta;
            double hi_delta;
            if (layer_stack) {
                if (!fbx_anim_layer_sample_scalar(layer_stack,
                                                  binding->channel_id,
                                                  NULL,
                                                  binding->default_percent,
                                                  lo,
                                                  &lo_percent,
                                                  NULL) ||
                    !fbx_anim_layer_sample_scalar(layer_stack,
                                                  binding->channel_id,
                                                  NULL,
                                                  binding->default_percent,
                                                  hi,
                                                  &hi_percent,
                                                  NULL)) {
                    return 0;
                }
            } else {
                lo_percent = fbx_anim_curve_value(curve, lo, binding->default_percent);
                hi_percent = fbx_anim_curve_value(curve, hi, binding->default_percent);
            }
            lo_delta = lo_percent - threshold;
            hi_delta = hi_percent - threshold;
            if (!isfinite(lo_delta) || !isfinite(hi_delta) || lo_delta == 0.0 || hi_delta == 0.0 ||
                (lo_delta < 0.0) == (hi_delta < 0.0)) {
                continue;
            }
            while (hi - lo > 1) {
                int64_t midpoint = lo + (hi - lo) / 2;
                double midpoint_percent;
                double midpoint_delta;
                if (layer_stack) {
                    if (!fbx_anim_layer_sample_scalar(layer_stack,
                                                      binding->channel_id,
                                                      NULL,
                                                      binding->default_percent,
                                                      midpoint,
                                                      &midpoint_percent,
                                                      NULL)) {
                        return 0;
                    }
                } else {
                    midpoint_percent =
                        fbx_anim_curve_value(curve, midpoint, binding->default_percent);
                }
                midpoint_delta = midpoint_percent - threshold;
                if (!isfinite(midpoint_delta))
                    return 0;
                if (midpoint_delta == 0.0) {
                    lo = midpoint;
                    hi = midpoint;
                    break;
                }
                if ((midpoint_delta < 0.0) == (lo_delta < 0.0)) {
                    lo = midpoint;
                    lo_delta = midpoint_delta;
                } else {
                    hi = midpoint;
                }
            }
            if (!fbx_anim_append_time(times, count, capacity, hi, context))
                return 0;
        }
    }
    return 1;
}

/// @brief Emit one combined weight-vector channel per Model instance of each animated morph mesh.
/// @details Groups bindings by mesh, combines all channel percentages into the mesh's
///          full runtime shape vector at every source/breakpoint tick, and duplicates
///          the resulting channel for every scene Model that instances the Geometry.
/// @param[in,out] clip Runtime NodeAnimation3D receiving weight channels.
/// @param objects Parsed FBX `Objects` node; currently reserved for graph extensions.
/// @param ct Connection table used to find Model instances of each Geometry.
/// @param models Complete Model builder table.
/// @param model_count Number of entries in @p models.
/// @param asset Loaded asset providing per-mesh MorphTarget3D resources.
/// @param bindings Imported progressive morph-channel metadata.
/// @param curves Legacy scalar curve views parallel to @p bindings.
/// @param layer_stack Optional authoritative layered-animation sampler.
/// @param binding_count Number of entries in @p bindings and @p curves.
/// @param clip_start Earliest FBX source tick, mapped to runtime time zero.
/// @param[in,out] context Loader context charged for samples and runtime channels.
/// @return 1 when any channel was emitted, 0 when none were targetable, or -1 on hard failure.
static int fbx_node_anim_emit_morph_channels(void *clip,
                                             fbx_node_t *objects,
                                             const fbx_conn_table_t *ct,
                                             const fbx_node_anim_model_builder_t *models,
                                             int32_t model_count,
                                             const rt_fbx_asset *asset,
                                             const fbx_morph_channel_binding_t *bindings,
                                             const fbx_anim_curve_view_t *curves,
                                             const fbx_anim_layer_stack_t *layer_stack,
                                             int32_t binding_count,
                                             int64_t clip_start,
                                             fbx_load_context_t *context) {
    int emitted = 0;
    (void)objects;
    if (!clip || !ct || !models || model_count <= 0 || !asset || !bindings || !curves ||
        binding_count <= 0 || !context) {
        return 0;
    }
    for (int32_t first = 0; first < binding_count; ++first) {
        int already_processed = 0;
        int has_animation = 0;
        int32_t mesh_index = bindings[first].mesh_index;
        int64_t geometry_id = bindings[first].geometry_id;
        int64_t *source_times = NULL;
        int32_t source_count = 0;
        int32_t source_capacity = 0;
        double *times = NULL;
        float *values = NULL;
        int64_t shape_count64;
        int32_t shape_count;
        for (int32_t previous = 0; previous < first; ++previous)
            if (bindings[previous].mesh_index == mesh_index) {
                already_processed = 1;
                break;
            }
        if (already_processed || mesh_index < 0 || mesh_index >= asset->morph_count ||
            !asset->morph_targets || !asset->morph_targets[mesh_index])
            continue;
        for (int32_t i = 0; i < binding_count; ++i) {
            int32_t previous_count = source_count;
            if (bindings[i].mesh_index != mesh_index)
                continue;
            if (layer_stack && !fbx_anim_layer_stack_append_times(&source_times,
                                                                  &source_count,
                                                                  &source_capacity,
                                                                  layer_stack,
                                                                  bindings[i].channel_id,
                                                                  0,
                                                                  NULL,
                                                                  context)) {
                free(source_times);
                return -1;
            }
            if (source_count > previous_count || fbx_anim_curve_has_data(&curves[i]))
                has_animation = 1;
            if (!layer_stack &&
                !fbx_anim_append_curve_sample_times(
                    &source_times, &source_count, &source_capacity, &curves[i], context)) {
                free(source_times);
                return -1;
            }
        }
        if (!has_animation) {
            free(source_times);
            continue;
        }
        fbx_anim_sort_unique_times(source_times, &source_count);
        for (int32_t i = 0; i < binding_count; ++i) {
            if (bindings[i].mesh_index == mesh_index &&
                !fbx_node_anim_append_morph_threshold_times(&source_times,
                                                            &source_count,
                                                            &source_capacity,
                                                            &bindings[i],
                                                            &curves[i],
                                                            layer_stack,
                                                            context)) {
                free(source_times);
                return -1;
            }
        }
        fbx_anim_sort_unique_times(source_times, &source_count);
        shape_count64 = rt_morphtarget3d_get_shape_count(asset->morph_targets[mesh_index]);
        if (source_count <= 0 || shape_count64 <= 0 || shape_count64 > 4096 ||
            (uint64_t)source_count * (uint64_t)shape_count64 > 4000000u ||
            (size_t)source_count > SIZE_MAX / sizeof(*times) ||
            (size_t)source_count > SIZE_MAX / ((size_t)shape_count64 * sizeof(*values))) {
            free(source_times);
            return -1;
        }
        shape_count = (int32_t)shape_count64;
        if (!fbx_budget_charge(context, (uint64_t)source_count, sizeof(*times)) ||
            !fbx_budget_charge(
                context, (uint64_t)source_count * (uint64_t)shape_count, sizeof(*values))) {
            free(source_times);
            return -1;
        }
        times = (double *)malloc((size_t)source_count * sizeof(*times));
        values = (float *)calloc((size_t)source_count * (size_t)shape_count, sizeof(*values));
        if (!times || !values) {
            free(source_times);
            free(times);
            free(values);
            return -1;
        }
        for (int32_t key = 0; key < source_count; ++key) {
            times[key] = ((double)source_times[key] - (double)clip_start) / (double)FBX_TIME_SECOND;
            for (int32_t binding_index = 0; binding_index < binding_count; ++binding_index) {
                const fbx_morph_channel_binding_t *binding = &bindings[binding_index];
                double percent;
                if (binding->mesh_index != mesh_index)
                    continue;
                if (layer_stack) {
                    if (!fbx_anim_layer_sample_scalar(layer_stack,
                                                      binding->channel_id,
                                                      NULL,
                                                      binding->default_percent,
                                                      source_times[key],
                                                      &percent,
                                                      NULL)) {
                        free(source_times);
                        free(times);
                        free(values);
                        return -1;
                    }
                } else {
                    percent = fbx_anim_curve_value(
                        &curves[binding_index], source_times[key], binding->default_percent);
                }
                for (int32_t local_shape = 0; local_shape < binding->shape_count; ++local_shape) {
                    int32_t shape_index = binding->shape_indices[local_shape];
                    if (shape_index >= 0 && shape_index < shape_count) {
                        values[(size_t)key * (size_t)shape_count + (size_t)shape_index] =
                            (float)fbx_morph_progressive_shape_weight(
                                binding, percent, local_shape);
                    }
                }
            }
        }
        for (int32_t model_index = 0; model_index < model_count; ++model_index) {
            rt_string target_name;
            int64_t channel_index;
            if (!fbx_node_anim_model_uses_geometry(ct, models[model_index].id, geometry_id))
                continue;
            if (!fbx_budget_charge(context, 1u, sizeof(rt_node_anim_channel3d)) ||
                !fbx_budget_charge(context, (uint64_t)source_count, sizeof(*times)) ||
                !fbx_budget_charge(
                    context, (uint64_t)source_count * (uint64_t)shape_count, sizeof(*values))) {
                free(source_times);
                free(times);
                free(values);
                return -1;
            }
            target_name = fbx_object_name_string(models[model_index].source, 1);
            if (!target_name || rt_str_is_empty(target_name)) {
                char generated[64];
                rt_string_unref(target_name);
                snprintf(
                    generated, sizeof(generated), "Model_%d", models[model_index].import_index);
                target_name = rt_const_cstr(generated);
            }
            channel_index = rt_node_animation3d_add_channel(clip,
                                                            target_name,
                                                            RT_NODE_ANIM_PATH_WEIGHTS,
                                                            RT_NODE_ANIM_INTERP_LINEAR,
                                                            source_count,
                                                            shape_count,
                                                            times,
                                                            values);
            rt_string_unref(target_name);
            if (channel_index < 0) {
                free(source_times);
                free(times);
                free(values);
                return -1;
            }
            rt_node_animation3d_set_channel_target_node_index(
                clip, channel_index, models[model_index].import_index);
            emitted = 1;
        }
        free(source_times);
        free(times);
        free(values);
    }
    return emitted;
}

/// @brief Allocate an evaluable constraint pose matching the Model builder table.
/// @param models Complete Model builder table.
/// @param model_count Number of entries in @p models.
/// @param[in,out] context Loader context charged for pose arrays.
/// @param[out] pose Zeroed pose that receives IDs, parents, sources, and a sorted lookup table.
/// @return Non-zero on success, or zero for invalid input, budget rejection,
///         or allocation failure.
static int fbx_node_anim_initialize_pose(const fbx_node_anim_model_builder_t *models,
                                         int32_t model_count,
                                         fbx_load_context_t *context,
                                         fbx_constraint_pose_t *pose) {
    if (!models || model_count <= 0 || !context || !pose ||
        !fbx_constraint_pose_allocate(pose, model_count, context))
        return 0;
    for (int32_t i = 0; i < model_count; ++i) {
        pose->ids[i] = models[i].id;
        pose->parent_indices[i] = models[i].parent_index;
        pose->sources[i] = models[i].source;
        pose->id_table[i].id = models[i].id;
        pose->id_table[i].index = i;
    }
    qsort(pose->id_table, (size_t)model_count, sizeof(*pose->id_table), fbx_model_id_entry_cmp);
    return 1;
}

/// @brief Extract NodeAnimation3D clips from every animated FBX AnimationStack.
/// @details Builds a stable Model table per stack, parses layered animation, establishes
///          complete transform/morph/camera bounds, samples non-skeletal Models through
///          constraints, emits derived camera and morph channels, and publishes only clips
///          containing at least one channel. Any hard failure releases all clips already
///          published by this call.
/// @param root Parsed FBX document root.
/// @param ct Validated object connection table.
/// @param constraints Optional constraint set applied during transform sampling.
/// @param z_up Non-zero to convert evaluated transforms into runtime Y-up space.
/// @param asset Loaded asset supplying MorphTarget3D resources.
/// @param morph_bindings Imported progressive morph-channel bindings.
/// @param morph_binding_count Number of entries in @p morph_bindings.
/// @param[out] out_animations Receives the owned reference array of emitted clips.
/// @param[out] out_count Receives the number of emitted clips.
/// @param[out] out_capacity Receives the allocated reference-array capacity.
/// @param[in,out] context Loader context charged for all scratch and published storage.
/// @return Non-zero on success, including when no clips are emitted; zero on invalid
///         input, malformed animation data, budget exhaustion, or allocation failure.
static int fbx_extract_node_animations(fbx_node_t *root,
                                       const fbx_conn_table_t *ct,
                                       fbx_constraint_set_t *constraints,
                                       int z_up,
                                       const rt_fbx_asset *asset,
                                       const fbx_morph_channel_binding_t *morph_bindings,
                                       int32_t morph_binding_count,
                                       void ***out_animations,
                                       int32_t *out_count,
                                       int32_t *out_capacity,
                                       fbx_load_context_t *context) {
    fbx_node_t *objects;
    if (!root || !ct || !out_animations || !out_count || !out_capacity || !context)
        return 0;
    *out_animations = NULL;
    *out_count = 0;
    *out_capacity = 0;
    objects = fbx_find_child(root, "Objects");
    if (!objects)
        return 1;
    for (int32_t stack_index = 0; stack_index < objects->child_count; ++stack_index) {
        fbx_node_t *stack = &objects->children[stack_index];
        fbx_node_anim_model_builder_t *models = NULL;
        fbx_model_id_entry_t *ids = NULL;
        fbx_anim_curve_view_t *morph_curves = NULL;
        fbx_anim_layer_stack_t layer_stack;
        fbx_constraint_pose_t pose;
        int32_t model_count = 0;
        int64_t minimum;
        int64_t maximum;
        int emitted = 0;
        void *clip = NULL;
        rt_string name = NULL;
        memset(&pose, 0, sizeof(pose));
        memset(&layer_stack, 0, sizeof(layer_stack));
        if (strcmp(stack->name, "AnimationStack") != 0 || stack->prop_count < 1 ||
            !fbx_anim_stack_has_layer(objects, ct, fbx_prop_i64(stack, 0))) {
            continue;
        }
        if (!fbx_anim_layer_stack_parse(objects, ct, fbx_prop_i64(stack, 0), context, &layer_stack))
            goto fail;
        if (!fbx_node_anim_make_model_table(objects, ct, &models, &model_count, &ids, context)) {
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        if (model_count == 0) {
            free(models);
            free(ids);
            fbx_anim_layer_stack_free(&layer_stack);
            continue;
        }
        fbx_node_anim_collect_transform_curves(
            objects, ct, fbx_prop_i64(stack, 0), &layer_stack, models, model_count, ids);
        if (morph_binding_count > 0) {
            if ((size_t)morph_binding_count > SIZE_MAX / sizeof(*morph_curves) ||
                !fbx_budget_charge(context, (uint64_t)morph_binding_count, sizeof(*morph_curves))) {
                free(models);
                free(ids);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            morph_curves =
                (fbx_anim_curve_view_t *)calloc((size_t)morph_binding_count, sizeof(*morph_curves));
            if (!morph_curves) {
                free(models);
                free(ids);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            fbx_node_anim_collect_morph_curves(objects,
                                               ct,
                                               fbx_prop_i64(stack, 0),
                                               morph_bindings,
                                               morph_binding_count,
                                               morph_curves);
        }
        int have_bounds = fbx_node_anim_transform_bounds(models, model_count, &minimum, &maximum);
        have_bounds = fbx_node_anim_include_morph_bounds(
            morph_curves, morph_binding_count, have_bounds, &minimum, &maximum);
        if (layer_stack.layer_count > 0) {
            int64_t layer_minimum = 0;
            int64_t layer_maximum = 0;
            if (fbx_anim_layer_stack_bounds(&layer_stack, 0, &layer_minimum, &layer_maximum)) {
                if (!have_bounds || layer_minimum < minimum)
                    minimum = layer_minimum;
                if (!have_bounds || layer_maximum > maximum)
                    maximum = layer_maximum;
                have_bounds = 1;
            }
        }
        if (!have_bounds) {
            free(models);
            free(ids);
            free(morph_curves);
            fbx_anim_layer_stack_free(&layer_stack);
            continue;
        }
        name = fbx_object_name_string(stack, 1);
        if (!name || rt_str_is_empty(name)) {
            char generated[64];
            rt_string_unref(name);
            snprintf(generated, sizeof(generated), "node_animation_%d", stack_index);
            name = rt_const_cstr(generated);
        }
        if (!fbx_budget_charge(context, 1u, sizeof(rt_node_animation3d))) {
            rt_string_unref(name);
            free(models);
            free(ids);
            free(morph_curves);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        clip = rt_node_animation3d_new(
            name, ((double)maximum - (double)minimum) / (double)FBX_TIME_SECOND);
        rt_string_unref(name);
        name = NULL;
        if (!clip) {
            free(models);
            free(ids);
            free(morph_curves);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        if (!fbx_node_anim_initialize_pose(models, model_count, context, &pose)) {
            fbx_release_ref(&clip);
            free(models);
            free(ids);
            free(morph_curves);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        for (int32_t model_index = 0; model_index < model_count; ++model_index) {
            if (models[model_index].is_skeleton)
                continue;
            int status = fbx_node_anim_emit_model_channels(
                clip, models, model_count, model_index, minimum, z_up, &pose, constraints, context);
            if (status < 0) {
                fbx_release_ref(&clip);
                fbx_constraint_pose_free(&pose);
                free(models);
                free(ids);
                free(morph_curves);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            if (status > 0)
                emitted = 1;
            status = fbx_node_anim_emit_camera_channels(
                clip, objects, ct, &models[model_index], &layer_stack, minimum, context);
            if (status < 0) {
                fbx_release_ref(&clip);
                fbx_constraint_pose_free(&pose);
                free(models);
                free(ids);
                free(morph_curves);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            if (status > 0)
                emitted = 1;
        }
        {
            int morph_status = fbx_node_anim_emit_morph_channels(clip,
                                                                 objects,
                                                                 ct,
                                                                 models,
                                                                 model_count,
                                                                 asset,
                                                                 morph_bindings,
                                                                 morph_curves,
                                                                 &layer_stack,
                                                                 morph_binding_count,
                                                                 minimum,
                                                                 context);
            if (morph_status < 0) {
                fbx_release_ref(&clip);
                fbx_constraint_pose_free(&pose);
                free(models);
                free(ids);
                free(morph_curves);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            if (morph_status > 0)
                emitted = 1;
        }
        fbx_constraint_pose_free(&pose);
        free(models);
        free(ids);
        free(morph_curves);
        fbx_anim_layer_stack_free(&layer_stack);
        if (!emitted) {
            fbx_release_ref(&clip);
            continue;
        }
        if (*out_count == INT32_MAX || !fbx_budget_charge(context, 1u, sizeof(void *)) ||
            !fbx_asset_reserve_ref_array(out_animations, out_capacity, *out_count + 1)) {
            fbx_release_ref(&clip);
            goto fail;
        }
        (*out_animations)[(*out_count)++] = clip;
    }
    return 1;
fail:
    fbx_asset_release_ref_array(out_animations, out_count, out_capacity);
    return 0;
}
