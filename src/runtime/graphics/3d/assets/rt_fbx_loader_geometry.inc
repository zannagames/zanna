//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_geometry.inc
// Purpose: FBX coordinate conversion plus mesh, procedural-surface, and material extraction.
// Key invariants:
//   - Geometry inputs are validated before node fields or type strings are inspected.
//   - Allocation products and imported indices remain inside runtime and aggregate-load limits.
// Ownership/Lifetime:
//   - Temporary remap/control-net arrays are owned locally and released on every failure path.
//   - Published Mesh3D/material objects transfer to the caller's runtime object graph.
// Links: rt_fbx_loader.c, rt_fbx_loader_parse.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_geometry.inc
 * @brief Implements FBX coordinate conversion, mesh extraction, procedural-surface
 *        tessellation, and material-property import.
 *
 * This internal implementation fragment converts parsed FBX geometry nodes into
 * runtime Mesh3D objects. It resolves FBX layer mapping/reference modes, preserves
 * control-point-to-emitted-vertex remaps for later deformation import, triangulates
 * polygon meshes, and tessellates NURBS and patch surfaces under the loader's
 * aggregate allocation budget. It also imports the material scalars understood by
 * the runtime.
 *
 * All node and property pointers are borrowed from the parsed FBX tree. Returned
 * meshes and materials are newly allocated runtime objects; temporary sampling,
 * remapping, and numeric buffers are released locally or transferred explicitly to
 * caller-owned remap structures.
 */

/*==========================================================================
 * Coordinate system detection + correction
 *=========================================================================*/

/// @brief Read `GlobalSettings.UpAxis` from the FBX header; returns 1 for Z-up, 0 for Y-up.
///
/// Most FBX exporters use Y-up but Maya / Blender / 3ds Max can
/// emit Z-up scenes. The extractors apply a coordinate-system swap
/// when this returns 1 so the scene is normalised to Zanna's Y-up convention.
/// @param root Root of the parsed FBX node tree; may be NULL.
/// @return Non-zero when the declared up axis is Z, or zero when it is Y, absent, or malformed.
static int fbx_is_z_up(fbx_node_t *root) {
    fbx_node_t *gs = fbx_find_child(root, "GlobalSettings");
    if (!gs)
        return 0;
    fbx_node_t *p70 = fbx_find_child(gs, "Properties70");
    if (!p70)
        return 0;
    for (int32_t i = 0; i < p70->child_count; i++) {
        fbx_node_t *p = &p70->children[i];
        if (strcmp(p->name, "P") != 0 || p->prop_count < 5)
            continue;
        const char *pname = fbx_prop_str(p, 0);
        if (strcmp(pname, "UpAxis") == 0) {
            int64_t axis = fbx_prop_i64(p, 4);
            return axis == 2; /* 2 = Z-up */
        }
    }
    return 0;
}

/// @brief Apply the FBX Z-up to runtime Y-up coordinate conversion in place.
/// @details Preserves @p x, assigns the original Z coordinate to @p y, and negates the
///          original Y coordinate into @p z to retain a right-handed coordinate system.
/// @param[in,out] x X coordinate, which is left unchanged.
/// @param[in,out] y Y coordinate, replaced by the original Z coordinate.
/// @param[in,out] z Z coordinate, replaced by the negated original Y coordinate.
static void fbx_correct_zup(double *x, double *y, double *z) {
    double tmp = *y;
    *y = *z;
    *z = -tmp;
}

/// @brief Release every allocation owned by a mesh-remap entry and reset it.
/// @details The remap object itself remains caller-owned and may be safely reused or
///          passed to this function again after the call.
/// @param[in,out] remap Remap to clear; NULL is accepted as a no-op.
static void fbx_mesh_remap_free(fbx_mesh_remap_t *remap) {
    if (!remap)
        return;
    if (remap->control_vertices) {
        for (int32_t i = 0; i < remap->control_count; i++)
            free(remap->control_vertices[i].vertices);
    }
    free(remap->control_vertices);
    free(remap->surface_vertices);
    free(remap->surface_control_indices);
    free(remap->surface_control_weights);
    remap->control_vertices = NULL;
    remap->surface_vertices = NULL;
    remap->surface_control_indices = NULL;
    remap->surface_control_weights = NULL;
    remap->control_count = 0;
    remap->surface_vertex_count = 0;
    remap->surface_contribution_count = 0;
    remap->id = 0;
}

/// @brief Release an array of mesh-remap entries and all storage owned by them.
/// @param[in,out] remaps Heap array to release; NULL is accepted as a no-op.
/// @param count Number of initialized entries in @p remaps.
static void fbx_mesh_remaps_free(fbx_mesh_remap_t *remaps, int32_t count) {
    if (!remaps)
        return;
    for (int32_t i = 0; i < count; i++)
        fbx_mesh_remap_free(&remaps[i]);
    free(remaps);
}

/// @brief Append a completed mesh remap to a heap-grown array.
/// @details On success the entry is copied into the array and @p remap is zeroed,
///          transferring ownership of all nested allocations. On failure both the
///          array and @p remap retain their original ownership and contents.
/// @param[in,out] remaps Address of the caller-owned array pointer.
/// @param[in,out] count Number of entries, incremented after a successful append.
/// @param[in,out] remap Completed entry whose allocations transfer on success.
/// @return Non-zero on success, or zero for invalid arguments, count/size overflow,
///         or allocation failure.
static int fbx_mesh_remaps_append(fbx_mesh_remap_t **remaps,
                                  int32_t *count,
                                  fbx_mesh_remap_t *remap) {
    fbx_mesh_remap_t *grown;
    if (!remaps || !count || !remap || *count < 0 || *count == INT32_MAX)
        return 0;
    size_t next_count = (size_t)(*count + 1);
    if (next_count > SIZE_MAX / sizeof(**remaps))
        return 0;
    grown = (fbx_mesh_remap_t *)realloc(*remaps, next_count * sizeof(**remaps));
    if (!grown)
        return 0;
    *remaps = grown;
    (*remaps)[*count] = *remap;
    (*count)++;
    memset(remap, 0, sizeof(*remap));
    return 1;
}

/// @brief Initialize a mesh remap for an FBX geometry's control vertices.
/// @details Existing contents are overwritten, so the caller must first release any
///          allocations already owned by @p remap. A non-positive control count creates
///          an otherwise valid empty remap.
/// @param[out] remap Entry to initialize.
/// @param id FBX object identifier associated with the geometry.
/// @param control_count Number of source control vertices to track.
/// @return Non-zero on success, or zero for a NULL destination, an untrusted-size
///         rejection, or allocation failure.
static int fbx_mesh_remap_init(fbx_mesh_remap_t *remap, int64_t id, int32_t control_count) {
    if (!remap)
        return 0;
    memset(remap, 0, sizeof(*remap));
    remap->id = id;
    remap->control_count = control_count;
    if (control_count <= 0)
        return 1;
    if (!rt_untrusted_count_ok(
            (int64_t)control_count, 3u * sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
        return 0;
    remap->control_vertices =
        (fbx_vertex_index_list_t *)calloc((size_t)control_count, sizeof(*remap->control_vertices));
    return remap->control_vertices != NULL;
}

/// @brief Append a new mesh vertex index to the list for a given control vertex.
/// @details Grows the per-control-vertex list as needed. Returning status lets the geometry builder
///          abort instead of silently losing the remap entry used later by skinning/bind data.
/// @param[in,out] remap Initialized geometry remap to update.
/// @param control_index Zero-based source control-vertex index.
/// @param vertex_index Non-negative emitted Mesh3D vertex index to append.
/// @return 1 on success, 0 on invalid indices, capacity overflow, or allocation failure.
static int fbx_mesh_remap_add_vertex(fbx_mesh_remap_t *remap,
                                     int32_t control_index,
                                     int32_t vertex_index) {
    fbx_vertex_index_list_t *list;
    int32_t new_capacity;
    int32_t *grown;
    if (!remap || !remap->control_vertices || control_index < 0 ||
        control_index >= remap->control_count || vertex_index < 0)
        return 0;
    list = &remap->control_vertices[control_index];
    if (list->count >= list->capacity) {
        if (list->capacity < 0 || list->capacity > INT32_MAX / 2)
            return 0;
        new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(*list->vertices))
            return 0;
        grown = (int32_t *)realloc(list->vertices, (size_t)new_capacity * sizeof(*list->vertices));
        if (!grown)
            return 0;
        list->vertices = grown;
        list->capacity = new_capacity;
    }
    list->vertices[list->count++] = vertex_index;
    return 1;
}

/// @brief Find the mesh remap associated with an FBX object identifier.
/// @param remaps Array of initialized remap entries.
/// @param count Number of entries available in @p remaps.
/// @param id FBX geometry identifier to match.
/// @return Borrowed pointer to the matching entry, or NULL when no entry matches.
static const fbx_mesh_remap_t *fbx_find_mesh_remap(const fbx_mesh_remap_t *remaps,
                                                   int32_t count,
                                                   int64_t id) {
    for (int32_t i = 0; i < count; i++)
        if (remaps[i].id == id)
            return &remaps[i];
    return NULL;
}

/// @brief Release a mesh's per-triangle material-slot map and reset it.
/// @param[in,out] map Material map to clear; NULL is accepted as a no-op.
static void fbx_mesh_material_map_free(fbx_mesh_material_map_t *map) {
    if (!map)
        return;
    free(map->triangle_slots);
    memset(map, 0, sizeof(*map));
}

/// @brief Release an array of mesh bindings and their nested material maps.
/// @param[in,out] bindings Heap array to release; NULL is accepted as a no-op.
/// @param count Number of initialized bindings in @p bindings.
static void fbx_mesh_bindings_free(fbx_mesh_binding_t *bindings, int32_t count) {
    if (!bindings)
        return;
    for (int32_t i = 0; i < count; i++)
        fbx_mesh_material_map_free(&bindings[i].material_map);
    free(bindings);
}

/// @brief Append @p triangle_count triangles, all assigned to @p material_slot, to the
///   map's per-triangle slot array (growing it with overflow checks) and widen the tracked
///   slot count. Negative slots are clamped to 0.
/// @param[in,out] map Material map that owns the heap-grown slot array.
/// @param material_slot FBX material slot assigned to each appended triangle.
/// @param triangle_count Number of consecutive triangle entries to append.
/// @return 1 on success or no-op (zero triangles); 0 on overflow or allocation failure.
static int fbx_mesh_material_map_append(fbx_mesh_material_map_t *map,
                                        int32_t material_slot,
                                        uint32_t triangle_count) {
    int32_t *grown;
    uint32_t old_count;
    if (!map || triangle_count == 0)
        return 1;
    if (material_slot < 0)
        material_slot = 0;
    if (map->triangle_count > UINT32_MAX - triangle_count)
        return 0;
    if (!rt_untrusted_count_ok(
            (int64_t)triangle_count, sizeof(int32_t), FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
        return 0;
    if ((size_t)(map->triangle_count + triangle_count) > SIZE_MAX / sizeof(*map->triangle_slots))
        return 0;
    grown = (int32_t *)realloc(map->triangle_slots,
                               (size_t)(map->triangle_count + triangle_count) *
                                   sizeof(*map->triangle_slots));
    if (!grown)
        return 0;
    map->triangle_slots = grown;
    old_count = map->triangle_count;
    for (uint32_t i = 0; i < triangle_count; i++)
        map->triangle_slots[old_count + i] = material_slot;
    map->triangle_count += triangle_count;
    map->has_slots = 1;
    if (material_slot + 1 > map->slot_count)
        map->slot_count = material_slot + 1;
    return 1;
}

/// @brief Resolve the material-slot index for polygon @p polygon_index per the FBX mapping
///   mode (@p by_polygon → per-polygon lookup, @p all_same → slot 0, else per-index).
///   Returns -1 for an out-of-range index or a negative slot value when a
///   material map is present; returns 0 when no material map exists.
/// @param slots Borrowed array of FBX material-slot indices.
/// @param slot_count Number of entries in @p slots.
/// @param by_polygon Non-zero when one slot is stored per polygon.
/// @param all_same Non-zero when all polygons use the first slot.
/// @param polygon_index Polygon whose material assignment is requested.
/// @return The non-negative material slot, zero when no map is present, or -1 for
///         an invalid mapped entry.
static int32_t fbx_material_slot_for_polygon(const int32_t *slots,
                                             uint32_t slot_count,
                                             int by_polygon,
                                             int all_same,
                                             uint32_t polygon_index) {
    uint32_t src_index = 0;
    if (!slots || slot_count == 0)
        return 0;
    if (by_polygon)
        src_index = polygon_index;
    else if (all_same)
        src_index = 0;
    else
        src_index = polygon_index;
    if (src_index >= slot_count)
        return -1;
    return slots[src_index] < 0 ? -1 : slots[src_index];
}

#include "rt_fbx_triangulation.inc"

/*==========================================================================
 * Geometry extraction
 *=========================================================================*/

/// @brief Borrowed view of one parsed FBX vertex-attribute layer.
/// @details The arrays remain owned by the parsed FBX property tree. Mapping flags
///          describe how a polygon vertex selects a direct attribute element.
typedef struct {
    /// Borrowed double-precision direct-value array, or NULL.
    double *data;
    /// Borrowed single-precision direct-value array, or NULL.
    float *data32;
    /// Borrowed IndexToDirect array, or NULL for direct reference mode.
    int32_t *indices;
    /// Number of complete elements in the direct-value array.
    uint32_t count;
    /// Number of entries available through @ref indices.
    uint32_t index_count;
    /// Scalar component count per direct element.
    int components;
    /// Non-zero for `ByPolygonVertex` mapping.
    int by_polygon_vertex;
    /// Non-zero for `ByPolygon` mapping.
    int by_polygon;
    /// Non-zero for `AllSame` mapping.
    int all_same;
    /// Non-zero when the mapped index must be resolved through @ref indices.
    int index_to_direct;
} fbx_vertex_layer_t;

/// @brief Borrowed view of a parsed FBX `LayerElementMaterial`.
typedef struct {
    /// Borrowed array of authored material-slot indices.
    int32_t *slots;
    /// Number of entries available in @ref slots.
    uint32_t slot_count;
    /// Non-zero when entries correspond to individual polygons.
    int by_polygon;
    /// Non-zero when the first entry applies to every polygon.
    int all_same;
} fbx_material_layer_t;

/// @brief Return true when a LayerElement node has an explicit numeric layer index.
/// @param layer Candidate FBX `LayerElement*` node.
/// @param out_index Receives the numeric layer index when present.
/// @return Non-zero when the first property is a numeric index.
static int fbx_layer_element_numeric_index(fbx_node_t *layer, int64_t *out_index) {
    fbx_prop_t *p;
    if (!layer || layer->prop_count < 1 || !out_index)
        return 0;
    p = &layer->props[0];
    if (p->type == 'Y' || p->type == 'I' || p->type == 'L' || p->type == 'C') {
        *out_index = fbx_prop_i64(layer, 0);
        return 1;
    }
    return 0;
}

/// @brief Locate a specific FBX vertex layer by numeric layer index or occurrence order.
/// @details Many FBX files contain several nodes with the same name, such as two
///          `LayerElementUV` children for UV0/UV1. The binary tree helper returns only the first
///          child, so this routine scans all siblings and prefers an explicit layer-index property
///          matching @p desired_index. When indices are absent, occurrence order is used.
/// @param geom_node Geometry node containing `LayerElement*` children.
/// @param layer_name Child node name, e.g. `LayerElementUV`.
/// @param desired_index Layer index to find.
/// @return Matching layer node, or NULL.
static fbx_node_t *fbx_find_vertex_layer(fbx_node_t *geom_node,
                                         const char *layer_name,
                                         int desired_index) {
    fbx_node_t *first = NULL;
    int occurrence = 0;
    if (!geom_node || !layer_name || desired_index < 0)
        return NULL;
    for (int32_t i = 0; i < geom_node->child_count; i++) {
        fbx_node_t *child = &geom_node->children[i];
        int64_t explicit_index = 0;
        if (strcmp(child->name, layer_name) != 0)
            continue;
        if (!first)
            first = child;
        if (fbx_layer_element_numeric_index(child, &explicit_index)) {
            if (explicit_index == desired_index)
                return child;
        } else if (occurrence == desired_index) {
            return child;
        }
        occurrence++;
    }
    return desired_index == 0 ? first : NULL;
}

/// @brief Parse a vertex-attribute layer (e.g. LayerElementNormal/-UV) of
///   `components` floats per element from @p geom_node into *out. @p index_name_alt
///   is an optional fallback index-array name (or NULL). @p layer_index selects among
///   repeated FBX layer nodes, allowing UV1/vertex-color imports in addition to the
///   first normal/UV layer. All fields are zeroed when the layer is absent or malformed.
/// @param geom_node Geometry node that owns the requested layer.
/// @param layer_name Name of the `LayerElement*` child to select.
/// @param data_name Name of the child containing direct attribute values.
/// @param index_name Primary name of the optional index-array child.
/// @param index_name_alt Alternate index-array name, or NULL when none is accepted.
/// @param components Number of scalar components expected per direct element.
/// @param layer_index Zero-based layer number to select among repeated layer nodes.
/// @param[out] out Borrowed layer view, always initialized before return.
static void fbx_parse_vertex_layer(fbx_node_t *geom_node,
                                   const char *layer_name,
                                   const char *data_name,
                                   const char *index_name,
                                   const char *index_name_alt,
                                   int components,
                                   int layer_index,
                                   fbx_vertex_layer_t *out) {
    int direct_compatibility_layer = 0;
    memset(out, 0, sizeof(*out));
    fbx_node_t *layer = fbx_find_vertex_layer(geom_node, layer_name, layer_index);
    if (!layer && layer_index == 0 && fbx_find_child(geom_node, data_name)) {
        /* Preserve the compact ASCII form historically accepted by Zanna fixtures/exporters:
         * direct attribute arrays under Geometry are interpreted as an implicit layer. */
        layer = geom_node;
        direct_compatibility_layer = 1;
    }
    if (!layer)
        return;
    fbx_node_t *d_node = fbx_find_child(layer, data_name);
    out->components = components;
    if (d_node && d_node->prop_count >= 1 && d_node->props[0].type == 'd') {
        out->data = (double *)d_node->props[0].v.array.data;
        out->count = d_node->props[0].v.array.count / (uint32_t)components;
        if (out->count > (uint32_t)INT32_MAX) {
            out->data = NULL;
            out->count = 0;
        }
    } else if (d_node && d_node->prop_count >= 1 && d_node->props[0].type == 'f') {
        out->data32 = (float *)d_node->props[0].v.array.data;
        out->count = d_node->props[0].v.array.count / (uint32_t)components;
        if (out->count > (uint32_t)INT32_MAX) {
            out->data32 = NULL;
            out->count = 0;
        }
    }
    fbx_node_t *mm = fbx_find_child(layer, "MappingInformationType");
    if (mm && mm->prop_count >= 1) {
        const char *mapping = fbx_prop_str(mm, 0);
        out->by_polygon_vertex = fbx_ascii_equals_i(mapping, "ByPolygonVertex");
        out->by_polygon = fbx_ascii_equals_i(mapping, "ByPolygon");
        out->all_same = fbx_ascii_equals_i(mapping, "AllSame");
    }
    fbx_node_t *rm = fbx_find_child(layer, "ReferenceInformationType");
    if (rm && rm->prop_count >= 1)
        out->index_to_direct = fbx_ascii_equals_i(fbx_prop_str(rm, 0), "IndexToDirect") ||
                               fbx_ascii_equals_i(fbx_prop_str(rm, 0), "Index");
    if (out->index_to_direct) {
        fbx_node_t *ni = fbx_find_child(layer, index_name);
        if (!ni && index_name_alt)
            ni = fbx_find_child(layer, index_name_alt);
        if (ni && ni->prop_count >= 1 && ni->props[0].type == 'i') {
            out->indices = (int32_t *)ni->props[0].v.array.data;
            out->index_count = ni->props[0].v.array.count;
        }
    } else if (direct_compatibility_layer) {
        fbx_node_t *ni = fbx_find_child(layer, index_name);
        if (!ni && index_name_alt)
            ni = fbx_find_child(layer, index_name_alt);
        if (ni && ni->prop_count >= 1 && ni->props[0].type == 'i') {
            out->index_to_direct = 1;
            out->by_polygon_vertex = 1;
            out->indices = (int32_t *)ni->props[0].v.array.data;
            out->index_count = ni->props[0].v.array.count;
        }
    }
}

/// @brief Read one component from a parsed vertex layer as double.
/// @param layer Parsed layer.
/// @param element_index Direct element index.
/// @param component Component index within the element.
/// @param fallback Value returned when data is absent or invalid.
/// @return Finite component value or @p fallback.
static double fbx_vertex_layer_component(const fbx_vertex_layer_t *layer,
                                         int32_t element_index,
                                         int component,
                                         double fallback) {
    double value;
    if (!layer || element_index < 0 || element_index >= (int32_t)layer->count || component < 0 ||
        component >= layer->components)
        return fallback;
    if (layer->data)
        value = layer->data[(size_t)element_index * (size_t)layer->components + (size_t)component];
    else if (layer->data32)
        value = (double)layer
                    ->data32[(size_t)element_index * (size_t)layer->components + (size_t)component];
    else
        return fallback;
    return isfinite(value) ? value : fallback;
}

/// @brief Parse a geometry node's material-assignment layer.
/// @details The result borrows the material index array from the FBX tree and is
///          zero-initialized when the layer is absent or malformed.
/// @param geom_node Geometry node containing an optional `LayerElementMaterial`.
/// @param[out] out Material-layer view to initialize.
static void fbx_parse_material_layer(fbx_node_t *geom_node, fbx_material_layer_t *out) {
    memset(out, 0, sizeof(*out));
    fbx_node_t *mat_layer = fbx_find_child(geom_node, "LayerElementMaterial");
    if (!mat_layer)
        return;
    fbx_node_t *m_node = fbx_find_child(mat_layer, "Materials");
    fbx_node_t *mm = fbx_find_child(mat_layer, "MappingInformationType");
    if (mm && mm->prop_count >= 1) {
        const char *mapping = fbx_prop_str(mm, 0);
        out->by_polygon = fbx_ascii_equals_i(mapping, "ByPolygon");
        out->all_same = fbx_ascii_equals_i(mapping, "AllSame");
    }
    if (m_node && m_node->prop_count >= 1 && m_node->props[0].type == 'i' &&
        m_node->props[0].v.array.data) {
        out->slots = (int32_t *)m_node->props[0].v.array.data;
        out->slot_count = m_node->props[0].v.array.count;
        if (out->slot_count > (uint32_t)INT32_MAX) {
            out->slots = NULL;
            out->slot_count = 0;
        }
    }
}

/// @brief Resolve the direct value index for one polygon-vertex within a vertex
///   layer, honoring its mapping (ByPolygonVertex/ByPolygon/ByVertex) and optional
///   IndexToDirect indirection. Returns -1 when out of range or unmapped.
/// @param layer Parsed layer and mapping metadata.
/// @param polygon_vertex_idx Running polygon-vertex index across the index stream.
/// @param polygon_idx Zero-based polygon index.
/// @param vi Source control-vertex index for ByVertex mapping.
/// @return A valid direct-array element index, or -1 when the mapping cannot be resolved.
static int32_t fbx_resolve_layer_index(const fbx_vertex_layer_t *layer,
                                       int32_t polygon_vertex_idx,
                                       uint32_t polygon_idx,
                                       int32_t vi) {
    uint32_t source_index = layer->by_polygon_vertex ? (uint32_t)polygon_vertex_idx
                            : layer->by_polygon      ? polygon_idx
                            : layer->all_same        ? 0u
                                                     : (uint32_t)vi;
    int32_t idx;
    if (layer->index_to_direct) {
        if (layer->indices && source_index < layer->index_count)
            idx = layer->indices[source_index];
        else
            idx = -1;
    } else {
        idx = (int32_t)source_index;
    }
    if (idx >= 0 && idx < (int32_t)layer->count)
        return idx;
    return -1;
}

/// @brief Common geometry-extraction failure cleanup: free a heap-grown polygon
///   buffer, release the partially-built mesh, and return NULL.
/// @param polygon Current polygon buffer, which may alias @p polygon_stack.
/// @param polygon_stack Address of the caller's inline polygon buffer.
/// @param mesh Partially built runtime mesh to release when non-NULL.
/// @return Always NULL.
static void *fbx_geom_fail(int32_t *polygon, int32_t *polygon_stack, void *mesh) {
    if (polygon != polygon_stack)
        free(polygon);
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    return NULL;
}

/// @brief Parse the optional FBX vertex attribute layers (normal, UV0/UV1, color) and the
///        material assignment for a geometry node into the caller-provided structs.
/// @param geom_node Geometry node containing the optional layer elements.
/// @param[out] norm_layer Receives the normal layer view.
/// @param[out] uv_layer Receives the primary texture-coordinate layer view.
/// @param[out] uv1_layer Receives the secondary texture-coordinate layer view.
/// @param[out] color_layer Receives the vertex-color layer view.
/// @param[out] mat Receives the material-assignment layer view.
static void fbx_parse_geometry_layers(fbx_node_t *geom_node,
                                      fbx_vertex_layer_t *norm_layer,
                                      fbx_vertex_layer_t *uv_layer,
                                      fbx_vertex_layer_t *uv1_layer,
                                      fbx_vertex_layer_t *color_layer,
                                      fbx_material_layer_t *mat) {
    fbx_parse_vertex_layer(geom_node,
                           "LayerElementNormal",
                           "Normals",
                           "NormalsIndex",
                           "NormalIndex",
                           3,
                           0,
                           norm_layer);
    fbx_parse_vertex_layer(geom_node, "LayerElementUV", "UV", "UVIndex", NULL, 2, 0, uv_layer);
    fbx_parse_vertex_layer(geom_node, "LayerElementUV", "UV", "UVIndex", NULL, 2, 1, uv1_layer);
    fbx_parse_vertex_layer(
        geom_node, "LayerElementColor", "Colors", "ColorIndex", "ColorIndex", 4, 0, color_layer);
    fbx_parse_material_layer(geom_node, mat);
}

#define FBX_SURFACE_MAX_STEP 256
#define FBX_SURFACE_DENOM_EPSILON 1e-12

/// @brief Topological closure mode for one parametric NURBS surface axis.
typedef enum {
    /// The axis has distinct start and end boundaries.
    FBX_SURFACE_OPEN = 0,
    /// The axis closes at its boundary without periodic control repetition.
    FBX_SURFACE_CLOSED = 1,
    /// The axis is periodic across its parameter domain.
    FBX_SURFACE_PERIODIC = 2,
} fbx_surface_form_t;

/// @brief Supported FBX patch basis families.
typedef enum {
    /// Cubic Bernstein/Bezier basis with four controls per span.
    FBX_PATCH_BEZIER = 0,
    /// Quadratic Bernstein/Bezier basis with three controls per span.
    FBX_PATCH_BEZIER_QUADRATIC = 1,
    /// Cubic cardinal-spline basis.
    FBX_PATCH_CARDINAL = 2,
    /// Uniform cubic B-spline basis.
    FBX_PATCH_BSPLINE = 3,
    /// Piecewise-linear basis with two controls per span.
    FBX_PATCH_LINEAR = 4,
} fbx_patch_basis_t;

/// @brief Sparse basis values for every generated sample along one surface axis.
typedef struct {
    /// Number of sampled parameter positions.
    int32_t sample_count;
    /// Number of tessellated line segments along the axis.
    int32_t segment_count;
    /// Fixed maximum number of contributing controls per sample.
    int32_t support;
    /// Non-zero when the last sample reconnects to the first.
    int closed;
    /// Flattened control indices, with @ref support entries per sample.
    int32_t *indices;
    /// Flattened normalized basis weights parallel to @ref indices.
    double *weights;
    /// Flattened analytic basis derivatives parallel to @ref indices.
    double *derivatives;
} fbx_surface_axis_samples_t;

/// @brief Release all heap storage owned by one sampled surface axis and reset it.
/// @param[in,out] axis Sample table to clear; NULL is accepted as a no-op.
static void fbx_surface_axis_samples_free(fbx_surface_axis_samples_t *axis) {
    if (!axis)
        return;
    free(axis->indices);
    free(axis->weights);
    free(axis->derivatives);
    memset(axis, 0, sizeof(*axis));
}

/// @brief Report a specific malformed procedural-surface diagnostic.
/// @param message Stable user-facing error text.
/// @return Zero for direct use in failure branches.
static int fbx_surface_error(const char *message) {
    rt_asset_error_set(RT_ASSET_ERROR_CORRUPT, message);
    return 0;
}

/// @brief Test whether an FBX property uses one of the supported scalar numeric encodings.
/// @param property Property to inspect; may be NULL.
/// @return Non-zero for integer, boolean, float, or double scalar encodings; otherwise zero.
static int fbx_surface_property_is_number(const fbx_prop_t *property) {
    if (!property)
        return 0;
    return property->type == 'Y' || property->type == 'I' || property->type == 'L' ||
           property->type == 'C' || property->type == 'F' || property->type == 'D';
}

/// @brief Read an exact pair of numeric scalar properties from a named child node.
/// @param geometry Geometry node containing the pair.
/// @param name Name of the required child node.
/// @param[out] out_values Receives the two values converted to signed 64-bit integers.
/// @return Non-zero when the child has exactly two numeric properties; otherwise zero.
static int fbx_surface_read_pair_i64(fbx_node_t *geometry,
                                     const char *name,
                                     int64_t out_values[2]) {
    fbx_node_t *child = fbx_find_child(geometry, name);
    if (!child || child->prop_count != 2 || !fbx_surface_property_is_number(&child->props[0]) ||
        !fbx_surface_property_is_number(&child->props[1]))
        return 0;
    out_values[0] = fbx_prop_i64(child, 0);
    out_values[1] = fbx_prop_i64(child, 1);
    return 1;
}

/// @brief Read and validate one floating-point array element as a double.
/// @param property FBX array property encoded as floats or doubles.
/// @param index Zero-based element index.
/// @param[out] out_value Receives the finite converted value.
/// @return Non-zero on success, or zero for an invalid property, type, index, or value.
static int fbx_surface_array_number(const fbx_prop_t *property, uint32_t index, double *out_value) {
    double value;
    if (!property || !out_value || !property->v.array.data || index >= property->v.array.count)
        return 0;
    if (property->type == 'd')
        value = ((const double *)property->v.array.data)[index];
    else if (property->type == 'f')
        value = (double)((const float *)property->v.array.data)[index];
    else
        return 0;
    if (!isfinite(value))
        return 0;
    *out_value = value;
    return 1;
}

/// @brief Parse one NURBS surface-form lane from text or the FBX SDK enum encoding.
/// @param form_node Node whose properties encode the U and V forms.
/// @param index Property lane to parse.
/// @param[out] out_form Receives the normalized surface form.
/// @return Non-zero when the lane contains a recognized form; otherwise zero.
static int fbx_surface_parse_form(fbx_node_t *form_node, int index, fbx_surface_form_t *out_form) {
    fbx_prop_t *property;
    if (!form_node || !out_form || index < 0 || index >= form_node->prop_count)
        return 0;
    property = &form_node->props[index];
    if (property->type == 'S') {
        const char *text = fbx_prop_str(form_node, index);
        if (fbx_ascii_equals_i(text, "Open"))
            *out_form = FBX_SURFACE_OPEN;
        else if (fbx_ascii_equals_i(text, "Closed"))
            *out_form = FBX_SURFACE_CLOSED;
        else if (fbx_ascii_equals_i(text, "Periodic"))
            *out_form = FBX_SURFACE_PERIODIC;
        else
            return 0;
        return 1;
    }
    if (fbx_surface_property_is_number(property)) {
        /* FbxNurbsSurface::EType is ePeriodic=0, eClosed=1, eOpen=2. */
        int64_t value = fbx_prop_i64(form_node, index);
        if (value == 0)
            *out_form = FBX_SURFACE_PERIODIC;
        else if (value == 1)
            *out_form = FBX_SURFACE_CLOSED;
        else if (value == 2)
            *out_form = FBX_SURFACE_OPEN;
        else
            return 0;
        return 1;
    }
    return 0;
}

/// @brief Parse one FBX patch-basis lane from text or the FBX SDK enum encoding.
/// @param type_node Node whose properties encode the U and V basis types.
/// @param index Property lane to parse.
/// @param[out] out_basis Receives the normalized patch basis.
/// @return Non-zero when the lane contains a supported basis; otherwise zero.
static int fbx_surface_parse_patch_basis(fbx_node_t *type_node,
                                         int index,
                                         fbx_patch_basis_t *out_basis) {
    fbx_prop_t *property;
    if (!type_node || !out_basis || index < 0 || index >= type_node->prop_count)
        return 0;
    property = &type_node->props[index];
    if (property->type == 'S') {
        const char *text = fbx_prop_str(type_node, index);
        if (fbx_ascii_equals_i(text, "Bezier"))
            *out_basis = FBX_PATCH_BEZIER;
        else if (fbx_ascii_equals_i(text, "BezierQuadric") ||
                 fbx_ascii_equals_i(text, "BezierQuadratic") ||
                 fbx_ascii_equals_i(text, "QuadraticBezier"))
            *out_basis = FBX_PATCH_BEZIER_QUADRATIC;
        else if (fbx_ascii_equals_i(text, "Cardinal"))
            *out_basis = FBX_PATCH_CARDINAL;
        else if (fbx_ascii_equals_i(text, "BSpline") || fbx_ascii_equals_i(text, "B-Spline"))
            *out_basis = FBX_PATCH_BSPLINE;
        else if (fbx_ascii_equals_i(text, "Linear"))
            *out_basis = FBX_PATCH_LINEAR;
        else
            return 0;
        return 1;
    }
    if (fbx_surface_property_is_number(property)) {
        int64_t value = fbx_prop_i64(type_node, index);
        if (value < FBX_PATCH_BEZIER || value > FBX_PATCH_LINEAR)
            return 0;
        *out_basis = (fbx_patch_basis_t)value;
        return 1;
    }
    return 0;
}

/// @brief Resolve and validate the authored U/V subdivision counts.
/// @details Values below one are normalized to one; values above
///          @ref FBX_SURFACE_MAX_STEP are rejected with an asset error.
/// @param authored Two authored subdivision counts.
/// @param[out] out_steps Receives validated positive U/V step counts.
/// @return Non-zero on success, or zero when either authored count exceeds the limit.
static int fbx_surface_validate_steps(const int64_t authored[2], int32_t out_steps[2]) {
    for (int axis = 0; axis < 2; ++axis) {
        if (authored[axis] > FBX_SURFACE_MAX_STEP)
            return fbx_surface_error("FBX: procedural surface step exceeds 256 subdivisions");
        out_steps[axis] = authored[axis] < 1 ? 1 : (int32_t)authored[axis];
    }
    return 1;
}

/// @brief Load and normalize an exact four-lane control net.
/// @details FBX NURBS points store Euclidean xyz plus a rational weight. Patch points use the
///          same four-lane representation but do not require a nonzero weight. Position
///          coordinates are sanitized and optionally converted from Z-up to Y-up.
/// @param geometry Procedural geometry node containing `Points` or `Vertices`.
/// @param count_u Number of controls along the U axis.
/// @param count_v Number of controls along the V axis.
/// @param rational Non-zero when control weights must be nonzero.
/// @param z_up Non-zero to apply FBX Z-up coordinate conversion.
/// @param[in,out] context Loader context charged for the normalized control buffer.
/// @return Newly allocated array of `count_u * count_v * 4` doubles, or NULL for
///         malformed input, limit/budget rejection, or allocation failure.
static double *fbx_surface_load_control_net(fbx_node_t *geometry,
                                            int32_t count_u,
                                            int32_t count_v,
                                            int rational,
                                            int z_up,
                                            fbx_load_context_t *context) {
    fbx_node_t *points_node = fbx_find_child(geometry, "Points");
    uint64_t control_count;
    uint64_t component_count;
    double *controls;
    if (!points_node)
        points_node = fbx_find_child(geometry, "Vertices");
    if (!points_node || points_node->prop_count != 1 || count_u <= 0 || count_v <= 0)
        return NULL;
    if ((uint64_t)count_u > UINT64_MAX / (uint64_t)count_v)
        return NULL;
    control_count = (uint64_t)count_u * (uint64_t)count_v;
    if (control_count > INT32_MAX || control_count > UINT64_MAX / 4u)
        return NULL;
    component_count = control_count * 4u;
    if ((points_node->props[0].type != 'd' && points_node->props[0].type != 'f') ||
        points_node->props[0].v.array.count != component_count ||
        !rt_untrusted_count_ok(
            (int64_t)component_count, sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !fbx_budget_charge(context, component_count, sizeof(double)))
        return NULL;
    controls = (double *)malloc((size_t)component_count * sizeof(*controls));
    if (!controls)
        return NULL;
    for (uint64_t control = 0; control < control_count; ++control) {
        double x, y, z, weight;
        uint32_t offset = (uint32_t)(control * 4u);
        if (!fbx_surface_array_number(&points_node->props[0], offset + 0u, &x) ||
            !fbx_surface_array_number(&points_node->props[0], offset + 1u, &y) ||
            !fbx_surface_array_number(&points_node->props[0], offset + 2u, &z) ||
            !fbx_surface_array_number(&points_node->props[0], offset + 3u, &weight) ||
            !fbx_sanitize_position3(&x, &y, &z) ||
            (rational && fabs(weight) <= FBX_SURFACE_DENOM_EPSILON)) {
            free(controls);
            return NULL;
        }
        if (z_up)
            fbx_correct_zup(&x, &y, &z);
        controls[offset + 0u] = x;
        controls[offset + 1u] = y;
        controls[offset + 2u] = z;
        controls[offset + 3u] = weight;
    }
    return controls;
}

/// @brief Copy and validate one NURBS knot vector, or synthesize a deterministic default.
/// @details An authored vector must contain exactly `control_count + order` finite,
///          nondecreasing values. Missing open-form vectors are clamped at their ends;
///          missing closed/periodic vectors use uniform integer knots.
/// @param geometry NURBS geometry node containing the optional vector.
/// @param name Child-node name for the requested U or V knot vector.
/// @param control_count Number of controls along the axis.
/// @param order NURBS order, which must be between two and @p control_count.
/// @param form Axis closure form used when synthesizing missing knots.
/// @param[in,out] context Loader context charged for the copied vector.
/// @return Newly allocated knot vector, or NULL for invalid dimensions, malformed
///         authored knots, limit/budget rejection, or allocation failure.
static double *fbx_surface_load_knots(fbx_node_t *geometry,
                                      const char *name,
                                      int32_t control_count,
                                      int32_t order,
                                      fbx_surface_form_t form,
                                      fbx_load_context_t *context) {
    fbx_node_t *knot_node = fbx_find_child(geometry, name);
    int32_t knot_count;
    double *knots;
    if (control_count <= 0 || order < 2 || order > control_count ||
        control_count > INT32_MAX - order)
        return NULL;
    knot_count = control_count + order;
    if (!rt_untrusted_count_ok(knot_count, sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !fbx_budget_charge(context, (uint64_t)knot_count, sizeof(double)))
        return NULL;
    knots = (double *)malloc((size_t)knot_count * sizeof(*knots));
    if (!knots)
        return NULL;
    if (knot_node) {
        if (knot_node->prop_count != 1 ||
            (knot_node->props[0].type != 'd' && knot_node->props[0].type != 'f') ||
            knot_node->props[0].v.array.count != (uint32_t)knot_count) {
            free(knots);
            return NULL;
        }
        for (int32_t i = 0; i < knot_count; ++i) {
            if (!fbx_surface_array_number(&knot_node->props[0], (uint32_t)i, &knots[i]) ||
                (i > 0 && knots[i] < knots[i - 1])) {
                free(knots);
                return NULL;
            }
        }
    } else if (form == FBX_SURFACE_OPEN) {
        int32_t span_count = control_count - order + 1;
        for (int32_t i = 0; i < knot_count; ++i) {
            if (i < order)
                knots[i] = 0.0;
            else if (i >= control_count)
                knots[i] = 1.0;
            else
                knots[i] = (double)(i - order + 1) / (double)span_count;
        }
    } else {
        for (int32_t i = 0; i < knot_count; ++i)
            knots[i] = (double)i;
    }
    if (!isfinite(knots[order - 1]) || !isfinite(knots[control_count]) ||
        knots[control_count] <= knots[order - 1]) {
        free(knots);
        return NULL;
    }
    return knots;
}

/// @brief Allocate fixed-width sparse basis tables for one sampled surface axis.
/// @details On success every control index is initialized to -1 and the weight and
///          derivative arrays are zeroed. On partial allocation failure @p axis is reset.
/// @param[out] axis Sample table to initialize.
/// @param sample_count Number of parameter positions that will be stored.
/// @param segment_count Number of tessellated segments represented by the table.
/// @param support Number of control contributions stored per sample.
/// @param closed Non-zero when the axis reconnects its final segment to its first sample.
/// @param[in,out] context Loader context charged for all three flattened arrays.
/// @return Non-zero on success, or zero for invalid dimensions, overflow,
///         limit/budget rejection, or allocation failure.
static int fbx_surface_axis_allocate(fbx_surface_axis_samples_t *axis,
                                     int32_t sample_count,
                                     int32_t segment_count,
                                     int32_t support,
                                     int closed,
                                     fbx_load_context_t *context) {
    uint64_t contribution_count;
    if (!axis || sample_count <= 0 || segment_count <= 0 || support <= 0 ||
        (uint64_t)sample_count > UINT64_MAX / (uint64_t)support)
        return 0;
    contribution_count = (uint64_t)sample_count * (uint64_t)support;
    if (contribution_count > SIZE_MAX / sizeof(*axis->indices) ||
        contribution_count > SIZE_MAX / sizeof(*axis->weights) ||
        contribution_count > SIZE_MAX / sizeof(*axis->derivatives) ||
        !rt_untrusted_count_ok((int64_t)contribution_count,
                               sizeof(*axis->indices),
                               FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !rt_untrusted_count_ok((int64_t)contribution_count,
                               sizeof(*axis->weights),
                               FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !rt_untrusted_count_ok((int64_t)contribution_count,
                               sizeof(*axis->derivatives),
                               FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !fbx_budget_charge(context, contribution_count, sizeof(*axis->indices)) ||
        !fbx_budget_charge(context, contribution_count, sizeof(*axis->weights)) ||
        !fbx_budget_charge(context, contribution_count, sizeof(*axis->derivatives)))
        return 0;
    memset(axis, 0, sizeof(*axis));
    axis->indices = (int32_t *)malloc((size_t)contribution_count * sizeof(*axis->indices));
    axis->weights = (double *)calloc((size_t)contribution_count, sizeof(*axis->weights));
    axis->derivatives = (double *)calloc((size_t)contribution_count, sizeof(*axis->derivatives));
    if (!axis->indices || !axis->weights || !axis->derivatives) {
        fbx_surface_axis_samples_free(axis);
        return 0;
    }
    for (uint64_t i = 0; i < contribution_count; ++i)
        axis->indices[i] = -1;
    axis->sample_count = sample_count;
    axis->segment_count = segment_count;
    axis->support = support;
    axis->closed = closed;
    return 1;
}

/// @brief Evaluate Cox-de Boor basis values of one order for a known knot span.
/// @param knots Valid nondecreasing knot vector.
/// @param order Number of basis values to produce.
/// @param span Knot span containing @p parameter.
/// @param parameter Parameter value to evaluate.
/// @param[out] basis Scratch/output array with room for @p order values.
/// @param[out] left Scratch array with room for @p order values.
/// @param[out] right Scratch array with room for @p order values.
/// @return Non-zero when finite basis values were evaluated, or zero for invalid inputs.
static int fbx_surface_nurbs_basis_values(const double *knots,
                                          int32_t order,
                                          int32_t span,
                                          double parameter,
                                          double *basis,
                                          double *left,
                                          double *right) {
    int32_t degree = order - 1;
    if (!knots || !basis || !left || !right || order < 1 || span < degree || !isfinite(parameter))
        return 0;
    memset(basis, 0, (size_t)order * sizeof(*basis));
    basis[0] = 1.0;
    for (int32_t j = 1; j <= degree; ++j) {
        double saved = 0.0;
        left[j] = parameter - knots[span + 1 - j];
        right[j] = knots[span + j] - parameter;
        for (int32_t r = 0; r < j; ++r) {
            double denominator = right[r + 1] + left[j - r];
            double temporary =
                fabs(denominator) > FBX_SURFACE_DENOM_EPSILON ? basis[r] / denominator : 0.0;
            basis[r] = saved + right[r + 1] * temporary;
            saved = left[j - r] * temporary;
        }
        basis[j] = saved;
    }
    return 1;
}

/// @brief Evaluate normalized nonzero NURBS basis functions and analytic derivatives.
/// @details The result describes exactly @p order consecutive controls for @p span.
///          Negligible values are canonicalized to zero, and weights are normalized
///          to sum to one before derivatives are evaluated.
/// @param knots Valid nondecreasing knot vector.
/// @param control_count Number of controls along the axis.
/// @param order NURBS order and number of output contributions.
/// @param span Knot span containing @p parameter.
/// @param parameter Parameter value to evaluate.
/// @param[out] out_indices Receives @p order control indices.
/// @param[out] out_weights Receives @p order normalized basis weights.
/// @param[out] out_derivatives Receives @p order analytic derivatives.
/// @param[out] basis Scratch array with room for @p order values.
/// @param[out] left Scratch array with room for @p order values.
/// @param[out] right Scratch array with room for @p order values.
/// @return Non-zero on success, or zero for an invalid span, non-finite/degenerate
///         basis, or invalid output storage.
static int fbx_surface_nurbs_basis(const double *knots,
                                   int32_t control_count,
                                   int32_t order,
                                   int32_t span,
                                   double parameter,
                                   int32_t *out_indices,
                                   double *out_weights,
                                   double *out_derivatives,
                                   double *basis,
                                   double *left,
                                   double *right) {
    int32_t degree = order - 1;
    double sum = 0.0;
    if (!knots || !out_indices || !out_weights || !out_derivatives || !basis || !left || !right ||
        order < 2 || span < degree || span >= control_count ||
        !fbx_surface_nurbs_basis_values(knots, order, span, parameter, basis, left, right))
        return 0;
    for (int32_t i = 0; i < order; ++i) {
        int32_t control_index = span - degree + i;
        if (control_index < 0 || control_index >= control_count || !isfinite(basis[i]))
            return 0;
        out_indices[i] = control_index;
        out_weights[i] = fabs(basis[i]) <= 1e-15 ? 0.0 : basis[i];
        sum += out_weights[i];
    }
    if (!isfinite(sum) || fabs(sum) <= FBX_SURFACE_DENOM_EPSILON)
        return 0;
    for (int32_t i = 0; i < order; ++i)
        out_weights[i] /= sum;
    if (!fbx_surface_nurbs_basis_values(knots, order - 1, span, parameter, basis, left, right))
        return 0;
    for (int32_t i = 0; i < order; ++i) {
        int32_t control_index = out_indices[i];
        double first = i > 0 ? basis[i - 1] : 0.0;
        double second = i < degree ? basis[i] : 0.0;
        double first_denominator = knots[control_index + degree] - knots[control_index];
        double second_denominator = knots[control_index + degree + 1] - knots[control_index + 1];
        double derivative = 0.0;
        if (fabs(first_denominator) > FBX_SURFACE_DENOM_EPSILON)
            derivative += (double)degree * first / first_denominator;
        if (fabs(second_denominator) > FBX_SURFACE_DENOM_EPSILON)
            derivative -= (double)degree * second / second_denominator;
        if (!isfinite(derivative))
            return 0;
        out_derivatives[i] = fabs(derivative) <= 1e-15 ? 0.0 : derivative;
    }
    return 1;
}

/// @brief Sample a complete NURBS axis at the authored subdivision frequency.
/// @details Empty knot spans are skipped. Open axes include the terminal parameter
///          sample, while closed/periodic axes omit the duplicate endpoint.
/// @param knots Valid nondecreasing knot vector.
/// @param control_count Number of controls along the axis.
/// @param order NURBS order.
/// @param step Number of subdivisions generated per nonempty knot span.
/// @param form Axis closure form.
/// @param[in,out] context Loader context charged for output and scratch storage.
/// @param[out] out_axis Receives the allocated sparse sample table.
/// @return Non-zero on success, or zero for invalid topology, evaluation failure,
///         limit/budget rejection, or allocation failure.
static int fbx_surface_build_nurbs_axis(const double *knots,
                                        int32_t control_count,
                                        int32_t order,
                                        int32_t step,
                                        fbx_surface_form_t form,
                                        fbx_load_context_t *context,
                                        fbx_surface_axis_samples_t *out_axis) {
    int32_t degree = order - 1;
    int32_t nonempty_spans = 0;
    int64_t segment_count64;
    int64_t sample_count64;
    int32_t sample_index = 0;
    double *scratch;
    double *basis;
    double *left;
    double *right;
    int closed = form != FBX_SURFACE_OPEN;
    if (!knots || !out_axis || control_count <= 0 || order < 2 || order > control_count ||
        step < 1 || step > FBX_SURFACE_MAX_STEP)
        return 0;
    for (int32_t span = degree; span < control_count; ++span) {
        if (!isfinite(knots[span]) || !isfinite(knots[span + 1]) || knots[span + 1] < knots[span])
            return 0;
        if (knots[span + 1] > knots[span])
            nonempty_spans++;
    }
    if (nonempty_spans <= 0)
        return 0;
    segment_count64 = (int64_t)nonempty_spans * (int64_t)step;
    sample_count64 = segment_count64 + (closed ? 0 : 1);
    if (segment_count64 <= 0 || segment_count64 > INT32_MAX || sample_count64 <= 1 ||
        sample_count64 > INT32_MAX ||
        !fbx_surface_axis_allocate(
            out_axis, (int32_t)sample_count64, (int32_t)segment_count64, order, closed, context) ||
        !fbx_budget_charge(context, (uint64_t)order * 3u, sizeof(double)))
        return 0;
    scratch = (double *)calloc((size_t)order * 3u, sizeof(*scratch));
    if (!scratch) {
        fbx_surface_axis_samples_free(out_axis);
        return 0;
    }
    basis = scratch;
    left = scratch + order;
    right = scratch + order * 2;
    for (int32_t span = degree; span < control_count; ++span) {
        double start = knots[span];
        double end = knots[span + 1];
        if (end <= start)
            continue;
        for (int32_t subdivision = 0; subdivision < step; ++subdivision) {
            double parameter = start + (end - start) * ((double)subdivision / (double)step);
            size_t offset = (size_t)sample_index * (size_t)order;
            if (!fbx_surface_nurbs_basis(knots,
                                         control_count,
                                         order,
                                         span,
                                         parameter,
                                         out_axis->indices + offset,
                                         out_axis->weights + offset,
                                         out_axis->derivatives + offset,
                                         basis,
                                         left,
                                         right)) {
                free(scratch);
                fbx_surface_axis_samples_free(out_axis);
                return 0;
            }
            sample_index++;
        }
    }
    if (!closed) {
        int32_t span = control_count - 1;
        size_t offset = (size_t)sample_index * (size_t)order;
        if (!fbx_surface_nurbs_basis(knots,
                                     control_count,
                                     order,
                                     span,
                                     knots[control_count],
                                     out_axis->indices + offset,
                                     out_axis->weights + offset,
                                     out_axis->derivatives + offset,
                                     basis,
                                     left,
                                     right)) {
            free(scratch);
            fbx_surface_axis_samples_free(out_axis);
            return 0;
        }
        sample_index++;
    }
    free(scratch);
    if (sample_index != out_axis->sample_count) {
        fbx_surface_axis_samples_free(out_axis);
        return 0;
    }
    return 1;
}

/// @brief Wrap a signed control index into one closed patch axis.
/// @param index Possibly negative or overflowing control index.
/// @param count Positive number of controls in the axis.
/// @return The equivalent index in `[0, count)`, or -1 when @p count is not positive.
static int32_t fbx_surface_wrap_index(int32_t index, int32_t count) {
    int32_t wrapped;
    if (count <= 0)
        return -1;
    wrapped = index % count;
    return wrapped < 0 ? wrapped + count : wrapped;
}

/// @brief Validate a patch axis and derive its span and basis-support counts.
/// @param basis Patch basis family.
/// @param control_count Number of authored controls along the axis.
/// @param closed Non-zero when the patch axis wraps.
/// @param[out] out_span_count Receives the number of evaluable basis spans.
/// @param[out] out_support Receives the number of control contributions per sample.
/// @return Non-zero for a valid basis/control layout, or zero when the basis is
///         unsupported or its required control relationship is not satisfied.
static int fbx_surface_patch_layout(fbx_patch_basis_t basis,
                                    int32_t control_count,
                                    int closed,
                                    int32_t *out_span_count,
                                    int32_t *out_support) {
    int32_t spans = 0;
    int32_t support = 0;
    if (!out_span_count || !out_support || control_count <= 0)
        return 0;
    switch (basis) {
        case FBX_PATCH_LINEAR:
            if (control_count < 2)
                return 0;
            spans = closed ? control_count : control_count - 1;
            support = 2;
            break;
        case FBX_PATCH_BEZIER:
            support = 4;
            if (closed) {
                if (control_count < 3 || control_count % 3 != 0)
                    return 0;
                spans = control_count / 3;
            } else {
                if (control_count < 4 || (control_count - 1) % 3 != 0)
                    return 0;
                spans = (control_count - 1) / 3;
            }
            break;
        case FBX_PATCH_BEZIER_QUADRATIC:
            support = 3;
            if (closed) {
                if (control_count < 2 || control_count % 2 != 0)
                    return 0;
                spans = control_count / 2;
            } else {
                if (control_count < 3 || (control_count - 1) % 2 != 0)
                    return 0;
                spans = (control_count - 1) / 2;
            }
            break;
        case FBX_PATCH_CARDINAL:
        case FBX_PATCH_BSPLINE:
            if (control_count < 4)
                return 0;
            spans = closed ? control_count : control_count - 3;
            support = 4;
            break;
        default:
            return 0;
    }
    if (spans <= 0)
        return 0;
    *out_span_count = spans;
    *out_support = support;
    return 1;
}

/// @brief Evaluate sparse control indices, basis values, and derivatives for a patch sample.
/// @param basis Patch basis family to evaluate.
/// @param control_count Number of controls along the axis.
/// @param closed Non-zero to wrap support indices at the axis boundary.
/// @param span Zero-based patch span.
/// @param t Local span parameter in the inclusive range `[0, 1]`.
/// @param[out] out_indices Receives the contributing control indices.
/// @param[out] out_weights Receives basis weights for those controls.
/// @param[out] out_derivatives Receives derivatives with respect to @p t.
/// @return Non-zero on success, or zero for an unsupported basis, invalid parameter,
///         output storage, or out-of-range control contribution.
static int fbx_surface_patch_basis(fbx_patch_basis_t basis,
                                   int32_t control_count,
                                   int closed,
                                   int32_t span,
                                   double t,
                                   int32_t *out_indices,
                                   double *out_weights,
                                   double *out_derivatives) {
    int32_t first = 0;
    int32_t support = 0;
    double one_minus_t = 1.0 - t;
    if (!out_indices || !out_weights || !out_derivatives || !isfinite(t) || t < 0.0 || t > 1.0)
        return 0;
    switch (basis) {
        case FBX_PATCH_LINEAR:
            support = 2;
            first = span;
            out_weights[0] = one_minus_t;
            out_weights[1] = t;
            out_derivatives[0] = -1.0;
            out_derivatives[1] = 1.0;
            break;
        case FBX_PATCH_BEZIER:
            support = 4;
            first = span * 3;
            out_weights[0] = one_minus_t * one_minus_t * one_minus_t;
            out_weights[1] = 3.0 * t * one_minus_t * one_minus_t;
            out_weights[2] = 3.0 * t * t * one_minus_t;
            out_weights[3] = t * t * t;
            out_derivatives[0] = -3.0 * one_minus_t * one_minus_t;
            out_derivatives[1] = 3.0 * one_minus_t * one_minus_t - 6.0 * t * one_minus_t;
            out_derivatives[2] = 6.0 * t * one_minus_t - 3.0 * t * t;
            out_derivatives[3] = 3.0 * t * t;
            break;
        case FBX_PATCH_BEZIER_QUADRATIC:
            support = 3;
            first = span * 2;
            out_weights[0] = one_minus_t * one_minus_t;
            out_weights[1] = 2.0 * t * one_minus_t;
            out_weights[2] = t * t;
            out_derivatives[0] = -2.0 * one_minus_t;
            out_derivatives[1] = 2.0 - 4.0 * t;
            out_derivatives[2] = 2.0 * t;
            break;
        case FBX_PATCH_CARDINAL: {
            double t2 = t * t;
            double t3 = t2 * t;
            support = 4;
            first = closed ? span - 1 : span;
            out_weights[0] = -0.5 * t + t2 - 0.5 * t3;
            out_weights[1] = 1.0 - 2.5 * t2 + 1.5 * t3;
            out_weights[2] = 0.5 * t + 2.0 * t2 - 1.5 * t3;
            out_weights[3] = -0.5 * t2 + 0.5 * t3;
            out_derivatives[0] = -0.5 + 2.0 * t - 1.5 * t2;
            out_derivatives[1] = -5.0 * t + 4.5 * t2;
            out_derivatives[2] = 0.5 + 4.0 * t - 4.5 * t2;
            out_derivatives[3] = -t + 1.5 * t2;
            break;
        }
        case FBX_PATCH_BSPLINE: {
            double t2 = t * t;
            double t3 = t2 * t;
            support = 4;
            first = closed ? span - 1 : span;
            out_weights[0] = one_minus_t * one_minus_t * one_minus_t / 6.0;
            out_weights[1] = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
            out_weights[2] = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
            out_weights[3] = t3 / 6.0;
            out_derivatives[0] = -0.5 * one_minus_t * one_minus_t;
            out_derivatives[1] = (9.0 * t2 - 12.0 * t) / 6.0;
            out_derivatives[2] = (-9.0 * t2 + 6.0 * t + 3.0) / 6.0;
            out_derivatives[3] = 0.5 * t2;
            break;
        }
        default:
            return 0;
    }
    for (int32_t i = 0; i < support; ++i) {
        int32_t index = first + i;
        if (closed)
            index = fbx_surface_wrap_index(index, control_count);
        if (index < 0 || index >= control_count || !isfinite(out_weights[i]) ||
            !isfinite(out_derivatives[i]))
            return 0;
        out_indices[i] = index;
        if (fabs(out_weights[i]) <= 1e-15)
            out_weights[i] = 0.0;
        if (fabs(out_derivatives[i]) <= 1e-15)
            out_derivatives[i] = 0.0;
    }
    return 1;
}

/// @brief Sample a complete FBX patch axis at its authored subdivision frequency.
/// @details Open axes include their final endpoint; closed axes omit that duplicate
///          sample because generated topology reconnects to the first sample.
/// @param basis Patch basis family.
/// @param control_count Number of controls along the axis.
/// @param step Number of subdivisions generated per patch span.
/// @param closed Non-zero when the axis wraps.
/// @param[in,out] context Loader context charged for the output sample table.
/// @param[out] out_axis Receives the allocated sparse samples.
/// @return Non-zero on success, or zero for invalid topology, evaluation failure,
///         limit/budget rejection, or allocation failure.
static int fbx_surface_build_patch_axis(fbx_patch_basis_t basis,
                                        int32_t control_count,
                                        int32_t step,
                                        int closed,
                                        fbx_load_context_t *context,
                                        fbx_surface_axis_samples_t *out_axis) {
    int32_t span_count;
    int32_t support;
    int64_t segment_count64;
    int64_t sample_count64;
    int32_t sample_index = 0;
    if (!out_axis || step < 1 || step > FBX_SURFACE_MAX_STEP ||
        !fbx_surface_patch_layout(basis, control_count, closed, &span_count, &support))
        return 0;
    segment_count64 = (int64_t)span_count * (int64_t)step;
    sample_count64 = segment_count64 + (closed ? 0 : 1);
    if (segment_count64 <= 0 || segment_count64 > INT32_MAX || sample_count64 <= 1 ||
        sample_count64 > INT32_MAX ||
        !fbx_surface_axis_allocate(
            out_axis, (int32_t)sample_count64, (int32_t)segment_count64, support, closed, context))
        return 0;
    for (int32_t span = 0; span < span_count; ++span) {
        for (int32_t subdivision = 0; subdivision < step; ++subdivision) {
            double t = (double)subdivision / (double)step;
            size_t offset = (size_t)sample_index * (size_t)support;
            if (!fbx_surface_patch_basis(basis,
                                         control_count,
                                         closed,
                                         span,
                                         t,
                                         out_axis->indices + offset,
                                         out_axis->weights + offset,
                                         out_axis->derivatives + offset)) {
                fbx_surface_axis_samples_free(out_axis);
                return 0;
            }
            sample_index++;
        }
    }
    if (!closed) {
        size_t offset = (size_t)sample_index * (size_t)support;
        if (!fbx_surface_patch_basis(basis,
                                     control_count,
                                     0,
                                     span_count - 1,
                                     1.0,
                                     out_axis->indices + offset,
                                     out_axis->weights + offset,
                                     out_axis->derivatives + offset)) {
            fbx_surface_axis_samples_free(out_axis);
            return 0;
        }
        sample_index++;
    }
    if (sample_index != out_axis->sample_count) {
        fbx_surface_axis_samples_free(out_axis);
        return 0;
    }
    return 1;
}

/// @brief Test whether an FBX Geometry subtype can be converted to a runtime triangle mesh.
/// @param geometry_type FBX geometry subtype string.
/// @return Non-zero for polygon meshes, NURBS surfaces, and patches; otherwise zero.
static int fbx_geometry_type_is_renderable(const char *geometry_type) {
    return geometry_type &&
           (strcmp(geometry_type, "Mesh") == 0 || strcmp(geometry_type, "NurbsSurface") == 0 ||
            strcmp(geometry_type, "Nurb") == 0 || strcmp(geometry_type, "Nurbs") == 0 ||
            strcmp(geometry_type, "Patch") == 0);
}

/// @brief Initialize a procedural mesh remap and allocate sparse control contributions.
/// @details The contribution arrays reserve a fixed worst-case number of entries per
///          generated vertex. Any failure releases the partially initialized remap.
/// @param[out] remap Remap to initialize and populate with owned arrays.
/// @param geometry_id FBX object identifier for the source geometry.
/// @param control_count Number of authored surface controls.
/// @param vertex_count Number of tessellated output vertices.
/// @param max_contributions_per_vertex Maximum tensor-product support per output vertex.
/// @param[in,out] context Loader context charged for every remap allocation.
/// @return Non-zero on success, or zero for invalid dimensions, overflow,
///         limit/budget rejection, or allocation failure.
static int fbx_surface_prepare_remap(fbx_mesh_remap_t *remap,
                                     int64_t geometry_id,
                                     int32_t control_count,
                                     int32_t vertex_count,
                                     int32_t max_contributions_per_vertex,
                                     fbx_load_context_t *context) {
    uint64_t contribution_capacity;
    if (!remap || control_count <= 0 || vertex_count <= 0 || max_contributions_per_vertex <= 0 ||
        (uint64_t)vertex_count > UINT64_MAX / (uint64_t)max_contributions_per_vertex ||
        !fbx_budget_charge(context, (uint64_t)control_count, sizeof(fbx_vertex_index_list_t)) ||
        !fbx_mesh_remap_init(remap, geometry_id, control_count))
        return 0;
    contribution_capacity = (uint64_t)vertex_count * (uint64_t)max_contributions_per_vertex;
    if (contribution_capacity > UINT32_MAX ||
        (uint64_t)vertex_count > SIZE_MAX / sizeof(*remap->surface_vertices) ||
        contribution_capacity > SIZE_MAX / sizeof(*remap->surface_control_indices) ||
        contribution_capacity > SIZE_MAX / sizeof(*remap->surface_control_weights) ||
        !fbx_budget_charge(context, (uint64_t)vertex_count, sizeof(*remap->surface_vertices)) ||
        !fbx_budget_charge(
            context, contribution_capacity, sizeof(*remap->surface_control_indices)) ||
        !fbx_budget_charge(
            context, contribution_capacity, sizeof(*remap->surface_control_weights))) {
        fbx_mesh_remap_free(remap);
        return 0;
    }
    remap->surface_vertices = (fbx_surface_vertex_remap_t *)calloc(
        (size_t)vertex_count, sizeof(*remap->surface_vertices));
    remap->surface_control_indices =
        (int32_t *)malloc((size_t)contribution_capacity * sizeof(*remap->surface_control_indices));
    remap->surface_control_weights =
        (double *)malloc((size_t)contribution_capacity * sizeof(*remap->surface_control_weights));
    if (!remap->surface_vertices || !remap->surface_control_indices ||
        !remap->surface_control_weights) {
        fbx_mesh_remap_free(remap);
        return 0;
    }
    remap->surface_vertex_count = (uint32_t)vertex_count;
    remap->surface_contribution_count = 0;
    return 1;
}

/// @brief Read an optional pair of surface-closure flags.
/// @param geometry Patch geometry node containing an optional `Closed` child.
/// @param[out] out_closed Receives normalized U/V boolean flags; both are false when absent.
/// @return Non-zero when the child is absent or has exactly two properties; otherwise zero.
static int fbx_surface_read_closed_pair(fbx_node_t *geometry, int out_closed[2]) {
    fbx_node_t *closed_node = fbx_find_child(geometry, "Closed");
    out_closed[0] = 0;
    out_closed[1] = 0;
    if (!closed_node)
        return 1;
    if (closed_node->prop_count != 2)
        return 0;
    out_closed[0] = fbx_prop_truthy(closed_node, 0);
    out_closed[1] = fbx_prop_truthy(closed_node, 1);
    return 1;
}

/// @brief Read the truth value of the first property on an optional named child.
/// @param geometry Geometry node containing the optional child.
/// @param name Child-node name to query.
/// @return Non-zero when the child exists and its first property is truthy; otherwise zero.
static int fbx_surface_child_truthy(fbx_node_t *geometry, const char *name) {
    fbx_node_t *child = fbx_find_child(geometry, name);
    return child && child->prop_count > 0 ? fbx_prop_truthy(child, 0) : 0;
}

/// @brief Evaluate a sampled surface, generate normals and topology, and create a Mesh3D.
/// @details Tensor-product basis contributions produce positions, analytic tangents,
///          and sparse control weights for each vertex. Degenerate analytic normals
///          fall back to neighboring finite differences. The generated grid is then
///          triangulated with wrapping on closed axes.
/// @param geometry Source FBX geometry node; its first property supplies the remap ID.
/// @param controls Flattened four-component control net in V-major, then U-major order.
/// @param count_u Number of controls along U.
/// @param count_v Number of controls along V.
/// @param axis_u Precomputed U-axis basis samples.
/// @param axis_v Precomputed V-axis basis samples.
/// @param rational Non-zero to apply rational control weights and quotient derivatives.
/// @param flip_normals Non-zero to reverse normals and triangle winding.
/// @param[in,out] context Loader context charged for generated buffers and mesh storage.
/// @param[out] remap Receives the owned sparse control-to-output-vertex mapping.
/// @param[out] material_map Optional material map, reset to empty on success.
/// @return Newly allocated runtime mesh on success, or NULL on invalid input,
///         arithmetic/limit failure, allocation failure, or invalid evaluated geometry.
/// @post On failure, any storage initialized in @p remap and any partial mesh are released.
static void *fbx_surface_build_mesh(fbx_node_t *geometry,
                                    const double *controls,
                                    int32_t count_u,
                                    int32_t count_v,
                                    const fbx_surface_axis_samples_t *axis_u,
                                    const fbx_surface_axis_samples_t *axis_v,
                                    int rational,
                                    int flip_normals,
                                    fbx_load_context_t *context,
                                    fbx_mesh_remap_t *remap,
                                    fbx_mesh_material_map_t *material_map) {
    uint64_t vertex_count64;
    uint64_t quad_u;
    uint64_t quad_v;
    uint64_t triangle_count64;
    uint64_t index_count64;
    uint64_t max_contributions64;
    double *positions = NULL;
    double *normals = NULL;
    void *mesh = NULL;
    uint32_t contribution_cursor = 0;
    if (!geometry || !controls || !axis_u || !axis_v || !context || !remap || count_u <= 0 ||
        count_v <= 0 || axis_u->sample_count <= 1 || axis_v->sample_count <= 1 ||
        axis_u->support <= 0 || axis_v->support <= 0)
        return NULL;
    if ((uint64_t)axis_u->sample_count > UINT64_MAX / (uint64_t)axis_v->sample_count)
        return NULL;
    vertex_count64 = (uint64_t)axis_u->sample_count * (uint64_t)axis_v->sample_count;
    quad_u = axis_u->closed ? (uint64_t)axis_u->sample_count : (uint64_t)axis_u->sample_count - 1u;
    quad_v = axis_v->closed ? (uint64_t)axis_v->sample_count : (uint64_t)axis_v->sample_count - 1u;
    if (quad_u == 0 || quad_v == 0 || quad_u > UINT64_MAX / quad_v ||
        quad_u * quad_v > UINT64_MAX / 2u)
        return NULL;
    triangle_count64 = quad_u * quad_v * 2u;
    if (triangle_count64 > UINT64_MAX / 3u)
        return NULL;
    index_count64 = triangle_count64 * 3u;
    if ((uint64_t)axis_u->support > UINT64_MAX / (uint64_t)axis_v->support)
        return NULL;
    max_contributions64 = (uint64_t)axis_u->support * (uint64_t)axis_v->support;
    if (vertex_count64 > INT32_MAX || vertex_count64 > UINT64_MAX / 6u ||
        triangle_count64 > UINT32_MAX / 3u || index_count64 > UINT32_MAX ||
        max_contributions64 > INT32_MAX ||
        !rt_untrusted_count_ok(
            (int64_t)(vertex_count64 * 3u), sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !fbx_budget_charge(context, vertex_count64 * 6u, sizeof(double)) ||
        !fbx_budget_charge(context, vertex_count64, sizeof(vgfx3d_vertex_t)) ||
        !fbx_budget_charge(context, index_count64, sizeof(uint32_t)) ||
        !fbx_surface_prepare_remap(remap,
                                   fbx_prop_i64(geometry, 0),
                                   count_u * count_v,
                                   (int32_t)vertex_count64,
                                   (int32_t)max_contributions64,
                                   context))
        return NULL;
    positions = (double *)malloc((size_t)vertex_count64 * 3u * sizeof(*positions));
    normals = (double *)malloc((size_t)vertex_count64 * 3u * sizeof(*normals));
    if (!positions || !normals)
        goto fail;

    for (int32_t v = 0; v < axis_v->sample_count; ++v) {
        size_t v_basis_offset = (size_t)v * (size_t)axis_v->support;
        for (int32_t u = 0; u < axis_u->sample_count; ++u) {
            size_t u_basis_offset = (size_t)u * (size_t)axis_u->support;
            uint32_t vertex =
                (uint32_t)((uint64_t)v * (uint64_t)axis_u->sample_count + (uint64_t)u);
            double denominator = rational ? 0.0 : 1.0;
            double denominator_u = 0.0;
            double denominator_v = 0.0;
            double position[3] = {0.0, 0.0, 0.0};
            double tangent_u[3] = {0.0, 0.0, 0.0};
            double tangent_v[3] = {0.0, 0.0, 0.0};
            fbx_surface_vertex_remap_t *vertex_remap = &remap->surface_vertices[vertex];
            vertex_remap->offset = contribution_cursor;
            vertex_remap->count = 0;
            if (rational) {
                for (int32_t sv = 0; sv < axis_v->support; ++sv) {
                    int32_t cv = axis_v->indices[v_basis_offset + (size_t)sv];
                    double bv = axis_v->weights[v_basis_offset + (size_t)sv];
                    double dbv = axis_v->derivatives[v_basis_offset + (size_t)sv];
                    for (int32_t su = 0; su < axis_u->support; ++su) {
                        int32_t cu = axis_u->indices[u_basis_offset + (size_t)su];
                        double bu = axis_u->weights[u_basis_offset + (size_t)su];
                        double dbu = axis_u->derivatives[u_basis_offset + (size_t)su];
                        int32_t control;
                        double control_weight;
                        if (cu < 0 || cu >= count_u || cv < 0 || cv >= count_v)
                            goto fail;
                        control = cv * count_u + cu;
                        control_weight = controls[(size_t)control * 4u + 3u];
                        denominator += bu * bv * control_weight;
                        denominator_u += dbu * bv * control_weight;
                        denominator_v += bu * dbv * control_weight;
                    }
                }
                if (!isfinite(denominator) || !isfinite(denominator_u) ||
                    !isfinite(denominator_v) || fabs(denominator) <= FBX_SURFACE_DENOM_EPSILON)
                    goto fail;
            }
            for (int32_t sv = 0; sv < axis_v->support; ++sv) {
                int32_t cv = axis_v->indices[v_basis_offset + (size_t)sv];
                double bv = axis_v->weights[v_basis_offset + (size_t)sv];
                double dbv = axis_v->derivatives[v_basis_offset + (size_t)sv];
                for (int32_t su = 0; su < axis_u->support; ++su) {
                    int32_t cu = axis_u->indices[u_basis_offset + (size_t)su];
                    double bu = axis_u->weights[u_basis_offset + (size_t)su];
                    double dbu = axis_u->derivatives[u_basis_offset + (size_t)su];
                    int32_t control;
                    double coefficient;
                    double derivative_u;
                    double derivative_v;
                    double control_weight;
                    if (cu < 0 || cu >= count_u || cv < 0 || cv >= count_v)
                        goto fail;
                    control = cv * count_u + cu;
                    control_weight = rational ? controls[(size_t)control * 4u + 3u] : 1.0;
                    coefficient = bu * bv * control_weight;
                    derivative_u = dbu * bv * control_weight;
                    derivative_v = bu * dbv * control_weight;
                    if (rational)
                        coefficient /= denominator;
                    if (!isfinite(coefficient) || !isfinite(derivative_u) ||
                        !isfinite(derivative_v))
                        goto fail;
                    for (int lane = 0; lane < 3; ++lane) {
                        double control_value = controls[(size_t)control * 4u + (size_t)lane];
                        position[lane] += coefficient * control_value;
                        tangent_u[lane] += derivative_u * control_value;
                        tangent_v[lane] += derivative_v * control_value;
                    }
                    if (fabs(coefficient) > 1e-15) {
                        if (contribution_cursor == UINT32_MAX)
                            goto fail;
                        remap->surface_control_indices[contribution_cursor] = control;
                        remap->surface_control_weights[contribution_cursor] = coefficient;
                        contribution_cursor++;
                        vertex_remap->count++;
                    }
                }
            }
            if (!fbx_sanitize_position3(&position[0], &position[1], &position[2]) ||
                vertex_remap->count == 0)
                goto fail;
            if (rational) {
                for (int lane = 0; lane < 3; ++lane) {
                    tangent_u[lane] =
                        (tangent_u[lane] - position[lane] * denominator_u) / denominator;
                    tangent_v[lane] =
                        (tangent_v[lane] - position[lane] * denominator_v) / denominator;
                }
            }
            positions[(size_t)vertex * 3u + 0u] = position[0];
            positions[(size_t)vertex * 3u + 1u] = position[1];
            positions[(size_t)vertex * 3u + 2u] = position[2];
            normals[(size_t)vertex * 3u + 0u] =
                tangent_u[1] * tangent_v[2] - tangent_u[2] * tangent_v[1];
            normals[(size_t)vertex * 3u + 1u] =
                tangent_u[2] * tangent_v[0] - tangent_u[0] * tangent_v[2];
            normals[(size_t)vertex * 3u + 2u] =
                tangent_u[0] * tangent_v[1] - tangent_u[1] * tangent_v[0];
        }
    }
    remap->surface_contribution_count = contribution_cursor;

    for (int32_t v = 0; v < axis_v->sample_count; ++v) {
        int32_t prev_v = v > 0 ? v - 1 : (axis_v->closed ? axis_v->sample_count - 1 : v);
        int32_t next_v = v + 1 < axis_v->sample_count ? v + 1 : (axis_v->closed ? 0 : v);
        for (int32_t u = 0; u < axis_u->sample_count; ++u) {
            int32_t prev_u = u > 0 ? u - 1 : (axis_u->closed ? axis_u->sample_count - 1 : u);
            int32_t next_u = u + 1 < axis_u->sample_count ? u + 1 : (axis_u->closed ? 0 : u);
            uint32_t vertex =
                (uint32_t)((uint64_t)v * (uint64_t)axis_u->sample_count + (uint64_t)u);
            uint32_t u0 =
                (uint32_t)((uint64_t)v * (uint64_t)axis_u->sample_count + (uint64_t)prev_u);
            uint32_t u1 =
                (uint32_t)((uint64_t)v * (uint64_t)axis_u->sample_count + (uint64_t)next_u);
            uint32_t v0 =
                (uint32_t)((uint64_t)prev_v * (uint64_t)axis_u->sample_count + (uint64_t)u);
            uint32_t v1 =
                (uint32_t)((uint64_t)next_v * (uint64_t)axis_u->sample_count + (uint64_t)u);
            double normal[3] = {normals[(size_t)vertex * 3u + 0u],
                                normals[(size_t)vertex * 3u + 1u],
                                normals[(size_t)vertex * 3u + 2u]};
            double normal_length2 =
                normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
            if (!isfinite(normal_length2) || normal_length2 <= 1e-20) {
                double du[3];
                double dv[3];
                for (int lane = 0; lane < 3; ++lane) {
                    du[lane] = positions[(size_t)u1 * 3u + (size_t)lane] -
                               positions[(size_t)u0 * 3u + (size_t)lane];
                    dv[lane] = positions[(size_t)v1 * 3u + (size_t)lane] -
                               positions[(size_t)v0 * 3u + (size_t)lane];
                }
                normal[0] = du[1] * dv[2] - du[2] * dv[1];
                normal[1] = du[2] * dv[0] - du[0] * dv[2];
                normal[2] = du[0] * dv[1] - du[1] * dv[0];
            }
            fbx_sanitize_normal3(&normal[0], &normal[1], &normal[2]);
            if (flip_normals) {
                normal[0] = -normal[0];
                normal[1] = -normal[1];
                normal[2] = -normal[2];
            }
            normals[(size_t)vertex * 3u + 0u] = normal[0];
            normals[(size_t)vertex * 3u + 1u] = normal[1];
            normals[(size_t)vertex * 3u + 2u] = normal[2];
        }
    }

    mesh = rt_mesh3d_new();
    if (!mesh)
        goto fail;
    rt_mesh3d_reserve(mesh, (int64_t)vertex_count64, (int64_t)triangle_count64);
    if (((rt_mesh3d *)mesh)->build_failed)
        goto fail;
    for (int32_t v = 0; v < axis_v->sample_count; ++v) {
        double texture_v = axis_v->closed ? (double)v / (double)axis_v->sample_count
                                          : (double)v / (double)(axis_v->sample_count - 1);
        for (int32_t u = 0; u < axis_u->sample_count; ++u) {
            uint32_t vertex =
                (uint32_t)((uint64_t)v * (uint64_t)axis_u->sample_count + (uint64_t)u);
            double texture_u = axis_u->closed ? (double)u / (double)axis_u->sample_count
                                              : (double)u / (double)(axis_u->sample_count - 1);
            rt_mesh3d_add_vertex(mesh,
                                 positions[(size_t)vertex * 3u + 0u],
                                 positions[(size_t)vertex * 3u + 1u],
                                 positions[(size_t)vertex * 3u + 2u],
                                 normals[(size_t)vertex * 3u + 0u],
                                 normals[(size_t)vertex * 3u + 1u],
                                 normals[(size_t)vertex * 3u + 2u],
                                 texture_u,
                                 texture_v);
            if (((rt_mesh3d *)mesh)->build_failed)
                goto fail;
        }
    }
    for (uint32_t v = 0; v < quad_v; ++v) {
        uint32_t next_v = (v + 1u) % (uint32_t)axis_v->sample_count;
        for (uint32_t u = 0; u < quad_u; ++u) {
            uint32_t next_u = (u + 1u) % (uint32_t)axis_u->sample_count;
            uint32_t a = v * (uint32_t)axis_u->sample_count + u;
            uint32_t b = v * (uint32_t)axis_u->sample_count + next_u;
            uint32_t c = next_v * (uint32_t)axis_u->sample_count + u;
            uint32_t d = next_v * (uint32_t)axis_u->sample_count + next_u;
            if (flip_normals) {
                rt_mesh3d_add_triangle(mesh, a, d, b);
                rt_mesh3d_add_triangle(mesh, a, c, d);
            } else {
                rt_mesh3d_add_triangle(mesh, a, b, d);
                rt_mesh3d_add_triangle(mesh, a, d, c);
            }
            if (((rt_mesh3d *)mesh)->build_failed)
                goto fail;
        }
    }
    if (material_map)
        memset(material_map, 0, sizeof(*material_map));
    free(normals);
    free(positions);
    return mesh;

fail:
    free(normals);
    free(positions);
    fbx_mesh_remap_free(remap);
    if (mesh && rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    return NULL;
}

/// @brief Convert an FBX NURBS surface or patch into an indexed runtime Mesh3D.
/// @details Validates dimensions, tessellation steps, basis metadata, knots, and
///          control points before sampling both parameter axes. Failures publish a
///          precise corrupt-input or aggregate-budget error through the asset-error API.
/// @param geometry Source FBX `Geometry` node.
/// @param geometry_type Geometry subtype, expected to identify a NURBS surface or `Patch`.
/// @param z_up Non-zero to convert control positions from Z-up to runtime Y-up space.
/// @param[in,out] context Loader context used for aggregate allocation accounting.
/// @param[out] remap Receives the generated vertex-to-control contribution mapping.
/// @param[out] material_map Receives an empty material map for the generated surface.
/// @return Newly allocated runtime mesh on success, or NULL with the asset error set
///         when metadata, topology, numeric data, budgets, or allocation are invalid.
static void *fbx_extract_procedural_geometry(fbx_node_t *geometry,
                                             const char *geometry_type,
                                             int z_up,
                                             fbx_load_context_t *context,
                                             fbx_mesh_remap_t *remap,
                                             fbx_mesh_material_map_t *material_map) {
    int64_t dimensions64[2];
    int64_t authored_steps[2] = {1, 1};
    int32_t dimensions[2];
    int32_t steps[2];
    double *controls = NULL;
    double *knots_u = NULL;
    double *knots_v = NULL;
    fbx_surface_axis_samples_t axis_u = {0};
    fbx_surface_axis_samples_t axis_v = {0};
    void *mesh = NULL;
    int rational;
    int flip_normals;
    if (!geometry || !geometry_type || !context || !remap || !material_map) {
        fbx_surface_error("FBX: procedural surface arguments are invalid");
        return NULL;
    }
    rational = strcmp(geometry_type, "Patch") != 0;
    flip_normals = fbx_surface_child_truthy(geometry, "FlipNormals");
    if (!fbx_surface_read_pair_i64(geometry, "Dimensions", dimensions64) || dimensions64[0] <= 0 ||
        dimensions64[0] > INT32_MAX || dimensions64[1] <= 0 || dimensions64[1] > INT32_MAX ||
        dimensions64[0] > INT32_MAX / dimensions64[1]) {
        fbx_surface_error("FBX: procedural surface dimensions are invalid");
        return NULL;
    }
    dimensions[0] = (int32_t)dimensions64[0];
    dimensions[1] = (int32_t)dimensions64[1];
    if (fbx_find_child(geometry, "Step") &&
        !fbx_surface_read_pair_i64(geometry, "Step", authored_steps)) {
        fbx_surface_error("FBX: procedural surface step is invalid");
        return NULL;
    }
    if (!fbx_surface_validate_steps(authored_steps, steps))
        return NULL;
    controls = fbx_surface_load_control_net(
        geometry, dimensions[0], dimensions[1], rational, z_up, context);
    if (!controls) {
        if (context->budget_exhausted)
            rt_asset_error_set(
                RT_ASSET_ERROR_TOO_LARGE,
                "FBX: procedural surface control net exceeds the aggregate load budget");
        else
            fbx_surface_error("FBX: procedural surface control net is malformed");
        goto done;
    }

    if (rational) {
        int64_t orders64[2];
        int32_t orders[2];
        fbx_surface_form_t forms[2];
        fbx_node_t *order_node = fbx_find_child(geometry, "NurbsSurfaceOrder");
        fbx_node_t *form_node = fbx_find_child(geometry, "Form");
        if (!order_node)
            order_node = fbx_find_child(geometry, "NurbOrder");
        if (!order_node)
            order_node = fbx_find_child(geometry, "Order");
        if (!order_node || order_node->prop_count != 2 ||
            !fbx_surface_property_is_number(&order_node->props[0]) ||
            !fbx_surface_property_is_number(&order_node->props[1])) {
            fbx_surface_error("FBX: NURBS surface order is invalid");
            goto done;
        }
        orders64[0] = fbx_prop_i64(order_node, 0);
        orders64[1] = fbx_prop_i64(order_node, 1);
        if (orders64[0] < 2 || orders64[0] > dimensions[0] || orders64[1] < 2 ||
            orders64[1] > dimensions[1]) {
            fbx_surface_error("FBX: NURBS surface order/control-count relationship is invalid");
            goto done;
        }
        orders[0] = (int32_t)orders64[0];
        orders[1] = (int32_t)orders64[1];
        if (!form_node || form_node->prop_count != 2 ||
            !fbx_surface_parse_form(form_node, 0, &forms[0]) ||
            !fbx_surface_parse_form(form_node, 1, &forms[1])) {
            fbx_surface_error("FBX: NURBS surface form is invalid");
            goto done;
        }
        knots_u = fbx_surface_load_knots(
            geometry, "KnotVectorU", dimensions[0], orders[0], forms[0], context);
        knots_v = fbx_surface_load_knots(
            geometry, "KnotVectorV", dimensions[1], orders[1], forms[1], context);
        if (!knots_u || !knots_v) {
            if (context->budget_exhausted)
                rt_asset_error_set(
                    RT_ASSET_ERROR_TOO_LARGE,
                    "FBX: NURBS surface knot vectors exceed the aggregate load budget");
            else
                fbx_surface_error("FBX: NURBS surface knot vector is malformed");
            goto done;
        }
        if (!fbx_surface_build_nurbs_axis(
                knots_u, dimensions[0], orders[0], steps[0], forms[0], context, &axis_u) ||
            !fbx_surface_build_nurbs_axis(
                knots_v, dimensions[1], orders[1], steps[1], forms[1], context, &axis_v)) {
            if (context->budget_exhausted)
                rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                                   "FBX: NURBS surface samples exceed the aggregate load budget");
            else
                fbx_surface_error("FBX: NURBS surface parameter domain is invalid");
            goto done;
        }
    } else {
        fbx_node_t *type_node = fbx_find_child(geometry, "PatchType");
        fbx_patch_basis_t basis[2];
        int closed[2];
        if (!type_node || type_node->prop_count != 2 ||
            !fbx_surface_parse_patch_basis(type_node, 0, &basis[0]) ||
            !fbx_surface_parse_patch_basis(type_node, 1, &basis[1])) {
            fbx_surface_error("FBX: patch basis type is invalid");
            goto done;
        }
        if (!fbx_surface_read_closed_pair(geometry, closed)) {
            fbx_surface_error("FBX: patch closed flags are invalid");
            goto done;
        }
        if (!fbx_surface_build_patch_axis(
                basis[0], dimensions[0], steps[0], closed[0], context, &axis_u) ||
            !fbx_surface_build_patch_axis(
                basis[1], dimensions[1], steps[1], closed[1], context, &axis_v)) {
            if (context->budget_exhausted)
                rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                                   "FBX: patch samples exceed the aggregate load budget");
            else
                fbx_surface_error("FBX: patch basis/control-count relationship is invalid");
            goto done;
        }
    }

    mesh = fbx_surface_build_mesh(geometry,
                                  controls,
                                  dimensions[0],
                                  dimensions[1],
                                  &axis_u,
                                  &axis_v,
                                  rational,
                                  flip_normals,
                                  context,
                                  remap,
                                  material_map);
    if (!mesh) {
        rt_asset_error_set_if_empty(
            context->budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
            context->budget_exhausted ? "FBX: procedural surface exceeds the aggregate load budget"
                                      : "FBX: procedural surface tessellation failed");
    }

done:
    fbx_surface_axis_samples_free(&axis_v);
    fbx_surface_axis_samples_free(&axis_u);
    free(knots_v);
    free(knots_u);
    free(controls);
    return mesh;
}

/// @brief Convert an FBX `Geometry` node into a Zanna `rt_mesh3d_t`.
///
/// Decodes the `Vertices` (positions), `PolygonVertexIndex`
/// (vertex indices, with the last index of each polygon negated
/// XOR'd with bit 31 to mark polygon end), `LayerElementNormal`,
/// `LayerElementUV`, and `LayerElementMaterial`. Triangulates n-gons
/// using ear clipping. If `z_up` is set, applies an axis
/// swap (positions and normals) to convert to Y-up.
/// @param geom_node Source FBX polygon-mesh `Geometry` node.
/// @param z_up Non-zero to convert positions and normals from Z-up to Y-up space.
/// @param[out] remap Receives the control-vertex to emitted-vertex mapping.
/// @param[out] material_map Optional per-triangle material assignment map.
/// @return A new mesh on success, NULL on missing or malformed geometry.
static void *fbx_extract_geometry(fbx_node_t *geom_node,
                                  int z_up,
                                  fbx_mesh_remap_t *remap,
                                  fbx_mesh_material_map_t *material_map) {
    if (!geom_node)
        return NULL;

    /* Find Vertices (double array) and PolygonVertexIndex (int32 array) */
    fbx_node_t *verts_node = fbx_find_child(geom_node, "Vertices");
    fbx_node_t *idx_node = fbx_find_child(geom_node, "PolygonVertexIndex");
    if (!verts_node || !idx_node)
        return NULL;
    if (verts_node->prop_count < 1 || idx_node->prop_count < 1)
        return NULL;

    fbx_prop_t *vp = &verts_node->props[0];
    fbx_prop_t *ip = &idx_node->props[0];
    if (vp->type != 'd' || ip->type != 'i')
        return NULL;

    double *positions = (double *)vp->v.array.data;
    int32_t *indices = (int32_t *)ip->v.array.data;
    if (vp->v.array.count % 3u != 0)
        return NULL;
    if (!rt_untrusted_count_ok(
            (int64_t)vp->v.array.count, sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !rt_untrusted_count_ok(
            (int64_t)ip->v.array.count, sizeof(int32_t), FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
        return NULL;
    uint32_t pos_count = vp->v.array.count / 3;
    uint32_t idx_count = ip->v.array.count;
    if (pos_count > (uint32_t)INT32_MAX || idx_count > (uint32_t)INT32_MAX)
        return NULL;
    if (!positions || !indices || pos_count == 0 || idx_count == 0)
        return NULL;
    if (remap && !fbx_mesh_remap_init(remap, fbx_prop_i64(geom_node, 0), (int32_t)pos_count))
        return NULL;
    if (material_map)
        memset(material_map, 0, sizeof(*material_map));

    /* Vertex attribute layers (optional) + material assignment (optional) */
    fbx_vertex_layer_t norm_layer;
    fbx_vertex_layer_t uv_layer;
    fbx_vertex_layer_t uv1_layer;
    fbx_vertex_layer_t color_layer;
    fbx_material_layer_t mat;
    fbx_parse_geometry_layers(geom_node, &norm_layer, &uv_layer, &uv1_layer, &color_layer, &mat);

    /* Build mesh: iterate polygon indices, triangulate with fan */
    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;

    int32_t polygon_stack[16];
    int32_t *polygon = polygon_stack;
    int32_t poly_count = 0;
    int32_t poly_capacity = (int32_t)(sizeof(polygon_stack) / sizeof(polygon_stack[0]));
    int32_t polygon_vertex_idx = 0; /* running index for ByPolygonVertex mapping */
    uint32_t polygon_idx = 0;
    int32_t mesh_vertex_count = 0;
    int polygon_invalid = 0;

    for (uint32_t i = 0; i < idx_count; i++) {
        int32_t raw_idx = indices[i];
        int end_of_polygon = (raw_idx < 0);
        int32_t vi = end_of_polygon ? ~raw_idx : raw_idx;

        if (vi < 0 || vi >= (int32_t)pos_count) {
            polygon_invalid = 1;
            polygon_vertex_idx++;
            if (end_of_polygon) {
                poly_count = 0;
                polygon_invalid = 0;
                polygon_idx++;
            }
            continue;
        }

        /* Get position */
        double px = positions[vi * 3 + 0];
        double py = positions[vi * 3 + 1];
        double pz = positions[vi * 3 + 2];
        if (z_up)
            fbx_correct_zup(&px, &py, &pz);
        if (!fbx_sanitize_position3(&px, &py, &pz)) {
            polygon_invalid = 1;
            polygon_vertex_idx++;
            if (end_of_polygon) {
                poly_count = 0;
                polygon_invalid = 0;
                polygon_idx++;
            }
            continue;
        }

        /* Get normal */
        double nx = 0, ny = 1, nz = 0;
        if (norm_layer.data || norm_layer.data32) {
            int32_t ni = fbx_resolve_layer_index(&norm_layer, polygon_vertex_idx, polygon_idx, vi);
            if (ni >= 0) {
                nx = fbx_vertex_layer_component(&norm_layer, ni, 0, nx);
                ny = fbx_vertex_layer_component(&norm_layer, ni, 1, ny);
                nz = fbx_vertex_layer_component(&norm_layer, ni, 2, nz);
                if (z_up)
                    fbx_correct_zup(&nx, &ny, &nz);
            }
        }
        fbx_sanitize_normal3(&nx, &ny, &nz);

        /* Get UV */
        double u = 0, v = 0;
        if (uv_layer.data || uv_layer.data32) {
            int32_t ui = fbx_resolve_layer_index(&uv_layer, polygon_vertex_idx, polygon_idx, vi);
            if (ui >= 0) {
                u = fbx_vertex_layer_component(&uv_layer, ui, 0, u);
                v = 1.0 - fbx_vertex_layer_component(&uv_layer, ui, 1, 1.0); /* FBX V flips */
            }
        }
        u = fbx_clamp_abs_or(u, 0.0, FBX_UV_ABS_MAX);
        v = fbx_clamp_abs_or(v, 0.0, FBX_UV_ABS_MAX);

        {
            int32_t emitted_vertex = mesh_vertex_count++;
            if (poly_count >= poly_capacity) {
                int32_t new_capacity = poly_capacity * 2;
                int32_t *grown;
                if (poly_capacity > INT32_MAX / 2 ||
                    (size_t)new_capacity > SIZE_MAX / sizeof(*polygon))
                    return fbx_geom_fail(polygon, polygon_stack, mesh);
                grown = (int32_t *)malloc((size_t)new_capacity * sizeof(*polygon));
                if (!grown)
                    return fbx_geom_fail(polygon, polygon_stack, mesh);
                memcpy(grown, polygon, (size_t)poly_count * sizeof(*polygon));
                if (polygon != polygon_stack)
                    free(polygon);
                polygon = grown;
                poly_capacity = new_capacity;
            }
            if (((rt_mesh3d *)mesh)->vertex_count == UINT32_MAX)
                return fbx_geom_fail(polygon, polygon_stack, mesh);
            rt_mesh3d_add_vertex(mesh, px, py, pz, nx, ny, nz, u, v);
            if (((rt_mesh3d *)mesh)->build_failed)
                return fbx_geom_fail(polygon, polygon_stack, mesh);
            if ((uv1_layer.data || uv1_layer.data32) && emitted_vertex >= 0 &&
                emitted_vertex < (int32_t)((rt_mesh3d *)mesh)->vertex_count) {
                int32_t ui1 =
                    fbx_resolve_layer_index(&uv1_layer, polygon_vertex_idx, polygon_idx, vi);
                if (ui1 >= 0) {
                    double u1 = fbx_vertex_layer_component(&uv1_layer, ui1, 0, u);
                    double v1 = 1.0 - fbx_vertex_layer_component(&uv1_layer, ui1, 1, 1.0 - v);
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].uv1[0] =
                        (float)fbx_clamp_abs_or(u1, u, FBX_UV_ABS_MAX);
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].uv1[1] =
                        (float)fbx_clamp_abs_or(v1, v, FBX_UV_ABS_MAX);
                }
            }
            if ((color_layer.data || color_layer.data32) && emitted_vertex >= 0 &&
                emitted_vertex < (int32_t)((rt_mesh3d *)mesh)->vertex_count) {
                int32_t ci =
                    fbx_resolve_layer_index(&color_layer, polygon_vertex_idx, polygon_idx, vi);
                if (ci >= 0) {
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].color[0] =
                        (float)fbx_clamp_double(
                            fbx_vertex_layer_component(&color_layer, ci, 0, 1.0), 0.0, 1.0, 1.0);
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].color[1] =
                        (float)fbx_clamp_double(
                            fbx_vertex_layer_component(&color_layer, ci, 1, 1.0), 0.0, 1.0, 1.0);
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].color[2] =
                        (float)fbx_clamp_double(
                            fbx_vertex_layer_component(&color_layer, ci, 2, 1.0), 0.0, 1.0, 1.0);
                    ((rt_mesh3d *)mesh)->vertices[emitted_vertex].color[3] =
                        (float)fbx_clamp_double(
                            fbx_vertex_layer_component(&color_layer, ci, 3, 1.0), 0.0, 1.0, 1.0);
                }
            }
            if (!fbx_mesh_remap_add_vertex(remap, vi, emitted_vertex))
                return fbx_geom_fail(polygon, polygon_stack, mesh);
            polygon[poly_count++] = emitted_vertex;
        }
        polygon_vertex_idx++;

        if (end_of_polygon) {
            uint32_t index_before = ((rt_mesh3d *)mesh)->index_count;
            int32_t material_slot = fbx_material_slot_for_polygon(
                mat.slots, mat.slot_count, mat.by_polygon, mat.all_same, polygon_idx);
            if (material_slot < 0)
                return fbx_geom_fail(polygon, polygon_stack, mesh);
            if (!polygon_invalid && !fbx_emit_polygon_triangles(mesh, polygon, poly_count))
                return fbx_geom_fail(polygon, polygon_stack, mesh);
            if (!polygon_invalid && material_map) {
                uint32_t index_after = ((rt_mesh3d *)mesh)->index_count;
                uint32_t added_triangles = (index_after - index_before) / 3u;
                if (!fbx_mesh_material_map_append(material_map, material_slot, added_triangles)) {
                    fbx_mesh_material_map_free(material_map);
                    return fbx_geom_fail(polygon, polygon_stack, mesh);
                }
            }
            poly_count = 0;
            polygon_invalid = 0;
            polygon_idx++;
        }
    }

    if (poly_count != 0 || polygon_invalid)
        return fbx_geom_fail(polygon, polygon_stack, mesh);
    if (polygon != polygon_stack)
        free(polygon);
    if (((rt_mesh3d *)mesh)->index_count < 3u) {
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }
    if ((!norm_layer.data && !norm_layer.data32) || norm_layer.count == 0)
        rt_mesh3d_recalc_normals(mesh);
    return mesh;
}

/*==========================================================================
 * Material extraction
 *=========================================================================*/

/// @brief Convert an FBX `Material` node into a Zanna `rt_material3d_t`.
///
/// Reads common legacy and PBR scalar properties from `Properties70`, including
/// diffuse/base color, shininess, opacity, emission, metallic, roughness,
/// ambient occlusion, normal scale, and double-sided state. Unknown
/// exporter-specific properties leave the runtime material defaults unchanged.
/// @param mat_node Source FBX `Material` node.
/// @return Newly allocated runtime material, retaining defaults for absent properties,
///         or NULL when @p mat_node is NULL or material allocation fails.
static void *fbx_extract_material(fbx_node_t *mat_node) {
    if (!mat_node)
        return NULL;
    void *mat = rt_material3d_new();
    if (!mat)
        return NULL;

    fbx_node_t *p70 = fbx_find_child(mat_node, "Properties70");
    if (!p70)
        return mat;

    for (int32_t i = 0; i < p70->child_count; i++) {
        fbx_node_t *p = &p70->children[i];
        if (strcmp(p->name, "P") != 0 || p->prop_count < 5)
            continue;
        const char *pname = fbx_prop_str(p, 0);
        if ((fbx_ascii_equals_i(pname, "DiffuseColor") || fbx_ascii_equals_i(pname, "BaseColor") ||
             fbx_ascii_equals_i(pname, "Albedo") || fbx_ascii_contains_i(pname, "basecolor") ||
             fbx_ascii_contains_i(pname, "base_color")) &&
            p->prop_count >= 7) {
            double r = fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 1.0);
            double g = fbx_clamp_double(fbx_prop_f64(p, 5), 0.0, 1.0, 1.0);
            double b = fbx_clamp_double(fbx_prop_f64(p, 6), 0.0, 1.0, 1.0);
            rt_material3d_set_color(mat, r, g, b);
        } else if (fbx_ascii_equals_i(pname, "Shininess") ||
                   fbx_ascii_equals_i(pname, "ShininessExponent")) {
            double s = fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1000000.0, 32.0);
            rt_material3d_set_shininess(mat, s);
        } else if (fbx_ascii_equals_i(pname, "Opacity") || fbx_ascii_equals_i(pname, "Alpha")) {
            double alpha = fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 1.0);
            rt_material3d_set_alpha(mat, alpha);
            if (alpha < 1.0)
                rt_material3d_set_alpha_mode(mat, 2);
        } else if (fbx_ascii_equals_i(pname, "TransparencyFactor") ||
                   fbx_ascii_equals_i(pname, "TransparentFactor")) {
            double alpha = 1.0 - fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 0.0);
            rt_material3d_set_alpha(mat, alpha);
            if (alpha < 1.0)
                rt_material3d_set_alpha_mode(mat, 2);
        } else if ((fbx_ascii_equals_i(pname, "EmissiveColor") ||
                    fbx_ascii_equals_i(pname, "EmissionColor") ||
                    fbx_ascii_contains_i(pname, "emissivecolor")) &&
                   p->prop_count >= 7) {
            double r = fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 0.0);
            double g = fbx_clamp_double(fbx_prop_f64(p, 5), 0.0, 1.0, 0.0);
            double b = fbx_clamp_double(fbx_prop_f64(p, 6), 0.0, 1.0, 0.0);
            rt_material3d_set_emissive_color(mat, r, g, b);
        } else if (fbx_ascii_equals_i(pname, "EmissiveFactor") ||
                   fbx_ascii_equals_i(pname, "EmissionFactor") ||
                   fbx_ascii_equals_i(pname, "EmissiveIntensity")) {
            rt_material3d_set_emissive_intensity(
                mat, fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1000000.0, 0.0));
        } else if (fbx_ascii_contains_i(pname, "metallic") ||
                   fbx_ascii_contains_i(pname, "metalness")) {
            rt_material3d_set_metallic(mat, fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 0.0));
        } else if (fbx_ascii_contains_i(pname, "roughness")) {
            rt_material3d_set_roughness(mat, fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 0.5));
        } else if (fbx_ascii_contains_i(pname, "occlusion") || fbx_ascii_equals_i(pname, "AO")) {
            rt_material3d_set_ao(mat, fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1.0, 1.0));
        } else if (fbx_ascii_equals_i(pname, "NormalMapScale") ||
                   fbx_ascii_equals_i(pname, "BumpFactor") || fbx_ascii_equals_i(pname, "Bump")) {
            rt_material3d_set_normal_scale(
                mat, fbx_clamp_double(fbx_prop_f64(p, 4), 0.0, 1000000.0, 1.0));
        } else if (fbx_ascii_equals_i(pname, "DoubleSided") ||
                   fbx_ascii_equals_i(pname, "TwoSided") ||
                   fbx_ascii_equals_i(pname, "DoubleSidedMaterial")) {
            if (fbx_prop_truthy(p, 4))
                rt_material3d_set_double_sided(mat, 1);
        } else if (fbx_ascii_equals_i(pname, "BackfaceCulling")) {
            if (!fbx_prop_truthy(p, 4))
                rt_material3d_set_double_sided(mat, 1);
        }
    }

    return mat;
}
