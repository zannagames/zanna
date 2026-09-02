//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_loader.inc
// Purpose: Coordinate FBX loading, extraction, rollback, and public load entry points.
// Key invariants:
//   - Binary and ASCII documents publish through one typed extraction pipeline.
//   - Any hard extraction, allocation, or aggregate-budget failure rolls back the whole asset.
// Ownership/Lifetime:
//   - The published FBX asset owns all retained runtime arrays and scene resources.
//   - Parser nodes, connection indexes, remaps, and channel bindings are load-local scratch state.
// Links: rt_fbx_loader.c, rt_fbx_loader_scene.inc, rt_fbx_loader_nodeanim.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_loader.inc
 * @brief Coordinates transactional FBX parsing, extraction, texture linking, and public
 *        load entry points.
 *
 * This internal implementation fragment owns the top-level load transaction. It reads
 * binary or ASCII FBX input, builds and validates the object/connection graph, extracts
 * runtime assets in dependency order, and rolls back all staged resources if any hard
 * phase fails. It also contains the bounded minimal ASCII helpers and recoverable-load
 * wrappers used by higher-level model import.
 *
 * Successful public loads return a newly allocated, reference-counted
 * @c rt_fbx_asset whose finalizer owns all published meshes, materials, skeletons,
 * animations, cameras, morph targets, and scene nodes. Parser trees and binding tables
 * remain transaction-local scratch storage.
 */

/*==========================================================================
 * Top-level FBX loader
 *=========================================================================*/

/// @brief GC finalizer for `rt_fbx_asset` — release every owned mesh / material / skeleton /
/// animation.
/// @param[in,out] obj Asset instance being finalized; NULL is accepted as a no-op.
static void rt_fbx_asset_finalize(void *obj) {
    rt_fbx_asset *fbx = (rt_fbx_asset *)obj;
    if (!fbx)
        return;
    fbx_asset_release_ref_array(&fbx->meshes, &fbx->mesh_count, &fbx->mesh_capacity);
    fbx_release_ref(&fbx->skeleton);
    fbx_asset_release_ref_array(&fbx->animations, &fbx->animation_count, &fbx->animation_capacity);
    fbx_asset_release_ref_array(
        &fbx->node_animations, &fbx->node_animation_count, &fbx->node_animation_capacity);
    fbx_asset_release_ref_array(&fbx->cameras, &fbx->camera_count, &fbx->camera_capacity);
    fbx_asset_release_ref_array(&fbx->materials, &fbx->material_count, &fbx->material_capacity);
    fbx_asset_release_ref_array(&fbx->morph_targets, &fbx->morph_count, &fbx->morph_capacity);
    fbx_release_ref(&fbx->scene_root);
}

/// @brief Ensure a double array holds at least @p needed elements, doubling capacity (min 64)
///   with overflow checks.
/// @param[in,out] values Address of the owned heap-array pointer.
/// @param[in,out] capacity Current element capacity, updated after reallocation.
/// @param needed Minimum required element capacity.
/// @return One on success, or zero on arithmetic overflow or allocation failure.
static int fbx_ascii_grow_double_array(double **values, size_t *capacity, size_t needed) {
    double *grown;
    size_t new_capacity;
    if (needed <= *capacity)
        return 1;
    new_capacity = *capacity ? *capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2u)
            return 0;
        new_capacity *= 2u;
    }
    if (new_capacity > SIZE_MAX / sizeof(**values))
        return 0;
    grown = (double *)realloc(*values, new_capacity * sizeof(**values));
    if (!grown)
        return 0;
    *values = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Ensure an int32 array holds at least @p needed elements, doubling capacity (min 64)
///   with overflow checks.
/// @param[in,out] values Address of the owned heap-array pointer.
/// @param[in,out] capacity Current element capacity, updated after reallocation.
/// @param needed Minimum required element capacity.
/// @return One on success, or zero on arithmetic overflow or allocation failure.
static int fbx_ascii_grow_i32_array(int32_t **values, size_t *capacity, size_t needed) {
    int32_t *grown;
    size_t new_capacity;
    if (needed <= *capacity)
        return 1;
    new_capacity = *capacity ? *capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2u)
            return 0;
        new_capacity *= 2u;
    }
    if (new_capacity > SIZE_MAX / sizeof(**values))
        return 0;
    grown = (int32_t *)realloc(*values, new_capacity * sizeof(**values));
    if (!grown)
        return 0;
    *values = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Return 10 raised to an integer exponent without calling libm `pow`.
/// @details ASCII FBX imports parse dense numeric arrays. This integer-only exponent scaler avoids
/// dispatching to the general-purpose `pow` implementation for each token while preserving the
/// caller's existing finite-result validation.
/// @param exponent Signed base-10 exponent to evaluate.
/// @return A double scale factor approximately equal to `10^exponent`.
static double fbx_ascii_pow10_i64(int64_t exponent) {
    uint64_t remaining;
    double base = exponent < 0 ? 0.1 : 10.0;
    double scale = 1.0;
    if (exponent < 0)
        remaining = (uint64_t)(-exponent);
    else
        remaining = (uint64_t)exponent;
    while (remaining != 0u) {
        if ((remaining & 1u) != 0u)
            scale *= base;
        remaining >>= 1u;
        if (remaining != 0u)
            base *= base;
    }
    return scale;
}

/// @brief Parse one finite ASCII-FBX double with '.' decimal semantics, independent of locale.
/// @details FBX text files are ASCII interchange data and always use `.` for fractional values.
///          Calling `strtod` directly makes parsing depend on the process locale, so this helper
///          accepts the numeric grammar emitted by FBX exporters and advances @p out_end to the
///          first unconsumed byte. The optional @p limit bounds scanning to the current line.
/// @param p First byte of the candidate number.
/// @param limit Optional exclusive end pointer; pass NULL to scan until a non-number byte.
/// @param out_end Receives the first unconsumed byte on success.
/// @param out Receives the parsed finite double.
/// @return 1 when a finite number was parsed, otherwise 0.
static int fbx_ascii_parse_double_span(const char *p,
                                       const char *limit,
                                       const char **out_end,
                                       double *out) {
    const char *s = p;
    double value = 0.0;
    int sign = 1;
    int digits = 0;
    int frac_digits = 0;
    int exp_sign = 1;
    int exp_value = 0;
    int exp_digits = 0;
    double result;
    if (!s || !out_end || !out)
        return 0;
    while ((!limit || s < limit) && isspace((unsigned char)*s))
        s++;
    if ((!limit || s < limit) && (*s == '+' || *s == '-')) {
        if (*s == '-')
            sign = -1;
        s++;
    }
    while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
        value = value * 10.0 + (double)(*s - '0');
        digits++;
        s++;
    }
    if ((!limit || s < limit) && *s == '.') {
        s++;
        while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
            value = value * 10.0 + (double)(*s - '0');
            digits++;
            frac_digits++;
            s++;
        }
    }
    if (digits == 0)
        return 0;
    if ((!limit || s < limit) && (*s == 'e' || *s == 'E')) {
        const char *exp_start = s;
        s++;
        if ((!limit || s < limit) && (*s == '+' || *s == '-')) {
            if (*s == '-')
                exp_sign = -1;
            s++;
        }
        while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
            if (exp_digits >= 4)
                return 0;
            exp_value = exp_value * 10 + (*s - '0');
            if (exp_value > 512 + frac_digits)
                return 0;
            exp_digits++;
            s++;
        }
        if (exp_digits == 0)
            s = exp_start;
    }
    result = (double)sign * value *
             fbx_ascii_pow10_i64((int64_t)exp_sign * (int64_t)exp_value - (int64_t)frac_digits);
    if (!isfinite(result))
        return 0;
    *out_end = s;
    *out = result;
    return 1;
}

/// @brief Locate the "a:" value payload of the ASCII-FBX array node named @p key within
///   @p text.
/// @param text NUL-terminated ASCII FBX document.
/// @param key Array-node label to locate.
/// @return Borrowed pointer immediately after the node's `a:` marker, or NULL when
///         the node or payload marker is absent.
static const char *fbx_ascii_array_payload(const char *text, const char *key) {
    const char *p = text ? strstr(text, key) : NULL;
    if (!p)
        return NULL;
    p = strstr(p, "a:");
    return p ? p + 2 : NULL;
}

/// @brief Parse a comma-separated list of doubles from an ASCII-FBX "a:" payload up to the
///   closing '}', rejecting non-finite values.
/// @param payload Borrowed pointer to the first byte after an `a:` marker.
/// @param[out] out Receives the newly allocated value array, or NULL on failure.
/// @param[out] out_count Receives the parsed scalar count, or zero on failure.
/// @return One when at least one value is parsed, or zero on invalid input, parse
///         failure, or allocation failure.
static int fbx_ascii_parse_double_array(const char *payload, double **out, size_t *out_count) {
    double *values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    const char *p = payload;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    if (!payload || !out || !out_count)
        return 0;
    while (*p && *p != '}') {
        const char *end = NULL;
        double value;
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        if (*p == '}')
            break;
        if (!fbx_ascii_parse_double_span(p, NULL, &end, &value) || end == p) {
            free(values);
            return 0;
        }
        if (!fbx_ascii_grow_double_array(&values, &capacity, count + 1)) {
            free(values);
            return 0;
        }
        values[count++] = value;
        p = end;
    }
    *out = values;
    *out_count = count;
    return count > 0;
}

/// @brief Parse a comma-separated list of int32s from an ASCII-FBX "a:" payload up to the
///   closing '}', rejecting out-of-range values.
/// @param payload Borrowed pointer to the first byte after an `a:` marker.
/// @param[out] out Receives the newly allocated integer array, or NULL on failure.
/// @param[out] out_count Receives the parsed integer count, or zero on failure.
/// @return One when at least one value is parsed, or zero on invalid input, parse
///         failure, range error, or allocation failure.
static int fbx_ascii_parse_i32_array(const char *payload, int32_t **out, size_t *out_count) {
    int32_t *values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    const char *p = payload;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    if (!payload || !out || !out_count)
        return 0;
    while (*p && *p != '}') {
        char *end = NULL;
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        if (*p == '}')
            break;
        long value = strtol(p, &end, 10);
        if (end == p || value < INT32_MIN || value > INT32_MAX) {
            free(values);
            return 0;
        }
        if (!fbx_ascii_grow_i32_array(&values, &capacity, count + 1)) {
            free(values);
            return 0;
        }
        values[count++] = (int32_t)value;
        p = end;
    }
    *out = values;
    *out_count = count;
    return count > 0;
}

/// @brief Resolve an optional ASCII-FBX per-corner/per-control-point index.
/// @details ASCII fallback parsing does not build a full LayerElement tree, so this helper uses
///          exporter-common heuristics: explicit index arrays win; arrays sized to polygon-vertex
///          count map by polygon corner; otherwise values map by control-point index.
/// @param direct_count Number of direct elements in the attribute array.
/// @param element_components Components per direct element.
/// @param explicit_indices Optional per-polygon-vertex index array.
/// @param explicit_index_count Count of @p explicit_indices.
/// @param polygon_vertex_idx Running polygon-vertex index.
/// @param control_index Control point index.
/// @param polygon_vertex_total Total polygon-vertex count from `PolygonVertexIndex`.
/// @return Direct element index, or -1 when unavailable.
static int32_t fbx_ascii_resolve_attribute_index(size_t direct_count,
                                                 size_t element_components,
                                                 const int32_t *explicit_indices,
                                                 size_t explicit_index_count,
                                                 size_t polygon_vertex_idx,
                                                 int32_t control_index,
                                                 size_t polygon_vertex_total) {
    size_t element_count = element_components > 0 ? direct_count / element_components : 0;
    int32_t idx;
    if (element_count == 0)
        return -1;
    if (explicit_indices && polygon_vertex_idx < explicit_index_count) {
        idx = explicit_indices[polygon_vertex_idx];
        return idx >= 0 && (size_t)idx < element_count ? idx : -1;
    }
    if (element_count == polygon_vertex_total)
        return polygon_vertex_idx <= (size_t)INT32_MAX ? (int32_t)polygon_vertex_idx : -1;
    return control_index >= 0 && (size_t)control_index < element_count ? control_index : -1;
}

/// @brief Build a Mesh3D from ASCII-FBX vertex positions and polygon-vertex indices.
/// @details FBX marks each polygon's final index with a bitwise complement (a negative value);
///   this triangulates arbitrary-sized polygons, skips out-of-range indices, and consumes optional
///   ASCII normals, UVs, and vertex colors using direct, indexed, or inferred mapping.
/// @param positions Flattened XYZ control-point array.
/// @param position_count Number of scalar values in @p positions.
/// @param indices Polygon-vertex control indices with complemented polygon terminators.
/// @param index_count Number of entries in @p indices.
/// @param normals Optional flattened XYZ direct normal array.
/// @param normal_count Number of scalar values in @p normals.
/// @param uvs Optional flattened UV direct texture-coordinate array.
/// @param uv_count Number of scalar values in @p uvs.
/// @param uv_indices Optional per-corner indirection into the UV direct array.
/// @param uv_index_count Number of entries in @p uv_indices.
/// @param colors Optional flattened RGBA direct vertex-color array.
/// @param color_count Number of scalar values in @p colors.
/// @param color_indices Optional per-corner indirection into the color direct array.
/// @param color_index_count Number of entries in @p color_indices.
/// @return The new mesh object, or NULL on malformed input or allocation failure.
static void *fbx_ascii_build_mesh(const double *positions,
                                  size_t position_count,
                                  const int32_t *indices,
                                  size_t index_count,
                                  const double *normals,
                                  size_t normal_count,
                                  const double *uvs,
                                  size_t uv_count,
                                  const int32_t *uv_indices,
                                  size_t uv_index_count,
                                  const double *colors,
                                  size_t color_count,
                                  const int32_t *color_indices,
                                  size_t color_index_count) {
    int32_t polygon_stack[16];
    int32_t *polygon = polygon_stack;
    int32_t poly_count = 0;
    int32_t poly_capacity = (int32_t)(sizeof(polygon_stack) / sizeof(polygon_stack[0]));
    int polygon_invalid = 0;
    size_t polygon_vertex_idx = 0;
    void *mesh;
    if (!positions || !indices || position_count % 3u != 0)
        return NULL;
    mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    for (size_t i = 0; i < index_count; i++) {
        int32_t raw = indices[i];
        int end = raw < 0;
        int32_t vi = end ? ~raw : raw;
        int32_t emitted;
        if (vi < 0 || (size_t)vi >= position_count / 3u) {
            polygon_invalid = 1;
            polygon_vertex_idx++;
            if (end) {
                poly_count = 0;
                polygon_invalid = 0;
            }
            continue;
        }
        if (poly_count >= poly_capacity) {
            int32_t new_capacity = poly_capacity * 2;
            int32_t *grown;
            if (poly_capacity > INT32_MAX / 2 || (size_t)new_capacity > SIZE_MAX / sizeof(*polygon))
                goto fail;
            grown = (int32_t *)malloc((size_t)new_capacity * sizeof(*polygon));
            if (!grown)
                goto fail;
            memcpy(grown, polygon, (size_t)poly_count * sizeof(*polygon));
            if (polygon != polygon_stack)
                free(polygon);
            polygon = grown;
            poly_capacity = new_capacity;
        }
        emitted = (int32_t)((rt_mesh3d *)mesh)->vertex_count;
        {
            double px = positions[(size_t)vi * 3u + 0];
            double py = positions[(size_t)vi * 3u + 1];
            double pz = positions[(size_t)vi * 3u + 2];
            double nx = 0.0;
            double ny = 1.0;
            double nz = 0.0;
            double u = 0.0;
            double v = 0.0;
            int32_t ni = fbx_ascii_resolve_attribute_index(
                normal_count, 3u, NULL, 0, polygon_vertex_idx, vi, index_count);
            int32_t ui = fbx_ascii_resolve_attribute_index(
                uv_count, 2u, uv_indices, uv_index_count, polygon_vertex_idx, vi, index_count);
            if (!fbx_sanitize_position3(&px, &py, &pz)) {
                polygon_invalid = 1;
                polygon_vertex_idx++;
                if (end) {
                    poly_count = 0;
                    polygon_invalid = 0;
                }
                continue;
            }
            if (normals && ni >= 0) {
                nx = normals[(size_t)ni * 3u + 0];
                ny = normals[(size_t)ni * 3u + 1];
                nz = normals[(size_t)ni * 3u + 2];
                fbx_sanitize_normal3(&nx, &ny, &nz);
            }
            if (uvs && ui >= 0) {
                u = fbx_clamp_abs_or(uvs[(size_t)ui * 2u + 0], 0.0, FBX_UV_ABS_MAX);
                v = fbx_clamp_abs_or(1.0 - uvs[(size_t)ui * 2u + 1], 0.0, FBX_UV_ABS_MAX);
            }
            rt_mesh3d_add_vertex(mesh, px, py, pz, nx, ny, nz, u, v);
        }
        if (((rt_mesh3d *)mesh)->build_failed)
            goto fail;
        if (colors && emitted >= 0 && emitted < (int32_t)((rt_mesh3d *)mesh)->vertex_count) {
            int32_t ci = fbx_ascii_resolve_attribute_index(color_count,
                                                           4u,
                                                           color_indices,
                                                           color_index_count,
                                                           polygon_vertex_idx,
                                                           vi,
                                                           index_count);
            if (ci >= 0) {
                ((rt_mesh3d *)mesh)->vertices[emitted].color[0] =
                    (float)fbx_clamp_double(colors[(size_t)ci * 4u + 0], 0.0, 1.0, 1.0);
                ((rt_mesh3d *)mesh)->vertices[emitted].color[1] =
                    (float)fbx_clamp_double(colors[(size_t)ci * 4u + 1], 0.0, 1.0, 1.0);
                ((rt_mesh3d *)mesh)->vertices[emitted].color[2] =
                    (float)fbx_clamp_double(colors[(size_t)ci * 4u + 2], 0.0, 1.0, 1.0);
                ((rt_mesh3d *)mesh)->vertices[emitted].color[3] =
                    (float)fbx_clamp_double(colors[(size_t)ci * 4u + 3], 0.0, 1.0, 1.0);
            }
        }
        polygon[poly_count++] = emitted;
        polygon_vertex_idx++;
        if (end) {
            if (!polygon_invalid && !fbx_emit_polygon_triangles(mesh, polygon, poly_count))
                goto fail;
            poly_count = 0;
            polygon_invalid = 0;
        }
    }
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    if (((rt_mesh3d *)mesh)->index_count < 3u) {
        if (polygon != polygon_stack)
            free(polygon);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }
    if (polygon != polygon_stack)
        free(polygon);
    if (!normals || normal_count < 3u)
        rt_mesh3d_recalc_normals(mesh);
    return mesh;
fail:
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    if (polygon != polygon_stack)
        free(polygon);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    return NULL;
}

/// @brief Advance to the payload after the Nth comma in an ASCII FBX property line.
/// @param p Start pointer inside a property line.
/// @param comma_count Number of commas to skip.
/// @return Pointer after the requested comma count, or NULL when the line ends first.
static const char *fbx_ascii_after_commas(const char *p, int comma_count) {
    if (!p || comma_count < 0)
        return NULL;
    while (*p && comma_count > 0) {
        if (*p == ',')
            comma_count--;
        p++;
        if (*p == '\n' || *p == '\r')
            return NULL;
    }
    return comma_count == 0 ? p : NULL;
}

/// @brief Return the first line terminator at or after @p p without importing `strpbrk`.
/// @details Native-linked Zanna runtimes use an explicit import allow-list; keeping this tiny scan
///          local avoids adding another C library dependency solely for ASCII FBX fallback parsing.
/// @param p Start of the text span to scan.
/// @return Pointer to '\r' or '\n', or NULL when the string ends first.
static const char *fbx_ascii_line_end(const char *p) {
    if (!p)
        return NULL;
    while (*p) {
        if (*p == '\r' || *p == '\n')
            return p;
        p++;
    }
    return NULL;
}

/// @brief Parse an ASCII-FBX `P: "Name", ...` vector3 property.
/// @param text ASCII FBX document.
/// @param property_name Property name to find.
/// @param out Receives three doubles when found.
/// @return Non-zero when a finite vector was parsed.
static int fbx_ascii_parse_property_vec3(const char *text,
                                         const char *property_name,
                                         double out[3]) {
    const char *p = text;
    if (!text || !property_name || !out)
        return 0;
    while ((p = strstr(p, "P:")) != NULL) {
        const char *line_end = fbx_ascii_line_end(p);
        const char *name = strstr(p, property_name);
        if (name && (!line_end || name < line_end)) {
            const char *nums = fbx_ascii_after_commas(p, 4);
            const char *end0 = NULL;
            const char *end1 = NULL;
            const char *end2 = NULL;
            if (!nums || (line_end && nums > line_end))
                return 0;
            if (!fbx_ascii_parse_double_span(nums, line_end, &end0, &out[0]))
                return 0;
            if (!fbx_ascii_parse_double_span(end0 + (*end0 == ','), line_end, &end1, &out[1]))
                return 0;
            if (!fbx_ascii_parse_double_span(end1 + (*end1 == ','), line_end, &end2, &out[2]))
                return 0;
            return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
        }
        p += 2;
    }
    return 0;
}

/// @brief Parse an ASCII-FBX `P: "Name", ...` scalar property.
/// @param text ASCII FBX document.
/// @param property_name Property name to find.
/// @param out Receives the parsed scalar when found.
/// @return Non-zero when a finite scalar was parsed.
static int fbx_ascii_parse_property_scalar(const char *text,
                                           const char *property_name,
                                           double *out) {
    const char *p = text;
    if (!text || !property_name || !out)
        return 0;
    while ((p = strstr(p, "P:")) != NULL) {
        const char *line_end = fbx_ascii_line_end(p);
        const char *name = strstr(p, property_name);
        if (name && (!line_end || name < line_end)) {
            const char *num = fbx_ascii_after_commas(p, 4);
            const char *end = NULL;
            if (!num || (line_end && num > line_end))
                return 0;
            return fbx_ascii_parse_double_span(num, line_end, &end, out) && end != num &&
                   isfinite(*out);
        }
        p += 2;
    }
    return 0;
}

/// @brief Apply common ASCII-FBX material properties to a runtime material.
/// @details The fallback parser does not build the full Material node graph, but it can still read
///          PBR-relevant scalar/vector properties from the text form used by Autodesk exporters.
/// @param text ASCII FBX document.
/// @param material Runtime material to mutate.
static void fbx_ascii_apply_material_properties(const char *text, void *material) {
    double color[3];
    double scalar;
    if (!text || !material)
        return;
    if (fbx_ascii_parse_property_vec3(text, "BaseColor", color) ||
        fbx_ascii_parse_property_vec3(text, "DiffuseColor", color) ||
        fbx_ascii_parse_property_vec3(text, "Albedo", color)) {
        rt_material3d_set_color(material,
                                fbx_clamp_double(color[0], 0.0, 1.0, 1.0),
                                fbx_clamp_double(color[1], 0.0, 1.0, 1.0),
                                fbx_clamp_double(color[2], 0.0, 1.0, 1.0));
    }
    if (fbx_ascii_parse_property_scalar(text, "Opacity", &scalar) ||
        fbx_ascii_parse_property_scalar(text, "Alpha", &scalar)) {
        scalar = fbx_clamp_double(scalar, 0.0, 1.0, 1.0);
        rt_material3d_set_alpha(material, scalar);
        if (scalar < 1.0)
            rt_material3d_set_alpha_mode(material, 2);
    }
    if (fbx_ascii_parse_property_vec3(text, "EmissiveColor", color) ||
        fbx_ascii_parse_property_vec3(text, "EmissionColor", color)) {
        rt_material3d_set_emissive_color(material,
                                         fbx_clamp_double(color[0], 0.0, 1.0, 0.0),
                                         fbx_clamp_double(color[1], 0.0, 1.0, 0.0),
                                         fbx_clamp_double(color[2], 0.0, 1.0, 0.0));
    }
    if (fbx_ascii_parse_property_scalar(text, "EmissiveFactor", &scalar) ||
        fbx_ascii_parse_property_scalar(text, "EmissionFactor", &scalar))
        rt_material3d_set_emissive_intensity(material,
                                             fbx_clamp_double(scalar, 0.0, 1000000.0, 0.0));
    if (fbx_ascii_parse_property_scalar(text, "Metallic", &scalar) ||
        fbx_ascii_parse_property_scalar(text, "Metalness", &scalar))
        rt_material3d_set_metallic(material, fbx_clamp_double(scalar, 0.0, 1.0, 0.0));
    if (fbx_ascii_parse_property_scalar(text, "Roughness", &scalar))
        rt_material3d_set_roughness(material, fbx_clamp_double(scalar, 0.0, 1.0, 0.5));
}

/// @brief Extract a readable model name from an ASCII-FBX `Model:` line.
/// @param text ASCII FBX document.
/// @param out Name output buffer.
/// @param out_size Size of @p out in bytes.
static void fbx_ascii_extract_model_name(const char *text, char *out, size_t out_size) {
    const char *model;
    const char *first_quote;
    const char *second_quote;
    char raw[128];
    size_t len;
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    model = text ? strstr(text, "Model:") : NULL;
    if (!model)
        return;
    first_quote = strchr(model, '"');
    if (!first_quote)
        return;
    second_quote = strchr(first_quote + 1, '"');
    if (!second_quote)
        return;
    len = (size_t)(second_quote - first_quote - 1);
    if (len >= sizeof(raw))
        len = sizeof(raw) - 1;
    memcpy(raw, first_quote + 1, len);
    raw[len] = '\0';
    fbx_decode_object_name(raw, out, out_size);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
/// @brief Build a minimal single-mesh runtime asset from an ASCII FBX document.
/// @details Parses required position/index arrays plus optional normals, UVs, colors,
///          material properties, and the first model name. Every temporary array and
///          partially built runtime object is released transactionally on failure.
/// @param text NUL-terminated ASCII FBX document.
/// @return Newly allocated FBX asset on success, or NULL when required arrays are
///         missing/malformed or any object/allocation step fails.
static void *fbx_load_ascii_minimal(const char *text) {
    double *positions = NULL;
    int32_t *indices = NULL;
    double *normals = NULL;
    double *uvs = NULL;
    int32_t *uv_indices = NULL;
    double *colors = NULL;
    int32_t *color_indices = NULL;
    size_t position_count = 0;
    size_t index_count = 0;
    size_t normal_count = 0;
    size_t uv_count = 0;
    size_t uv_index_count = 0;
    size_t color_count = 0;
    size_t color_index_count = 0;
    char model_name[128];
    void *mesh = NULL;
    void *material = NULL;
    void *scene_root = NULL;
    void *mesh_node = NULL;
    rt_fbx_asset *asset = NULL;
    if (!fbx_ascii_parse_double_array(
            fbx_ascii_array_payload(text, "Vertices:"), &positions, &position_count) ||
        !fbx_ascii_parse_i32_array(
            fbx_ascii_array_payload(text, "PolygonVertexIndex:"), &indices, &index_count))
        goto fail;
    (void)fbx_ascii_parse_double_array(
        fbx_ascii_array_payload(text, "Normals:"), &normals, &normal_count);
    (void)fbx_ascii_parse_double_array(fbx_ascii_array_payload(text, "UV:"), &uvs, &uv_count);
    (void)fbx_ascii_parse_i32_array(
        fbx_ascii_array_payload(text, "UVIndex:"), &uv_indices, &uv_index_count);
    (void)fbx_ascii_parse_double_array(
        fbx_ascii_array_payload(text, "Colors:"), &colors, &color_count);
    (void)fbx_ascii_parse_i32_array(
        fbx_ascii_array_payload(text, "ColorIndex:"), &color_indices, &color_index_count);
    mesh = fbx_ascii_build_mesh(positions,
                                position_count,
                                indices,
                                index_count,
                                normals,
                                normal_count,
                                uvs,
                                uv_count,
                                uv_indices,
                                uv_index_count,
                                colors,
                                color_count,
                                color_indices,
                                color_index_count);
    material = rt_material3d_new();
    scene_root = rt_scene_node3d_new();
    mesh_node = rt_scene_node3d_new();
    asset =
        (rt_fbx_asset *)rt_obj_new_i64(RT_G3D_FBX_ASSET_CLASS_ID, (int64_t)sizeof(rt_fbx_asset));
    if (!mesh || !material || !scene_root || !mesh_node || !asset)
        goto fail;
    memset(asset, 0, sizeof(*asset));
    rt_obj_set_finalizer(asset, rt_fbx_asset_finalize);
    asset->meshes = (void **)calloc(1, sizeof(void *));
    asset->materials = (void **)calloc(1, sizeof(void *));
    if (!asset->meshes || !asset->materials)
        goto fail;
    asset->meshes[0] = mesh;
    asset->mesh_count = 1;
    asset->mesh_capacity = 1;
    asset->materials[0] = material;
    asset->material_count = 1;
    asset->material_capacity = 1;
    fbx_ascii_apply_material_properties(text, material);
    fbx_ascii_extract_model_name(text, model_name, sizeof(model_name));
    {
        rt_string name = rt_const_cstr(model_name[0] ? model_name : "mesh_0");
        rt_scene_node3d_set_name(mesh_node, name);
        rt_string_unref(name);
    }
    rt_scene_node3d_set_mesh(mesh_node, mesh);
    rt_scene_node3d_set_material(mesh_node, material);
    if (!rt_scene_node3d_try_add_child(scene_root, mesh_node))
        goto fail;
    asset->scene_root = scene_root;
    fbx_release_ref(&mesh_node);
    free(positions);
    free(indices);
    free(normals);
    free(uvs);
    free(uv_indices);
    free(colors);
    free(color_indices);
    return asset;
fail:
    free(positions);
    free(indices);
    free(normals);
    free(uvs);
    free(uv_indices);
    free(colors);
    free(color_indices);
    fbx_release_ref(&mesh_node);
    fbx_release_ref(&scene_root);
    fbx_release_ref(&material);
    fbx_release_ref(&mesh);
    fbx_release_ref((void **)&asset);
    return NULL;
}

/// @brief Obtain the raw embedded image payload from an FBX `Video` node.
/// @param video Video node containing an optional raw `Content` child.
/// @param[out] out_len Optional destination for the raw payload length; initialized to zero.
/// @return Borrowed pointer into the parsed node tree, or NULL when raw content is absent.
static const uint8_t *fbx_video_content_bytes(fbx_node_t *video, uint32_t *out_len) {
    fbx_node_t *content;
    if (out_len)
        *out_len = 0;
    if (!video)
        return NULL;
    content = fbx_find_child(video, "Content");
    if (!content || content->prop_count < 1)
        return NULL;
    if (content->props[0].type == 'R' && content->props[0].v.raw.data) {
        if (out_len)
            *out_len = content->props[0].v.raw.len;
        return content->props[0].v.raw.data;
    }
    return NULL;
}

/// @brief Return true when an FBX property/child name is likely to contain a media filename.
/// @details Different exporters use `RelativeFilename`, `Filename`, `OriginalFilename`, `Path`,
///          `URI`, or `TextureName` for the same concept. Matching these aliases improves both
///          external texture loading and embedded payload codec hints.
/// @param name Candidate property or child node name.
/// @return Non-zero for known filename/path aliases.
static int fbx_media_filename_alias(const char *name) {
    return fbx_ascii_equals_i(name, "RelativeFilename") || fbx_ascii_equals_i(name, "FileName") ||
           fbx_ascii_equals_i(name, "Filename") || fbx_ascii_equals_i(name, "OriginalFilename") ||
           fbx_ascii_equals_i(name, "OriginalFileName") || fbx_ascii_equals_i(name, "FullName") ||
           fbx_ascii_equals_i(name, "Media") || fbx_ascii_equals_i(name, "Path") ||
           fbx_ascii_equals_i(name, "URI") || fbx_ascii_equals_i(name, "URL") ||
           fbx_ascii_equals_i(name, "Url") || fbx_ascii_equals_i(name, "TextureName");
}

/// @brief Best-effort texture/media filename for an FBX node.
/// @details Checks `Properties70` P-nodes and direct child nodes using the broad alias list in
///          `fbx_media_filename_alias`. The returned pointer is borrowed from the FBX node tree
///          and remains valid until that tree is freed.
/// @param node Texture or Video node.
/// @return Borrowed filename/path hint, or an empty string when none is present.
static const char *fbx_node_media_filename_hint(fbx_node_t *node) {
    fbx_node_t *p70;
    if (!node)
        return "";
    p70 = fbx_find_child(node, "Properties70");
    if (p70) {
        for (int32_t pi = 0; pi < p70->child_count; pi++) {
            fbx_node_t *p = &p70->children[pi];
            const char *pname;
            if (strcmp(p->name, "P") != 0 || p->prop_count < 5)
                continue;
            pname = fbx_prop_str(p, 0);
            if (fbx_media_filename_alias(pname)) {
                const char *value = fbx_prop_str(p, 4);
                if (value && *value)
                    return value;
            }
        }
    }
    for (int32_t i = 0; i < node->child_count; i++) {
        fbx_node_t *child = &node->children[i];
        if (fbx_media_filename_alias(child->name) && child->prop_count > 0 &&
            *fbx_prop_str(child, 0))
            return fbx_prop_str(child, 0);
    }
    return "";
}

/// @brief Find the Video node connected to texture @p texture_id and decode its embedded
///   image content into a Pixels object, picking the codec from the Video filename hint (or
///   @p texture_hint / @p fallback_hint).
/// @param objects Parsed FBX `Objects` node used to resolve connected Video objects.
/// @param ct Connection table linking Texture and Video object identifiers.
/// @param texture_id Identifier of the Texture object being resolved.
/// @param texture_hint Preferred filename/path hint from the Texture node.
/// @param fallback_hint Secondary codec hint attempted when the preferred hint fails.
/// @return Newly allocated runtime Pixels object on successful decode, or NULL when
///         no connected embedded payload can be decoded.
static void *fbx_try_decode_embedded_texture(fbx_node_t *objects,
                                             const fbx_conn_table_t *ct,
                                             int64_t texture_id,
                                             const char *texture_hint,
                                             const char *fallback_hint) {
    if (!objects || !ct)
        return NULL;
    for (int32_t ci = fbx_connections_first_for_parent(ct, texture_id); ci >= 0;
         ci = fbx_connections_next_for_parent(ct, ci)) {
        fbx_node_t *video;
        uint32_t content_len = 0;
        const uint8_t *content;
        const char *hint;
        void *pixels;
        video = fbx_find_object_by_id(objects, ct->entries[ci].child_id);
        if (!video || strcmp(video->name, "Video") != 0)
            continue;
        content = fbx_video_content_bytes(video, &content_len);
        if (!content || content_len == 0)
            continue;
        hint = fbx_node_media_filename_hint(video);
        pixels =
            fbx_decode_texture_payload(hint && *hint ? hint : texture_hint, content, content_len);
        if (!pixels && fallback_hint && fallback_hint != texture_hint)
            pixels = fbx_decode_texture_payload(fallback_hint, content, content_len);
        if (pixels)
            return pixels;
    }
    return NULL;
}

/// @brief Test whether a texture connection property denotes a normal or bump map.
/// @param prop Connection property name.
/// @return Non-zero for recognized normal/bump aliases; otherwise zero.
static int fbx_texture_prop_is_normal(const char *prop) {
    return fbx_ascii_streq_ignore_case(prop, "NormalMap") ||
           fbx_ascii_streq_ignore_case(prop, "Bump") ||
           fbx_ascii_streq_ignore_case(prop, "normalCamera") ||
           fbx_ascii_contains_i(prop, "normal") || fbx_ascii_contains_i(prop, "bump");
}

/// @brief Test whether a texture connection property denotes a specular map.
/// @param prop Connection property name.
/// @return Non-zero for recognized specular aliases; otherwise zero.
static int fbx_texture_prop_is_specular(const char *prop) {
    return fbx_ascii_streq_ignore_case(prop, "SpecularColor") ||
           fbx_ascii_streq_ignore_case(prop, "SpecularFactor") ||
           fbx_ascii_contains_i(prop, "specular");
}

/// @brief Test whether a texture connection property denotes an emissive map.
/// @param prop Connection property name.
/// @return Non-zero for recognized emission aliases; otherwise zero.
static int fbx_texture_prop_is_emissive(const char *prop) {
    return fbx_ascii_streq_ignore_case(prop, "EmissiveColor") ||
           fbx_ascii_streq_ignore_case(prop, "EmissiveFactor") ||
           fbx_ascii_contains_i(prop, "emissive") || fbx_ascii_contains_i(prop, "emission");
}

/// @brief Test whether a texture property denotes metallic or roughness data.
/// @param prop Connection property name.
/// @return Non-zero for recognized metallic/metalness/roughness aliases; otherwise zero.
static int fbx_texture_prop_is_metallic_roughness(const char *prop) {
    return fbx_ascii_contains_i(prop, "metallic") || fbx_ascii_contains_i(prop, "metalness") ||
           fbx_ascii_contains_i(prop, "roughness") || fbx_ascii_contains_i(prop, "rough");
}

/// @brief Test whether a texture connection property denotes ambient occlusion.
/// @param prop Connection property name.
/// @return Non-zero for recognized ambient-occlusion aliases; otherwise zero.
static int fbx_texture_prop_is_ao(const char *prop) {
    return fbx_ascii_contains_i(prop, "ambientocclusion") ||
           fbx_ascii_contains_i(prop, "occlusion") || fbx_ascii_streq_ignore_case(prop, "AO") ||
           fbx_ascii_contains_i(prop, "ambient_occlusion");
}

/// @brief Test whether a texture property denotes diffuse or base-color data.
/// @param prop Connection property name; NULL or empty is treated as diffuse.
/// @return Non-zero for absent names and recognized diffuse/base-color aliases.
static int fbx_texture_prop_is_diffuse(const char *prop) {
    return !prop || *prop == '\0' || fbx_ascii_streq_ignore_case(prop, "DiffuseColor") ||
           fbx_ascii_streq_ignore_case(prop, "DiffuseFactor") ||
           fbx_ascii_streq_ignore_case(prop, "BaseColor") ||
           fbx_ascii_streq_ignore_case(prop, "Albedo") || fbx_ascii_contains_i(prop, "basecolor") ||
           fbx_ascii_contains_i(prop, "base_color") || fbx_ascii_contains_i(prop, "albedo") ||
           fbx_ascii_contains_i(prop, "diffuse");
}

/// @brief Assign an FBX texture as the material base-color slot.
/// @details Explicit diffuse/base-color properties and unrecognized properties both fall back to
///          the same base-color slot. Keeping this in one helper avoids duplicate branches while
///          preserving the diffuse predicate for documentation and future slot-specific handling.
/// @param[in,out] mat Runtime material receiving the texture reference.
/// @param pixels Decoded runtime Pixels object to assign.
/// @param prop_name FBX connection property name, used to retain diffuse classification behavior.
static void fbx_assign_base_color_texture(void *mat, void *pixels, const char *prop_name) {
    (void)fbx_texture_prop_is_diffuse(prop_name);
    rt_material3d_set_texture(mat, pixels);
}

/// @brief Resolve and attach textures to the asset's materials: for each Texture node, load
///   its image (external file relative to @p cpath, or embedded Video content) and assign it
///   to the connected material's slots per @p material_bindings.
/// @param[in,out] asset Asset whose material array bounds the linking pass.
/// @param objects Parsed FBX `Objects` node containing Texture and Video objects.
/// @param cpath Source FBX path or explicit texture-base path for relative resolution.
/// @param ct Connection table used to link textures, videos, and materials.
/// @param material_bindings Object-ID to runtime-material binding array.
/// @param material_binding_count Number of entries in @p material_bindings.
static void fbx_load_link_textures(rt_fbx_asset *asset,
                                   fbx_node_t *objects,
                                   const char *cpath,
                                   fbx_conn_table_t ct,
                                   fbx_material_binding_t *material_bindings,
                                   int32_t material_binding_count) {
    if (asset && objects) {
        int32_t material_count =
            fbx_asset_safe_count(asset->materials, asset->material_count, asset->material_capacity);
        if (material_count <= 0)
            return;
        if (!material_bindings || material_binding_count < 0)
            material_binding_count = 0;
        // Collect Texture nodes and their filenames
        for (int32_t i = 0; i < objects->child_count; i++) {
            fbx_node_t *obj = &objects->children[i];
            const char *rel_filename;
            const char *filename;
            void *pixels;
            int64_t tex_id;
            if (strcmp(obj->name, "Texture") != 0)
                continue;
            if (obj->prop_count < 1)
                continue;
            tex_id = fbx_prop_i64(obj, 0);
            rel_filename = fbx_node_media_filename_hint(obj);
            filename = rel_filename;

            // Load texture via auto-detect loader
            pixels = fbx_try_load_texture_path(cpath, rel_filename);
            if (!pixels && filename && *filename &&
                (!rel_filename || !*rel_filename || strcmp(filename, rel_filename) != 0))
                pixels = fbx_try_load_texture_path(cpath, filename);
            if (!pixels)
                pixels =
                    fbx_try_decode_embedded_texture(objects, &ct, tex_id, rel_filename, filename);
            if (!pixels) {
                const char *texture_name =
                    (rel_filename && *rel_filename) ? rel_filename : filename;
                rt_asset_error_add_warningf("FBX texture '%s' not loaded: %s",
                                            texture_name ? texture_name : "(embedded)",
                                            rt_asset_error_get_message()[0]
                                                ? rt_asset_error_get_message()
                                                : "content unavailable");
                continue;
            }

            // Find which material this texture connects to via Connections
            for (int32_t ci = fbx_connections_first_for_child(&ct, tex_id); ci >= 0;
                 ci = fbx_connections_next_for_child(&ct, ci)) {
                int64_t mat_id = ct.entries[ci].parent_id;
                const char *prop_name = ct.entries[ci].prop;

                void *mat = NULL;
                for (int32_t mi = 0; mi < material_binding_count; mi++) {
                    if (material_bindings[mi].id == mat_id) {
                        mat = material_bindings[mi].material;
                        break;
                    }
                }
                if (!mat)
                    continue;

                // Assign based on property name in Connection
                if (fbx_texture_prop_is_normal(prop_name))
                    rt_material3d_set_normal_map(mat, pixels);
                else if (fbx_texture_prop_is_specular(prop_name))
                    rt_material3d_set_specular_map(mat, pixels);
                else if (fbx_texture_prop_is_emissive(prop_name))
                    rt_material3d_set_emissive_map(mat, pixels);
                else if (fbx_texture_prop_is_metallic_roughness(prop_name))
                    rt_material3d_set_metallic_roughness_map(mat, pixels);
                else if (fbx_texture_prop_is_ao(prop_name))
                    rt_material3d_set_ao_map(mat, pixels);
                else
                    fbx_assign_base_color_texture(mat, pixels, prop_name);
                break;
            }
            fbx_release_ref(&pixels);
        }
    }
}

#include "rt_fbx_loader_morph.inc"

/// @brief Determine whether a surface Geometry participates in an active trim graph.
/// @param objects Parsed FBX `Objects` node.
/// @param connections Connection table used to find neighboring Geometry objects.
/// @param geometry_id Identifier of the candidate NURBS or patch surface.
/// @return Non-zero when a connected trim surface, boundary, or NURBS curve exists.
static int fbx_geometry_has_active_trim(fbx_node_t *objects,
                                        const fbx_conn_table_t *connections,
                                        int64_t geometry_id) {
    if (!objects || !connections)
        return 0;
    for (int32_t i = 0; i < connections->count; ++i) {
        int64_t other_id;
        fbx_node_t *other;
        const char *other_type;
        if (connections->entries[i].child_id == geometry_id)
            other_id = connections->entries[i].parent_id;
        else if (connections->entries[i].parent_id == geometry_id)
            other_id = connections->entries[i].child_id;
        else
            continue;
        other = fbx_find_object_by_id(objects, other_id);
        if (!other || strcmp(other->name, "Geometry") != 0 || other->prop_count < 3)
            continue;
        other_type = fbx_prop_str(other, 2);
        if (strcmp(other_type, "TrimNurbsSurface") == 0 || strcmp(other_type, "Boundary") == 0 ||
            strcmp(other_type, "NurbsCurve") == 0)
            return 1;
    }
    return 0;
}

/// @brief Import each renderable Geometry, with optional Z-up correction, into a Mesh3D.
/// @details Polygon meshes and tessellated NURBS/patches share the same binding/remap arrays so
///          later material, skin, morph, and scene-node passes do not need a parallel path. Shape
///          and non-renderable curve records remain available to their dedicated passes. A
///          declared renderable Geometry must decode into at least one valid triangle.
/// @param[in,out] asset Staged asset that receives owned mesh and material references.
/// @param objects Parsed FBX `Objects` node.
/// @param connections Validated connection table used to detect trim relationships.
/// @param[in,out] context Loader context used for procedural-surface budget accounting.
/// @param z_up Non-zero to convert imported geometry into runtime Y-up coordinates.
/// @param[in,out] p_mesh_bindings Address of the heap-grown mesh-binding array.
/// @param[in,out] p_mesh_binding_count Address of its initialized entry count.
/// @param[in,out] p_mesh_remaps Address of the heap-grown geometry-remap array.
/// @param[in,out] p_mesh_remap_count Address of its initialized entry count.
/// @param[in,out] p_material_bindings Address of the heap-grown material-binding array.
/// @param[in,out] p_material_binding_count Address of its initialized entry count.
/// @return One on success; zero on allocation failure or malformed renderable Geometry.
static int fbx_load_collect_geometry(rt_fbx_asset *asset,
                                     fbx_node_t *objects,
                                     const fbx_conn_table_t *connections,
                                     fbx_load_context_t *context,
                                     int z_up,
                                     fbx_mesh_binding_t **p_mesh_bindings,
                                     int32_t *p_mesh_binding_count,
                                     fbx_mesh_remap_t **p_mesh_remaps,
                                     int32_t *p_mesh_remap_count,
                                     fbx_material_binding_t **p_material_bindings,
                                     int32_t *p_material_binding_count) {
    fbx_mesh_binding_t *mesh_bindings = *p_mesh_bindings;
    int32_t mesh_binding_count = *p_mesh_binding_count;
    fbx_mesh_remap_t *mesh_remaps = *p_mesh_remaps;
    int32_t mesh_remap_count = *p_mesh_remap_count;
    fbx_material_binding_t *material_bindings = *p_material_bindings;
    int32_t material_binding_count = *p_material_binding_count;
    if (objects && objects->children && objects->child_count > 0) {
        for (int32_t i = 0; i < objects->child_count; i++) {
            fbx_node_t *obj = &objects->children[i];
            if (strcmp(obj->name, "Geometry") == 0) {
                fbx_mesh_remap_t remap;
                fbx_mesh_material_map_t material_map;
                const char *geometry_type;
                if (obj->prop_count < 3)
                    return 0;
                geometry_type = fbx_prop_str(obj, 2);
                if (strcmp(geometry_type, "TrimNurbsSurface") == 0) {
                    return fbx_surface_error("FBX: trimmed NURBS surfaces are malformed or "
                                             "unsupported by this file version");
                }
                if (!fbx_geometry_type_is_renderable(geometry_type))
                    continue;
                if (strcmp(geometry_type, "Mesh") != 0 &&
                    fbx_geometry_has_active_trim(objects, connections, fbx_prop_i64(obj, 0))) {
                    return fbx_surface_error("FBX: trimmed NURBS surfaces are malformed or "
                                             "unsupported by this file version");
                }
                memset(&remap, 0, sizeof(remap));
                memset(&material_map, 0, sizeof(material_map));
                void *mesh = strcmp(geometry_type, "Mesh") == 0
                                 ? fbx_extract_geometry(obj, z_up, &remap, &material_map)
                                 : fbx_extract_procedural_geometry(
                                       obj, geometry_type, z_up, context, &remap, &material_map);
                if (!mesh) {
                    fbx_mesh_remap_free(&remap);
                    fbx_mesh_material_map_free(&material_map);
                    return 0;
                }
                {
                    asset->mesh_count = fbx_asset_safe_count(
                        asset->meshes, asset->mesh_count, asset->mesh_capacity);
                    if (asset->mesh_count == INT32_MAX) {
                        fbx_release_ref(&mesh);
                        fbx_mesh_remap_free(&remap);
                        return 0;
                    }
                    int32_t nc = asset->mesh_count + 1;
                    if ((size_t)nc > SIZE_MAX / sizeof(*mesh_bindings)) {
                        fbx_release_ref(&mesh);
                        fbx_mesh_remap_free(&remap);
                        fbx_mesh_material_map_free(&material_map);
                        return 0;
                    }
                    if (!fbx_asset_reserve_ref_array(&asset->meshes, &asset->mesh_capacity, nc)) {
                        fbx_release_ref(&mesh);
                        fbx_mesh_remap_free(&remap);
                        fbx_mesh_material_map_free(&material_map);
                        return 0;
                    }
                    void *nb = realloc(mesh_bindings, (size_t)nc * sizeof(*mesh_bindings));
                    if (!nb) {
                        fbx_release_ref(&mesh);
                        fbx_mesh_remap_free(&remap);
                        fbx_mesh_material_map_free(&material_map);
                        return 0;
                    }
                    mesh_bindings = (fbx_mesh_binding_t *)nb;
                    *p_mesh_bindings = mesh_bindings;
                    if (!fbx_mesh_remaps_append(&mesh_remaps, &mesh_remap_count, &remap)) {
                        fbx_release_ref(&mesh);
                        fbx_mesh_remap_free(&remap);
                        fbx_mesh_material_map_free(&material_map);
                        return 0;
                    }
                    *p_mesh_remaps = mesh_remaps;
                    *p_mesh_remap_count = mesh_remap_count;
                    asset->meshes[asset->mesh_count] = mesh;
                    asset->mesh_count = nc;
                    mesh_bindings[mesh_binding_count].id = fbx_prop_i64(obj, 0);
                    mesh_bindings[mesh_binding_count].mesh = mesh;
                    mesh_bindings[mesh_binding_count].material_map = material_map;
                    memset(&material_map, 0, sizeof(material_map));
                    mesh_binding_count++;
                    *p_mesh_binding_count = mesh_binding_count;
                }
                fbx_mesh_remap_free(&remap);
                fbx_mesh_material_map_free(&material_map);
            } else if (strcmp(obj->name, "Material") == 0) {
                void *mat = fbx_extract_material(obj);
                if (mat) {
                    asset->material_count = fbx_asset_safe_count(
                        asset->materials, asset->material_count, asset->material_capacity);
                    if (asset->material_count == INT32_MAX) {
                        fbx_release_ref(&mat);
                        return 0;
                    }
                    int32_t nc = asset->material_count + 1;
                    if ((size_t)nc > SIZE_MAX / sizeof(*material_bindings)) {
                        fbx_release_ref(&mat);
                        return 0;
                    }
                    if (!fbx_asset_reserve_ref_array(
                            &asset->materials, &asset->material_capacity, nc)) {
                        fbx_release_ref(&mat);
                        return 0;
                    }
                    void *nb = realloc(material_bindings, (size_t)nc * sizeof(*material_bindings));
                    if (!nb) {
                        fbx_release_ref(&mat);
                        return 0;
                    }
                    material_bindings = (fbx_material_binding_t *)nb;
                    *p_material_bindings = material_bindings;
                    asset->materials[asset->material_count] = mat;
                    asset->material_count = nc;
                    material_bindings[material_binding_count].id = fbx_prop_i64(obj, 0);
                    material_bindings[material_binding_count].material = mat;
                    material_binding_count++;
                    *p_material_binding_count = material_binding_count;
                }
            }
        }
    }
    *p_mesh_bindings = mesh_bindings;
    *p_mesh_binding_count = mesh_binding_count;
    *p_mesh_remaps = mesh_remaps;
    *p_mesh_remap_count = mesh_remap_count;
    *p_material_bindings = material_bindings;
    *p_material_binding_count = material_binding_count;
    return 1;
}

/// @brief Add one asset-owned default material when geometry has no authored material objects.
/// @details Compact ASCII FBX and geometry-only binary files remain immediately renderable and
///          preserve the loader's historical one-default-material contract. The material is added
///          only after all authored objects have been collected, so real FBX materials always win.
/// @param asset Transaction-local asset under construction.
/// @return Nonzero when no default was needed or one was added; zero on allocation failure.
static int fbx_asset_ensure_default_material(rt_fbx_asset *asset) {
    void *material;
    if (!asset || asset->mesh_count <= 0 || asset->material_count > 0)
        return asset != NULL;
    material = rt_material3d_new();
    if (!material)
        return 0;
    if (!fbx_asset_reserve_ref_array(&asset->materials, &asset->material_capacity, 1)) {
        fbx_release_ref(&material);
        return 0;
    }
    asset->materials[0] = material;
    asset->material_count = 1;
    return 1;
}

/// @brief Read an entire FBX file into a NUL-padded, budgeted malloc buffer.
/// @details Content and I/O failures set asset diagnostics and return NULL. The source allocation
///          is charged before allocation so a lowered aggregate load budget applies even before
///          compressed arrays and typed parser metadata are considered. Allocation failure still
///          traps because it is a runtime resource failure, not bad content.
/// @param cpath Native UTF-8 path.
/// @param out_size Receives the exact source byte count.
/// @param context Active per-load aggregate budget.
/// @return Owned NUL-padded file bytes, or NULL on failure.
static uint8_t *fbx_read_entire_file(const char *cpath,
                                     long *out_size,
                                     fbx_load_context_t *context) {
    uint64_t max_file_bytes = fbx_max_file_bytes();
    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "FBX.Load: '%s' not found", cpath);
        return NULL;
    }
    if (rt_file_stdio_seek64(f, 0, SEEK_END) != 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to seek '%s'", cpath);
        return NULL;
    }
    int64_t fsize = rt_file_stdio_tell64(f);
    if (fsize < 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to tell '%s'", cpath);
        return NULL;
    }
    if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to seek '%s'", cpath);
        return NULL;
    }
    if (fsize <= 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_BAD_MAGIC, "FBX.Load: '%s' is too small", cpath);
        return NULL;
    }
    if ((uint64_t)fsize > max_file_bytes || (uint64_t)fsize >= (uint64_t)SIZE_MAX) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_TOO_LARGE, "FBX.Load: '%s' is too large", cpath);
        return NULL;
    }

    if (!fbx_budget_charge(context, (uint64_t)fsize + 1u, 1u)) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_TOO_LARGE, "FBX.Load: '%s' exceeds the aggregate load budget", cpath);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)fsize + 1u);
    if (!data) {
        fclose(f);
        rt_trap("FBX.Load: out of memory");
        return NULL;
    }
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(data);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to read '%s'", cpath);
        return NULL;
    }
    data[(size_t)fsize] = 0;
    fclose(f);
    *out_size = (long)fsize;
    return data;
}

/// @brief Test whether a byte stream starts with the complete FBX binary magic sequence.
/// @param data Candidate file bytes.
/// @param size Number of readable bytes in @p data.
/// @return Non-zero when the full magic, including its trailing NUL byte, matches.
static int fbx_has_binary_magic(const uint8_t *data, long size) {
    static const uint8_t magic[] = {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X',
                                    ' ', 'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', 0x00};
    return data && size >= (long)sizeof(magic) && memcmp(data, magic, sizeof(magic)) == 0;
}

/// @brief Sniff the bounded initial comment block used by ASCII FBX files.
/// @param data Candidate file bytes.
/// @param size Number of readable bytes in @p data.
/// @return Non-zero when the first 256 bytes contain a semicolon comment beginning
///         with case-insensitive `FBX`; otherwise zero.
static int fbx_has_ascii_fbx_signature(const uint8_t *data, long size) {
    size_t limit;
    if (!data || size <= 0)
        return 0;
    limit = (size_t)size < 256u ? (size_t)size : 256u;
    for (size_t i = 0; i < limit; i++) {
        size_t j;
        if (data[i] != ';')
            continue;
        j = i + 1u;
        while (j < limit && (data[j] == ' ' || data[j] == '\t'))
            j++;
        if (j + 2u < limit && (data[j] == 'F' || data[j] == 'f') &&
            (data[j + 1u] == 'B' || data[j + 1u] == 'b') &&
            (data[j + 2u] == 'X' || data[j + 2u] == 'x'))
            return 1;
    }
    return 0;
}

/// @brief Release every parser/extractor scratch table owned by one top-level load attempt.
/// @details All arguments may be NULL/zero. Runtime resources already published on the staged
///          asset are intentionally excluded and remain the asset finalizer's responsibility.
/// @param[in,out] root Parsed node-tree root to recursively clear.
/// @param[in,out] connections Connection table to release.
/// @param[in,out] constraints Parsed constraint set to release.
/// @param[in,out] constraint_pose Evaluated static constraint pose to release.
/// @param[in,out] mesh_bindings Mesh-binding array, including nested material maps.
/// @param mesh_binding_count Number of initialized entries in @p mesh_bindings.
/// @param[in,out] mesh_remaps Geometry-remap array and its nested allocations.
/// @param mesh_remap_count Number of initialized entries in @p mesh_remaps.
/// @param[in,out] material_bindings Heap material-binding array.
/// @param[in,out] bone_bindings Heap bone-binding array.
/// @param[in,out] morph_bindings Morph-channel bindings and their nested allocations.
/// @param morph_binding_count Number of initialized entries in @p morph_bindings.
static void fbx_load_cleanup_scratch(fbx_node_t *root,
                                     fbx_conn_table_t *connections,
                                     fbx_constraint_set_t *constraints,
                                     fbx_constraint_pose_t *constraint_pose,
                                     fbx_mesh_binding_t *mesh_bindings,
                                     int32_t mesh_binding_count,
                                     fbx_mesh_remap_t *mesh_remaps,
                                     int32_t mesh_remap_count,
                                     fbx_material_binding_t *material_bindings,
                                     fbx_bone_binding_t *bone_bindings,
                                     fbx_morph_channel_binding_t *morph_bindings,
                                     int32_t morph_binding_count) {
    fbx_constraint_pose_free(constraint_pose);
    fbx_constraint_set_free(constraints);
    fbx_free_connections(connections);
    fbx_mesh_bindings_free(mesh_bindings, mesh_binding_count);
    fbx_mesh_remaps_free(mesh_remaps, mesh_remap_count);
    free(material_bindings);
    free(bone_bindings);
    fbx_morph_channel_bindings_free(morph_bindings, morph_binding_count);
    fbx_free_node(root);
}

/// @brief Load an FBX binary file and extract meshes, skeleton, animations, and materials.
/// @details Parses the FBX binary format (magic header "Kaydara FBX Binary"),
///          decodes geometry, deformers (skin clusters for skeletal binding),
///          animation curves, and materials. Z-up models are automatically
///          corrected to Y-up. Supports FBX versions 7.1–7.5 (most common).
/// @param path File path to the .fbx file (runtime string).
/// @return Opaque FBX asset handle containing meshes/skeleton/animations, or NULL.
static void *fbx_load_impl(rt_string path) {
    fbx_load_context_t context;
    fbx_node_t root;
    fbx_conn_table_t ct;
    fbx_constraint_set_t constraints;
    fbx_constraint_pose_t constraint_pose;
    fbx_node_t *objects = NULL;
    int is_binary = 0;
    int is_ascii_signature = 0;
    int owns_data = 0;
    fbx_mesh_binding_t *mesh_bindings = NULL;
    int32_t mesh_binding_count = 0;
    fbx_mesh_remap_t *mesh_remaps = NULL;
    int32_t mesh_remap_count = 0;
    fbx_material_binding_t *material_bindings = NULL;
    int32_t material_binding_count = 0;
    fbx_bone_binding_t *bone_bindings = NULL;
    int32_t bone_binding_count = 0;
    fbx_morph_channel_binding_t *morph_channel_bindings = NULL;
    int32_t morph_channel_binding_count = 0;
    memset(&root, 0, sizeof(root));
    memset(&ct, 0, sizeof(ct));
    memset(&constraints, 0, sizeof(constraints));
    memset(&constraint_pose, 0, sizeof(constraint_pose));
    fbx_load_context_init(&context);
    if (!path) {
        rt_trap("FBX.Load: null path");
        return NULL;
    }
    const char *cpath = rt_string_cstr(path);
    if (!cpath) {
        rt_trap("FBX.Load: invalid path");
        return NULL;
    }

    /* Read a path source or borrow the synchronous in-memory bridge. */
    long fsize;
    const uint8_t *data;
    uint8_t *owned_data = NULL;
    if (g_fbx_source_bytes_override) {
        uint64_t max_file_bytes = fbx_max_file_bytes();
        if (g_fbx_source_size_override == 0 || g_fbx_source_size_override > (size_t)LONG_MAX ||
            (uint64_t)g_fbx_source_size_override > max_file_bytes ||
            g_fbx_source_size_override == SIZE_MAX ||
            !fbx_budget_charge(&context, (uint64_t)g_fbx_source_size_override + 1u, 1u)) {
            rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                               "FBX.Load: in-memory source exceeds a configured limit");
            return NULL;
        }
        data = g_fbx_source_bytes_override;
        fsize = (long)g_fbx_source_size_override;
    } else {
        owned_data = fbx_read_entire_file(cpath, &fsize, &context);
        if (!owned_data)
            return NULL;
        data = owned_data;
        owns_data = 1;
    }

    is_binary = fbx_has_binary_magic(data, fsize);
    is_ascii_signature = fbx_has_ascii_fbx_signature(data, fsize);
    if (is_binary) {
        fbx_reader_t reader;
        memset(&reader, 0, sizeof(reader));
        if (fsize < 27) {
            if (owns_data)
                free(owned_data);
            rt_asset_error_set(RT_ASSET_ERROR_BAD_MAGIC, "FBX.Load: truncated binary FBX header");
            return NULL;
        }
        reader.data = data;
        reader.len = (size_t)fsize;
        reader.pos = 27; /* skip 23-byte magic + 2 unknown + 4 version */
        reader.version = (uint32_t)data[23] | ((uint32_t)data[24] << 8u) |
                         ((uint32_t)data[25] << 16u) | ((uint32_t)data[26] << 24u);
        reader.is_64bit = reader.version >= 7500;
        reader.context = &context;
        if (!fbx_node_set_name(&root, "_root_", 6u, &context))
            reader.error = 1;
        while (!reader.error && !fbx_eof(&reader)) {
            size_t sentinel = reader.is_64bit ? 25u : 13u;
            int is_null = 0;
            if (reader.pos + sentinel <= reader.len) {
                is_null = 1;
                for (size_t si = 0; si < sentinel; si++) {
                    if (reader.data[reader.pos + si] != 0) {
                        is_null = 0;
                        break;
                    }
                }
            }
            if (is_null)
                break;
            if (!fbx_node_reserve_child(&root, &context)) {
                reader.error = 1;
                break;
            }
            if (fbx_parse_node(&reader, &root.children[root.child_count], 0) < 0) {
                fbx_free_node(&root.children[root.child_count]);
                reader.error = 1;
                break;
            }
            root.child_count++;
        }
        if (owns_data)
            free(owned_data);
        if (reader.error) {
            fbx_free_node(&root);
            if (context.budget_exhausted) {
                rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                                   "FBX.Load: binary FBX exceeds the aggregate load budget");
            } else if (context.structural_limit_exceeded) {
                rt_asset_error_set(
                    RT_ASSET_ERROR_TOO_LARGE,
                    "FBX.Load: binary FBX exceeds a parser structural limit (record child "
                    "count, property count, or nesting depth)");
            } else {
                rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                                   "FBX.Load: malformed or truncated binary FBX");
            }
            return NULL;
        }
    } else {
        int parsed = fbx_parse_ascii_document(data, (size_t)fsize, &root, &context);
        if (owns_data)
            free(owned_data);
        if (!parsed) {
            if (context.budget_exhausted) {
                rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                                   "FBX.Load: ASCII FBX exceeds the aggregate load budget");
            } else if (context.structural_limit_exceeded) {
                rt_asset_error_set(
                    RT_ASSET_ERROR_TOO_LARGE,
                    "FBX.Load: ASCII FBX exceeds a parser structural limit (record child "
                    "count, property count, or nesting depth)");
            } else {
                rt_asset_error_set(is_ascii_signature ? RT_ASSET_ERROR_CORRUPT
                                                      : RT_ASSET_ERROR_BAD_MAGIC,
                                   is_ascii_signature ? "FBX.Load: malformed ASCII FBX"
                                                      : "FBX.Load: not a supported FBX file");
            }
            return NULL;
        }
    }

    objects = fbx_find_child(&root, "Objects");
    if (!objects || !fbx_build_object_index(objects, &context) ||
        !fbx_parse_connections(&root, &ct, &context)) {
        fbx_load_cleanup_scratch(
            &root, &ct, &constraints, &constraint_pose, NULL, 0, NULL, 0, NULL, NULL, NULL, 0);
        rt_asset_error_set(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted ? "FBX.Load: FBX graph exceeds the aggregate load budget"
                                     : "FBX.Load: invalid Objects or Connections graph");
        return NULL;
    }

    /* Detect coordinate system */
    int z_up = fbx_is_z_up(&root);

    if (!fbx_parse_constraints(objects, &ct, &context, &constraints) ||
        (constraints.count > 0 &&
         !fbx_constraint_build_static_pose(
             objects, &ct, z_up, &constraints, &context, &constraint_pose))) {
        fbx_load_cleanup_scratch(
            &root, &ct, &constraints, &constraint_pose, NULL, 0, NULL, 0, NULL, NULL, NULL, 0);
        rt_asset_error_set_if_empty(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted ? "FBX.Load: constraints exceed the aggregate load budget"
                                     : "FBX.Load: invalid constraint graph or evaluated pose");
        return NULL;
    }

    /* Extract assets */
    rt_fbx_asset *asset =
        (rt_fbx_asset *)rt_obj_new_i64(RT_G3D_FBX_ASSET_CLASS_ID, (int64_t)sizeof(rt_fbx_asset));
    if (!asset) {
        fbx_load_cleanup_scratch(&root,
                                 &ct,
                                 &constraints,
                                 &constraint_pose,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 material_bindings,
                                 NULL,
                                 NULL,
                                 0);
        rt_trap("FBX.Load: out of memory");
        return NULL;
    }
    asset->vptr = NULL;
    asset->meshes = NULL;
    asset->mesh_count = 0;
    asset->mesh_capacity = 0;
    asset->skeleton = NULL;
    asset->animations = NULL;
    asset->animation_count = 0;
    asset->animation_capacity = 0;
    asset->node_animations = NULL;
    asset->node_animation_count = 0;
    asset->node_animation_capacity = 0;
    asset->cameras = NULL;
    asset->camera_count = 0;
    asset->camera_capacity = 0;
    asset->materials = NULL;
    asset->material_count = 0;
    asset->material_capacity = 0;
    asset->morph_targets = NULL;
    asset->morph_count = 0;
    asset->morph_capacity = 0;
    asset->scene_root = NULL;
    rt_obj_set_finalizer(asset, rt_fbx_asset_finalize);

    /* Extract geometry */
    if (!fbx_load_collect_geometry(asset,
                                   objects,
                                   &ct,
                                   &context,
                                   z_up,
                                   &mesh_bindings,
                                   &mesh_binding_count,
                                   &mesh_remaps,
                                   &mesh_remap_count,
                                   &material_bindings,
                                   &material_binding_count) ||
        !fbx_asset_ensure_default_material(asset)) {
        fbx_load_cleanup_scratch(&root,
                                 &ct,
                                 &constraints,
                                 &constraint_pose,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 material_bindings,
                                 NULL,
                                 NULL,
                                 0);
        fbx_release_ref((void **)&asset);
        rt_asset_error_set_if_empty(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted
                ? "FBX.Load: geometry/material extraction exceeds the aggregate load budget"
                : "FBX.Load: failed to collect geometry/material bindings");
        return NULL;
    }

    /* Extract textures and link to materials */
    {
        const char *texture_base =
            g_fbx_texture_base_override ? rt_string_cstr(g_fbx_texture_base_override) : NULL;
        fbx_load_link_textures(asset,
                               objects,
                               texture_base && texture_base[0] ? texture_base : cpath,
                               ct,
                               material_bindings,
                               material_binding_count);
    }

    /* Extract morph targets (BlendShape deformers) */
    if (!fbx_load_extract_morphs(asset,
                                 objects,
                                 &ct,
                                 z_up,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 &morph_channel_bindings,
                                 &morph_channel_binding_count,
                                 &context)) {
        fbx_load_cleanup_scratch(&root,
                                 &ct,
                                 &constraints,
                                 &constraint_pose,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 material_bindings,
                                 NULL,
                                 morph_channel_bindings,
                                 morph_channel_binding_count);
        fbx_release_ref((void **)&asset);
        rt_asset_error_set(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted ? "FBX.Load: morph targets exceed the aggregate load budget"
                                     : "FBX.Load: failed to build complete FBX morph targets");
        return NULL;
    }
    /* Extract skeleton */
    asset->skeleton = fbx_extract_skeleton(&root,
                                           &ct,
                                           constraints.count > 0 ? &constraint_pose : NULL,
                                           z_up,
                                           &bone_bindings,
                                           &bone_binding_count);
    fbx_apply_skinning(objects,
                       &ct,
                       asset->skeleton,
                       bone_bindings,
                       bone_binding_count,
                       mesh_bindings,
                       mesh_binding_count,
                       mesh_remaps,
                       mesh_remap_count);

    /* Extract animations */
    if (!fbx_extract_animations(&root,
                                &ct,
                                asset->skeleton,
                                bone_bindings,
                                bone_binding_count,
                                z_up,
                                &constraints,
                                &asset->animations,
                                &asset->animation_count,
                                &asset->animation_capacity,
                                &context)) {
        fbx_load_cleanup_scratch(&root,
                                 &ct,
                                 &constraints,
                                 &constraint_pose,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 material_bindings,
                                 bone_bindings,
                                 morph_channel_bindings,
                                 morph_channel_binding_count);
        fbx_release_ref((void **)&asset);
        rt_asset_error_set_if_empty(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted
                ? "FBX.Load: animation samples exceed the aggregate load budget"
                : "FBX.Load: failed to build complete FBX animations");
        return NULL;
    }

    if (!fbx_extract_node_animations(&root,
                                     &ct,
                                     &constraints,
                                     z_up,
                                     asset,
                                     morph_channel_bindings,
                                     morph_channel_binding_count,
                                     &asset->node_animations,
                                     &asset->node_animation_count,
                                     &asset->node_animation_capacity,
                                     &context)) {
        fbx_load_cleanup_scratch(&root,
                                 &ct,
                                 &constraints,
                                 &constraint_pose,
                                 mesh_bindings,
                                 mesh_binding_count,
                                 mesh_remaps,
                                 mesh_remap_count,
                                 material_bindings,
                                 bone_bindings,
                                 morph_channel_bindings,
                                 morph_channel_binding_count);
        fbx_release_ref((void **)&asset);
        rt_asset_error_set_if_empty(
            context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context.budget_exhausted
                ? "FBX.Load: node-animation samples exceed the aggregate load budget"
                : "FBX.Load: failed to build complete FBX node animations");
        return NULL;
    }

    {
        int scene_failed = 0;
        asset->scene_root = fbx_build_scene_root(asset,
                                                 &root,
                                                 objects,
                                                 &ct,
                                                 constraints.count > 0 ? &constraint_pose : NULL,
                                                 mesh_bindings,
                                                 mesh_binding_count,
                                                 material_bindings,
                                                 material_binding_count,
                                                 z_up,
                                                 &context,
                                                 &scene_failed);
        if (scene_failed) {
            fbx_load_cleanup_scratch(&root,
                                     &ct,
                                     &constraints,
                                     &constraint_pose,
                                     mesh_bindings,
                                     mesh_binding_count,
                                     mesh_remaps,
                                     mesh_remap_count,
                                     material_bindings,
                                     bone_bindings,
                                     morph_channel_bindings,
                                     morph_channel_binding_count);
            fbx_release_ref((void **)&asset);
            rt_asset_error_set_if_empty(
                context.budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
                context.budget_exhausted
                    ? "FBX.Load: scene resources exceed the aggregate load budget"
                    : "FBX.Load: failed to build scene hierarchy");
            return NULL;
        }
    }

    /* Cleanup parser data */
    fbx_load_cleanup_scratch(&root,
                             &ct,
                             &constraints,
                             &constraint_pose,
                             mesh_bindings,
                             mesh_binding_count,
                             mesh_remaps,
                             mesh_remap_count,
                             material_bindings,
                             bone_bindings,
                             morph_channel_bindings,
                             morph_channel_binding_count);

    return asset;
}

/// @brief Load an FBX asset and manage public diagnostic/statistics transaction state.
/// @details Validates the runtime path, delegates parsing and extraction to
///          @ref fbx_load_impl, and commits either the success or failure counters and
///          last-error record maintained by the asset-error subsystem.
/// @param path Runtime string containing the native FBX file path.
/// @return Newly allocated FBX asset handle on success, or NULL on load failure.
void *rt_fbx_load(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("FBX.Load: null path");
        return NULL;
    }
    if (!rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_trap("FBX.Load: invalid path");
        return NULL;
    }
    void *asset = fbx_load_impl(path);
    if (asset) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "FBX.Load: failed to load FBX");
        rt_asset_error_end_load_failure();
    }
    return asset;
}

/// @brief Read a candidate FBX file into a NUL-terminated buffer without I/O traps.
/// @details This helper is used by `rt_fbx_load_recoverable()` to separate ordinary
/// filesystem or short-file failures from strict `FBX.Load` diagnostics. It returns
/// NULL for missing files, invalid paths, seek/tell failures, short files, and read
/// errors. Allocation failure still traps because continuing would hide a runtime
/// resource exhaustion condition.
/// @param path Runtime string path to the candidate FBX file.
/// @param out_size Receives the number of bytes read when the function succeeds.
/// @return Heap buffer containing the file bytes plus a trailing NUL, or NULL.
static uint8_t *fbx_read_recoverable_file(rt_string path, long *out_size) {
    uint64_t max_file_bytes = fbx_max_file_bytes();
    if (out_size)
        *out_size = 0;
    if (!path || !out_size)
        return NULL;
    const char *cpath = rt_string_cstr(path);
    if (!cpath)
        return NULL;

    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "FBX.Load: '%s' not found", cpath);
        return NULL;
    }
    if (rt_file_stdio_seek64(f, 0, SEEK_END) != 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to seek '%s'", cpath);
        return NULL;
    }
    int64_t fsize = rt_file_stdio_tell64(f);
    if (fsize <= 0 || (uint64_t)fsize > max_file_bytes || (uint64_t)fsize >= (uint64_t)SIZE_MAX) {
        fclose(f);
        rt_asset_error_setf((uint64_t)fsize > max_file_bytes ? RT_ASSET_ERROR_TOO_LARGE
                                                             : RT_ASSET_ERROR_BAD_MAGIC,
                            "FBX.Load: '%s' is not a supported FBX",
                            cpath);
        return NULL;
    }
    if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to seek '%s'", cpath);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)fsize + 1u);
    if (!data) {
        fclose(f);
        rt_trap("FBX.Load: out of memory");
        return NULL;
    }
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(data);
        rt_asset_error_setf(RT_ASSET_ERROR_UNREADABLE, "FBX.Load: failed to read '%s'", cpath);
        return NULL;
    }
    data[(size_t)fsize] = 0;
    fclose(f);
    *out_size = (long)fsize;
    return data;
}

/// @brief Recoverably load a binary or ASCII FBX asset for high-level model loading.
/// @details The preliminary bounded read retains the established recoverable filesystem behavior.
///          All non-empty candidates are then delegated to `rt_fbx_load()`, where binary and ASCII
///          inputs share the typed graph, aggregate budget, object index, connection adjacency,
///          and transactional extraction path. Unsupported text therefore cannot fall through to
///          the former substring-based single-mesh implementation.
/// @param path Runtime string path to the candidate FBX file.
/// @return An FBX handle on success, or NULL for recoverable load failures.
void *rt_fbx_load_recoverable(rt_string path) {
    long fsize = 0;
    uint8_t *data = fbx_read_recoverable_file(path, &fsize);
    if (!data)
        return NULL;

    free(data);
    return rt_fbx_load(path);
}

/// @brief Recoverably load an FBX while temporarily overriding relative texture resolution.
/// @details The previous process-local override is restored before return. The override
///          string is borrowed only for the duration of the synchronous load.
/// @param path Runtime string containing the FBX file path.
/// @param texture_base Runtime string used as the relative texture lookup base.
/// @return Newly allocated FBX asset handle on success, or NULL for a recoverable load failure.
void *rt_fbx_load_recoverable_with_texture_base(rt_string path, rt_string texture_base) {
    rt_string previous = g_fbx_texture_base_override;
    void *asset;
    g_fbx_texture_base_override = texture_base;
    asset = rt_fbx_load_recoverable(path);
    g_fbx_texture_base_override = previous;
    return asset;
}

/// @brief Parse borrowed FBX bytes directly while retaining the logical texture base path.
void *fbx_load_recoverable_bytes_internal(rt_string original_path,
                                          const uint8_t *bytes,
                                          size_t size) {
    const uint8_t *previous_bytes = g_fbx_source_bytes_override;
    size_t previous_size = g_fbx_source_size_override;
    rt_string previous_base = g_fbx_texture_base_override;
    void *asset;
    if (!original_path || !rt_string_cstr(original_path) || !bytes || size == 0)
        return NULL;
    rt_asset_error_begin_load();
    g_fbx_source_bytes_override = bytes;
    g_fbx_source_size_override = size;
    g_fbx_texture_base_override = original_path;
    asset = fbx_load_impl(original_path);
    g_fbx_texture_base_override = previous_base;
    g_fbx_source_bytes_override = previous_bytes;
    g_fbx_source_size_override = previous_size;
    if (asset) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "FBX.Load: failed to load FBX bytes");
        rt_asset_error_end_load_failure();
    }
    return asset;
}
