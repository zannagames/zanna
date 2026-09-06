//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_scene.inc
// Purpose: Build FBX scene hierarchies, typed cameras/lights, and material-split meshes.
// Key invariants:
//   - Numeric FBX IDs resolve hierarchy/resources; display names never establish identity.
//   - Static nodes and animated samples share the complete FBX transform-stack composer.
// Ownership/Lifetime:
//   - Returned roots own retained node resources; FBX asset arrays retain standalone cameras.
//   - Model tables, parent indexes, and mesh remaps are scratch allocations freed before return.
// Links: rt_fbx_loader.c, rt_fbx_loader_loader.inc, rt_fbx_loader_nodeanim.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_scene.inc
 * @brief Builds the runtime scene hierarchy, FBX transform stacks, material-split
 *        meshes, lights, and cameras.
 *
 * Numeric FBX object connections are resolved into SceneNode3D ownership and parent
 * relationships. Static transforms use the same row-major pivot, rotation-order,
 * geometric-transform, inheritance, and coordinate-conversion machinery as animation
 * sampling. Camera and light NodeAttributes are normalized into runtime resources, and
 * multi-material geometry is cloned into compact per-slot meshes when required.
 */

/*==========================================================================
 * Scene graph extraction
 *=========================================================================*/

/// @brief Release a GC-managed object held in `*slot` and NULL-out the slot.
/// @param[in,out] slot Address of a runtime reference slot to release.
static void fbx_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Assign one FBX object's exact namespace-stripped name to a `SceneNode3D`.
/// @details The length-carrying property view preserves arbitrarily long distinct names and stops
///          at the first embedded binary suffix NUL. Empty names leave the scene node's existing
///          default unchanged.
/// @param scene_node Runtime scene node to rename.
/// @param object_node Typed FBX object whose property one contains its display name.
static void fbx_set_clean_object_name(void *scene_node, fbx_node_t *object_node) {
    const char *data = "";
    size_t length = 0;
    rt_string runtime_name;
    if (!scene_node || !fbx_object_name_view(object_node, 1, &data, &length) || length == 0)
        return;
    runtime_name = rt_string_from_bytes(data, length);
    if (!runtime_name)
        return;
    rt_scene_node3d_set_name(scene_node, runtime_name);
    rt_string_unref(runtime_name);
}

/// @brief Parsed FBX `Model` transform properties before matrix composition.
/// @details FBX stores a model transform as a layered stack: local TRS, pre/post rotations,
///          rotation/scaling pivots, offsets, and a geometric transform. Keeping these fields
///          explicit lets the scene-node and skeleton importers share the same interpretation
///          instead of each rebuilding a partial Euler-only path.
typedef struct {
    /// Local translation.
    double translation[3];
    /// Local Euler rotation in degrees.
    double rotation[3];
    /// Local non-uniform scale.
    double scale[3];
    /// FBX pre-rotation Euler angles.
    double pre_rotation[3];
    /// FBX post-rotation Euler angles.
    double post_rotation[3];
    /// Translation applied before the rotation pivot.
    double rotation_offset[3];
    /// Rotation pivot position.
    double rotation_pivot[3];
    /// Translation applied before the scaling pivot.
    double scaling_offset[3];
    /// Scaling pivot position.
    double scaling_pivot[3];
    /// Geometry-only translation appended after the Model transform.
    double geometric_translation[3];
    /// Geometry-only Euler rotation.
    double geometric_rotation[3];
    /// Geometry-only scale.
    double geometric_scale[3];
    /// Optional already-sampled quaternion used instead of Euler rotation.
    double sampled_rotation_quaternion[4];
    /// FBX Euler RotationOrder enum.
    int rotation_order;
    /// Non-zero when authored local rotation is active.
    int rotation_active;
    /// FBX TransformInheritType enum.
    int inherit_type;
    /// Non-zero to compose @ref sampled_rotation_quaternion.
    int use_sampled_rotation_quaternion;
} fbx_transform_components_t;

static void fbx_mat4_quaternion_rotation(const double quat[4], double out[16]);

/// @brief Initialize an FBX transform component record to identity/default values.
/// @param out Destination component record; ignored when NULL.
static void fbx_transform_components_init(fbx_transform_components_t *out) {
    static const fbx_transform_components_t zero_components = {0};
    if (!out)
        return;
    *out = zero_components;
    out->scale[0] = 1.0;
    out->scale[1] = 1.0;
    out->scale[2] = 1.0;
    out->geometric_scale[0] = 1.0;
    out->geometric_scale[1] = 1.0;
    out->geometric_scale[2] = 1.0;
    out->rotation_order = 0;
    out->rotation_active = 1;
    out->inherit_type = 0;
    out->sampled_rotation_quaternion[3] = 1.0;
}

/// @brief Read a vector-style `Properties70` P-node into a fixed XYZ array.
/// @details FBX vector properties store their numeric payload at property slots 4, 5, and 6.
///          Non-finite lanes are ignored by returning 0 so callers can keep prior defaults.
/// @param p The P-node to read.
/// @param out_xyz Receives X, Y, Z on success.
/// @return Non-zero when all three components were finite and copied.
static int fbx_read_p_xyz(fbx_node_t *p, double out_xyz[3]) {
    double x;
    double y;
    double z;
    if (!p || !out_xyz || p->prop_count < 7)
        return 0;
    x = fbx_prop_f64(p, 4);
    y = fbx_prop_f64(p, 5);
    z = fbx_prop_f64(p, 6);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return 0;
    out_xyz[0] = x;
    out_xyz[1] = y;
    out_xyz[2] = z;
    return 1;
}

/// @brief Find a named `Properties70/P` record.
/// @param node FBX object containing an optional `Properties70` child.
/// @param name Case-insensitive authored property name.
/// @return Borrowed pointer to the matching `P` node, or NULL when absent.
static fbx_node_t *fbx_find_property70(fbx_node_t *node, const char *name) {
    fbx_node_t *properties;
    if (!node || !name)
        return NULL;
    properties = fbx_find_child(node, "Properties70");
    if (!properties)
        return NULL;
    for (int32_t i = 0; i < properties->child_count; ++i) {
        fbx_node_t *property = &properties->children[i];
        if (strcmp(property->name, "P") == 0 && property->prop_count > 0 &&
            fbx_ascii_equals_i(fbx_prop_str(property, 0), name)) {
            return property;
        }
    }
    return NULL;
}

/// @brief Read one scalar from `Properties70` or an exporter-style direct child.
/// @param node FBX object carrying the property.
/// @param name Property/child name.
/// @param fallback Returned when the value is absent or non-finite.
/// @param out_found Optional presence flag, set only for a finite numeric value.
/// @return The finite imported scalar, or @p fallback when unavailable.
static double fbx_read_object_number(fbx_node_t *node,
                                     const char *name,
                                     double fallback,
                                     int *out_found) {
    fbx_node_t *property;
    double value;
    if (out_found)
        *out_found = 0;
    property = fbx_find_property70(node, name);
    if (property && property->prop_count >= 5) {
        value = fbx_prop_f64(property, 4);
        if (isfinite(value)) {
            if (out_found)
                *out_found = 1;
            return value;
        }
    }
    property = node ? fbx_find_child(node, name) : NULL;
    if (property && property->prop_count > 0) {
        value = fbx_prop_f64(property, 0);
        if (isfinite(value)) {
            if (out_found)
                *out_found = 1;
            return value;
        }
    }
    return fallback;
}

/// @brief Read a finite XYZ property from `Properties70` or a three-scalar direct child.
/// @param node FBX object carrying the property.
/// @param name Property/child name.
/// @param[out] out_xyz Receives the finite XYZ vector.
/// @return Non-zero when a complete finite vector is found; otherwise zero.
static int fbx_read_object_xyz(fbx_node_t *node, const char *name, double out_xyz[3]) {
    fbx_node_t *property;
    if (!node || !name || !out_xyz)
        return 0;
    property = fbx_find_property70(node, name);
    if (property && fbx_read_p_xyz(property, out_xyz))
        return 1;
    property = fbx_find_child(node, name);
    if (!property || property->prop_count < 3)
        return 0;
    for (int i = 0; i < 3; ++i) {
        out_xyz[i] = fbx_prop_f64(property, i);
        if (!isfinite(out_xyz[i]))
            return 0;
    }
    return 1;
}

/// @brief Test whether an FBX Model subtype belongs to the skeletal hierarchy.
/// @param type Model subtype string.
/// @return Non-zero for LimbNode, Limb, or Root; otherwise zero.
static int fbx_model_type_is_skeleton(const char *type) {
    return type && (strcmp(type, "LimbNode") == 0 || strcmp(type, "Limb") == 0 ||
                    strcmp(type, "Root") == 0);
}

/// @brief Parse every FBX transform-stack property from a `Model` node.
/// @details Reads the common Autodesk stack fields: local TRS, PreRotation, PostRotation,
///          RotationOffset/Pivot, ScalingOffset/Pivot, RotationOrder, RotationActive, and
///          Geometric* values. Unknown properties are ignored, preserving forward compatibility.
/// @param model_node FBX `Model` node.
/// @param out Receives a complete default-filled transform component record.
static void fbx_read_model_transform_components(fbx_node_t *model_node,
                                                fbx_transform_components_t *out) {
    fbx_node_t *p70;
    fbx_transform_components_init(out);
    if (!model_node || !out)
        return;
    p70 = fbx_find_child(model_node, "Properties70");
    if (!p70)
        return;
    for (int32_t pi = 0; pi < p70->child_count; pi++) {
        fbx_node_t *p = &p70->children[pi];
        const char *pn;
        if (strcmp(p->name, "P") != 0 || p->prop_count < 1)
            continue;
        pn = fbx_prop_str(p, 0);
        if (fbx_ascii_equals_i(pn, "Lcl Translation")) {
            (void)fbx_read_p_xyz(p, out->translation);
        } else if (fbx_ascii_equals_i(pn, "Lcl Rotation")) {
            (void)fbx_read_p_xyz(p, out->rotation);
        } else if (fbx_ascii_equals_i(pn, "Lcl Scaling")) {
            (void)fbx_read_p_xyz(p, out->scale);
        } else if (fbx_ascii_equals_i(pn, "PreRotation")) {
            (void)fbx_read_p_xyz(p, out->pre_rotation);
        } else if (fbx_ascii_equals_i(pn, "PostRotation")) {
            (void)fbx_read_p_xyz(p, out->post_rotation);
        } else if (fbx_ascii_equals_i(pn, "RotationOffset")) {
            (void)fbx_read_p_xyz(p, out->rotation_offset);
        } else if (fbx_ascii_equals_i(pn, "RotationPivot")) {
            (void)fbx_read_p_xyz(p, out->rotation_pivot);
        } else if (fbx_ascii_equals_i(pn, "ScalingOffset")) {
            (void)fbx_read_p_xyz(p, out->scaling_offset);
        } else if (fbx_ascii_equals_i(pn, "ScalingPivot")) {
            (void)fbx_read_p_xyz(p, out->scaling_pivot);
        } else if (fbx_ascii_equals_i(pn, "GeometricTranslation")) {
            (void)fbx_read_p_xyz(p, out->geometric_translation);
        } else if (fbx_ascii_equals_i(pn, "GeometricRotation")) {
            (void)fbx_read_p_xyz(p, out->geometric_rotation);
        } else if (fbx_ascii_equals_i(pn, "GeometricScaling")) {
            (void)fbx_read_p_xyz(p, out->geometric_scale);
        } else if (fbx_ascii_equals_i(pn, "RotationOrder") && p->prop_count >= 5) {
            int64_t order = fbx_prop_i64(p, 4);
            if (order >= 0 && order <= 6)
                out->rotation_order = (int)order;
        } else if (fbx_ascii_equals_i(pn, "RotationActive") && p->prop_count >= 5) {
            out->rotation_active = fbx_prop_truthy(p, 4);
        } else if (fbx_ascii_equals_i(pn, "InheritType") && p->prop_count >= 5) {
            int64_t inherit_type = fbx_prop_i64(p, 4);
            if (inherit_type >= 0 && inherit_type <= 2)
                out->inherit_type = (int)inherit_type;
        }
    }
    for (int i = 0; i < 3; i++) {
        out->translation[i] = fbx_clamp_abs_or(out->translation[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->rotation[i] = fbx_sanitize_rotation_degrees(out->rotation[i]);
        out->scale[i] = fbx_scale_or_unit(out->scale[i]);
        out->pre_rotation[i] = fbx_sanitize_rotation_degrees(out->pre_rotation[i]);
        out->post_rotation[i] = fbx_sanitize_rotation_degrees(out->post_rotation[i]);
        out->rotation_offset[i] =
            fbx_clamp_abs_or(out->rotation_offset[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->rotation_pivot[i] = fbx_clamp_abs_or(out->rotation_pivot[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->scaling_offset[i] = fbx_clamp_abs_or(out->scaling_offset[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->scaling_pivot[i] = fbx_clamp_abs_or(out->scaling_pivot[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->geometric_translation[i] =
            fbx_clamp_abs_or(out->geometric_translation[i], 0.0, FBX_NUMERIC_ABS_MAX);
        out->geometric_rotation[i] = fbx_sanitize_rotation_degrees(out->geometric_rotation[i]);
        out->geometric_scale[i] = fbx_scale_or_unit(out->geometric_scale[i]);
    }
}

/// @brief Write a row-major 4x4 identity matrix into @p out.
/// @param out Sixteen-element row-major matrix buffer.
static void fbx_mat4_identity_local(double out[16]) {
    if (!out)
        return;
    memset(out, 0, 16u * sizeof(double));
    out[0] = 1.0;
    out[5] = 1.0;
    out[10] = 1.0;
    out[15] = 1.0;
}

/// @brief Multiply row-major 4x4 matrices (`out = a * b`), safe for aliased output.
/// @param a Left-hand matrix.
/// @param b Right-hand matrix.
/// @param out Receives the product.
static void fbx_mat4_mul_local(const double a[16], const double b[16], double out[16]) {
    double r[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            r[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
                               a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
        }
    }
    memcpy(out, r, sizeof(r));
}

/// @brief Post-multiply @p acc by @p rhs (`acc = acc * rhs`).
/// @param acc Accumulated transform matrix, modified in place.
/// @param rhs Transform matrix appended to the stack.
static void fbx_mat4_append_local(double acc[16], const double rhs[16]) {
    fbx_mat4_mul_local(acc, rhs, acc);
}

/// @brief Post-multiply @p acc by a translation (`acc = acc * T`) without a general multiply.
/// @details Only column three of a row-major affine matrix depends on a translation, and it is
///          computed with exactly the same expression and operand order the general product uses
///          (`T[3][3]` is 1, so that term is an identity multiply). Columns zero through two are
///          multiplied by an identity block and are copied through untouched. This replaces 64
///          multiplies and 48 adds with three multiplies and three adds per row; the transform
///          stack applies six translations per bone per sample, so it dominates import cost.
/// @param acc Accumulated transform matrix, modified in place.
/// @param x Translation along X, pre-clamped by the caller.
/// @param y Translation along Y, pre-clamped by the caller.
/// @param z Translation along Z, pre-clamped by the caller.
static void fbx_mat4_append_translation_local(double acc[16], double x, double y, double z) {
    if (!acc)
        return;
    for (int row = 0; row < 4; row++) {
        double *r = &acc[row * 4];
        r[3] = r[0] * x + r[1] * y + r[2] * z + r[3];
    }
}

/// @brief Post-multiply @p acc by a scale (`acc = acc * S`) without a general multiply.
/// @details A row-major affine scale touches only columns zero through two, each by its own
///          factor; column three is multiplied by `S[3][3]` (1) and copied through. Same
///          per-element arithmetic as the general product, without the zero terms.
/// @param acc Accumulated transform matrix, modified in place.
/// @param x Scale along X, pre-sanitized by the caller.
/// @param y Scale along Y, pre-sanitized by the caller.
/// @param z Scale along Z, pre-sanitized by the caller.
static void fbx_mat4_append_scale_local(double acc[16], double x, double y, double z) {
    if (!acc)
        return;
    for (int row = 0; row < 4; row++) {
        double *r = &acc[row * 4];
        r[0] *= x;
        r[1] *= y;
        r[2] *= z;
    }
}

/// @brief Build a row-major translation matrix.
/// @param x Translation along X.
/// @param y Translation along Y.
/// @param z Translation along Z.
/// @param out Receives the matrix.
static void fbx_mat4_translate_local(double x, double y, double z, double out[16]) {
    fbx_mat4_identity_local(out);
    out[3] = fbx_clamp_abs_or(x, 0.0, FBX_NUMERIC_ABS_MAX);
    out[7] = fbx_clamp_abs_or(y, 0.0, FBX_NUMERIC_ABS_MAX);
    out[11] = fbx_clamp_abs_or(z, 0.0, FBX_NUMERIC_ABS_MAX);
}

/// @brief Build a row-major non-uniform scale matrix.
/// @param x Scale along X.
/// @param y Scale along Y.
/// @param z Scale along Z.
/// @param out Receives the matrix.
static void fbx_mat4_scale_local(double x, double y, double z, double out[16]) {
    fbx_mat4_identity_local(out);
    out[0] = fbx_scale_or_unit(x);
    out[5] = fbx_scale_or_unit(y);
    out[10] = fbx_scale_or_unit(z);
}

/// @brief Build a row-major single-axis rotation matrix from degrees.
/// @param axis Axis selector: 0 = X, 1 = Y, 2 = Z.
/// @param degrees Rotation angle in degrees.
/// @param out Receives the matrix.
static void fbx_mat4_rotate_axis_degrees(int axis, double degrees, double out[16]) {
    double radians = fbx_sanitize_rotation_degrees(degrees) * 3.14159265358979323846 / 180.0;
    double c = cos(radians);
    double s = sin(radians);
    fbx_mat4_identity_local(out);
    if (!isfinite(c) || !isfinite(s))
        return;
    if (axis == 0) {
        out[5] = c;
        out[6] = -s;
        out[9] = s;
        out[10] = c;
    } else if (axis == 1) {
        out[0] = c;
        out[2] = s;
        out[8] = -s;
        out[10] = c;
    } else {
        out[0] = c;
        out[1] = -s;
        out[4] = s;
        out[5] = c;
    }
}

/// @brief Append one axis rotation to an Euler composition matrix.
/// @param acc Accumulated Euler matrix.
/// @param axis Axis selector: 0 = X, 1 = Y, 2 = Z.
/// @param degrees Axis angle in degrees.
static void fbx_mat4_append_axis_rotation(double acc[16], int axis, double degrees) {
    double r[16];
    fbx_mat4_rotate_axis_degrees(axis, degrees, r);
    fbx_mat4_append_local(acc, r);
}

/// @brief Build an FBX Euler rotation matrix for the requested RotationOrder.
/// @details FBX's order value names the local axis sequence. With Zanna's row-major matrices and
///          column vectors, preserving the legacy XYZ result means composing in reverse axis order
///          (`XYZ` becomes `Rz * Ry * Rx`). The same rule is applied to the other FBX orders:
///          0 XYZ, 1 XZY, 2 YZX, 3 YXZ, 4 ZXY, 5 ZYX, 6 SphericXYZ (treated as XYZ).
/// @param xyz_degrees Euler components in degrees.
/// @param rotation_order FBX RotationOrder integer.
/// @param out Receives the rotation matrix.
static void fbx_mat4_euler_degrees(const double xyz_degrees[3],
                                   int rotation_order,
                                   double out[16]) {
    int order = (rotation_order >= 0 && rotation_order <= 6) ? rotation_order : 0;
    fbx_mat4_identity_local(out);
    switch (order) {
        case 1: /* XZY */
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            break;
        case 2: /* YZX */
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            break;
        case 3: /* YXZ */
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            break;
        case 4: /* ZXY */
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            break;
        case 5: /* ZYX */
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            break;
        case 0: /* XYZ */
        case 6: /* SphericXYZ fallback */
        default:
            fbx_mat4_append_axis_rotation(out, 2, xyz_degrees[2]);
            fbx_mat4_append_axis_rotation(out, 1, xyz_degrees[1]);
            fbx_mat4_append_axis_rotation(out, 0, xyz_degrees[0]);
            break;
    }
}

/// @brief Apply FBX Z-up to Zanna Y-up conversion to a full affine matrix.
/// @details Vector correction alone is not enough once pivots and bind matrices are involved.
///          This performs the basis change `C * M * C^-1`, where C maps `(x,y,z)` to
///          `(x,z,-y)`.
/// @param matrix Row-major matrix updated in place.
static void fbx_mat4_apply_zup(double matrix[16]) {
    static const double c[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        -1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    static const double c_inv[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        -1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    double tmp[16];
    if (!matrix)
        return;
    fbx_mat4_mul_local(c, matrix, tmp);
    fbx_mat4_mul_local(tmp, c_inv, matrix);
}

/// @brief Compose the full FBX model transform stack into one affine matrix.
/// @details The stack follows the FBX SDK transform order for local transforms, including
///          rotation/scaling pivots and offsets. Geometric translation is folded into the initial
///          node translation to preserve Zanna's existing scene-node behavior; geometric rotation
///          and scale remain appended because there is no separate per-geometry transform slot.
/// @param c Parsed transform components.
/// @param out Receives a row-major affine matrix.
static void fbx_build_model_transform_matrix(const fbx_transform_components_t *c, double out[16]) {
    double step[16];
    double neg[3];
    double post_inverse[3];
    if (!c || !out) {
        if (out)
            fbx_mat4_identity_local(out);
        return;
    }
    fbx_mat4_identity_local(out);

    fbx_mat4_append_translation_local(
        out,
        fbx_clamp_abs_or(c->translation[0] + c->geometric_translation[0], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->translation[1] + c->geometric_translation[1], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(
            c->translation[2] + c->geometric_translation[2], 0.0, FBX_NUMERIC_ABS_MAX));

    fbx_mat4_append_translation_local(
        out,
        fbx_clamp_abs_or(c->rotation_offset[0], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->rotation_offset[1], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->rotation_offset[2], 0.0, FBX_NUMERIC_ABS_MAX));
    fbx_mat4_append_translation_local(
        out,
        fbx_clamp_abs_or(c->rotation_pivot[0], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->rotation_pivot[1], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->rotation_pivot[2], 0.0, FBX_NUMERIC_ABS_MAX));
    if (c->rotation_active) {
        fbx_mat4_euler_degrees(c->pre_rotation, c->rotation_order, step);
        fbx_mat4_append_local(out, step);
        if (c->use_sampled_rotation_quaternion)
            fbx_mat4_quaternion_rotation(c->sampled_rotation_quaternion, step);
        else
            fbx_mat4_euler_degrees(c->rotation, c->rotation_order, step);
        fbx_mat4_append_local(out, step);
        post_inverse[0] = -c->post_rotation[0];
        post_inverse[1] = -c->post_rotation[1];
        post_inverse[2] = -c->post_rotation[2];
        fbx_mat4_euler_degrees(post_inverse, c->rotation_order, step);
        fbx_mat4_append_local(out, step);
    }
    neg[0] = -c->rotation_pivot[0];
    neg[1] = -c->rotation_pivot[1];
    neg[2] = -c->rotation_pivot[2];
    fbx_mat4_append_translation_local(out,
                                      fbx_clamp_abs_or(neg[0], 0.0, FBX_NUMERIC_ABS_MAX),
                                      fbx_clamp_abs_or(neg[1], 0.0, FBX_NUMERIC_ABS_MAX),
                                      fbx_clamp_abs_or(neg[2], 0.0, FBX_NUMERIC_ABS_MAX));

    fbx_mat4_append_translation_local(
        out,
        fbx_clamp_abs_or(c->scaling_offset[0], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->scaling_offset[1], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->scaling_offset[2], 0.0, FBX_NUMERIC_ABS_MAX));
    fbx_mat4_append_translation_local(
        out,
        fbx_clamp_abs_or(c->scaling_pivot[0], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->scaling_pivot[1], 0.0, FBX_NUMERIC_ABS_MAX),
        fbx_clamp_abs_or(c->scaling_pivot[2], 0.0, FBX_NUMERIC_ABS_MAX));
    fbx_mat4_append_scale_local(out,
                                fbx_scale_or_unit(c->scale[0]),
                                fbx_scale_or_unit(c->scale[1]),
                                fbx_scale_or_unit(c->scale[2]));
    neg[0] = -c->scaling_pivot[0];
    neg[1] = -c->scaling_pivot[1];
    neg[2] = -c->scaling_pivot[2];
    fbx_mat4_append_translation_local(out,
                                      fbx_clamp_abs_or(neg[0], 0.0, FBX_NUMERIC_ABS_MAX),
                                      fbx_clamp_abs_or(neg[1], 0.0, FBX_NUMERIC_ABS_MAX),
                                      fbx_clamp_abs_or(neg[2], 0.0, FBX_NUMERIC_ABS_MAX));

    fbx_mat4_euler_degrees(c->geometric_rotation, c->rotation_order, step);
    fbx_mat4_append_local(out, step);
    fbx_mat4_append_scale_local(out,
                                fbx_scale_or_unit(c->geometric_scale[0]),
                                fbx_scale_or_unit(c->geometric_scale[1]),
                                fbx_scale_or_unit(c->geometric_scale[2]));
}

/// @brief Invert an affine row-major 4x4 matrix.
/// @details Only the upper-left 3x3 and translation column are used; the bottom row is assumed
///          to be `[0 0 0 1]`, matching FBX transform/bind matrices. Singular matrices return 0.
/// @param m Matrix to invert.
/// @param out Receives the inverse when the function returns non-zero.
/// @return Non-zero on success, zero for singular or malformed matrices.
static int fbx_mat4_inverse_affine(const double m[16], double out[16]) {
    double a00;
    double a01;
    double a02;
    double a10;
    double a11;
    double a12;
    double a20;
    double a21;
    double a22;
    double det;
    double inv_det;
    if (!m || !out)
        return 0;
    a00 = m[0];
    a01 = m[1];
    a02 = m[2];
    a10 = m[4];
    a11 = m[5];
    a12 = m[6];
    a20 = m[8];
    a21 = m[9];
    a22 = m[10];
    det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
          a02 * (a10 * a21 - a11 * a20);
    if (!isfinite(det) || fabs(det) < 1e-18)
        return 0;
    inv_det = 1.0 / det;
    out[0] = (a11 * a22 - a12 * a21) * inv_det;
    out[1] = (a02 * a21 - a01 * a22) * inv_det;
    out[2] = (a01 * a12 - a02 * a11) * inv_det;
    out[4] = (a12 * a20 - a10 * a22) * inv_det;
    out[5] = (a00 * a22 - a02 * a20) * inv_det;
    out[6] = (a02 * a10 - a00 * a12) * inv_det;
    out[8] = (a10 * a21 - a11 * a20) * inv_det;
    out[9] = (a01 * a20 - a00 * a21) * inv_det;
    out[10] = (a00 * a11 - a01 * a10) * inv_det;
    out[3] = -(out[0] * m[3] + out[1] * m[7] + out[2] * m[11]);
    out[7] = -(out[4] * m[3] + out[5] * m[7] + out[6] * m[11]);
    out[11] = -(out[8] * m[3] + out[9] * m[7] + out[10] * m[11]);
    out[12] = 0.0;
    out[13] = 0.0;
    out[14] = 0.0;
    out[15] = 1.0;
    return 1;
}

/// @brief Convert a row-major matrix into a runtime Mat4 object.
/// @param m Sixteen-element row-major matrix.
/// @return New Mat4 object, or NULL if allocation fails.
static void *fbx_mat4_to_rt(const double m[16]) {
    if (!m)
        return NULL;
    return rt_mat4_new(m[0],
                       m[1],
                       m[2],
                       m[3],
                       m[4],
                       m[5],
                       m[6],
                       m[7],
                       m[8],
                       m[9],
                       m[10],
                       m[11],
                       m[12],
                       m[13],
                       m[14],
                       m[15]);
}

/// @brief Read a 16-element matrix child as either double-array, float-array, or scalar props.
/// @details Cluster `Transform` / `TransformLink` nodes are usually array properties, but older
///          ASCII-to-binary conversion tools can emit sixteen scalar properties. Both layouts are
///          accepted so valid bind poses are not discarded due to representation.
/// @param node Parent node containing the matrix child.
/// @param child_name Child node name to read.
/// @param out Receives a row-major matrix on success.
/// @return Non-zero when a finite 16-value matrix was read.
static int fbx_read_child_matrix16(fbx_node_t *node, const char *child_name, double out[16]) {
    fbx_node_t *child;
    if (!node || !child_name || !out)
        return 0;
    child = fbx_find_child(node, child_name);
    if (!child || child->prop_count < 1)
        return 0;
    if ((child->props[0].type == 'd' || child->props[0].type == 'f') &&
        child->props[0].v.array.data && child->props[0].v.array.count >= 16u) {
        if (child->props[0].type == 'd') {
            const double *src = (const double *)child->props[0].v.array.data;
            for (int i = 0; i < 16; i++)
                out[i] = src[i];
        } else {
            const float *src = (const float *)child->props[0].v.array.data;
            for (int i = 0; i < 16; i++)
                out[i] = (double)src[i];
        }
    } else if (child->prop_count >= 16) {
        for (int i = 0; i < 16; i++) {
            fbx_prop_t *p = &child->props[i];
            if (p->type == 'D' || p->type == 'F')
                out[i] = fbx_prop_f64(child, i);
            else if (p->type == 'L' || p->type == 'I' || p->type == 'Y' || p->type == 'C')
                out[i] = (double)fbx_prop_i64(child, i);
            else
                return 0;
        }
    } else {
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        if (!isfinite(out[i]))
            return 0;
        out[i] = fbx_clamp_abs_or(out[i], 0.0, FBX_NUMERIC_ABS_MAX);
    }
    return 1;
}

/// @brief Decompose a row-major affine matrix into position, quaternion, and scale.
/// @details SceneNode3D stores TRS rather than a raw matrix. This extracts translation from the
///          matrix column, scale from basis-vector lengths, and a normalized quaternion from the
///          remaining orthonormalized rotation matrix. Shear is intentionally discarded.
/// @param m Source row-major affine matrix.
/// @param pos Receives XYZ translation when non-NULL.
/// @param quat Receives XYZW quaternion when non-NULL.
/// @param scale Receives XYZ scale when non-NULL.
static void fbx_decompose_matrix_trs(const double m[16], double *pos, double *quat, double *scale) {
    double sx;
    double sy;
    double sz;
    double r00;
    double r01;
    double r02;
    double r10;
    double r11;
    double r12;
    double r20;
    double r21;
    double r22;
    double det;
    double trace;
    double qx = 0.0;
    double qy = 0.0;
    double qz = 0.0;
    double qw = 1.0;
    if (pos) {
        pos[0] = m ? fbx_clamp_abs_or(m[3], 0.0, FBX_NUMERIC_ABS_MAX) : 0.0;
        pos[1] = m ? fbx_clamp_abs_or(m[7], 0.0, FBX_NUMERIC_ABS_MAX) : 0.0;
        pos[2] = m ? fbx_clamp_abs_or(m[11], 0.0, FBX_NUMERIC_ABS_MAX) : 0.0;
    }
    if (!m) {
        if (quat) {
            quat[0] = quat[1] = quat[2] = 0.0;
            quat[3] = 1.0;
        }
        if (scale) {
            scale[0] = scale[1] = scale[2] = 1.0;
        }
        return;
    }
    sx = sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    sy = sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    sz = sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    sx = fbx_scale_or_unit(sx);
    sy = fbx_scale_or_unit(sy);
    sz = fbx_scale_or_unit(sz);
    det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
          m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (isfinite(det) && det < 0.0)
        sx = -sx;
    r00 = m[0] / sx;
    r01 = m[1] / sy;
    r02 = m[2] / sz;
    r10 = m[4] / sx;
    r11 = m[5] / sy;
    r12 = m[6] / sz;
    r20 = m[8] / sx;
    r21 = m[9] / sy;
    r22 = m[10] / sz;
    trace = r00 + r11 + r22;
    if (trace > 0.0) {
        double s = sqrt(trace + 1.0) * 2.0;
        if (s > 1e-12) {
            qw = 0.25 * s;
            qx = (r21 - r12) / s;
            qy = (r02 - r20) / s;
            qz = (r10 - r01) / s;
        }
    } else if (r00 > r11 && r00 > r22) {
        double s = sqrt(1.0 + r00 - r11 - r22) * 2.0;
        if (s > 1e-12) {
            qw = (r21 - r12) / s;
            qx = 0.25 * s;
            qy = (r01 + r10) / s;
            qz = (r02 + r20) / s;
        }
    } else if (r11 > r22) {
        double s = sqrt(1.0 + r11 - r00 - r22) * 2.0;
        if (s > 1e-12) {
            qw = (r02 - r20) / s;
            qx = (r01 + r10) / s;
            qy = 0.25 * s;
            qz = (r12 + r21) / s;
        }
    } else {
        double s = sqrt(1.0 + r22 - r00 - r11) * 2.0;
        if (s > 1e-12) {
            qw = (r10 - r01) / s;
            qx = (r02 + r20) / s;
            qy = (r12 + r21) / s;
            qz = 0.25 * s;
        }
    }
    {
        double qlen = sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (isfinite(qlen) && qlen > 1e-12) {
            qx /= qlen;
            qy /= qlen;
            qz /= qlen;
            qw /= qlen;
        } else {
            qx = qy = qz = 0.0;
            qw = 1.0;
        }
    }
    if (quat) {
        quat[0] = qx;
        quat[1] = qy;
        quat[2] = qz;
        quat[3] = qw;
    }
    if (scale) {
        scale[0] = fbx_scale_or_unit(sx);
        scale[1] = fbx_scale_or_unit(sy);
        scale[2] = fbx_scale_or_unit(sz);
    }
}

/// @brief Build a row-major pure-rotation matrix from a normalized XYZW quaternion.
/// @details Non-finite or degenerate input falls back to identity. The helper matches the matrix
///          convention used by @ref fbx_decompose_matrix_trs and is used to separate a parent's
///          global rotation from its inherited shear/scale component.
/// @param quat Four-element XYZW quaternion.
/// @param out Receives a row-major affine rotation matrix.
static void fbx_mat4_quaternion_rotation(const double quat[4], double out[16]) {
    double x;
    double y;
    double z;
    double w;
    double length;
    fbx_mat4_identity_local(out);
    if (!quat)
        return;
    x = quat[0];
    y = quat[1];
    z = quat[2];
    w = quat[3];
    length = sqrt(x * x + y * y + z * z + w * w);
    if (!isfinite(length) || length <= 1e-12)
        return;
    x /= length;
    y /= length;
    z /= length;
    w /= length;
    out[0] = 1.0 - 2.0 * (y * y + z * z);
    out[1] = 2.0 * (x * y - z * w);
    out[2] = 2.0 * (x * z + y * w);
    out[4] = 2.0 * (x * y + z * w);
    out[5] = 1.0 - 2.0 * (x * x + z * z);
    out[6] = 2.0 * (y * z - x * w);
    out[8] = 2.0 * (x * z - y * w);
    out[9] = 2.0 * (y * z + x * w);
    out[10] = 1.0 - 2.0 * (x * x + y * y);
}

/// @brief Compose one child's FBX global matrix according to its scaling-inheritance mode.
/// @details This follows Autodesk's published evaluator decomposition. Translation is transformed
///          by the complete parent global matrix. The linear part is then composed as:
///          `RrSs = parentR * localR * parentGlobalScaleShear * localS`,
///          `RSrs = parentR * parentGlobalScaleShear * localR * localS`, or
///          `Rrs = parentR * localR * parentScaleWithoutLocal * localS`.
///          Pivots, pre/post rotation, rotation order, and geometric transforms have already been
///          folded into @p child_local by the shared local composer. Passing no parent copies the
///          local matrix unchanged.
/// @param parent_global Parent's already evaluated global affine matrix, or NULL for a root.
/// @param parent_components Parent's authored transform components, required for Rrs compensation.
/// @param child_local Child matrix produced by @ref fbx_build_model_transform_matrix.
/// @param child_components Child components carrying the InheritType selector.
/// @param out Receives the evaluated child global affine matrix.
static void fbx_build_inherited_global_matrix(const double parent_global[16],
                                              const fbx_transform_components_t *parent_components,
                                              const double child_local[16],
                                              const fbx_transform_components_t *child_components,
                                              double out[16]) {
    double standard[16];
    double parent_position[3];
    double parent_quaternion[4];
    double child_quaternion[4];
    double child_scale[3];
    double parent_translation[16];
    double inverse_parent_translation[16];
    double parent_rotation[16];
    double inverse_parent_rotation[16];
    double parent_rs[16];
    double parent_scale_shear[16];
    double child_rotation[16];
    double child_scaling[16];
    double global_rs[16];
    double step[16];
    int inherit_type;
    if (!out)
        return;
    if (!child_local) {
        fbx_mat4_identity_local(out);
        return;
    }
    if (!parent_global || !child_components) {
        memcpy(out, child_local, 16u * sizeof(double));
        return;
    }
    fbx_mat4_mul_local(parent_global, child_local, standard);
    fbx_decompose_matrix_trs(parent_global, parent_position, parent_quaternion, NULL);
    fbx_decompose_matrix_trs(child_local, NULL, child_quaternion, child_scale);
    fbx_mat4_translate_local(
        parent_position[0], parent_position[1], parent_position[2], parent_translation);
    if (!fbx_mat4_inverse_affine(parent_translation, inverse_parent_translation)) {
        memcpy(out, standard, sizeof(standard));
        return;
    }
    fbx_mat4_quaternion_rotation(parent_quaternion, parent_rotation);
    if (!fbx_mat4_inverse_affine(parent_rotation, inverse_parent_rotation)) {
        memcpy(out, standard, sizeof(standard));
        return;
    }
    fbx_mat4_mul_local(inverse_parent_translation, parent_global, parent_rs);
    fbx_mat4_mul_local(inverse_parent_rotation, parent_rs, parent_scale_shear);
    fbx_mat4_quaternion_rotation(child_quaternion, child_rotation);
    fbx_mat4_scale_local(child_scale[0], child_scale[1], child_scale[2], child_scaling);
    inherit_type = child_components->inherit_type;
    if (inherit_type == 1) {
        fbx_mat4_mul_local(parent_rotation, parent_scale_shear, global_rs);
        fbx_mat4_append_local(global_rs, child_rotation);
        fbx_mat4_append_local(global_rs, child_scaling);
    } else if (inherit_type == 2) {
        double parent_local_scaling[16];
        double inverse_parent_local_scaling[16];
        double compensated_parent_scale[16];
        double parent_scale[3] = {1.0, 1.0, 1.0};
        if (parent_components) {
            parent_scale[0] = parent_components->scale[0];
            parent_scale[1] = parent_components->scale[1];
            parent_scale[2] = parent_components->scale[2];
        }
        fbx_mat4_scale_local(
            parent_scale[0], parent_scale[1], parent_scale[2], parent_local_scaling);
        if (!fbx_mat4_inverse_affine(parent_local_scaling, inverse_parent_local_scaling)) {
            memcpy(out, standard, sizeof(standard));
            return;
        }
        fbx_mat4_mul_local(
            parent_scale_shear, inverse_parent_local_scaling, compensated_parent_scale);
        fbx_mat4_mul_local(parent_rotation, child_rotation, global_rs);
        fbx_mat4_append_local(global_rs, compensated_parent_scale);
        fbx_mat4_append_local(global_rs, child_scaling);
    } else {
        fbx_mat4_mul_local(parent_rotation, child_rotation, global_rs);
        fbx_mat4_append_local(global_rs, parent_scale_shear);
        fbx_mat4_append_local(global_rs, child_scaling);
    }
    fbx_mat4_translate_local(standard[3], standard[7], standard[11], step);
    fbx_mat4_mul_local(step, global_rs, out);
}

/// @brief Pull the complete FBX transform stack off a `Model` node and expose it as TRS.
/// @details Unlike the older local-TRS-only path, this honors RotationOrder, pivots, offsets,
///          PreRotation/PostRotation, and geometric transforms before decomposing to the
///          SceneNode3D representation. `z_up` applies a full matrix basis conversion so rotated
///          and pivoted models convert correctly.
/// @param model_node Source FBX Model node; NULL leaves identity defaults.
/// @param z_up Non-zero to convert the composed matrix into runtime Y-up space.
/// @param[out] pos Optional three-component translation output.
/// @param[out] quat Optional four-component XYZW quaternion output.
/// @param[out] scale Optional three-component scale output.
static void fbx_extract_model_trs(
    fbx_node_t *model_node, int z_up, double *pos, double *quat, double *scale) {
    fbx_transform_components_t components;
    double matrix[16];
    if (pos) {
        pos[0] = 0.0;
        pos[1] = 0.0;
        pos[2] = 0.0;
    }
    if (quat) {
        quat[0] = 0.0;
        quat[1] = 0.0;
        quat[2] = 0.0;
        quat[3] = 1.0;
    }
    if (scale) {
        scale[0] = 1.0;
        scale[1] = 1.0;
        scale[2] = 1.0;
    }
    if (!model_node)
        return;
    fbx_read_model_transform_components(model_node, &components);
    fbx_build_model_transform_matrix(&components, matrix);
    if (z_up)
        fbx_mat4_apply_zup(matrix);
    fbx_decompose_matrix_trs(matrix, pos, quat, scale);
}

/// @brief Find a mesh binding by FBX Geometry identifier.
/// @param bindings Geometry-ID to mesh binding array.
/// @param count Number of entries in @p bindings.
/// @param id Geometry identifier to match.
/// @return Borrowed pointer to the matching binding, or NULL.
static const fbx_mesh_binding_t *fbx_lookup_mesh_binding_entry(const fbx_mesh_binding_t *bindings,
                                                               int32_t count,
                                                               int64_t id) {
    for (int32_t i = 0; i < count; i++)
        if (bindings[i].id == id)
            return &bindings[i];
    return NULL;
}

/// @brief Build a new Mesh3D containing only the triangles of @p src assigned to
///   @p material_slot, compacting the referenced vertices through a remap table so the
///   sub-mesh has no unused vertices.
/// @details Copies high-precision positions when present and remaps MorphTarget3D shapes
///          to the compact vertex order. The source mesh and material map remain borrowed.
/// @param src Source runtime mesh.
/// @param map Per-triangle FBX material-slot assignment.
/// @param material_slot Non-negative slot whose triangles are selected.
/// @return The new mesh object, or NULL if no triangle uses the slot or on allocation failure.
static void *fbx_clone_mesh_for_material_slot(const rt_mesh3d *src,
                                              const fbx_mesh_material_map_t *map,
                                              int32_t material_slot) {
    void *mesh_obj;
    rt_mesh3d *dst;
    uint32_t selected_triangles = 0;
    uint32_t *vertex_map = NULL;
    uint32_t *new_to_old = NULL;
    if (!src || !map || !map->triangle_slots || material_slot < 0)
        return NULL;
    for (uint32_t tri = 0; tri < map->triangle_count; tri++) {
        if (map->triangle_slots[tri] == material_slot)
            selected_triangles++;
    }
    if (selected_triangles == 0 || selected_triangles > UINT32_MAX / 3u)
        return NULL;
    mesh_obj = rt_mesh3d_new();
    if (!mesh_obj)
        return NULL;
    dst = (rt_mesh3d *)mesh_obj;
    rt_mesh3d_begin_geometry_batch(dst);
    rt_mesh3d_reserve(mesh_obj, (int64_t)selected_triangles * 3, selected_triangles);
    if (dst->build_failed)
        goto fail;
    if (src->positions64 && src->vertex_count > 0) {
        if (!rt_alloc_count_ok(dst->vertex_capacity, 3u * sizeof(double)))
            goto fail;
        dst->positions64 = (double *)calloc((size_t)dst->vertex_capacity * 3u, sizeof(double));
        if (!dst->positions64)
            goto fail;
    }
    if (!rt_alloc_count_ok(src->vertex_count, sizeof(*vertex_map)))
        goto fail;
    vertex_map = (uint32_t *)malloc((size_t)src->vertex_count * sizeof(*vertex_map));
    if (!rt_alloc_count_ok(dst->vertex_capacity, sizeof(*new_to_old)))
        goto fail;
    new_to_old = (uint32_t *)malloc((size_t)dst->vertex_capacity * sizeof(*new_to_old));
    if (!vertex_map || !new_to_old)
        goto fail;
    for (uint32_t i = 0; i < src->vertex_count; i++)
        vertex_map[i] = UINT32_MAX;
    for (uint32_t tri = 0; tri < map->triangle_count; tri++) {
        if (map->triangle_slots[tri] != material_slot)
            continue;
        for (uint32_t corner = 0; corner < 3u; corner++) {
            uint32_t old_index = src->indices[tri * 3u + corner];
            uint32_t new_index;
            if (old_index >= src->vertex_count)
                goto fail;
            if (vertex_map[old_index] == UINT32_MAX) {
                if (dst->vertex_count >= dst->vertex_capacity)
                    goto fail;
                new_index = dst->vertex_count++;
                vertex_map[old_index] = new_index;
                new_to_old[new_index] = old_index;
                dst->vertices[new_index] = src->vertices[old_index];
                if (dst->positions64 && src->positions64) {
                    memcpy(&dst->positions64[(size_t)new_index * 3u],
                           &src->positions64[(size_t)old_index * 3u],
                           3u * sizeof(double));
                }
            } else {
                new_index = vertex_map[old_index];
            }
            if (dst->index_count >= dst->index_capacity)
                goto fail;
            dst->indices[dst->index_count++] = new_index;
        }
    }
    dst->bone_count = src->bone_count;
    dst->bounds_dirty = 1;
    rt_mesh3d_end_geometry_batch(dst);
    if (src->morph_targets_ref) {
        void *remapped_morph =
            rt_morphtarget3d_clone_remapped(src->morph_targets_ref, new_to_old, dst->vertex_count);
        if (!remapped_morph)
            goto fail_after_batch;
        rt_mesh3d_set_morph_targets(dst, remapped_morph);
        fbx_release_ref(&remapped_morph);
    }
    free(vertex_map);
    free(new_to_old);
    return mesh_obj;
fail_after_batch:
    free(vertex_map);
    free(new_to_old);
    if (rt_obj_release_check0(mesh_obj))
        rt_obj_free(mesh_obj);
    return NULL;
fail:
    rt_mesh3d_end_geometry_batch(dst);
    free(vertex_map);
    free(new_to_old);
    if (rt_obj_release_check0(mesh_obj))
        rt_obj_free(mesh_obj);
    return NULL;
}

/// @brief Linear search for a `Material3D` associated with an FBX object ID. Mirror of
/// `fbx_lookup_mesh_binding` for the material table.
/// @param bindings Material-ID to runtime-material binding array.
/// @param count Number of entries in @p bindings.
/// @param id FBX Material identifier to match.
/// @return Borrowed runtime material pointer, or NULL when no binding matches.
static void *fbx_lookup_material_binding(const fbx_material_binding_t *bindings,
                                         int32_t count,
                                         int64_t id) {
    for (int32_t i = 0; i < count; i++)
        if (bindings[i].id == id)
            return bindings[i].material;
    return NULL;
}

/// @brief Decompose an evaluated affine matrix and assign its TRS to one scene node.
/// @details SceneNode3D stores TRS rather than a raw affine matrix. Any inheritance-generated shear
///          is reduced through the same deterministic decomposition used by animation keyframes.
/// @param node Runtime scene node to mutate.
/// @param matrix Evaluated parent-relative affine matrix.
static void fbx_apply_matrix_to_scene_node(void *node, const double matrix[16]) {
    void *quat_obj;
    double pos[3];
    double quat[4];
    double scale[3];
    if (!node || !matrix)
        return;
    fbx_decompose_matrix_trs(matrix, pos, quat, scale);
    rt_scene_node3d_set_position(node, pos[0], pos[1], pos[2]);
    quat_obj = rt_quat_new(quat[0], quat[1], quat[2], quat[3]);
    if (quat_obj) {
        rt_scene_node3d_set_rotation(node, quat_obj);
        fbx_release_ref(&quat_obj);
    }
    rt_scene_node3d_set_scale(node, scale[0], scale[1], scale[2]);
}

/// @brief Apply an FBX Model node's exact name and locally composed transform to a scene node.
/// @details This initial local assignment is later replaced with an inheritance-adjusted matrix
///          after the full model parent table is available. Keeping it here makes root/orphan
///          behavior explicit and provides one shared local conversion path.
/// @param obj Typed FBX Model node.
/// @param node Fresh runtime scene node.
/// @param z_up Nonzero to apply the FBX-to-Zanna basis conversion.
static void fbx_apply_model_node_trs(fbx_node_t *obj, void *node, int z_up) {
    void *quat_obj;
    double position[3];
    double quaternion[4];
    double scale[3];
    fbx_set_clean_object_name(node, obj);
    fbx_extract_model_trs(obj, z_up, position, quaternion, scale);
    rt_scene_node3d_set_position(node, position[0], position[1], position[2]);
    quat_obj = rt_quat_new(quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
    if (quat_obj) {
        rt_scene_node3d_set_rotation(node, quat_obj);
        fbx_release_ref(&quat_obj);
    }
    rt_scene_node3d_set_scale(node, scale[0], scale[1], scale[2]);
}

/// @brief (id, model-index) pair used to build a sorted lookup table for FBX parent
///   resolution, replacing repeated O(n) id scans with O(log n) binary search.
typedef struct {
    /// Numeric FBX Model identifier used as the sort key.
    int64_t id;
    /// Original index of the Model in the unsorted table.
    int32_t index;
} fbx_model_id_entry_t;

/// @brief One imported non-skeleton FBX Model and its evaluated runtime scene node.
typedef struct {
    /// Numeric FBX Model identifier.
    int64_t id;
    /// Numeric parent Model identifier, or zero.
    int64_t parent_id;
    /// Stable ordinal in the FBX `Objects` child array.
    int32_t import_index;
    /// Borrowed source Model node.
    fbx_node_t *source;
    /// Owned-until-publication runtime scene-node reference.
    void *node;
    /// Parsed static FBX transform-stack components.
    fbx_transform_components_t components;
    /// Composed parent-independent local matrix.
    double local_matrix[16];
    /// Evaluated world/global matrix.
    double global_matrix[16];
    /// Non-zero after @ref global_matrix has been evaluated.
    int global_evaluated;
} fbx_scene_model_binding_t;

/// @brief qsort comparator ordering fbx_model_id_entry_t by ascending id.
/// @param a First lookup entry.
/// @param b Second lookup entry.
/// @return Negative, zero, or positive according to ascending numeric ID order.
static int fbx_model_id_entry_cmp(const void *a, const void *b) {
    int64_t ia = ((const fbx_model_id_entry_t *)a)->id;
    int64_t ib = ((const fbx_model_id_entry_t *)b)->id;
    return (ia > ib) - (ia < ib);
}

/// @brief Binary-search a sorted Model-ID lookup table.
/// @param sorted Lookup table sorted by ascending ID.
/// @param count Number of entries in @p sorted.
/// @param id Model identifier to find.
/// @return Original Model index, or -1 when absent.
static int32_t fbx_model_index_for_id(const fbx_model_id_entry_t *sorted,
                                      int32_t count,
                                      int64_t id) {
    int32_t lo = 0;
    int32_t hi = count - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (sorted[mid].id == id)
            return sorted[mid].index;
        if (sorted[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/// @brief Find one scene-model binding by FBX numeric ID.
/// @param models Scene-model binding array.
/// @param model_count Number of entries in @p models.
/// @param id Non-zero Model identifier to find.
/// @return Borrowed pointer to the matching binding, or NULL.
static fbx_scene_model_binding_t *fbx_scene_model_for_id(fbx_scene_model_binding_t *models,
                                                         int32_t model_count,
                                                         int64_t id) {
    if (!models || id == 0)
        return NULL;
    for (int32_t i = 0; i < model_count; ++i) {
        if (models[i].id == id)
            return &models[i];
    }
    return NULL;
}

/// @brief Find a connected `NodeAttribute` with the requested subtype.
/// @details Standard FBX connects `NodeAttribute -> Model`; the reverse direction is accepted for
/// exporter compatibility. Numeric connection endpoints remain the only identity keys.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Connection table used for bidirectional adjacency.
/// @param model_id Owning Model identifier.
/// @param subtype Required NodeAttribute subtype, such as `Light` or `Camera`.
/// @return Borrowed matching NodeAttribute, or NULL.
static fbx_node_t *fbx_find_model_node_attribute(fbx_node_t *objects,
                                                 const fbx_conn_table_t *ct,
                                                 int64_t model_id,
                                                 const char *subtype) {
    if (!objects || !ct || !subtype)
        return NULL;
    for (int32_t ci = fbx_connections_first_for_parent(ct, model_id); ci >= 0;
         ci = fbx_connections_next_for_parent(ct, ci)) {
        if (ct->entries[ci].prop && ct->entries[ci].prop[0] != '\0')
            continue;
        fbx_node_t *object = fbx_find_object_by_id(objects, ct->entries[ci].child_id);
        if (object && strcmp(object->name, "NodeAttribute") == 0 && object->prop_count >= 3 &&
            fbx_ascii_equals_i(fbx_prop_str(object, 2), subtype)) {
            return object;
        }
    }
    for (int32_t ci = fbx_connections_first_for_child(ct, model_id); ci >= 0;
         ci = fbx_connections_next_for_child(ct, ci)) {
        if (ct->entries[ci].prop && ct->entries[ci].prop[0] != '\0')
            continue;
        fbx_node_t *object = fbx_find_object_by_id(objects, ct->entries[ci].parent_id);
        if (object && strcmp(object->name, "NodeAttribute") == 0 && object->prop_count >= 3 &&
            fbx_ascii_equals_i(fbx_prop_str(object, 2), subtype)) {
            return object;
        }
    }
    return NULL;
}

/// @brief Read a scalar from a NodeAttribute, falling back to its owning Model.
/// @param attribute Preferred NodeAttribute object; may be NULL.
/// @param model Owning Model fallback; may be NULL.
/// @param name Property name to read.
/// @param fallback Value returned when neither object supplies a finite scalar.
/// @param[out] out_found Optional flag indicating that an imported value was found.
/// @return Imported finite scalar or @p fallback.
static double fbx_read_attribute_number(
    fbx_node_t *attribute, fbx_node_t *model, const char *name, double fallback, int *out_found) {
    int found = 0;
    double value = fbx_read_object_number(attribute, name, fallback, &found);
    if (!found)
        value = fbx_read_object_number(model, name, fallback, &found);
    if (out_found)
        *out_found = found;
    return value;
}

/// @brief Read the first scalar found under a list of exporter-compatible aliases.
/// @param attribute Preferred NodeAttribute object.
/// @param model Owning Model fallback.
/// @param names Ordered array of property-name aliases.
/// @param name_count Number of entries in @p names.
/// @param fallback Value returned when no alias supplies a finite scalar.
/// @param[out] out_found Optional flag indicating that an imported value was found.
/// @return First finite aliased scalar, or @p fallback.
static double fbx_read_attribute_number_aliases(fbx_node_t *attribute,
                                                fbx_node_t *model,
                                                const char *const *names,
                                                size_t name_count,
                                                double fallback,
                                                int *out_found) {
    if (out_found)
        *out_found = 0;
    if (!names)
        return fallback;
    for (size_t i = 0; i < name_count; i++) {
        int found = 0;
        double value;
        if (!names[i] || names[i][0] == '\0')
            continue;
        value = fbx_read_attribute_number(attribute, model, names[i], fallback, &found);
        if (found) {
            if (out_found)
                *out_found = 1;
            return value;
        }
    }
    return fallback;
}

/// @brief Validate a required positive finite imported light dimension.
/// @param value Authored dimension.
/// @param property_name Name used in the asset-error diagnostic.
/// @return Non-zero when the value lies in the accepted positive range; otherwise
///         zero after setting a corrupt-input error.
static int fbx_validate_light_dimension(double value, const char *property_name) {
    if (isfinite(value) && value > 1e-6 && value <= 1000000.0)
        return 1;
    rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                        "FBX.Load: light %s must be finite and greater than zero",
                        property_name ? property_name : "dimension");
    return 0;
}

/// @brief Read an XYZ property from a NodeAttribute, falling back to its owning Model.
/// @param attribute Preferred NodeAttribute object.
/// @param model Owning Model fallback.
/// @param name Property name to read.
/// @param[out] out_xyz Receives the finite vector.
/// @return Non-zero when either object supplies a complete finite vector.
static int fbx_read_attribute_xyz(fbx_node_t *attribute,
                                  fbx_node_t *model,
                                  const char *name,
                                  double out_xyz[3]) {
    return fbx_read_object_xyz(attribute, name, out_xyz) ||
           fbx_read_object_xyz(model, name, out_xyz);
}

/// @brief Normalize a finite vector, substituting a requested fallback on degeneracy.
/// @param[in,out] vector Three-component vector to normalize.
/// @param x Fallback X component.
/// @param y Fallback Y component.
/// @param z Fallback Z component.
static void fbx_normalize_vec3_or(double vector[3], double x, double y, double z) {
    double maximum;
    double length;
    if (!vector)
        return;
    maximum = fmax(fabs(vector[0]), fmax(fabs(vector[1]), fabs(vector[2])));
    if (!isfinite(maximum) || maximum <= 1e-12) {
        vector[0] = x;
        vector[1] = y;
        vector[2] = z;
        return;
    }
    vector[0] /= maximum;
    vector[1] /= maximum;
    vector[2] /= maximum;
    length = sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
    if (!isfinite(length) || length <= 1e-12) {
        vector[0] = x;
        vector[1] = y;
        vector[2] = z;
        return;
    }
    vector[0] /= length;
    vector[1] /= length;
    vector[2] /= length;
}

/// @brief Build a node-local Light3D from an FBX light attribute.
/// @details Imports type, color, normalized intensity, range/decay, spot angles,
///          area dimensions, enabled state, and shadow state. Required FBX enum and
///          area-dimension violations set a corrupt-input diagnostic.
/// @param attribute Preferred Light NodeAttribute.
/// @param model Owning Model used as an exporter-compatible property fallback.
/// @param[in,out] context Loader context charged for the runtime light allocation.
/// @return Newly allocated Light3D on success, or NULL for invalid metadata, budget
///         rejection, or allocation failure.
static void *fbx_make_model_light(fbx_node_t *attribute,
                                  fbx_node_t *model,
                                  fbx_load_context_t *context) {
    void *light = NULL;
    void *position = NULL;
    void *direction = NULL;
    double color[3] = {1.0, 1.0, 1.0};
    double intensity;
    double range;
    double attenuation = RT_LIGHT3D_DEFAULT_ATTENUATION;
    double inner_angle;
    double outer_angle;
    int found = 0;
    int range_found = 0;
    int decay_type;
    int light_type;
    if ((!attribute && !model) || !context || !fbx_budget_charge(context, 1u, sizeof(rt_light3d)))
        return NULL;
    (void)fbx_read_attribute_xyz(attribute, model, "Color", color);
    for (int i = 0; i < 3; ++i)
        color[i] = fbx_clamp_double(color[i], 0.0, 1.0, 1.0);
    intensity = fbx_read_attribute_number(attribute, model, "Intensity", 100.0, NULL) / 100.0;
    intensity = fbx_clamp_double(intensity, 0.0, 1000000.0, 1.0);
    {
        double light_type_value =
            fbx_read_attribute_number(attribute, model, "LightType", 0.0, NULL);
        double decay_type_value =
            fbx_read_attribute_number(attribute, model, "DecayType", 2.0, NULL);
        if (!isfinite(light_type_value) || light_type_value < 0.0 || light_type_value > 4.0 ||
            floor(light_type_value) != light_type_value) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "FBX.Load: light type is outside the FBX SDK enum range");
            return NULL;
        }
        if (!isfinite(decay_type_value) || decay_type_value < 0.0 || decay_type_value > 3.0 ||
            floor(decay_type_value) != decay_type_value) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "FBX.Load: light decay type is outside the FBX SDK enum range");
            return NULL;
        }
        light_type = (int)light_type_value;
        decay_type = (int)decay_type_value;
    }
    range = fbx_read_attribute_number(attribute, model, "FarAttenuationEnd", 0.0, &range_found);
    if (!range_found)
        range = fbx_read_attribute_number(attribute, model, "DecayStart", 0.0, &range_found);
    if (range_found) {
        if (!fbx_validate_light_dimension(range, "range"))
            return NULL;
        attenuation = 1.0 / (range * range);
        if (!isfinite(attenuation) || attenuation <= 0.0)
            attenuation = RT_LIGHT3D_DEFAULT_ATTENUATION;
        else if (attenuation < RT_LIGHT3D_MIN_ATTENUATION)
            attenuation = RT_LIGHT3D_MIN_ATTENUATION;
    } else {
        range = 1.0 / sqrt(RT_LIGHT3D_DEFAULT_ATTENUATION);
    }
    position = rt_vec3_new(0.0, 0.0, 0.0);
    direction = rt_vec3_new(0.0, 0.0, -1.0);
    if (!position || !direction) {
        fbx_release_ref(&position);
        fbx_release_ref(&direction);
        return NULL;
    }
    switch (light_type) {
        case 1:
            light = rt_light3d_new_directional(direction, color[0], color[1], color[2]);
            break;
        case 2:
            outer_angle =
                fbx_read_attribute_number(attribute, model, "OuterAngle", 45.0, NULL) * 0.5;
            inner_angle =
                fbx_read_attribute_number(attribute, model, "InnerAngle", 30.0, NULL) * 0.5;
            outer_angle = fbx_clamp_double(outer_angle, 0.01, 89.0, 22.5);
            inner_angle = fbx_clamp_double(inner_angle, 0.0, outer_angle - 0.01, 15.0);
            light = rt_light3d_new_spot(position,
                                        direction,
                                        color[0],
                                        color[1],
                                        color[2],
                                        attenuation,
                                        inner_angle,
                                        outer_angle);
            break;
        case 3: {
            static const char *const width_names[] = {"Width", "AreaLightWidth", "Size"};
            static const char *const height_names[] = {"Height", "AreaLightHeight", "Size"};
            static const char *const radius_names[] = {"Radius", "AreaLightRadius", "Size"};
            double shape_value =
                fbx_read_attribute_number(attribute, model, "AreaLightShape", 0.0, NULL);
            int shape;
            if (!isfinite(shape_value) || shape_value < 0.0 || shape_value > 1.0 ||
                floor(shape_value) != shape_value) {
                rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                                   "FBX.Load: area light shape is outside the FBX SDK enum range");
                break;
            }
            shape = (int)shape_value;
            if (shape == 0) {
                double width =
                    fbx_read_attribute_number_aliases(attribute,
                                                      model,
                                                      width_names,
                                                      sizeof(width_names) / sizeof(width_names[0]),
                                                      1.0,
                                                      &found);
                double height = fbx_read_attribute_number_aliases(attribute,
                                                                  model,
                                                                  height_names,
                                                                  sizeof(height_names) /
                                                                      sizeof(height_names[0]),
                                                                  1.0,
                                                                  &found);
                if (!fbx_validate_light_dimension(width, "rectangle width") ||
                    !fbx_validate_light_dimension(height, "rectangle height"))
                    break;
                light = rt_light3d_new_area_rectangle(position,
                                                      direction,
                                                      width,
                                                      height,
                                                      color[0],
                                                      color[1],
                                                      color[2],
                                                      attenuation,
                                                      range);
            } else {
                double radius = fbx_read_attribute_number_aliases(attribute,
                                                                  model,
                                                                  radius_names,
                                                                  sizeof(radius_names) /
                                                                      sizeof(radius_names[0]),
                                                                  1.0,
                                                                  &found);
                if (!fbx_validate_light_dimension(radius, "sphere radius"))
                    break;
                light = rt_light3d_new_area_sphere(
                    position, radius, color[0], color[1], color[2], range);
                rt_light3d_set_attenuation(light, attenuation);
            }
            break;
        }
        case 4: {
            static const char *const radius_names[] = {"VolumeRadius", "Radius", "Size"};
            double radius =
                fbx_read_attribute_number_aliases(attribute,
                                                  model,
                                                  radius_names,
                                                  sizeof(radius_names) / sizeof(radius_names[0]),
                                                  1.0,
                                                  &found);
            if (!fbx_validate_light_dimension(radius, "volume radius"))
                break;
            light = rt_light3d_new_volume(position, radius, color[0], color[1], color[2], range);
            break;
        }
        default:
            light = rt_light3d_new_point(position, color[0], color[1], color[2], attenuation);
            break;
    }
    fbx_release_ref(&position);
    fbx_release_ref(&direction);
    if (!light)
        return NULL;
    rt_light3d_set_intensity(light, intensity);
    rt_light3d_set_decay_type(light, decay_type);
    if (light_type == 0 || light_type == 2)
        ((rt_light3d *)light)->range = range_found ? range : 0.0;
    rt_light3d_set_enabled(
        light,
        fbx_read_attribute_number(
            attribute,
            model,
            "CastLight",
            fbx_read_attribute_number(attribute, model, "CastLightOnObject", 1.0, NULL),
            NULL) != 0.0);
    rt_light3d_set_casts_shadows(
        light, fbx_read_attribute_number(attribute, model, "CastShadows", 0.0, NULL) != 0.0);
    return light;
}

/// @brief Resolve a camera look-at or up target through a named FBX relationship.
/// @param models Evaluated scene-model binding array.
/// @param model_count Number of entries in @p models.
/// @param ct Connection table containing relationship properties.
/// @param model_id Camera Model identifier.
/// @param attribute_id Camera NodeAttribute identifier, or zero.
/// @param relationship Required connection property name.
/// @return Borrowed matching target Model binding, or NULL.
static fbx_scene_model_binding_t *fbx_find_camera_target(fbx_scene_model_binding_t *models,
                                                         int32_t model_count,
                                                         const fbx_conn_table_t *ct,
                                                         int64_t model_id,
                                                         int64_t attribute_id,
                                                         const char *relationship) {
    if (!models || !ct || !relationship)
        return NULL;
    for (int32_t i = 0; i < ct->count; ++i) {
        const fbx_conn_t *connection = &ct->entries[i];
        int64_t other = 0;
        if (!connection->prop || !fbx_ascii_equals_i(connection->prop, relationship))
            continue;
        if (connection->parent_id == model_id || connection->parent_id == attribute_id)
            other = connection->child_id;
        else if (connection->child_id == model_id || connection->child_id == attribute_id)
            other = connection->parent_id;
        if (other != 0) {
            fbx_scene_model_binding_t *target = fbx_scene_model_for_id(models, model_count, other);
            if (target)
                return target;
        }
    }
    return NULL;
}

/// @brief Raw FBX camera projection properties before conversion to Camera3D's vertical-FOV form.
typedef struct {
    /// FBX projection enum: perspective or orthographic.
    double projection_type;
    /// FBX aperture-mode enum.
    double aperture_mode;
    /// Generic authored field-of-view angle.
    double field_of_view;
    /// Authored horizontal field of view.
    double field_of_view_x;
    /// Authored vertical field of view.
    double field_of_view_y;
    /// Focal length in millimetres.
    double focal_length;
    /// Film aperture width in inches.
    double film_width;
    /// Film aperture height in inches.
    double film_height;
    /// Render aspect-width component.
    double aspect_width;
    /// Render aspect-height component.
    double aspect_height;
    /// Near clipping distance.
    double near_plane;
    /// Far clipping distance.
    double far_plane;
    /// Orthographic zoom/size.
    double ortho_zoom;
    /// Non-zero when generic FOV was explicitly authored.
    int has_field_of_view;
    /// Non-zero when horizontal FOV was explicitly authored.
    int has_field_of_view_x;
    /// Non-zero when vertical FOV was explicitly authored.
    int has_field_of_view_y;
    /// Non-zero when focal length was explicitly authored.
    int has_focal_length;
} fbx_camera_property_state_t;

/// @brief Read one camera's complete scalar state with documented FBX SDK defaults.
/// @param attribute Preferred Camera NodeAttribute.
/// @param model Owning Model used as an exporter-compatible fallback.
/// @param[out] state Camera property state initialized and populated before return.
static void fbx_read_camera_property_state(fbx_node_t *attribute,
                                           fbx_node_t *model,
                                           fbx_camera_property_state_t *state) {
    int found_ortho_zoom = 0;
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->projection_type =
        fbx_read_attribute_number(attribute, model, "CameraProjectionType", 0.0, NULL);
    /* Autodesk's current SDK default is eVertical (enum value 2). */
    state->aperture_mode = fbx_read_attribute_number(attribute, model, "ApertureMode", 2.0, NULL);
    state->field_of_view =
        fbx_read_attribute_number(attribute, model, "FieldOfView", 60.0, &state->has_field_of_view);
    state->field_of_view_x = fbx_read_attribute_number(
        attribute, model, "FieldOfViewX", 60.0, &state->has_field_of_view_x);
    state->field_of_view_y = fbx_read_attribute_number(
        attribute, model, "FieldOfViewY", 60.0, &state->has_field_of_view_y);
    state->focal_length =
        fbx_read_attribute_number(attribute, model, "FocalLength", 0.0, &state->has_focal_length);
    state->film_width = fbx_read_attribute_number(attribute, model, "FilmWidth", 0.816, NULL);
    state->film_height = fbx_read_attribute_number(attribute, model, "FilmHeight", 0.612, NULL);
    state->aspect_width = fbx_read_attribute_number(attribute, model, "AspectWidth", 1.0, NULL);
    state->aspect_height = fbx_read_attribute_number(attribute, model, "AspectHeight", 1.0, NULL);
    state->near_plane = fbx_read_attribute_number(attribute, model, "NearPlane", 0.1, NULL);
    state->far_plane = fbx_read_attribute_number(attribute, model, "FarPlane", 1000.0, NULL);
    state->ortho_zoom =
        fbx_read_attribute_number(attribute, model, "OrthoZoom", 10.0, &found_ortho_zoom);
    if (!found_ortho_zoom)
        state->ortho_zoom =
            fbx_read_attribute_number(attribute, model, "CameraOrthoZoom", 10.0, NULL);
}

/// @brief Derive a finite positive FBX camera aspect ratio.
/// @param state Camera property state.
/// @return Clamped width/height aspect, or 1.0 for absent, non-finite, or non-positive input.
static double fbx_camera_state_aspect(const fbx_camera_property_state_t *state) {
    if (!state || !isfinite(state->aspect_width) || !isfinite(state->aspect_height) ||
        state->aspect_width <= 0.0 || state->aspect_height <= 0.0)
        return 1.0;
    return fbx_clamp_double(state->aspect_width / state->aspect_height, 1e-6, 1000000.0, 1.0);
}

/// @brief Convert an FBX horizontal field of view to Camera3D's vertical convention.
/// @param horizontal_degrees Horizontal angle in degrees.
/// @param aspect Positive width/height aspect ratio.
/// @return Clamped vertical angle in degrees, or the 60-degree fallback for invalid input.
static double fbx_camera_horizontal_to_vertical_fov(double horizontal_degrees, double aspect) {
    static const double pi = 3.14159265358979323846;
    double radians;
    double vertical;
    if (!isfinite(horizontal_degrees) || horizontal_degrees <= 0.0 || horizontal_degrees >= 180.0 ||
        !isfinite(aspect) || aspect <= 0.0)
        return 60.0;
    radians = horizontal_degrees * pi / 180.0;
    vertical = 2.0 * atan(tan(radians * 0.5) / aspect) * 180.0 / pi;
    return fbx_clamp_double(vertical, 1.0, 179.0, 60.0);
}

/// @brief Evaluate the active FBX aperture mode into a vertical runtime field of view.
/// @details FBX stores film aperture in inches and focal length in millimetres. The SDK treats
///          focal-length mode as horizontal unless the active aperture mode is explicitly
///          vertical; the runtime always stores a vertical FOV, so horizontal results are
///          converted through the evaluated AspectWidth/AspectHeight ratio.
/// @param state Complete camera property state.
/// @return Vertical runtime field of view in the range `[1, 179]` degrees, using
///         exporter-compatible fallbacks when the selected lane is absent.
static double fbx_camera_state_vertical_fov(const fbx_camera_property_state_t *state) {
    static const double pi = 3.14159265358979323846;
    int aperture_mode;
    double aspect;
    double horizontal;
    if (!state)
        return 60.0;
    aspect = fbx_camera_state_aspect(state);
    aperture_mode = (int)floor(state->aperture_mode + 0.5);
    if (aperture_mode == 0) { /* eHorizAndVert */
        if (state->has_field_of_view_y)
            return fbx_clamp_double(state->field_of_view_y, 1.0, 179.0, 60.0);
        if (state->has_field_of_view_x)
            return fbx_camera_horizontal_to_vertical_fov(state->field_of_view_x, aspect);
    } else if (aperture_mode == 1) { /* eHorizontal */
        if (state->has_field_of_view)
            return fbx_camera_horizontal_to_vertical_fov(state->field_of_view, aspect);
        if (state->has_field_of_view_x)
            return fbx_camera_horizontal_to_vertical_fov(state->field_of_view_x, aspect);
    } else if (aperture_mode == 2) { /* eVertical */
        if (state->has_field_of_view)
            return fbx_clamp_double(state->field_of_view, 1.0, 179.0, 60.0);
        if (state->has_field_of_view_y)
            return fbx_clamp_double(state->field_of_view_y, 1.0, 179.0, 60.0);
    } else if (aperture_mode == 3 && state->has_focal_length && state->focal_length > 0.0 &&
               state->film_width > 0.0) { /* eFocalLength */
        horizontal =
            2.0 * atan((state->film_width * 25.4 * 0.5) / state->focal_length) * 180.0 / pi;
        return fbx_camera_horizontal_to_vertical_fov(horizontal, aspect);
    }
    /* Exporter-compatible fallback for sparse camera records. */
    if (state->has_field_of_view_y)
        return fbx_clamp_double(state->field_of_view_y, 1.0, 179.0, 60.0);
    if (state->has_field_of_view)
        return fbx_clamp_double(state->field_of_view, 1.0, 179.0, 60.0);
    if (state->has_field_of_view_x)
        return fbx_camera_horizontal_to_vertical_fov(state->field_of_view_x, aspect);
    if (state->has_focal_length && state->focal_length > 0.0 && state->film_height > 0.0)
        return fbx_clamp_double(
            2.0 * atan((state->film_height * 25.4 * 0.5) / state->focal_length) * 180.0 / pi,
            1.0,
            179.0,
            60.0);
    return 60.0;
}

/// @brief Validate the discrete FBX camera enums required to construct a projection.
/// @param state Camera property state to validate.
/// @return Non-zero when projection and aperture modes are finite integral values in
///         their SDK ranges; otherwise zero after setting an asset error.
static int fbx_camera_state_validate_enums(const fbx_camera_property_state_t *state) {
    if (!state || !isfinite(state->projection_type) ||
        state->projection_type != floor(state->projection_type) || state->projection_type < 0.0 ||
        state->projection_type > 1.0) {
        rt_asset_error_set_if_empty(
            RT_ASSET_ERROR_CORRUPT,
            "FBX.Load: camera projection mode must be perspective or orthographic");
        return 0;
    }
    if (!isfinite(state->aperture_mode) || state->aperture_mode != floor(state->aperture_mode) ||
        state->aperture_mode < 0.0 || state->aperture_mode > 3.0) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "FBX.Load: camera aperture mode is outside the FBX enum range");
        return 0;
    }
    return 1;
}

/// @brief Build and orient one Camera3D from an evaluated FBX camera Model.
/// @details Converts projection state into runtime perspective/orthographic parameters,
///          derives eye/forward/up vectors from the evaluated global matrix, and honors
///          optional `LookAtProperty` and `UpVectorProperty` target relationships.
/// @param attribute Preferred Camera NodeAttribute.
/// @param model Evaluated owning Model binding.
/// @param models Complete evaluated Model table for target resolution.
/// @param model_count Number of entries in @p models.
/// @param ct Connection table containing camera target relationships.
/// @param[in,out] context Loader context charged for the runtime camera allocation.
/// @return Newly allocated and oriented Camera3D, or NULL for invalid metadata,
///         budget rejection, or allocation failure.
static void *fbx_make_model_camera(fbx_node_t *attribute,
                                   fbx_scene_model_binding_t *model,
                                   fbx_scene_model_binding_t *models,
                                   int32_t model_count,
                                   const fbx_conn_table_t *ct,
                                   fbx_load_context_t *context) {
    fbx_camera_property_state_t state;
    double aspect;
    void *camera;
    double eye[3];
    double forward[3];
    double up[3];
    double target[3];
    int64_t attribute_id = attribute && attribute->prop_count > 0 ? fbx_prop_i64(attribute, 0) : 0;
    fbx_scene_model_binding_t *look_at;
    fbx_scene_model_binding_t *up_target;
    if (!model || !ct || !context || !fbx_budget_charge(context, 1u, sizeof(rt_camera3d)))
        return NULL;
    fbx_read_camera_property_state(attribute, model->source, &state);
    if (!fbx_camera_state_validate_enums(&state))
        return NULL;
    aspect = fbx_camera_state_aspect(&state);
    if ((int)state.projection_type == 1)
        camera = rt_camera3d_new_ortho(
            fabs(state.ortho_zoom), aspect, state.near_plane, state.far_plane);
    else
        camera = rt_camera3d_new(
            fbx_camera_state_vertical_fov(&state), aspect, state.near_plane, state.far_plane);
    if (!camera)
        return NULL;
    eye[0] = model->global_matrix[3];
    eye[1] = model->global_matrix[7];
    eye[2] = model->global_matrix[11];
    forward[0] = -model->global_matrix[2];
    forward[1] = -model->global_matrix[6];
    forward[2] = -model->global_matrix[10];
    up[0] = model->global_matrix[1];
    up[1] = model->global_matrix[5];
    up[2] = model->global_matrix[9];
    fbx_normalize_vec3_or(forward, 0.0, 0.0, -1.0);
    fbx_normalize_vec3_or(up, 0.0, 1.0, 0.0);
    target[0] = eye[0] + forward[0];
    target[1] = eye[1] + forward[1];
    target[2] = eye[2] + forward[2];
    look_at =
        fbx_find_camera_target(models, model_count, ct, model->id, attribute_id, "LookAtProperty");
    if (look_at) {
        target[0] = look_at->global_matrix[3];
        target[1] = look_at->global_matrix[7];
        target[2] = look_at->global_matrix[11];
    }
    up_target = fbx_find_camera_target(
        models, model_count, ct, model->id, attribute_id, "UpVectorProperty");
    if (up_target) {
        up[0] = up_target->global_matrix[3] - eye[0];
        up[1] = up_target->global_matrix[7] - eye[1];
        up[2] = up_target->global_matrix[11] - eye[2];
        fbx_normalize_vec3_or(up, 0.0, 1.0, 0.0);
    }
    rt_camera3d_look_at_components(
        camera, eye[0], eye[1], eye[2], target[0], target[1], target[2], up[0], up[1], up[2]);
    return camera;
}

/// @brief Three-phase builder that assembles a Zanna `SceneNode3D` graph from the flat
/// FBX `Objects` / `Connections` soup. Phase 1: iterate `Model` entries, skip skeleton
/// limbs (handled by the skeleton importer), allocate a scene node per model with
/// decoded name and extracted TRS, recording its FBX id + parent id. Phase 2: walk
/// connections again to attach Meshes, Materials, and extra-mesh groupings (via
/// auxiliary child nodes) to their parent models. Phase 3: parent nodes to each other
/// using the recorded parent ids, rooting orphans under a synthesized scene root.
/// Returns the scene root or NULL on any allocation failure (intermediate nodes are
/// released via the GC on failure — no manual cleanup needed). This is the "scene
/// graph" half of the FBX importer; the flat mesh/material/animation lists live on
/// `rt_fbx_asset` alongside the root.
/// @param[in,out] asset Staged asset that receives standalone camera references.
/// @param root Parsed FBX document root.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Validated object connection table.
/// @param constraint_pose Optional pre-evaluated static constrained pose.
/// @param mesh_bindings Geometry-ID to runtime-mesh binding array.
/// @param mesh_binding_count Number of entries in @p mesh_bindings.
/// @param material_bindings Material-ID to runtime-material binding array.
/// @param material_binding_count Number of entries in @p material_bindings.
/// @param z_up Non-zero to convert static model transforms into runtime Y-up space.
/// @param[in,out] context Loader context charged for published resources.
/// @param[out] out_failed Optional hard-failure flag, initialized to zero and set when
///                        NULL represents transaction failure rather than an empty scene.
/// @return Newly allocated scene root owning the assembled hierarchy, or NULL for
///         invalid input or any hard allocation, topology, transform, camera, or light failure.
static void *fbx_build_scene_root(rt_fbx_asset *asset,
                                  fbx_node_t *root,
                                  fbx_node_t *objects,
                                  const fbx_conn_table_t *ct,
                                  const fbx_constraint_pose_t *constraint_pose,
                                  const fbx_mesh_binding_t *mesh_bindings,
                                  int32_t mesh_binding_count,
                                  const fbx_material_binding_t *material_bindings,
                                  int32_t material_binding_count,
                                  int z_up,
                                  fbx_load_context_t *context,
                                  int *out_failed) {
    fbx_scene_model_binding_t *models = NULL;
    int32_t model_count = 0;
    int32_t model_capacity = 0;
    void *scene_root = NULL;
    void *default_material = NULL;
    int failed = 0;

    if (out_failed)
        *out_failed = 0;

    if (!asset || !root || !objects || !context)
        return NULL;

    for (int32_t i = 0; i < objects->child_count; i++) {
        fbx_node_t *obj = &objects->children[i];
        const char *type_str;
        void *node;
        int64_t model_id;

        if (strcmp(obj->name, "Model") != 0 || obj->prop_count < 3)
            continue;
        type_str = fbx_prop_str(obj, 2);
        if (fbx_model_type_is_skeleton(type_str)) {
            continue;
        }

        if (model_count >= model_capacity) {
            if (model_capacity < 0 || model_capacity > INT32_MAX / 2 ||
                (size_t)(model_capacity == 0 ? 16 : model_capacity * 2) >
                    SIZE_MAX / sizeof(*models)) {
                failed = 1;
                break;
            }
            int32_t new_capacity = model_capacity == 0 ? 16 : model_capacity * 2;
            void *nm = realloc(models, (size_t)new_capacity * sizeof(*models));
            if (!nm) {
                failed = 1;
                break;
            }
            models = (fbx_scene_model_binding_t *)nm;
            model_capacity = new_capacity;
        }

        node = rt_scene_node3d_new();
        if (!node) {
            failed = 1;
            break;
        }
        model_id = fbx_prop_i64(obj, 0);
        fbx_apply_model_node_trs(obj, node, z_up);

        models[model_count].id = model_id;
        models[model_count].parent_id = fbx_find_model_parent(objects, ct, model_id);
        models[model_count].import_index = i;
        models[model_count].source = obj;
        models[model_count].node = node;
        ((rt_scene_node3d *)node)->import_index = i;
        fbx_read_model_transform_components(obj, &models[model_count].components);
        fbx_build_model_transform_matrix(&models[model_count].components,
                                         models[model_count].local_matrix);
        if (z_up)
            fbx_mat4_apply_zup(models[model_count].local_matrix);
        models[model_count].global_evaluated = 0;
        model_count++;
    }

    if (!failed) {
        scene_root = rt_scene_node3d_new();
        if (!scene_root)
            failed = 1;
        default_material = rt_material3d_new();
        if (!default_material)
            failed = 1;
    }

    for (int32_t i = 0; !failed && i < model_count; i++) {
        void *primary_mesh = NULL;
        void *primary_material = NULL;
        int32_t extra_mesh_count = 0;
        void **model_materials = NULL;
        int32_t model_material_count = 0;
        int32_t model_material_capacity = 0;
        for (int32_t ci = fbx_connections_first_for_parent(ct, models[i].id); ci >= 0;
             ci = fbx_connections_next_for_parent(ct, ci)) {
            int64_t child_id = ct->entries[ci].child_id;
            fbx_node_t *child_obj;
            void *material;

            child_obj = fbx_find_object_by_id(objects, child_id);
            if (!child_obj || child_obj->prop_count < 1)
                continue;
            if (strcmp(child_obj->name, "Material") != 0)
                continue;
            material =
                fbx_lookup_material_binding(material_bindings, material_binding_count, child_id);
            if (material) {
                if (model_material_count >= model_material_capacity) {
                    int32_t new_capacity =
                        model_material_capacity == 0 ? 4 : model_material_capacity * 2;
                    void **grown;
                    if (model_material_capacity > INT32_MAX / 2 ||
                        (size_t)new_capacity > SIZE_MAX / sizeof(void *)) {
                        failed = 1;
                        break;
                    }
                    grown =
                        (void **)realloc(model_materials, (size_t)new_capacity * sizeof(void *));
                    if (!grown) {
                        failed = 1;
                        break;
                    }
                    model_materials = grown;
                    model_material_capacity = new_capacity;
                }
                model_materials[model_material_count++] = material;
                if (!primary_material)
                    primary_material = material;
            }
        }
        for (int32_t ci = fbx_connections_first_for_parent(ct, models[i].id); !failed && ci >= 0;
             ci = fbx_connections_next_for_parent(ct, ci)) {
            int64_t child_id = ct->entries[ci].child_id;
            fbx_node_t *child_obj;
            const fbx_mesh_binding_t *binding;
            void *mesh;
            void *material;
            void *mesh_node;

            child_obj = fbx_find_object_by_id(objects, child_id);
            if (!child_obj || child_obj->prop_count < 3)
                continue;
            if (strcmp(child_obj->name, "Geometry") != 0 ||
                !fbx_geometry_type_is_renderable(fbx_prop_str(child_obj, 2))) {
                continue;
            }
            binding = fbx_lookup_mesh_binding_entry(mesh_bindings, mesh_binding_count, child_id);
            if (!binding || !binding->mesh)
                continue;
            mesh = binding->mesh;
            material = primary_material ? primary_material : default_material;
            if (binding->material_map.has_slots && binding->material_map.triangle_count > 0 &&
                binding->material_map.triangle_slots) {
                int32_t slot = binding->material_map.triangle_slots[0];
                if (slot >= 0 && slot < model_material_count && model_materials[slot])
                    material = model_materials[slot];
            }
            if (binding->material_map.has_slots && binding->material_map.slot_count > 1 &&
                model_material_count > 1) {
                int32_t split_added = 0;
                for (int32_t slot = 0; slot < binding->material_map.slot_count; slot++) {
                    void *slot_mesh = fbx_clone_mesh_for_material_slot(
                        (const rt_mesh3d *)mesh, &binding->material_map, slot);
                    void *slot_material = slot < model_material_count && model_materials[slot]
                                              ? model_materials[slot]
                                              : default_material;
                    if (!slot_mesh)
                        continue;
                    mesh_node = rt_scene_node3d_new();
                    if (!mesh_node) {
                        fbx_release_ref(&slot_mesh);
                        failed = 1;
                        break;
                    }
                    {
                        char generated_name[96];
                        snprintf(generated_name,
                                 sizeof(generated_name),
                                 "mesh_%d_material_%d",
                                 extra_mesh_count,
                                 slot);
                        rt_string runtime_name = rt_const_cstr(generated_name);
                        rt_scene_node3d_set_name(mesh_node, runtime_name);
                        rt_string_unref(runtime_name);
                    }
                    rt_scene_node3d_set_mesh(mesh_node, slot_mesh);
                    rt_scene_node3d_set_material(mesh_node, slot_material);
                    if (!rt_scene_node3d_try_add_child(models[i].node, mesh_node)) {
                        fbx_release_ref(&mesh_node);
                        fbx_release_ref(&slot_mesh);
                        failed = 1;
                        break;
                    }
                    fbx_release_ref(&mesh_node);
                    fbx_release_ref(&slot_mesh);
                    extra_mesh_count++;
                    split_added++;
                }
                if (failed)
                    break;
                if (split_added > 0)
                    continue;
            }
            if (!primary_mesh) {
                primary_mesh = mesh;
                primary_material = material;
                continue;
            }
            mesh_node = rt_scene_node3d_new();
            if (!mesh_node) {
                failed = 1;
                break;
            }
            if (child_obj->prop_count >= 2)
                fbx_set_clean_object_name(mesh_node, child_obj);
            if (extra_mesh_count > 0) {
                char generated_name[64];
                snprintf(generated_name, sizeof(generated_name), "mesh_%d", extra_mesh_count);
                rt_string runtime_name = rt_const_cstr(generated_name);
                rt_scene_node3d_set_name(mesh_node, runtime_name);
                rt_string_unref(runtime_name);
            }
            rt_scene_node3d_set_mesh(mesh_node, mesh);
            rt_scene_node3d_set_material(mesh_node, material ? material : default_material);
            if (!rt_scene_node3d_try_add_child(models[i].node, mesh_node)) {
                fbx_release_ref(&mesh_node);
                failed = 1;
                break;
            }
            fbx_release_ref(&mesh_node);
            extra_mesh_count++;
        }
        free(model_materials);
        if (failed)
            break;
        if (primary_mesh) {
            rt_scene_node3d_set_mesh(models[i].node, primary_mesh);
            rt_scene_node3d_set_material(models[i].node,
                                         primary_material ? primary_material : default_material);
        }
        {
            fbx_node_t *light_attribute =
                fbx_find_model_node_attribute(objects, ct, models[i].id, "Light");
            if (light_attribute ||
                (models[i].source->prop_count >= 3 &&
                 fbx_ascii_equals_i(fbx_prop_str(models[i].source, 2), "Light"))) {
                void *light = fbx_make_model_light(light_attribute, models[i].source, context);
                if (!light) {
                    failed = 1;
                    break;
                }
                rt_scene_node3d_set_light(models[i].node, light);
                fbx_release_ref(&light);
            }
        }
    }

    /* Resolve each model's direct parent index once (O(n log n)) so the hierarchy
     * pass below is O(n * depth) instead of O(n^3) on deep or adversarial scenes. */
    int32_t *parent_idx = NULL;
    if (!failed && model_count > 0) {
        parent_idx = (int32_t *)malloc((size_t)model_count * sizeof(*parent_idx));
        if (!parent_idx) {
            failed = 1;
        } else {
            fbx_model_id_entry_t *id_table =
                (fbx_model_id_entry_t *)malloc((size_t)model_count * sizeof(*id_table));
            if (id_table) {
                for (int32_t i = 0; i < model_count; i++) {
                    id_table[i].id = models[i].id;
                    id_table[i].index = i;
                }
                qsort(id_table, (size_t)model_count, sizeof(*id_table), fbx_model_id_entry_cmp);
                for (int32_t i = 0; i < model_count; i++)
                    parent_idx[i] =
                        models[i].parent_id != 0
                            ? fbx_model_index_for_id(id_table, model_count, models[i].parent_id)
                            : -1;
                free(id_table);
            } else {
                /* Rare OOM on the lookup table: fill via one linear pass (O(n^2)),
                 * still far better than the O(n^3) loop it replaces. */
                for (int32_t i = 0; i < model_count; i++) {
                    parent_idx[i] = -1;
                    if (models[i].parent_id != 0)
                        for (int32_t pi = 0; pi < model_count; pi++)
                            if (models[pi].id == models[i].parent_id) {
                                parent_idx[i] = pi;
                                break;
                            }
                }
            }
        }
    }

    /* Evaluate parent globals in topological waves using the same FBX inheritance composer that
     * animation sampling uses, then derive the parent-relative TRS consumed by SceneNode3D. */
    if (!failed && model_count > 0 && constraint_pose) {
        for (int32_t i = 0; i < model_count; ++i) {
            const double *evaluated =
                fbx_constraint_static_global_for_id(constraint_pose, models[i].id);
            if (!evaluated) {
                failed = 1;
                break;
            }
            memcpy(models[i].global_matrix, evaluated, sizeof(models[i].global_matrix));
            models[i].global_evaluated = 1;
        }
        if (!failed) {
            for (int32_t i = 0; i < model_count; ++i) {
                int32_t parent = parent_idx[i];
                double runtime_local[16];
                if (parent >= 0) {
                    double inverse_parent[16];
                    if (!fbx_mat4_inverse_affine(models[parent].global_matrix, inverse_parent)) {
                        failed = 1;
                        break;
                    }
                    fbx_mat4_mul_local(inverse_parent, models[i].global_matrix, runtime_local);
                } else {
                    memcpy(runtime_local, models[i].global_matrix, sizeof(runtime_local));
                }
                fbx_apply_matrix_to_scene_node(models[i].node, runtime_local);
            }
        }
    } else if (!failed && model_count > 0) {
        int32_t evaluated_count = 0;
        for (int32_t pass = 0; pass < model_count && evaluated_count < model_count; pass++) {
            int32_t progress = 0;
            for (int32_t i = 0; i < model_count; i++) {
                int32_t parent = parent_idx[i];
                if (models[i].global_evaluated ||
                    (parent >= 0 && !models[parent].global_evaluated)) {
                    continue;
                }
                if (parent >= 0) {
                    fbx_build_inherited_global_matrix(models[parent].global_matrix,
                                                      &models[parent].components,
                                                      models[i].local_matrix,
                                                      &models[i].components,
                                                      models[i].global_matrix);
                } else {
                    memcpy(models[i].global_matrix,
                           models[i].local_matrix,
                           sizeof(models[i].global_matrix));
                }
                models[i].global_evaluated = 1;
                evaluated_count++;
                progress++;
            }
            if (progress == 0)
                break;
        }
        if (evaluated_count != model_count) {
            failed = 1;
        } else {
            for (int32_t i = 0; i < model_count; i++) {
                int32_t parent = parent_idx[i];
                double runtime_local[16];
                if (parent >= 0) {
                    double inverse_parent[16];
                    if (!fbx_mat4_inverse_affine(models[parent].global_matrix, inverse_parent)) {
                        failed = 1;
                        break;
                    }
                    fbx_mat4_mul_local(inverse_parent, models[i].global_matrix, runtime_local);
                } else {
                    memcpy(runtime_local, models[i].global_matrix, sizeof(runtime_local));
                }
                fbx_apply_matrix_to_scene_node(models[i].node, runtime_local);
            }
        }
    }

    /* Build cameras only after every model's global transform is evaluated so authored look-at
     * and up-vector target relationships resolve against stable world-space positions. The asset
     * inventory and owning model node retain the same template camera; scene instantiation deep
     * clones the node attachment for independent mutable view/projection state. */
    for (int32_t i = 0; !failed && i < model_count; ++i) {
        fbx_node_t *camera_attribute =
            fbx_find_model_node_attribute(objects, ct, models[i].id, "Camera");
        if (camera_attribute || (models[i].source->prop_count >= 3 &&
                                 fbx_ascii_equals_i(fbx_prop_str(models[i].source, 2), "Camera"))) {
            void *camera = fbx_make_model_camera(
                camera_attribute, &models[i], models, model_count, ct, context);
            if (!camera || asset->camera_count == INT32_MAX ||
                !fbx_budget_charge(context, 1u, sizeof(void *)) ||
                !fbx_asset_reserve_ref_array(
                    &asset->cameras, &asset->camera_capacity, asset->camera_count + 1)) {
                fbx_release_ref(&camera);
                failed = 1;
                break;
            }
            rt_scene_node3d_set_camera(models[i].node, camera);
            asset->cameras[asset->camera_count++] = camera;
        }
    }

    for (int32_t i = 0; !failed && i < model_count; i++) {
        void *parent = scene_root;
        if (models[i].parent_id != 0) {
            int32_t guard = 0;
            int32_t walk = parent_idx[i];
            /* Walk ancestors by index; detect self-cycles and over-long chains. */
            while (walk >= 0) {
                if (walk == i || ++guard > model_count) {
                    failed = 1;
                    break;
                }
                walk = parent_idx[walk];
            }
            if (failed)
                break;
            if (parent_idx[i] >= 0)
                parent = models[parent_idx[i]].node;
        }
        if (!rt_scene_node3d_try_add_child(parent, models[i].node)) {
            failed = 1;
            break;
        }
    }
    free(parent_idx);

    for (int32_t i = 0; i < model_count; i++)
        fbx_release_ref(&models[i].node);
    free(models);
    fbx_release_ref(&default_material);

    if (failed) {
        if (out_failed)
            *out_failed = 1;
        fbx_release_ref(&scene_root);
        return NULL;
    }
    return scene_root;
}
