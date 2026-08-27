//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/render/rt_mesh3d_obj.inc  (#included from rt_mesh3d.c; not a
// standalone translation unit) Purpose: Mesh3D Wavefront OBJ loader and tangent computation
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Wavefront OBJ parsing, face triangulation, grouping, and mesh tangents.
/// @details This include fragment is compiled as part of `rt_mesh3d.c`. It converts locale-neutral
///          OBJ text into deduplicated Mesh3D vertices, preserves material group splits when
///          requested, fills missing normals, and computes tangent frames for normal mapping.

/*==========================================================================
 * Wavefront OBJ loader (~300 LOC)
 *
 * Supports: v, vn, vt, f (v, v/vt, v/vt/vn, v//vn), # comments,
 *           negative indices, quad auto-triangulation.
 * Zanna-specific limitation: geometry-only import. Authored OBJ material/group
 * splits must be pre-flattened before reaching this runtime loader.
 *=========================================================================*/

/* Parse one integer from a face index string, advancing *p past it.
 * Returns 0 if no integer found (empty between slashes). */
// ---------------------------------------------------------------------------
// Wavefront OBJ parser helpers — OBJ is a text format where each
// line begins with a tag (`v` for vertex, `vt` for tex coord, `vn`
// for normal, `f` for face). These parsers walk a `**p` cursor
// through the line, consuming whitespace and tokens.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t vi;
    int64_t ti;
    int64_t ni;
} obj_face_vert_t;

/// @brief Parse a signed decimal integer from `*p`, advancing `*p` past it.
/// @details Empty optional fields at a slash or delimiter resolve to zero without consuming input.
///          Magnitudes that would exceed `INT64_MAX` are rejected.
/// @param p Address of the input cursor, updated to the first unconsumed byte.
/// @param out Receives the parsed integer or zero for an empty optional field.
/// @return One on success, or zero for malformed input or overflow.
static int obj_parse_int(const char **p, int64_t *out) {
    const char *s = *p;
    int negative = 0;
    uint64_t val = 0;
    int saw_digit = 0;
    if (!*s || *s == '/' || *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        *out = 0;
        return 1;
    }
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        uint64_t digit = (uint64_t)(*s - '0');
        if (val > ((uint64_t)INT64_MAX - digit) / 10u)
            return 0;
        val = val * 10u + digit;
        saw_digit = 1;
        s++;
    }
    if (!saw_digit)
        return 0;
    *out = negative ? -(int64_t)val : (int64_t)val;
    *p = s;
    return 1;
}

/* Parse a face vertex index: v[/vt[/vn]] or v//vn
 * Sets vi, ti, ni to 1-based indices (0 = not present) */
/// @brief Parse a face vertex spec `v[/[vt][/vn]]` — slash-separated optional indices.
///
/// The OBJ face syntax allows three forms:
///   - `v`         (vertex only — `ti = ni = 0`)
///   - `v/vt`      (vertex + tex)
///   - `v//vn`     (vertex + normal, no tex)
///   - `v/vt/vn`   (all three)
/// 1-based indices in the file (OBJ convention); negative values
/// reference from the end of the current vertex list.
/// @param p Address of the face-token cursor, advanced on success.
/// @param vi Receives the required raw position index.
/// @param ti Receives the optional raw texture-coordinate index, or zero when absent.
/// @param ni Receives the optional raw normal index, or zero when absent.
/// @return One for a syntactically valid nonzero position reference; otherwise zero.
static int obj_parse_face_vert(const char **p, int64_t *vi, int64_t *ti, int64_t *ni) {
    const char *start = p ? *p : NULL;
    if (!p || !*p || !vi || !ti || !ni)
        return 0;
    if (!obj_parse_int(p, vi))
        return 0;
    *ti = 0;
    *ni = 0;
    if (**p == '/') {
        (*p)++;
        if (**p != '/') {
            if (**p == '\0' || **p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
                return 0;
            if (!obj_parse_int(p, ti))
                return 0;
        }
        if (**p == '/') {
            (*p)++;
            if (**p == '\0' || **p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
                return 0;
            if (!obj_parse_int(p, ni))
                return 0;
        }
    }
    return *p != start && *vi != 0;
}

/// @brief Parse a locale-independent ASCII double from `*p`, advancing `*p` past it.
/// @param p Address of a cursor positioned before optional horizontal whitespace and a number.
/// @param out Receives the finite parsed value.
/// @return One on success, or zero when the token is malformed or non-finite.
static int obj_parse_double(const char **p, double *out) {
    const char *end;
    while (**p == ' ' || **p == '\t')
        (*p)++;
    if (!mesh_parse_ascii_double_span(*p, NULL, &end, out))
        return 0;
    *p = end;
    return 1;
}

/// @brief Double the capacity of a float accumulator array used during OBJ parsing.
/// @details The OBJ parser pre-allocates small position/normal/UV arrays and
///   grows them geometrically via this helper when the current capacity is
///   exhausted.  The `components` parameter (2 for UV, 3 for positions/normals)
///   is folded into the byte-count calculation so both array types share one
///   growth path.  Guards against INT_MAX/2 overflow on the new-capacity
///   integer and against SIZE_MAX overflow on the byte count before calling
///   realloc.  Returns 0 on any overflow or allocation failure.
/// @param array Address of the owned float-array pointer, updated after successful growth.
/// @param capacity Address of the element capacity, doubled on success.
/// @param components Number of floats per logical element.
/// @return One after successful growth, or zero for invalid input, overflow, or allocation failure.
static int obj_grow_float_array(float **array, int *capacity, int components) {
    if (!array || !capacity || components <= 0)
        return 0;
    if (*capacity > INT_MAX / 2)
        return 0;
    int new_capacity = *capacity * 2;
    size_t component_bytes = (size_t)components * sizeof(float);
    if ((size_t)new_capacity > SIZE_MAX / component_bytes)
        return 0;
    float *tmp = (float *)realloc(*array, (size_t)new_capacity * component_bytes);
    if (!tmp)
        return 0;
    *array = tmp;
    *capacity = new_capacity;
    return 1;
}

typedef struct {
    int64_t vi;
    int64_t ti;
    int64_t ni;
    uint32_t mesh_index;
    uint8_t used;
} obj_vertex_cache_entry_t;

typedef struct {
    obj_vertex_cache_entry_t *entries;
    size_t capacity;
    size_t count;
} obj_vertex_cache_t;

/// @brief FNV-1a-ish mix of the `(v, vt, vn)` OBJ face triplet into a 64-bit hash.
/// @details The mixing constants are the standard FNV offset basis + prime, combined with
///   Knuth's golden-ratio constant and a Boost-style XOR-shift round to distribute low bits.
///   Collisions fall back to linear probing in the cache — any well-distributed hash works
///   here; this one was picked for speed and stability across platforms.
/// @param vi Resolved position index.
/// @param ti Resolved texture-coordinate index, or zero.
/// @param ni Resolved normal index, or zero.
/// @return Deterministic 64-bit hash for the complete OBJ vertex tuple.
static uint64_t obj_hash_index_triplet(int64_t vi, int64_t ti, int64_t ni) {
    uint64_t h = 1469598103934665603ull;
    uint64_t values[3] = {(uint64_t)vi, (uint64_t)ti, (uint64_t)ni};
    for (int i = 0; i < 3; i++) {
        h ^= values[i] + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 1099511628211ull;
    }
    return h;
}

/// @brief Allocate an open-addressing hash table for the OBJ vertex-deduplication cache.
/// @details Capacity is floored to 64 and must be a power of two for the `(h + probe) & (cap-1)`
///   slot wrap used by lookup/insert — the initial call sites in `rt_mesh3d_from_obj`
///   respect this by passing 1024, and `obj_vertex_cache_grow` doubles. Returns 1 on
///   success, 0 if the underlying calloc fails.
/// @param cache Cache object to initialize.
/// @param capacity Requested minimum entry capacity, rounded up to a power of two.
/// @return One on success, or zero for invalid input, overflow, or allocation failure.
static int obj_vertex_cache_init(obj_vertex_cache_t *cache, size_t capacity) {
    if (!cache)
        return 0;
    if (capacity < 64)
        capacity = 64;
    if (capacity > (SIZE_MAX >> 1u))
        return 0;
    capacity--;
    capacity |= capacity >> 1u;
    capacity |= capacity >> 2u;
    capacity |= capacity >> 4u;
    capacity |= capacity >> 8u;
    capacity |= capacity >> 16u;
#if SIZE_MAX > UINT32_MAX
    capacity |= capacity >> 32u;
#endif
    capacity++;
    if (capacity > SIZE_MAX / sizeof(*cache->entries))
        return 0;
    cache->entries = (obj_vertex_cache_entry_t *)calloc(capacity, sizeof(*cache->entries));
    cache->capacity = cache->entries ? capacity : 0;
    cache->count = 0;
    return cache->entries != NULL;
}

/// @brief Release the entry array owned by an OBJ vertex cache; safe on an already-empty cache.
/// @param cache Cache to clear; NULL is ignored.
static void obj_vertex_cache_free(obj_vertex_cache_t *cache) {
    if (!cache)
        return;
    free(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

/// @brief Double the hash table capacity and rehash all live entries.
/// @details Called by `obj_vertex_cache_insert` when the load factor (count/capacity) would
///   exceed 70 %. Growing early keeps the open-addressing probe chains short so lookup
///   stays close to O(1) even when a big OBJ file reuses the same vertex triplets
///   thousands of times. Returns 0 if the new allocation fails or capacity would overflow.
/// @param cache Initialized cache to double and rehash.
/// @return One on success, or zero for invalid state, overflow, or allocation failure.
static int obj_vertex_cache_grow(obj_vertex_cache_t *cache) {
    obj_vertex_cache_t grown;
    size_t new_capacity;
    if (!cache || !cache->entries)
        return 0;
    if (cache->capacity > SIZE_MAX / 2u)
        return 0;
    new_capacity = cache->capacity * 2u;
    if (!obj_vertex_cache_init(&grown, new_capacity))
        return 0;
    for (size_t i = 0; i < cache->capacity; i++) {
        obj_vertex_cache_entry_t entry = cache->entries[i];
        if (!entry.used)
            continue;
        uint64_t h = obj_hash_index_triplet(entry.vi, entry.ti, entry.ni);
        for (size_t probe = 0; probe < grown.capacity; probe++) {
            size_t slot = (size_t)((h + probe) & (uint64_t)(grown.capacity - 1u));
            if (!grown.entries[slot].used) {
                grown.entries[slot] = entry;
                grown.count++;
                break;
            }
        }
    }
    free(cache->entries);
    *cache = grown;
    return 1;
}

/// @brief Linear-probe lookup for `(vi, ti, ni)` in the OBJ vertex cache.
/// @details Returns 1 and writes the cached mesh-vertex index when found, 0 otherwise.
///   Probing stops at the first unused slot — matching the invariant maintained by
///   `obj_vertex_cache_insert` that entries are contiguous along their probe chain.
/// @param cache Initialized deduplication cache.
/// @param vi Resolved position index.
/// @param ti Resolved texture-coordinate index, or zero.
/// @param ni Resolved normal index, or zero.
/// @param out_index Optional destination for the cached Mesh3D vertex index.
/// @return One when the tuple is present; otherwise zero.
static int obj_vertex_cache_lookup(
    const obj_vertex_cache_t *cache, int64_t vi, int64_t ti, int64_t ni, uint32_t *out_index) {
    if (!cache || !cache->entries || cache->capacity == 0)
        return 0;
    uint64_t h = obj_hash_index_triplet(vi, ti, ni);
    for (size_t probe = 0; probe < cache->capacity; probe++) {
        size_t slot = (size_t)((h + probe) & (uint64_t)(cache->capacity - 1u));
        const obj_vertex_cache_entry_t *entry = &cache->entries[slot];
        if (!entry->used)
            return 0;
        if (entry->vi == vi && entry->ti == ti && entry->ni == ni) {
            if (out_index)
                *out_index = entry->mesh_index;
            return 1;
        }
    }
    return 0;
}

/// @brief Record that `(vi, ti, ni)` has been emitted as mesh vertex `mesh_index`.
/// @details Auto-grows when load would exceed ~70 % to keep probe chains short. Returns
///   1 on success, 0 on allocation failure in grow (which propagates upward to fail the
///   OBJ import). Duplicate keys never reach this function — callers always do a lookup
///   first via `obj_get_or_add_mesh_vertex`.
/// @param cache Initialized deduplication cache.
/// @param vi Resolved position index.
/// @param ti Resolved texture-coordinate index, or zero.
/// @param ni Resolved normal index, or zero.
/// @param mesh_index Mesh3D vertex index associated with the tuple.
/// @return One after insertion, or zero when growth or probing cannot provide a slot.
static int obj_vertex_cache_insert(
    obj_vertex_cache_t *cache, int64_t vi, int64_t ti, int64_t ni, uint32_t mesh_index) {
    if (!cache || !cache->entries)
        return 0;
    size_t next_count = cache->count + 1u;
    size_t grow_limit = (cache->capacity / 10u) * 7u + ((cache->capacity % 10u) * 7u) / 10u;
    if (next_count < cache->count || next_count > grow_limit) {
        if (!obj_vertex_cache_grow(cache))
            return 0;
    }
    uint64_t h = obj_hash_index_triplet(vi, ti, ni);
    for (size_t probe = 0; probe < cache->capacity; probe++) {
        size_t slot = (size_t)((h + probe) & (uint64_t)(cache->capacity - 1u));
        obj_vertex_cache_entry_t *entry = &cache->entries[slot];
        if (!entry->used) {
            entry->vi = vi;
            entry->ti = ti;
            entry->ni = ni;
            entry->mesh_index = mesh_index;
            entry->used = 1;
            cache->count++;
            return 1;
        }
    }
    return 0;
}

/// @brief Store the first failing OBJ line number without overwriting an earlier cause.
/// @param error_line Address of the zero-initialized first-error field; NULL is ignored.
/// @param line_number One-based source line to record when no earlier line was stored.
static void obj_record_error_line(uint64_t *error_line, uint64_t line_number) {
    if (error_line && *error_line == 0)
        *error_line = line_number;
}

/// @brief Turn an OBJ face index into a 1-based absolute index, or reject it.
/// @details OBJ face indices are 1-based and can be negative (counting from the end of
///   the currently-defined list). A raw value of 0 means "no index at this slot" —
///   accepted only when `allow_missing` is nonzero (used for optional UV/normal slots;
///   position is always required). Writes the resolved positive index into `*out` and
///   returns 1 on success, 0 on out-of-range or a missing-but-required zero.
/// @param raw Raw OBJ index, which may be positive, negative, or zero.
/// @param count Number of attributes defined at the point of reference.
/// @param allow_missing Nonzero to accept zero as an absent optional attribute.
/// @param out Receives a positive one-based absolute index or accepted missing zero.
/// @return One on success, or zero for invalid destination, missing required data, or range error.
static int obj_resolve_index(int64_t raw, int count, int allow_missing, int64_t *out) {
    int64_t resolved = raw;
    if (!out)
        return 0;
    if (raw == 0) {
        if (allow_missing) {
            *out = 0;
            return 1;
        }
        return 0;
    }
    if (raw < 0)
        resolved = (int64_t)count + raw + 1;
    if (resolved < 1 || resolved > count)
        return 0;
    *out = resolved;
    return 1;
}

/// @brief Resolve an OBJ face triplet to a mesh-vertex index, emitting a new vertex on miss.
/// @details This is the workhorse of vertex deduplication. First tries the cache; on a
///   hit, returns the existing mesh index (the same GPU vertex is reused across faces).
///   On a miss, reads the referenced position/normal/UV from the parallel OBJ attribute
///   arrays, appends a new mesh vertex, records the mapping in the cache, and returns
///   the new index. Missing normal/UV indices (zero) emit a zeroed attribute — caller
///   recomputes normals via `rt_mesh3d_recalc_normals` if the file had none.
/// @param mesh Destination Mesh3D under construction.
/// @param cache Deduplication cache mapping resolved OBJ tuples to mesh vertices.
/// @param positions Packed three-float OBJ position array.
/// @param cnt_p Number of defined positions.
/// @param normals Packed three-float OBJ normal array.
/// @param cnt_n Number of defined normals.
/// @param texcoords Packed two-float OBJ texture-coordinate array.
/// @param cnt_t Number of defined texture coordinates.
/// @param vi Resolved one-based position index.
/// @param ti Resolved one-based texture-coordinate index, or zero.
/// @param ni Resolved one-based normal index, or zero.
/// @param out_index Optional destination for the reused or appended Mesh3D vertex index.
/// @return 1 on success, 0 on any failure (cache full, vertex-count overflow, or
///         `rt_mesh3d_add_vertex` trapped).
static int obj_get_or_add_mesh_vertex(void *mesh,
                                      obj_vertex_cache_t *cache,
                                      const float *positions,
                                      int cnt_p,
                                      const float *normals,
                                      int cnt_n,
                                      const float *texcoords,
                                      int cnt_t,
                                      int64_t vi,
                                      int64_t ti,
                                      int64_t ni,
                                      uint32_t *out_index) {
    uint32_t cached_index;
    (void)cnt_p;
    if (obj_vertex_cache_lookup(cache, vi, ti, ni, &cached_index)) {
        if (out_index)
            *out_index = cached_index;
        return 1;
    }

    float px = positions[(vi - 1) * 3 + 0];
    float py = positions[(vi - 1) * 3 + 1];
    float pz = positions[(vi - 1) * 3 + 2];
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float tu = 0.0f, tv = 0.0f;

    if (ni >= 1 && ni <= cnt_n) {
        nx = normals[(ni - 1) * 3 + 0];
        ny = normals[(ni - 1) * 3 + 1];
        nz = normals[(ni - 1) * 3 + 2];
    }
    if (ti >= 1 && ti <= cnt_t) {
        tu = texcoords[(ti - 1) * 2 + 0];
        tv = texcoords[(ti - 1) * 2 + 1];
    }

    rt_mesh3d *m = (rt_mesh3d *)mesh;
    if (m->vertex_count == UINT32_MAX)
        return 0;
    cached_index = m->vertex_count;
    rt_mesh3d_add_vertex(mesh, px, py, pz, nx, ny, nz, tu, tv);
    if (m->build_failed || m->vertex_count <= cached_index)
        return 0;
    if (!obj_vertex_cache_insert(cache, vi, ti, ni, cached_index))
        return 0;
    if (out_index)
        *out_index = cached_index;
    return 1;
}

/// @brief Signed area (×2) of a polygon projected onto axes (@p ax0, @p ax1) via the shoelace
/// formula.
/// @details Its sign gives the polygon's winding in that projection, which the ear-clip uses to
///          orient inside/outside tests.
/// @param mesh Mesh containing polygon positions.
/// @param indices Ordered Mesh3D vertex indices around the face.
/// @param count Number of polygon vertices.
/// @param ax0 First coordinate axis retained in the projection.
/// @param ax1 Second coordinate axis retained in the projection.
/// @return Twice the signed projected polygon area.
static double obj_projected_area2(
    const rt_mesh3d *mesh, const uint32_t *indices, int count, int ax0, int ax1) {
    double area = 0.0;
    for (int i = 0; i < count; ++i) {
        double a[3], b[3];
        mesh_position_f64_at(mesh, indices[i], a);
        mesh_position_f64_at(mesh, indices[(i + 1) % count], b);
        area += a[ax0] * b[ax1] - b[ax0] * a[ax1];
    }
    return area;
}

/// @brief Pick the two axes to project a planar polygon onto for 2D triangulation.
/// @details Computes the polygon's Newell normal and drops the axis with the largest |normal|
///          component, keeping the projection that preserves the most area (avoids edge-on
///          degeneracy).
/// @param mesh Mesh containing polygon positions.
/// @param indices Ordered Mesh3D vertex indices around the face.
/// @param count Number of polygon vertices.
/// @param ax0 Receives the first retained coordinate axis.
/// @param ax1 Receives the second retained coordinate axis.
static void obj_choose_projection_axes(
    const rt_mesh3d *mesh, const uint32_t *indices, int count, int *ax0, int *ax1) {
    double normal[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < count; ++i) {
        double a[3], b[3];
        mesh_position_f64_at(mesh, indices[i], a);
        mesh_position_f64_at(mesh, indices[(i + 1) % count], b);
        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    if (fabs(normal[0]) >= fabs(normal[1]) && fabs(normal[0]) >= fabs(normal[2])) {
        *ax0 = 1;
        *ax1 = 2;
    } else if (fabs(normal[1]) >= fabs(normal[2])) {
        *ax0 = 0;
        *ax1 = 2;
    } else {
        *ax0 = 0;
        *ax1 = 1;
    }
}

/// @brief 2D orientation of points (ia, ib, ic) in the (@p ax0, @p ax1) projection.
/// @details The signed cross product (b-a)×(c-a); >0 is CCW, <0 CW, ~0 collinear.
/// @param mesh Mesh containing the indexed positions.
/// @param ia First vertex index.
/// @param ib Second vertex index.
/// @param ic Third vertex index.
/// @param ax0 First projection axis.
/// @param ax1 Second projection axis.
/// @return Signed projected two-edge cross product.
static double obj_orient2(
    const rt_mesh3d *mesh, uint32_t ia, uint32_t ib, uint32_t ic, int ax0, int ax1) {
    double a[3], b[3], c[3];
    mesh_position_f64_at(mesh, ia, a);
    mesh_position_f64_at(mesh, ib, b);
    mesh_position_f64_at(mesh, ic, c);
    double ab0 = b[ax0] - a[ax0];
    double ab1 = b[ax1] - a[ax1];
    double ac0 = c[ax0] - a[ax0];
    double ac1 = c[ax1] - a[ax1];
    return ab0 * ac1 - ab1 * ac0;
}

/// @brief Whether projected point @p p lies strictly inside triangle (a, b, c) for the given
/// winding.
/// @details Checks @p p is on the interior side of all three edges (scaled by @p winding so it
/// works
///          for either orientation); the 1e-12 epsilon excludes on-edge points so ears don't
///          overlap.
/// @param mesh Mesh containing all indexed positions.
/// @param p Candidate point index.
/// @param a First triangle vertex index.
/// @param b Second triangle vertex index.
/// @param c Third triangle vertex index.
/// @param ax0 First projection axis.
/// @param ax1 Second projection axis.
/// @param winding Positive one for counter-clockwise polygons or negative one for clockwise.
/// @return Nonzero only when @p p lies strictly inside the projected triangle.
static int obj_point_in_triangle2(const rt_mesh3d *mesh,
                                  uint32_t p,
                                  uint32_t a,
                                  uint32_t b,
                                  uint32_t c,
                                  int ax0,
                                  int ax1,
                                  double winding) {
    double o0 = obj_orient2(mesh, a, b, p, ax0, ax1) * winding;
    double o1 = obj_orient2(mesh, b, c, p, ax0, ax1) * winding;
    double o2 = obj_orient2(mesh, c, a, p, ax0, ax1) * winding;
    const double eps = 1e-12;
    return o0 > eps && o1 > eps && o2 > eps;
}

/// @brief Triangulate an OBJ n-gon face into the mesh via ear-clipping (fan fallback for
/// triangles).
/// @details Projects the face to its best 2D plane, determines winding, then repeatedly clips a
/// valid
///          ear (a convex corner whose triangle contains no other vertex), emitting one triangle
///          each step. A guard counter bounds the loop against degenerate/self-intersecting faces.
/// @param mesh_obj Destination Mesh3D under construction.
/// @param mesh_indices Ordered Mesh3D vertex indices for the face.
/// @param face_count Number of vertices in the face.
/// @return Non-zero on success; 0 if the face is invalid or a triangle add failed.
static int obj_triangulate_face(void *mesh_obj, const uint32_t *mesh_indices, int face_count) {
    rt_mesh3d *mesh = (rt_mesh3d *)mesh_obj;
    int ax0 = 0;
    int ax1 = 1;
    double area;
    double winding;
    int *order;
    int remaining;
    int guard;
    int emitted = 0;
    if (!mesh || !mesh_indices || face_count < 3)
        return 0;
    if (face_count == 3) {
        if (mesh_indices_form_triangle(mesh, mesh_indices[0], mesh_indices[1], mesh_indices[2]))
            rt_mesh3d_add_triangle(mesh_obj, mesh_indices[0], mesh_indices[1], mesh_indices[2]);
        return !mesh->build_failed;
    }
    obj_choose_projection_axes(mesh, mesh_indices, face_count, &ax0, &ax1);
    area = obj_projected_area2(mesh, mesh_indices, face_count, ax0, ax1);
    if (!isfinite(area) || fabs(area) <= 1e-12) {
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                            "Mesh3D.FromOBJ: degenerate or self-intersecting polygon");
        return 0;
    }
    winding = area > 0.0 ? 1.0 : -1.0;
    if (face_count > MESH3D_OBJ_MAX_EARCLIP_VERTS) {
        /* A fan is linear and exact for a convex mega-face. Concave input must fail instead of
         * silently emitting overlapping/out-of-polygon triangles. */
        for (int i = 0; i < face_count; i++) {
            uint32_t ia = mesh_indices[(i + face_count - 1) % face_count];
            uint32_t ib = mesh_indices[i];
            uint32_t ic = mesh_indices[(i + 1) % face_count];
            if (obj_orient2(mesh, ia, ib, ic, ax0, ax1) * winding < -1e-12) {
                rt_asset_error_setf(
                    RT_ASSET_ERROR_CORRUPT,
                    "Mesh3D.FromOBJ: concave polygon exceeds the 1024-vertex triangulation limit");
                return 0;
            }
        }
        for (int i = 1; i + 1 < face_count; ++i) {
            if (mesh_indices_form_triangle(
                    mesh, mesh_indices[0], mesh_indices[i], mesh_indices[i + 1]))
                rt_mesh3d_add_triangle(
                    mesh_obj, mesh_indices[0], mesh_indices[i], mesh_indices[i + 1]);
            if (mesh->build_failed)
                return 0;
        }
        return !mesh->build_failed;
    }
    order = (int *)malloc((size_t)face_count * sizeof(int));
    if (!order)
        return 0;
    for (int i = 0; i < face_count; ++i)
        order[i] = i;
    remaining = face_count;
    guard = face_count * face_count;
    while (remaining > 3 && guard-- > 0) {
        int clipped = 0;
        for (int oi = 0; oi < remaining; ++oi) {
            int prev_i = order[(oi + remaining - 1) % remaining];
            int curr_i = order[oi];
            int next_i = order[(oi + 1) % remaining];
            uint32_t ia = mesh_indices[prev_i];
            uint32_t ib = mesh_indices[curr_i];
            uint32_t ic = mesh_indices[next_i];
            int contains = 0;
            if (obj_orient2(mesh, ia, ib, ic, ax0, ax1) * winding <= 1e-12)
                continue;
            if (!mesh_indices_form_triangle(mesh, ia, ib, ic))
                continue;
            for (int oj = 0; oj < remaining; ++oj) {
                int test_i = order[oj];
                if (test_i == prev_i || test_i == curr_i || test_i == next_i)
                    continue;
                if (obj_point_in_triangle2(
                        mesh, mesh_indices[test_i], ia, ib, ic, ax0, ax1, winding)) {
                    contains = 1;
                    break;
                }
            }
            if (contains)
                continue;
            rt_mesh3d_add_triangle(mesh_obj, ia, ib, ic);
            emitted++;
            if (mesh->build_failed) {
                free(order);
                return 0;
            }
            for (int move = oi; move < remaining - 1; ++move)
                order[move] = order[move + 1];
            remaining--;
            clipped = 1;
            break;
        }
        if (!clipped) {
            for (int oi = 0; oi < remaining; ++oi) {
                int prev_i = order[(oi + remaining - 1) % remaining];
                int curr_i = order[oi];
                int next_i = order[(oi + 1) % remaining];
                uint32_t ia = mesh_indices[prev_i];
                uint32_t ib = mesh_indices[curr_i];
                uint32_t ic = mesh_indices[next_i];
                double orient = obj_orient2(mesh, ia, ib, ic, ax0, ax1) * winding;
                if (orient <= 1e-12 || !mesh_indices_form_triangle(mesh, ia, ib, ic)) {
                    for (int move = oi; move < remaining - 1; ++move)
                        order[move] = order[move + 1];
                    remaining--;
                    clipped = 1;
                    break;
                }
            }
            if (!clipped) {
                free(order);
                rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                    "Mesh3D.FromOBJ: polygon triangulation stalled");
                return 0;
            }
        }
    }
    if (remaining == 3) {
        uint32_t ia = mesh_indices[order[0]];
        uint32_t ib = mesh_indices[order[1]];
        uint32_t ic = mesh_indices[order[2]];
        if (mesh_indices_form_triangle(mesh, ia, ib, ic)) {
            rt_mesh3d_add_triangle(mesh_obj, ia, ib, ic);
            emitted++;
        }
    }
    free(order);
    (void)emitted;
    return !mesh->build_failed;
}

/// @brief Calculate per-vertex tangent vectors for normal mapping (Lengyel's method).
/// @details Tangent space is the local coordinate system at each vertex where the
///          U texture axis aligns with the tangent and V aligns with the bitangent.
///          Normal maps are authored in this space, so we need per-vertex T to read
///          them correctly.
///
///          Two-phase algorithm (classic Eric Lengyel, "Mathematics for 3D Game
///          Programming and Computer Graphics"):
///
///          Phase 1: For each triangle derive the tangent (`sdir`) and bitangent
///          (`tdir`) directions by solving the 2x2 system that takes (dU,dV) to
///          (dPos). `det = duv1.x*duv2.y - duv1.y*duv2.x` is the UV-triangle's
///          signed area; triangles with zero UV area contribute nothing (would
///          divide by zero). The per-vertex tangent/bitangent accumulators
///          `tan1[]`/`tan2[]` sum contributions from every incident triangle so
///          the final directions are area-weighted.
///
///          Phase 2: For each vertex, run Gram-Schmidt against the normal to make
///          the tangent strictly perpendicular to N (so N, T, B form an orthonormal
///          frame even if the summed tangent wasn't quite tangential). Then derive
///          handedness: `dot(cross(N,T), summed_bitangent) < 0` means the UV chart
///          is mirrored on this face relative to the object, and the bitangent in
///          the shader must be negated — we encode that as `tangent.w = -1`. The
///          +1/-1 W channel is how the shader reconstructs B cheaply.
///
///          Degenerate cases (zero-length tangent after orthogonalization) fall back
///          to a canonical +X tangent with W=+1 so the vertex still produces sensible
///          output in the shader.
///
///          Allocates two transient float buffers (`tan1`, `tan2`) sized to the
///          vertex count; both are freed before returning. No-op on empty meshes.
/// @param m Mesh whose `tangent[4]` slot on every vertex gets written in place; NULL is ignored.
void rt_mesh3d_calc_tangents_impl(rt_mesh3d *m) {
    if (!m)
        return;
    if (m->tangents_ready && m->tangent_revision == m->geometry_revision)
        return;
    if (m->vertex_count == 0) {
        m->tangents_ready = 1;
        m->tangent_revision = m->geometry_revision;
        return;
    }
    if (m->index_count == 0) {
        for (uint32_t i = 0; i < m->vertex_count; i++)
            mesh_default_tangent_from_normal(m->vertices[i].normal, m->vertices[i].tangent);
        mesh3d_bump_vertex_revision(m, 0);
        m->tangents_ready = 1;
        m->tangent_revision = m->geometry_revision;
        return;
    }

    if (!rt_alloc_count_ok(m->vertex_count, 3u * sizeof(double))) {
        rt_trap("Mesh3D.CalcTangents: tangent allocation overflow");
        return;
    }
    size_t tangent_floats = (size_t)m->vertex_count * 3u;
    double *tan1 = (double *)calloc(tangent_floats, sizeof(double));
    double *tan2 = (double *)calloc(tangent_floats, sizeof(double));
    if (!tan1 || !tan2) {
        free(tan1);
        free(tan2);
        rt_trap("Mesh3D.CalcTangents: memory allocation failed");
        return;
    }

    /* Accumulate tangents and bitangents per triangle (Lengyel's method). */
    for (uint32_t i = 0; i + 2 < m->index_count; i += 3) {
        uint32_t i0 = m->indices[i], i1 = m->indices[i + 1], i2 = m->indices[i + 2];
        if (i0 >= m->vertex_count || i1 >= m->vertex_count || i2 >= m->vertex_count)
            continue;

        float *p0 = m->vertices[i0].pos;
        float *p1 = m->vertices[i1].pos;
        float *p2 = m->vertices[i2].pos;
        float *uv0 = m->vertices[i0].uv;
        float *uv1 = m->vertices[i1].uv;
        float *uv2 = m->vertices[i2].uv;

        double edge1[3] = {(double)p1[0] - (double)p0[0],
                           (double)p1[1] - (double)p0[1],
                           (double)p1[2] - (double)p0[2]};
        double edge2[3] = {(double)p2[0] - (double)p0[0],
                           (double)p2[1] - (double)p0[1],
                           (double)p2[2] - (double)p0[2]};
        double duv1[2] = {(double)uv1[0] - (double)uv0[0], (double)uv1[1] - (double)uv0[1]};
        double duv2[2] = {(double)uv2[0] - (double)uv0[0], (double)uv2[1] - (double)uv0[1]};

        double det = duv1[0] * duv2[1] - duv1[1] * duv2[0];
        if (!isfinite(det) || fabs(det) < 1e-8)
            continue; /* degenerate UV */
        double inv_det = 1.0 / det;

        double sdir_d[3] = {(edge1[0] * duv2[1] - edge2[0] * duv1[1]) * inv_det,
                            (edge1[1] * duv2[1] - edge2[1] * duv1[1]) * inv_det,
                            (edge1[2] * duv2[1] - edge2[2] * duv1[1]) * inv_det};
        double tdir_d[3] = {(edge2[0] * duv1[0] - edge1[0] * duv2[0]) * inv_det,
                            (edge2[1] * duv1[0] - edge1[1] * duv2[0]) * inv_det,
                            (edge2[2] * duv1[0] - edge1[2] * duv2[0]) * inv_det};
        if (!mesh_value_fits_float(sdir_d[0]) || !mesh_value_fits_float(sdir_d[1]) ||
            !mesh_value_fits_float(sdir_d[2]) || !mesh_value_fits_float(tdir_d[0]) ||
            !mesh_value_fits_float(tdir_d[1]) || !mesh_value_fits_float(tdir_d[2]))
            continue;
        size_t b0 = (size_t)i0 * 3u;
        size_t b1 = (size_t)i1 * 3u;
        size_t b2 = (size_t)i2 * 3u;

        tan1[b0 + 0] += sdir_d[0];
        tan1[b0 + 1] += sdir_d[1];
        tan1[b0 + 2] += sdir_d[2];
        tan1[b1 + 0] += sdir_d[0];
        tan1[b1 + 1] += sdir_d[1];
        tan1[b1 + 2] += sdir_d[2];
        tan1[b2 + 0] += sdir_d[0];
        tan1[b2 + 1] += sdir_d[1];
        tan1[b2 + 2] += sdir_d[2];

        tan2[b0 + 0] += tdir_d[0];
        tan2[b0 + 1] += tdir_d[1];
        tan2[b0 + 2] += tdir_d[2];
        tan2[b1 + 0] += tdir_d[0];
        tan2[b1 + 1] += tdir_d[1];
        tan2[b1 + 2] += tdir_d[2];
        tan2[b2 + 0] += tdir_d[0];
        tan2[b2 + 1] += tdir_d[1];
        tan2[b2 + 2] += tdir_d[2];
    }

    /* Normalize, orthogonalize against the normal, and store handedness in tangent.w. */
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        float *t = m->vertices[i].tangent;
        float *n = m->vertices[i].normal;
        double *tan = &tan1[(size_t)i * 3u];
        double *bitan = &tan2[(size_t)i * 3u];

        /* Gram-Schmidt: T = T - N * dot(N, T) */
        double dot = (double)n[0] * tan[0] + (double)n[1] * tan[1] + (double)n[2] * tan[2];
        double tx = tan[0] - (double)n[0] * dot;
        double ty = tan[1] - (double)n[1] * dot;
        double tz = tan[2] - (double)n[2] * dot;

        /* Normalize */
        double len = sqrt(tx * tx + ty * ty + tz * tz);
        if (isfinite(len) && len > 1e-8f) {
            tx /= len;
            ty /= len;
            tz /= len;
            t[0] = mesh_value_fits_float(tx) ? (float)tx : 1.0f;
            t[1] = mesh_value_fits_float(ty) ? (float)ty : 0.0f;
            t[2] = mesh_value_fits_float(tz) ? (float)tz : 0.0f;
            {
                double cross_nt[3] = {(double)n[1] * (double)t[2] - (double)n[2] * (double)t[1],
                                      (double)n[2] * (double)t[0] - (double)n[0] * (double)t[2],
                                      (double)n[0] * (double)t[1] - (double)n[1] * (double)t[0]};
                double handedness =
                    cross_nt[0] * bitan[0] + cross_nt[1] * bitan[1] + cross_nt[2] * bitan[2];
                t[3] = (isfinite(handedness) && handedness < 0.0) ? -1.0f : 1.0f;
            }
        } else {
            mesh_default_tangent_from_normal(n, t);
        }
    }
    free(tan1);
    free(tan2);
    mesh3d_bump_vertex_revision(m, 0);
    m->tangents_ready = 1;
    m->tangent_revision = m->geometry_revision;
}

/// @brief Recompute per-vertex tangents for the mesh (used by normal/parallax mapping).
/// @details Validates the handle and delegates to the implementation, which derives tangents from
///          positions and UVs and falls back to a normal-derived tangent on degenerate UVs.
/// @param obj Mesh3D receiver; invalid handles are ignored.
void rt_mesh3d_calc_tangents(void *obj) {
    rt_mesh3d_calc_tangents_impl(mesh3d_checked(obj));
}

//=============================================================================
// OBJ Loading (Wavefront)
//=============================================================================

/// @brief Read one line from an OBJ file into a dynamically resizing buffer.
/// @details Grows `*line` geometrically (starting at 256 bytes) when the line
///   exceeds the current capacity. NUL-terminates the line including the
///   trailing newline if present. Returns 1 for a successful read, 0 on
///   end-of-file with no trailing newline, -1 on allocation failure, or -2
///   when the line exceeds @p max_bytes.
/// @param f Open binary stream to read.
/// @param line Address of the reusable owned line buffer.
/// @param cap Address of the current buffer capacity, updated after growth.
/// @param max_bytes Hard maximum buffer/line size including termination space.
/// @return One for a line, zero for clean EOF, negative one for invalid input or allocation
///         failure, or negative two when the line exceeds @p max_bytes.
static int mesh3d_read_text_line(FILE *f, char **line, size_t *cap, size_t max_bytes) {
    size_t len = 0;
    int ch;
    if (!f || !line || !cap)
        return -1;
    if (max_bytes < 2)
        return -1;
    if (!*line || *cap == 0) {
        *cap = 256;
        if (*cap > max_bytes)
            *cap = max_bytes;
        *line = (char *)malloc(*cap);
        if (!*line) {
            *cap = 0;
            return -1;
        }
    }
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1 >= *cap) {
            size_t new_cap;
            char *tmp;
            if (*cap > SIZE_MAX / 2u)
                return -1;
            new_cap = *cap * 2u;
            if (new_cap > max_bytes)
                new_cap = max_bytes;
            if (new_cap <= *cap)
                return -2;
            tmp = (char *)realloc(*line, new_cap);
            if (!tmp)
                return -1;
            *line = tmp;
            *cap = new_cap;
        }
        (*line)[len++] = (char)ch;
        if (len >= max_bytes)
            return -2;
        if (ch == '\n')
            break;
    }
    if (ch == EOF && len == 0)
        return 0;
    (*line)[len] = '\0';
    return 1;
}

/// @brief Parse `components` (2 or 3) floats from *p into array[*cnt * components],
///   growing the array as needed. Matches the v/vn/vt directive semantics: all
///   components are parsed, then range-checked, then stored. Returns 1 on success,
///   0 on a parse, overflow, or non-finite-value failure.
/// @param p Address of the source-line cursor.
/// @param array Address of the packed float attribute array, updated if it grows.
/// @param cnt Address of the logical attribute count, incremented on success.
/// @param cap Address of the logical capacity, updated after growth.
/// @param components Number of components to parse and store for this directive.
/// @return One on success, or zero for malformed values, range error, overflow, or allocation
///         failure.
static int obj_parse_float_attrib(
    const char **p, float **array, int *cnt, int *cap, int components) {
    if (*cnt >= *cap) {
        if (!obj_grow_float_array(array, cap, components))
            return 0;
    }
    double vals[3];
    for (int i = 0; i < components; i++) {
        if (!obj_parse_double(p, &vals[i]))
            return 0;
    }
    for (int i = 0; i < components; i++) {
        if (!mesh_value_fits_float(vals[i]))
            return 0;
    }
    for (int i = 0; i < components; i++)
        (*array)[(*cnt) * components + i] = (float)vals[i];
    (*cnt)++;
    return 1;
}

/// @brief Parse one OBJ `f` face (p points just past "f "): collect its v/vt/vn
///   vertices, resolve and emit/reuse mesh vertices, then ear-clip triangulate.
///   Sets *out_missing_normals when any vertex lacked a normal. Returns 1 to keep
///   parsing (including faces skipped for having < 3 vertices), 0 on a fatal
///   parse/allocation failure.
/// @param p Cursor immediately after the OBJ face directive.
/// @param mesh Destination Mesh3D under construction.
/// @param vertex_cache Per-mesh OBJ tuple deduplication cache.
/// @param positions Packed three-float position array.
/// @param cnt_p Number of defined positions.
/// @param normals Packed three-float normal array.
/// @param cnt_n Number of defined normals.
/// @param texcoords Packed two-float texture-coordinate array.
/// @param cnt_t Number of defined texture coordinates.
/// @param out_missing_normals Sticky output set when a face omits a normal index.
/// @return One when parsing may continue, or zero on a fatal face, allocation, or mesh-build error.
static int obj_parse_face_line(const char *p,
                               void *mesh,
                               obj_vertex_cache_t *vertex_cache,
                               const float *positions,
                               int cnt_p,
                               const float *normals,
                               int cnt_n,
                               const float *texcoords,
                               int cnt_t,
                               int *out_missing_normals) {
    size_t face_capacity = 8;
    obj_face_vert_t face_stack[8];
    uint32_t mesh_indices_stack[8];
    obj_face_vert_t *face = face_stack;
    uint32_t *mesh_indices = mesh_indices_stack;
    int face_count = 0;
    int parse_failed = 0;

    while (*p && *p != '\n' && *p != '\r') {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#')
            break;
        if (face_count >= MESH3D_OBJ_MAX_FACE_VERTS) {
            parse_failed = 1;
            break;
        }

        if ((size_t)face_count >= face_capacity) {
            if (face_capacity > SIZE_MAX / 2u || face_capacity * 2u > SIZE_MAX / sizeof(*face)) {
                parse_failed = 1;
                break;
            }
            size_t new_capacity = face_capacity * 2u;
            obj_face_vert_t *new_face = (obj_face_vert_t *)malloc(new_capacity * sizeof(*new_face));
            if (!new_face) {
                parse_failed = 1;
                break;
            }
            memcpy(new_face, face, (size_t)face_count * sizeof(*face));
            if (face != face_stack)
                free(face);
            face = new_face;
            face_capacity = new_capacity;
        }

        const char *before = p;
        if (!obj_parse_face_vert(
                &p, &face[face_count].vi, &face[face_count].ti, &face[face_count].ni) ||
            p == before || face[face_count].vi == 0) {
            parse_failed = 1;
            break;
        }
        face_count++;
    }

    if (parse_failed) {
        if (face != face_stack)
            free(face);
        return 0;
    }

    if (face_count < 3) {
        if (face != face_stack)
            free(face);
        return 1;
    }

    if (face_count > (int)(sizeof(mesh_indices_stack) / sizeof(mesh_indices_stack[0]))) {
        mesh_indices = (uint32_t *)malloc((size_t)face_count * sizeof(uint32_t));
        if (!mesh_indices) {
            if (face != face_stack)
                free(face);
            return 0;
        }
    }

    /* Resolve indices and emit or reuse vertices */
    for (int fi = 0; fi < face_count; fi++) {
        int64_t vi = face[fi].vi;
        int64_t ti = face[fi].ti;
        int64_t ni = face[fi].ni;
        if (!obj_resolve_index(vi, cnt_p, 0, &vi) || !obj_resolve_index(ti, cnt_t, 1, &ti) ||
            !obj_resolve_index(ni, cnt_n, 1, &ni) ||
            !obj_get_or_add_mesh_vertex(mesh,
                                        vertex_cache,
                                        positions,
                                        cnt_p,
                                        normals,
                                        cnt_n,
                                        texcoords,
                                        cnt_t,
                                        vi,
                                        ti,
                                        ni,
                                        &mesh_indices[fi])) {
            parse_failed = 1;
            break;
        }
        if (ni == 0)
            *out_missing_normals = 1;
    }

    /* Ear-clip triangulation preserves concave n-gons. */
    if (!parse_failed) {
        if (!obj_triangulate_face(mesh, mesh_indices, face_count))
            parse_failed = 1;
    }
    if (face != face_stack)
        free(face);
    if (mesh_indices != mesh_indices_stack)
        free(mesh_indices);
    return !parse_failed;
}

/// @brief Parse an opened OBJ stream into `mesh`, accumulating v/vn/vt into the temp
///   arrays (grown in place) and emitting de-duplicated faces. Extracted parse loop of
///   rt_mesh3d_from_obj; owns the line buffer. Sets *p_parse_failed on malformed input.
/// @param f Open OBJ stream.
/// @param mesh Destination Mesh3D under construction.
/// @param vertex_cache Per-mesh OBJ tuple deduplication cache.
/// @param p_positions Address of the owned packed position array.
/// @param p_cnt_p Address of the position count.
/// @param p_cap_p Address of the position capacity.
/// @param p_normals Address of the owned packed normal array.
/// @param p_cnt_n Address of the normal count.
/// @param p_cap_n Address of the normal capacity.
/// @param p_texcoords Address of the owned packed texture-coordinate array.
/// @param p_cnt_t Address of the texture-coordinate count.
/// @param p_cap_t Address of the texture-coordinate capacity.
/// @param p_parse_failed Sticky malformed-input or allocation failure output.
/// @param p_missing_normals Sticky output indicating at least one omitted normal.
/// @param p_line_too_long Optional sticky output for the line-size limit.
/// @param p_error_line Optional first-failing one-based line number.
static void obj_parse_into_mesh(FILE *f,
                                void *mesh,
                                obj_vertex_cache_t *vertex_cache,
                                float **p_positions,
                                int *p_cnt_p,
                                int *p_cap_p,
                                float **p_normals,
                                int *p_cnt_n,
                                int *p_cap_n,
                                float **p_texcoords,
                                int *p_cnt_t,
                                int *p_cap_t,
                                int *p_parse_failed,
                                int *p_missing_normals,
                                int *p_line_too_long,
                                uint64_t *p_error_line) {
    float *positions = *p_positions;
    int cnt_p = *p_cnt_p;
    int cap_p = *p_cap_p;
    float *normals = *p_normals;
    int cnt_n = *p_cnt_n;
    int cap_n = *p_cap_n;
    float *texcoords = *p_texcoords;
    int cnt_t = *p_cnt_t;
    int cap_t = *p_cap_t;
    int parse_failed = *p_parse_failed;
    int missing_normals = *p_missing_normals;
    char *line = NULL;
    size_t line_cap = 0;
    int line_status;
    uint64_t line_number = 0;
    while ((line_status = mesh3d_read_text_line(f, &line, &line_cap, MESH3D_OBJ_MAX_LINE_BYTES)) >
           0) {
        const char *p = line;
        line_number++;
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        if (p[0] == 'v' && p[1] == ' ') {
            /* Vertex position: v x y z */
            p += 2;
            if (!obj_parse_float_attrib(&p, &positions, &cnt_p, &cap_p, 3)) {
                parse_failed = 1;
                obj_record_error_line(p_error_line, line_number);
                break;
            }
        } else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
            /* Vertex normal: vn x y z */
            p += 3;
            if (!obj_parse_float_attrib(&p, &normals, &cnt_n, &cap_n, 3)) {
                parse_failed = 1;
                obj_record_error_line(p_error_line, line_number);
                break;
            }
        } else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') {
            /* Texture coordinate: vt u v */
            p += 3;
            if (!obj_parse_float_attrib(&p, &texcoords, &cnt_t, &cap_t, 2)) {
                parse_failed = 1;
                obj_record_error_line(p_error_line, line_number);
                break;
            }
        } else if (p[0] == 'f' && p[1] == ' ') {
            /* Face: f v1[/vt1[/vn1]] v2[/vt2[/vn2]] ... */
            p += 2;
            if (!obj_parse_face_line(p,
                                     mesh,
                                     vertex_cache,
                                     positions,
                                     cnt_p,
                                     normals,
                                     cnt_n,
                                     texcoords,
                                     cnt_t,
                                     &missing_normals)) {
                parse_failed = 1;
                obj_record_error_line(p_error_line, line_number);
                break;
            }
        } else if (strncmp(p, "mtllib ", 7) == 0 || strncmp(p, "usemtl ", 7) == 0 ||
                   strncmp(p, "g ", 2) == 0 || strncmp(p, "o ", 2) == 0) {
            continue;
        }
        /* Ignore: s and other non-geometry directives. */
    }
    if (line_status == -2) {
        parse_failed = 1;
        if (p_line_too_long)
            *p_line_too_long = 1;
        obj_record_error_line(p_error_line, line_number + 1u);
    } else if (line_status < 0) {
        parse_failed = 1;
        obj_record_error_line(p_error_line, line_number + 1u);
    }
    free(line);
    *p_positions = positions;
    *p_cnt_p = cnt_p;
    *p_cap_p = cap_p;
    *p_normals = normals;
    *p_cnt_n = cnt_n;
    *p_cap_n = cap_n;
    *p_texcoords = texcoords;
    *p_cnt_t = cnt_t;
    *p_cap_t = cap_t;
    *p_parse_failed = parse_failed;
    *p_missing_normals = missing_normals;
}

/// @brief Load a mesh from a Wavefront OBJ file (positions, normals, UVs, faces).
/// @details Parses v/vn/vt/f lines from the OBJ text format. Supports
///          triangulated and quad faces (quads are split into two triangles).
///          Vertex data is de-duplicated by unique (position, normal, UV) tuples.
/// @param path File path to the .obj file (runtime string).
/// @return Mesh handle, or NULL on parse/load failure.
void *rt_mesh3d_from_obj(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Mesh3D.FromOBJ: path must not be null");
        return NULL;
    }
    const char *filepath = rt_string_cstr(path);
    if (!filepath) {
        rt_asset_error_end_load_failure();
        rt_trap("Mesh3D.FromOBJ: invalid path");
        return NULL;
    }

    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Mesh3D.FromOBJ: '%s' not found", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    /* Temporary arrays for positions, normals, UVs */
    int cap_p = 256, cap_n = 256, cap_t = 256;
    int cnt_p = 0, cnt_n = 0, cnt_t = 0;
    int parse_failed = 0;
    int missing_normals = 0;
    int line_too_long = 0;
    uint64_t error_line = 0;
    float *positions = (float *)malloc((size_t)cap_p * 3 * sizeof(float));
    float *normals = (float *)malloc((size_t)cap_n * 3 * sizeof(float));
    float *texcoords = (float *)malloc((size_t)cap_t * 2 * sizeof(float));
    obj_vertex_cache_t vertex_cache;
    memset(&vertex_cache, 0, sizeof(vertex_cache));

    void *mesh = rt_mesh3d_new();
    if (!mesh || !positions || !normals || !texcoords ||
        !obj_vertex_cache_init(&vertex_cache, 1024)) {
        fclose(f);
        free(positions);
        free(normals);
        free(texcoords);
        obj_vertex_cache_free(&vertex_cache);
        if (mesh && rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);

    obj_parse_into_mesh(f,
                        mesh,
                        &vertex_cache,
                        &positions,
                        &cnt_p,
                        &cap_p,
                        &normals,
                        &cnt_n,
                        &cap_n,
                        &texcoords,
                        &cnt_t,
                        &cap_t,
                        &parse_failed,
                        &missing_normals,
                        &line_too_long,
                        &error_line);

    fclose(f);
    free(positions);
    free(normals);
    free(texcoords);
    obj_vertex_cache_free(&vertex_cache);

    if (parse_failed || ((rt_mesh3d *)mesh)->build_failed) {
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        if (error_line != 0) {
            rt_asset_error_setf(
                line_too_long ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
                line_too_long ? "Mesh3D.FromOBJ: line %llu exceeds 1 MiB limit"
                              : "Mesh3D.FromOBJ: invalid or unsupported geometry at line %llu",
                (unsigned long long)error_line);
        } else {
            rt_asset_error_set(line_too_long ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
                               line_too_long ? "Mesh3D.FromOBJ: line exceeds 1 MiB limit"
                                             : "Mesh3D.FromOBJ: invalid or unsupported geometry");
        }
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (((rt_mesh3d *)mesh)->index_count < 3) {
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        rt_asset_error_set(RT_ASSET_ERROR_UNSUPPORTED,
                           "Mesh3D.FromOBJ: no supported triangle geometry");
        rt_asset_error_end_load_failure();
        return NULL;
    }

    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);

    if ((cnt_n == 0 || missing_normals) && ((rt_mesh3d *)mesh)->vertex_count > 0 &&
        !mesh3d_fill_missing_normals((rt_mesh3d *)mesh)) {
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_TOO_LARGE,
                                    "Mesh3D.FromOBJ: failed to fill missing normals");
        rt_asset_error_end_load_failure();
        return NULL;
    }

    rt_asset_error_end_load_success();
    return mesh;
}

typedef struct {
    rt_mesh3d_obj_group_t group;
    obj_vertex_cache_t vertex_cache;
    int missing_normals;
} obj_group_builder_t;

/// @brief Free an array of in-progress OBJ group builders: each group's material name, vertex
///   cache, and partially built mesh, then the array.
/// @param groups Owned builder array; NULL is accepted.
/// @param count Number of initialized entries to release.
static void obj_group_builders_free(obj_group_builder_t *groups, int32_t count) {
    if (!groups)
        return;
    for (int32_t i = 0; i < count; i++) {
        free(groups[i].group.material_name);
        groups[i].group.material_name = NULL;
        obj_vertex_cache_free(&groups[i].vertex_cache);
        if (groups[i].group.mesh && rt_obj_release_check0(groups[i].group.mesh))
            rt_obj_free(groups[i].group.mesh);
        groups[i].group.mesh = NULL;
    }
    free(groups);
}

/// @brief Free an array of OBJ mesh groups (each group's material name and Mesh3D) plus the
///   array itself. Pairs with rt_mesh3d_from_obj_groups.
/// @param groups Owned public group array; NULL is accepted.
/// @param count Number of initialized groups to release.
void rt_mesh3d_obj_groups_free(rt_mesh3d_obj_group_t *groups, int32_t count) {
    if (!groups)
        return;
    for (int32_t i = 0; i < count; i++) {
        free(groups[i].material_name);
        groups[i].material_name = NULL;
        if (groups[i].mesh && rt_obj_release_check0(groups[i].mesh))
            rt_obj_free(groups[i].mesh);
        groups[i].mesh = NULL;
    }
    free(groups);
}

/// @brief Copy an OBJ/MTL directive's value with surrounding whitespace trimmed; NULL on
///   allocation failure.
/// @param p Nullable directive-value text.
/// @return Newly allocated NUL-terminated trimmed value, or NULL on allocation failure.
static char *obj_copy_directive_value(const char *p) {
    const char *start;
    const char *end;
    char *copy;
    size_t len;
    if (!p)
        p = "";
    while (*p == ' ' || *p == '\t')
        p++;
    start = p;
    end = p + strlen(p);
    while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        end--;
    len = (size_t)(end - start);
    copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Copy one OBJ directive token, honoring single or double quotes for names with spaces.
/// @param p Nullable cursor before optional whitespace and a token.
/// @return Newly allocated token, or NULL for an unterminated quote or allocation failure.
static char *obj_copy_directive_token(const char *p) {
    const char *start;
    const char *end;
    if (!p)
        p = "";
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        size_t len;
        char *copy;
        start = p;
        while (*p && *p != quote && *p != '\n' && *p != '\r')
            p++;
        if (*p != quote)
            return NULL;
        end = p;
        len = (size_t)(end - start);
        copy = (char *)malloc(len + 1u);
        if (!copy)
            return NULL;
        memcpy(copy, start, len);
        copy[len] = '\0';
        return copy;
    }
    start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#')
        p++;
    end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    {
        size_t len = (size_t)(end - start);
        char *copy = (char *)malloc(len + 1u);
        if (!copy)
            return NULL;
        memcpy(copy, start, len);
        copy[len] = '\0';
        return copy;
    }
}

/// @brief Find the OBJ group builder for @p material_name, or create one (with a fresh mesh
///   and vertex cache) and append it, growing the array as needed.
/// @param groups Address of the owned builder-array pointer.
/// @param count Address of the initialized builder count.
/// @param capacity Address of the builder-array capacity.
/// @param material_name Nullable material name used as the group key.
/// @param out_group Receives the existing or newly initialized builder.
/// @return 1 with @p out_group set, 0 on allocation failure.
static int obj_group_find_or_create(obj_group_builder_t **groups,
                                    int32_t *count,
                                    int32_t *capacity,
                                    const char *material_name,
                                    obj_group_builder_t **out_group) {
    const char *name = material_name ? material_name : "";
    obj_group_builder_t *grown;
    char *name_copy;
    void *mesh;
    if (!groups || !count || !capacity || !out_group)
        return 0;
    for (int32_t i = 0; i < *count; i++) {
        const char *existing = (*groups)[i].group.material_name;
        if (strcmp(existing ? existing : "", name) == 0) {
            *out_group = &(*groups)[i];
            return 1;
        }
    }
    if (*count >= *capacity) {
        int32_t new_capacity = *capacity > 0 ? *capacity * 2 : 4;
        if (*capacity > INT32_MAX / 2 || (size_t)new_capacity > SIZE_MAX / sizeof(**groups))
            return 0;
        grown = (obj_group_builder_t *)realloc(*groups, (size_t)new_capacity * sizeof(**groups));
        if (!grown)
            return 0;
        memset(grown + *capacity, 0, (size_t)(new_capacity - *capacity) * sizeof(*grown));
        *groups = grown;
        *capacity = new_capacity;
    }
    name_copy = obj_copy_directive_value(name);
    mesh = rt_mesh3d_new();
    if (!name_copy || !mesh) {
        free(name_copy);
        if (mesh && rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return 0;
    }
    if (!obj_vertex_cache_init(&(*groups)[*count].vertex_cache, 1024)) {
        free(name_copy);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return 0;
    }
    (*groups)[*count].group.material_name = name_copy;
    (*groups)[*count].group.mesh = mesh;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    *out_group = &(*groups)[*count];
    (*count)++;
    return 1;
}

/// @brief Move the finished meshes and material names from the internal @p builders into a
///   newly allocated public group array (@p out_groups / @p out_count), transferring ownership.
/// @param builders Owned internal builder array.
/// @param builder_count Number of initialized builders to transfer.
/// @param out_groups Receives the newly allocated public group array.
/// @param out_count Receives the number of transferred groups.
/// @return 1 on success, 0 on allocation failure.
static int obj_groups_transfer(obj_group_builder_t *builders,
                               int32_t builder_count,
                               rt_mesh3d_obj_group_t **out_groups,
                               int32_t *out_count) {
    rt_mesh3d_obj_group_t *groups;
    if (!out_groups || !out_count)
        return 0;
    *out_groups = NULL;
    *out_count = 0;
    if (builder_count <= 0)
        return 1;
    groups = (rt_mesh3d_obj_group_t *)calloc((size_t)builder_count, sizeof(*groups));
    if (!groups)
        return 0;
    for (int32_t i = 0; i < builder_count; i++) {
        groups[i].material_name = builders[i].group.material_name;
        groups[i].mesh = builders[i].group.mesh;
        builders[i].group.material_name = NULL;
        builders[i].group.mesh = NULL;
        obj_vertex_cache_free(&builders[i].vertex_cache);
    }
    free(builders);
    *out_groups = groups;
    *out_count = builder_count;
    return 1;
}

/// @brief Parse one OBJ `f` face (p just past "f ") into the material group named
///   by `current_material`, creating the group on demand. Collects v/vt/vn
///   vertices, resolves and emits/reuses mesh vertices, then ear-clip
///   triangulates. Returns 1 to keep parsing (including < 3-vertex faces, which
///   are skipped), 0 on a fatal parse/allocation failure.
/// @param p Cursor immediately after the OBJ face directive.
/// @param groups Address of the owned material-group builder array.
/// @param group_count Address of the initialized group count.
/// @param group_capacity Address of the group-array capacity.
/// @param current_material Current `usemtl` name used to select or create the destination group.
/// @param positions Packed three-float position array.
/// @param cnt_p Number of defined positions.
/// @param normals Packed three-float normal array.
/// @param cnt_n Number of defined normals.
/// @param texcoords Packed two-float texture-coordinate array.
/// @param cnt_t Number of defined texture coordinates.
/// @return One when parsing may continue, or zero on a fatal face, allocation, or mesh-build error.
static int obj_parse_face_line_grouped(const char *p,
                                       obj_group_builder_t **groups,
                                       int32_t *group_count,
                                       int32_t *group_capacity,
                                       const char *current_material,
                                       const float *positions,
                                       int cnt_p,
                                       const float *normals,
                                       int cnt_n,
                                       const float *texcoords,
                                       int cnt_t) {
    size_t face_capacity = 8;
    obj_face_vert_t face_stack[8];
    uint32_t mesh_indices_stack[8];
    obj_face_vert_t *face = face_stack;
    uint32_t *mesh_indices = mesh_indices_stack;
    int face_count = 0;
    int parse_failed = 0;
    obj_group_builder_t *group = NULL;

    while (*p && *p != '\n' && *p != '\r') {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '#')
            break;
        if (face_count >= MESH3D_OBJ_MAX_FACE_VERTS) {
            parse_failed = 1;
            break;
        }
        if ((size_t)face_count >= face_capacity) {
            size_t new_capacity;
            obj_face_vert_t *new_face;
            if (face_capacity > SIZE_MAX / 2u || face_capacity * 2u > SIZE_MAX / sizeof(*face)) {
                parse_failed = 1;
                break;
            }
            new_capacity = face_capacity * 2u;
            if (face == face_stack) {
                new_face = (obj_face_vert_t *)malloc(new_capacity * sizeof(*new_face));
                if (new_face)
                    memcpy(new_face, face_stack, (size_t)face_count * sizeof(*face_stack));
            } else {
                new_face = (obj_face_vert_t *)realloc(face, new_capacity * sizeof(*new_face));
            }
            if (!new_face) {
                parse_failed = 1;
                break;
            }
            face = new_face;
            face_capacity = new_capacity;
        }
        {
            const char *before = p;
            if (!obj_parse_face_vert(
                    &p, &face[face_count].vi, &face[face_count].ti, &face[face_count].ni) ||
                p == before || face[face_count].vi == 0) {
                parse_failed = 1;
                break;
            }
        }
        face_count++;
    }
    if (!parse_failed && face_count >= 3) {
        if (face_count > (int)(sizeof(mesh_indices_stack) / sizeof(mesh_indices_stack[0]))) {
            mesh_indices = (uint32_t *)malloc((size_t)face_count * sizeof(uint32_t));
            if (!mesh_indices)
                parse_failed = 1;
        }
        if (!parse_failed && !obj_group_find_or_create(
                                 groups, group_count, group_capacity, current_material, &group)) {
            parse_failed = 1;
        }
        for (int fi = 0; !parse_failed && fi < face_count; fi++) {
            int64_t vi = face[fi].vi;
            int64_t ti = face[fi].ti;
            int64_t ni = face[fi].ni;
            if (!obj_resolve_index(vi, cnt_p, 0, &vi) || !obj_resolve_index(ti, cnt_t, 1, &ti) ||
                !obj_resolve_index(ni, cnt_n, 1, &ni) ||
                !obj_get_or_add_mesh_vertex(group->group.mesh,
                                            &group->vertex_cache,
                                            positions,
                                            cnt_p,
                                            normals,
                                            cnt_n,
                                            texcoords,
                                            cnt_t,
                                            vi,
                                            ti,
                                            ni,
                                            &mesh_indices[fi])) {
                parse_failed = 1;
                break;
            }
            if (ni == 0)
                group->missing_normals = 1;
        }
        if (!parse_failed && !obj_triangulate_face(group->group.mesh, mesh_indices, face_count))
            parse_failed = 1;
    }
    if (face != face_stack)
        free(face);
    if (mesh_indices != mesh_indices_stack)
        free(mesh_indices);
    return !parse_failed;
}

/// @brief Finalize each material group's mesh: end its geometry batch, drop empty
///   groups, recompute or backfill missing normals, and compact survivors to the
///   front of `groups`. Returns the surviving group count; sets *p_parse_failed if
///   any group's geometry build failed.
/// @param groups Builder array to finalize and compact in place.
/// @param group_count Number of initialized builders.
/// @param cnt_n Total number of OBJ normals, used to detect files without authored normals.
/// @param p_parse_failed Address of the sticky parse-failure flag.
/// @return Surviving group count on success, or the original count when failure requires full
///         cleanup.
static int32_t obj_finalize_groups(obj_group_builder_t *groups,
                                   int32_t group_count,
                                   int cnt_n,
                                   int *p_parse_failed) {
    int parse_failed = *p_parse_failed;
    int32_t original_group_count = group_count;
    int32_t write_count = 0;
    for (int32_t i = 0; i < group_count; i++) {
        rt_mesh3d *mesh = (rt_mesh3d *)groups[i].group.mesh;
        if (!mesh)
            continue;
        rt_mesh3d_end_geometry_batch(mesh);
        if (mesh->build_failed) {
            parse_failed = 1;
            break;
        }
        if (mesh->index_count < 3u) {
            free(groups[i].group.material_name);
            groups[i].group.material_name = NULL;
            obj_vertex_cache_free(&groups[i].vertex_cache);
            if (groups[i].group.mesh && rt_obj_release_check0(groups[i].group.mesh))
                rt_obj_free(groups[i].group.mesh);
            groups[i].group.mesh = NULL;
            continue;
        }
        if (!parse_failed && (cnt_n == 0 || groups[i].missing_normals) && mesh->vertex_count > 0 &&
            !mesh3d_fill_missing_normals(mesh))
            parse_failed = 1;
        if (write_count != i) {
            groups[write_count] = groups[i];
            memset(&groups[i], 0, sizeof(groups[i]));
        }
        write_count++;
    }
    *p_parse_failed = parse_failed;
    return parse_failed ? original_group_count : write_count;
}

/// @brief Parse the Wavefront OBJ file at @p path into one Mesh3D per material, returned via
///   @p out_groups / @p out_count (caller frees with rt_mesh3d_obj_groups_free).
/// @param path Runtime string naming the OBJ file.
/// @param out_groups Receives an owned array of material-name and Mesh3D pairs.
/// @param out_count Receives the number of groups in @p out_groups.
/// @return 1 on success, 0 on open/parse/allocation failure.
int rt_mesh3d_from_obj_groups(rt_string path,
                              rt_mesh3d_obj_group_t **out_groups,
                              int32_t *out_count) {
    const char *filepath;
    FILE *f;
    int cap_p = 256, cap_n = 256, cap_t = 256;
    int cnt_p = 0, cnt_n = 0, cnt_t = 0;
    int parse_failed = 0;
    int line_too_long = 0;
    uint64_t line_number = 0;
    uint64_t error_line = 0;
    float *positions;
    float *normals;
    float *texcoords;
    obj_group_builder_t *groups = NULL;
    int32_t group_count = 0;
    int32_t group_capacity = 0;
    char *current_material = NULL;
    char *line = NULL;
    size_t line_cap = 0;
    int line_status;

    if (out_groups)
        *out_groups = NULL;
    if (out_count)
        *out_count = 0;
    if (!path || !out_groups || !out_count) {
        rt_trap("Mesh3D.FromOBJ: path must not be null");
        return 0;
    }
    filepath = rt_string_cstr(path);
    if (!filepath) {
        rt_trap("Mesh3D.FromOBJ: invalid path");
        return 0;
    }
    f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Mesh3D.FromOBJ: '%s' not found", filepath);
        return 0;
    }
    positions = (float *)malloc((size_t)cap_p * 3u * sizeof(float));
    normals = (float *)malloc((size_t)cap_n * 3u * sizeof(float));
    texcoords = (float *)malloc((size_t)cap_t * 2u * sizeof(float));
    current_material = obj_copy_directive_value("");
    if (!positions || !normals || !texcoords || !current_material) {
        parse_failed = 1;
        goto done_reading;
    }

    while ((line_status = mesh3d_read_text_line(f, &line, &line_cap, MESH3D_OBJ_MAX_LINE_BYTES)) >
           0) {
        const char *p = line;
        line_number++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        if (p[0] == 'v' && p[1] == ' ') {
            p += 2;
            if (!obj_parse_float_attrib(&p, &positions, &cnt_p, &cap_p, 3)) {
                parse_failed = 1;
                obj_record_error_line(&error_line, line_number);
                break;
            }
        } else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
            p += 3;
            if (!obj_parse_float_attrib(&p, &normals, &cnt_n, &cap_n, 3)) {
                parse_failed = 1;
                obj_record_error_line(&error_line, line_number);
                break;
            }
        } else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') {
            p += 3;
            if (!obj_parse_float_attrib(&p, &texcoords, &cnt_t, &cap_t, 2)) {
                parse_failed = 1;
                obj_record_error_line(&error_line, line_number);
                break;
            }
        } else if (strncmp(p, "usemtl ", 7) == 0) {
            char *next = obj_copy_directive_token(p + 7);
            if (!next) {
                parse_failed = 1;
                obj_record_error_line(&error_line, line_number);
                break;
            }
            free(current_material);
            current_material = next;
        } else if (p[0] == 'f' && p[1] == ' ') {
            p += 2;
            if (!obj_parse_face_line_grouped(p,
                                             &groups,
                                             &group_count,
                                             &group_capacity,
                                             current_material,
                                             positions,
                                             cnt_p,
                                             normals,
                                             cnt_n,
                                             texcoords,
                                             cnt_t)) {
                parse_failed = 1;
                obj_record_error_line(&error_line, line_number);
                break;
            }
        }
    }
    if (line_status == -2) {
        parse_failed = 1;
        line_too_long = 1;
        obj_record_error_line(&error_line, line_number + 1u);
    } else if (line_status < 0) {
        parse_failed = 1;
        obj_record_error_line(&error_line, line_number + 1u);
    }

done_reading:
    fclose(f);
    free(line);
    free(current_material);
    free(positions);
    free(normals);
    free(texcoords);

    group_count = obj_finalize_groups(groups, group_count, cnt_n, &parse_failed);

    if (parse_failed || group_count <= 0) {
        obj_group_builders_free(groups, group_count);
        if (error_line != 0) {
            rt_asset_error_setf(
                line_too_long ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
                line_too_long ? "Mesh3D.FromOBJ: line %llu exceeds 1 MiB limit"
                              : "Mesh3D.FromOBJ: invalid or unsupported geometry at line %llu",
                (unsigned long long)error_line);
        } else {
            rt_asset_error_set(line_too_long ? RT_ASSET_ERROR_TOO_LARGE : RT_ASSET_ERROR_CORRUPT,
                               line_too_long ? "Mesh3D.FromOBJ: line exceeds 1 MiB limit"
                                             : "Mesh3D.FromOBJ: invalid or unsupported geometry");
        }
        return 0;
    }
    if (!obj_groups_transfer(groups, group_count, out_groups, out_count)) {
        obj_group_builders_free(groups, group_count);
        rt_trap("Mesh3D.FromOBJ: group allocation failed");
        return 0;
    }
    return 1;
}
