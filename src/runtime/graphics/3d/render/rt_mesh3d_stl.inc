//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/render/rt_mesh3d_stl.inc  (#included from rt_mesh3d.c; not a
// standalone translation unit) Purpose: Mesh3D STL loader (binary + ASCII)
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded binary and ASCII STL decoding into Mesh3D geometry.
/// @details This include fragment is compiled as part of `rt_mesh3d.c`. Both loaders validate
///          finite coordinates, skip degenerate facets, deduplicate exact shared vertices,
///          reconcile winding with authored facet normals, and recalculate smooth mesh normals.

//=============================================================================
// STL Loading (Binary + ASCII)
//=============================================================================

// ---------------------------------------------------------------------------
// STL parser helpers — STL is a much simpler 3D mesh format with
// two variants: binary (84-byte header + per-triangle records) and
// ASCII (text "facet normal …" blocks). Both lack texture coords
// and vertex sharing — every triangle is stored in full.
// ---------------------------------------------------------------------------

#define STL_BINARY_INITIAL_RESERVE_TRIANGLES 65536u
#define STL_MAX_FILE_BYTES (INT64_C(512) * 1024 * 1024)

/// @brief Read a little-endian uint32 from a binary STL byte stream.
/// @param p Pointer to at least four encoded bytes.
/// @return Host-order unsigned 32-bit value.
static uint32_t stl_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Read a little-endian IEEE-754 float from a binary STL byte stream.
/// @param p Pointer to at least four encoded bytes.
/// @return Decoded float bit pattern.
static float stl_read_f32_le(const uint8_t *p) {
    uint32_t bits =
        (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

/// @brief Return non-zero if a facet normal is finite and directionally useful.
/// @param normal Three-component facet normal.
/// @return Nonzero when all components are finite and squared length exceeds the degeneracy limit.
static int stl_normal_is_usable(const float *normal) {
    double nx;
    double ny;
    double nz;
    double len_sq;
    if (!normal || !isfinite(normal[0]) || !isfinite(normal[1]) || !isfinite(normal[2]))
        return 0;
    nx = (double)normal[0];
    ny = (double)normal[1];
    nz = (double)normal[2];
    len_sq = nx * nx + ny * ny + nz * nz;
    return len_sq > 1e-20;
}

/// @brief Cap the initial triangle reservation for a binary STL to a safe ceiling.
/// @details Avoids over-allocating from an untrusted header count; the buffer grows as facets are
///          actually read.
/// @param tri_count Untrusted declared facet count.
/// @return Initial facet reservation no greater than the configured cap.
static uint32_t stl_initial_reserve_triangles(uint32_t tri_count) {
    return tri_count < STL_BINARY_INITIAL_RESERVE_TRIANGLES ? tri_count
                                                            : STL_BINARY_INITIAL_RESERVE_TRIANGLES;
}

typedef struct {
    uint32_t x_bits;
    uint32_t y_bits;
    uint32_t z_bits;
    uint32_t mesh_index;
    uint8_t used;
} stl_vertex_cache_entry_t;

typedef struct {
    stl_vertex_cache_entry_t *entries;
    size_t capacity;
    size_t count;
} stl_vertex_cache_t;

/// @brief Convert a finite float coordinate to its stable IEEE bit pattern for exact matching.
/// @details Negative zero is canonicalized to positive zero so geometrically identical zero
///          coordinates share a cache key.
/// @param value Finite coordinate to encode.
/// @return Raw canonicalized IEEE-754 bits.
static uint32_t stl_float_bits(float value) {
    uint32_t bits;
    if (value == 0.0f)
        value = 0.0f;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// @brief Hash an exact STL vertex key into a 64-bit open-addressing probe seed.
/// @param x_bits Canonical X-coordinate bits.
/// @param y_bits Canonical Y-coordinate bits.
/// @param z_bits Canonical Z-coordinate bits.
/// @return Mixed 64-bit hash of the exact coordinate tuple.
static uint64_t stl_hash_vertex_bits(uint32_t x_bits, uint32_t y_bits, uint32_t z_bits) {
    uint64_t h = 1469598103934665603ull;
    h = (h ^ (uint64_t)x_bits) * 1099511628211ull;
    h = (h ^ (uint64_t)y_bits) * 1099511628211ull;
    h = (h ^ (uint64_t)z_bits) * 1099511628211ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

/// @brief Initialize the STL exact-vertex cache with power-of-two capacity.
/// @details STL has no explicit index stream, but many exporters repeat identical vertex triples
///          across adjacent facets. This cache maps exact float coordinate bit patterns to emitted
///          Mesh3D indices so the loader can build indexed geometry without merging authored seams
///          that are merely close rather than identical.
/// @param cache Cache object to initialize.
/// @param capacity Requested minimum entry capacity, rounded up to a power of two.
/// @return One on success, or zero for invalid input, overflow, or allocation failure.
static int stl_vertex_cache_init(stl_vertex_cache_t *cache, size_t capacity) {
    if (!cache)
        return 0;
    if (capacity < 1024u)
        capacity = 1024u;
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
    cache->entries = (stl_vertex_cache_entry_t *)calloc(capacity, sizeof(*cache->entries));
    cache->capacity = cache->entries ? capacity : 0;
    cache->count = 0;
    return cache->entries != NULL;
}

/// @brief Release storage owned by an STL exact-vertex cache.
/// @param cache Cache to clear; NULL is ignored.
static void stl_vertex_cache_free(stl_vertex_cache_t *cache) {
    if (!cache)
        return;
    free(cache->entries);
    cache->entries = NULL;
    cache->capacity = 0;
    cache->count = 0;
}

/// @brief Grow and rehash an STL exact-vertex cache.
/// @param cache Initialized cache to double.
/// @return One after successful allocation and rehash, or zero on invalid state or failure.
static int stl_vertex_cache_grow(stl_vertex_cache_t *cache) {
    stl_vertex_cache_t grown;
    size_t new_capacity;
    if (!cache || !cache->entries || cache->capacity > SIZE_MAX / 2u)
        return 0;
    new_capacity = cache->capacity * 2u;
    if (!stl_vertex_cache_init(&grown, new_capacity))
        return 0;
    for (size_t i = 0; i < cache->capacity; i++) {
        stl_vertex_cache_entry_t entry = cache->entries[i];
        if (!entry.used)
            continue;
        uint64_t h = stl_hash_vertex_bits(entry.x_bits, entry.y_bits, entry.z_bits);
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

/// @brief Look up an exact STL vertex key in the cache.
/// @param cache Initialized exact-coordinate cache.
/// @param x_bits Canonical X-coordinate bits.
/// @param y_bits Canonical Y-coordinate bits.
/// @param z_bits Canonical Z-coordinate bits.
/// @param out_index Optional destination for the cached Mesh3D vertex index.
/// @return One when the key exists; otherwise zero.
static int stl_vertex_cache_lookup(const stl_vertex_cache_t *cache,
                                   uint32_t x_bits,
                                   uint32_t y_bits,
                                   uint32_t z_bits,
                                   uint32_t *out_index) {
    if (!cache || !cache->entries || cache->capacity == 0)
        return 0;
    uint64_t h = stl_hash_vertex_bits(x_bits, y_bits, z_bits);
    for (size_t probe = 0; probe < cache->capacity; probe++) {
        size_t slot = (size_t)((h + probe) & (uint64_t)(cache->capacity - 1u));
        const stl_vertex_cache_entry_t *entry = &cache->entries[slot];
        if (!entry->used)
            return 0;
        if (entry->x_bits == x_bits && entry->y_bits == y_bits && entry->z_bits == z_bits) {
            if (out_index)
                *out_index = entry->mesh_index;
            return 1;
        }
    }
    return 0;
}

/// @brief Insert an exact STL vertex key into the cache, growing before long probe chains form.
/// @param cache Initialized exact-coordinate cache.
/// @param x_bits Canonical X-coordinate bits.
/// @param y_bits Canonical Y-coordinate bits.
/// @param z_bits Canonical Z-coordinate bits.
/// @param mesh_index Mesh3D vertex index associated with the key.
/// @return One after insertion, or zero when growth or probing fails.
static int stl_vertex_cache_insert(stl_vertex_cache_t *cache,
                                   uint32_t x_bits,
                                   uint32_t y_bits,
                                   uint32_t z_bits,
                                   uint32_t mesh_index) {
    if (!cache || !cache->entries)
        return 0;
    size_t next_count = cache->count + 1u;
    size_t grow_limit = (cache->capacity / 10u) * 7u + ((cache->capacity % 10u) * 7u) / 10u;
    if (next_count < cache->count || next_count > grow_limit) {
        if (!stl_vertex_cache_grow(cache))
            return 0;
    }
    uint64_t h = stl_hash_vertex_bits(x_bits, y_bits, z_bits);
    for (size_t probe = 0; probe < cache->capacity; probe++) {
        size_t slot = (size_t)((h + probe) & (uint64_t)(cache->capacity - 1u));
        stl_vertex_cache_entry_t *entry = &cache->entries[slot];
        if (!entry->used) {
            entry->x_bits = x_bits;
            entry->y_bits = y_bits;
            entry->z_bits = z_bits;
            entry->mesh_index = mesh_index;
            entry->used = 1;
            cache->count++;
            return 1;
        }
    }
    return 0;
}

/// @brief Emit or reuse one STL vertex and return its Mesh3D index.
/// @details Cache insertion is best-effort. If the dedup table cannot grow, the loader still emits
///          a valid non-deduplicated vertex rather than failing an otherwise valid STL asset.
/// @param mesh Destination Mesh3D under construction.
/// @param cache Optional exact-coordinate deduplication cache.
/// @param x Vertex X coordinate.
/// @param y Vertex Y coordinate.
/// @param z Vertex Z coordinate.
/// @param out_index Receives the reused or appended Mesh3D vertex index.
/// @return One on successful reuse or append; zero for invalid input, overflow, or mesh-build
///         failure.
static int stl_get_or_add_vertex(
    void *mesh, stl_vertex_cache_t *cache, float x, float y, float z, uint32_t *out_index) {
    rt_mesh3d *m = (rt_mesh3d *)mesh;
    uint32_t x_bits;
    uint32_t y_bits;
    uint32_t z_bits;
    uint32_t index;
    if (!m || !out_index)
        return 0;
    x_bits = stl_float_bits(x);
    y_bits = stl_float_bits(y);
    z_bits = stl_float_bits(z);
    if (stl_vertex_cache_lookup(cache, x_bits, y_bits, z_bits, out_index))
        return 1;
    if (m->vertex_count == UINT32_MAX)
        return 0;
    index = m->vertex_count;
    rt_mesh3d_add_vertex(mesh, x, y, z, 0, 0, 0, 0, 0);
    if (m->build_failed || m->vertex_count <= index)
        return 0;
    (void)stl_vertex_cache_insert(cache, x_bits, y_bits, z_bits, index);
    *out_index = index;
    return 1;
}

/// @brief Return non-zero when STL vertex order should be flipped to match facet normal.
/// @param verts Packed three-vertex position array with nine floats.
/// @param normal Authored facet normal; unusable normals preserve source winding.
/// @return Nonzero when the geometric cross product opposes the authored normal.
static int stl_triangle_should_flip(const float *verts, const float *normal) {
    double abx;
    double aby;
    double abz;
    double acx;
    double acy;
    double acz;
    double nx;
    double ny;
    double nz;
    double dot;
    if (!verts || !stl_normal_is_usable(normal))
        return 0;
    abx = (double)verts[3] - (double)verts[0];
    aby = (double)verts[4] - (double)verts[1];
    abz = (double)verts[5] - (double)verts[2];
    acx = (double)verts[6] - (double)verts[0];
    acy = (double)verts[7] - (double)verts[1];
    acz = (double)verts[8] - (double)verts[2];
    nx = aby * acz - abz * acy;
    ny = abz * acx - abx * acz;
    nz = abx * acy - aby * acx;
    dot = nx * (double)normal[0] + ny * (double)normal[1] + nz * (double)normal[2];
    return dot < 0.0;
}

/// @brief Append one STL triangle, skipping zero-area faces without failing the entire import.
/// @param mesh Destination Mesh3D under construction.
/// @param cache Exact-coordinate vertex deduplication cache.
/// @param verts Packed three-vertex position array with nine finite floats.
/// @param normal Authored facet normal used only to reconcile winding.
/// @return One when the facet is skipped or emitted successfully; zero on vertex or mesh-build
///         failure.
static int stl_emit_triangle(void *mesh,
                             stl_vertex_cache_t *cache,
                             const float *verts,
                             const float *normal) {
    uint32_t i0;
    uint32_t i1;
    uint32_t i2;
    if (!mesh || !verts)
        return 0;
    if (!mesh_positions_form_triangle(verts, verts + 3, verts + 6))
        return 1;
    if (!stl_get_or_add_vertex(mesh, cache, verts[0], verts[1], verts[2], &i0) ||
        !stl_get_or_add_vertex(mesh, cache, verts[3], verts[4], verts[5], &i1) ||
        !stl_get_or_add_vertex(mesh, cache, verts[6], verts[7], verts[8], &i2)) {
        return 0;
    }
    if (((rt_mesh3d *)mesh)->build_failed)
        return 0;
    if (stl_triangle_should_flip(verts, normal))
        rt_mesh3d_add_triangle(mesh, i0, i2, i1);
    else
        rt_mesh3d_add_triangle(mesh, i0, i1, i2);
    return !((rt_mesh3d *)mesh)->build_failed;
}

/// @brief Decode a binary-STL byte buffer into a Mesh3D.
///
/// Layout: 80-byte header + `uint32` triangle count + 50 bytes
/// per triangle (12-byte normal + 3 × 12-byte vertices + 2-byte
/// attribute count). Vertices are not deduplicated — STL emits
/// each face's vertices in full.
/// @param data Complete binary STL byte buffer.
/// @param len Number of readable bytes in @p data.
/// @return New Mesh3D with recalculated normals, or NULL for malformed, empty, or failed input.
MESH3D_UNUSED_PRIVATE static void *stl_load_binary(const uint8_t *data, size_t len) {
    if (len < 84)
        return NULL;
    uint32_t tri_count = stl_read_u32_le(data + 80);
    uint32_t reserve_triangles;
    stl_vertex_cache_t vertex_cache = {0};
    size_t tri_count_size = (size_t)tri_count;
    if (!rt_untrusted_count_ok((int64_t)tri_count, 50u, len - 84u))
        return NULL;
    if (tri_count_size > (SIZE_MAX - 84u) / 50u)
        return NULL;
    size_t expected = 84u + tri_count_size * 50u;
    if (len < expected || tri_count == 0 || tri_count > UINT32_MAX / 3u)
        return NULL;

    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;
    reserve_triangles = stl_initial_reserve_triangles(tri_count);
    if (!mesh3d_reserve_storage(
            (rt_mesh3d *)mesh, reserve_triangles * 3u, reserve_triangles * 3u, "Mesh3D.FromSTL")) {
        mesh_mark_build_failed((rt_mesh3d *)mesh);
        return mesh_return_null_if_build_failed(mesh);
    }
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    (void)stl_vertex_cache_init(&vertex_cache, (size_t)reserve_triangles * 3u);

    for (uint32_t i = 0; i < tri_count; i++) {
        const uint8_t *tri = data + 84 + (size_t)i * 50;
        float normal[3] = {
            stl_read_f32_le(tri + 0),
            stl_read_f32_le(tri + 4),
            stl_read_f32_le(tri + 8),
        };
        float verts[9] = {
            stl_read_f32_le(tri + 12),
            stl_read_f32_le(tri + 16),
            stl_read_f32_le(tri + 20),
            stl_read_f32_le(tri + 24),
            stl_read_f32_le(tri + 28),
            stl_read_f32_le(tri + 32),
            stl_read_f32_le(tri + 36),
            stl_read_f32_le(tri + 40),
            stl_read_f32_le(tri + 44),
        };
        if (!isfinite(verts[0]) || !isfinite(verts[1]) || !isfinite(verts[2]) ||
            !isfinite(verts[3]) || !isfinite(verts[4]) || !isfinite(verts[5]) ||
            !isfinite(verts[6]) || !isfinite(verts[7]) || !isfinite(verts[8])) {
            mesh_mark_build_failed((rt_mesh3d *)mesh);
            break;
        }
        if (!stl_emit_triangle(mesh, &vertex_cache, verts, normal))
            break;
    }

    if (((rt_mesh3d *)mesh)->build_failed) {
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        return mesh_return_null_if_build_failed(mesh);
    }
    if (((rt_mesh3d *)mesh)->index_count == 0) {
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }
    stl_vertex_cache_free(&vertex_cache);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_recalc_normals(mesh);
    return mesh;
}

/// @brief Read @p tri_count triangles from an open binary-STL stream (positioned at the
///   first facet record) into a new Mesh3D with recomputed normals; returns NULL on a
///   zero/overflowing count or read error.
/// @param f Open binary stream positioned at the first 50-byte facet record.
/// @param tri_count Validated number of facet records to consume.
/// @return New nonempty Mesh3D with recalculated normals, or NULL on validation, read, allocation,
///         or build failure.
static void *stl_load_binary_stream(FILE *f, uint32_t tri_count) {
    uint32_t reserve_triangles;
    stl_vertex_cache_t vertex_cache = {0};
    if (!f || tri_count == 0 || tri_count > UINT32_MAX / 3u)
        return NULL;
    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;
    reserve_triangles = stl_initial_reserve_triangles(tri_count);
    if (!mesh3d_reserve_storage(
            (rt_mesh3d *)mesh, reserve_triangles * 3u, reserve_triangles * 3u, "Mesh3D.FromSTL")) {
        mesh_mark_build_failed((rt_mesh3d *)mesh);
        return mesh_return_null_if_build_failed(mesh);
    }
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    (void)stl_vertex_cache_init(&vertex_cache, (size_t)reserve_triangles * 3u);

    uint8_t tri[50];
    for (uint32_t i = 0; i < tri_count; i++) {
        if (fread(tri, 1, sizeof(tri), f) != sizeof(tri)) {
            mesh_mark_build_failed((rt_mesh3d *)mesh);
            break;
        }
        float normal[3] = {
            stl_read_f32_le(tri + 0),
            stl_read_f32_le(tri + 4),
            stl_read_f32_le(tri + 8),
        };
        float verts[9] = {
            stl_read_f32_le(tri + 12),
            stl_read_f32_le(tri + 16),
            stl_read_f32_le(tri + 20),
            stl_read_f32_le(tri + 24),
            stl_read_f32_le(tri + 28),
            stl_read_f32_le(tri + 32),
            stl_read_f32_le(tri + 36),
            stl_read_f32_le(tri + 40),
            stl_read_f32_le(tri + 44),
        };
        if (!isfinite(verts[0]) || !isfinite(verts[1]) || !isfinite(verts[2]) ||
            !isfinite(verts[3]) || !isfinite(verts[4]) || !isfinite(verts[5]) ||
            !isfinite(verts[6]) || !isfinite(verts[7]) || !isfinite(verts[8])) {
            mesh_mark_build_failed((rt_mesh3d *)mesh);
            break;
        }
        if (!stl_emit_triangle(mesh, &vertex_cache, verts, normal))
            break;
    }

    if (((rt_mesh3d *)mesh)->build_failed) {
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        return mesh_return_null_if_build_failed(mesh);
    }
    if (((rt_mesh3d *)mesh)->index_count == 0) {
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }
    stl_vertex_cache_free(&vertex_cache);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_recalc_normals(mesh);
    return mesh;
}

/// @brief Parse a double from an ASCII STL stream, advancing the cursor past it.
/// @param pp Address of the bounded text cursor, advanced on success.
/// @param end One-past-end pointer for the current line span.
/// @param out Receives the finite parsed value.
/// @return One on success, or zero for exhausted or malformed input.
static int stl_parse_double(const char **pp, const char *end, double *out) {
    const char *parse_end = NULL;
    while (*pp < end && (**pp == ' ' || **pp == '\t'))
        (*pp)++;
    if (*pp >= end)
        return 0;
    if (!mesh_parse_ascii_double_span(*pp, end, &parse_end, out))
        return 0;
    *pp = parse_end;
    return 1;
}

/// @brief Advance past spaces and tabs, returning the first non-blank pointer (≤ `end`).
/// @param p Current cursor within a line span.
/// @param end One-past-end bound.
/// @return First non-horizontal-whitespace position, not beyond @p end.
static const char *stl_skip_horizontal_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/// @brief Trim trailing spaces, tabs, LF, and CR from a line span, returning the new end pointer.
/// @param start First byte of the line span.
/// @param end Original one-past-end pointer.
/// @return One-past-end pointer after removing trailing whitespace.
static const char *stl_trim_line_end(const char *start, const char *end) {
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    return end;
}

/// @brief True if the `[start,end)` span equals NUL-terminated `text` exactly.
/// @param start First byte of the bounded line span.
/// @param end One-past-end pointer.
/// @param text NUL-terminated comparison string.
/// @return Nonzero for an exact length and byte match; otherwise zero.
static int stl_line_equals(const char *start, const char *end, const char *text) {
    size_t len = strlen(text);
    return (size_t)(end - start) == len && strncmp(start, text, len) == 0;
}

/// @brief True if the span begins with `keyword` followed by end-of-span or whitespace.
/// @details The trailing-boundary check stops "facetx" from matching keyword "facet".
/// @param start First byte of the bounded line span.
/// @param end One-past-end pointer.
/// @param keyword NUL-terminated keyword to match.
/// @return Nonzero when the keyword occupies the first complete token; otherwise zero.
static int stl_line_starts_keyword(const char *start, const char *end, const char *keyword) {
    size_t len = strlen(keyword);
    if ((size_t)(end - start) < len || strncmp(start, keyword, len) != 0)
        return 0;
    return (size_t)(end - start) == len || start[len] == ' ' || start[len] == '\t';
}

/// @brief Parse an ASCII-STL `facet normal nx ny nz` line into `normal[3]`.
/// @param start First nonblank byte of the bounded line.
/// @param end Trimmed one-past-end pointer.
/// @param normal Three-float destination.
/// @return 1 on a well-formed line with three finite, float-range components and
///         no trailing junk; 0 otherwise.
static int stl_parse_facet_normal_line(const char *start, const char *end, float *normal) {
    double nx;
    double ny;
    double nz;
    const char *p = start;
    if ((size_t)(end - p) < 6u || strncmp(p, "facet", 5) != 0 || (p[5] != ' ' && p[5] != '\t'))
        return 0;
    p = stl_skip_horizontal_ws(p + 5, end);
    if ((size_t)(end - p) < 7u || strncmp(p, "normal", 6) != 0 || (p[6] != ' ' && p[6] != '\t'))
        return 0;
    p = p + 6;
    if (!stl_parse_double(&p, end, &nx) || !stl_parse_double(&p, end, &ny) ||
        !stl_parse_double(&p, end, &nz) || !mesh_value_fits_float(nx) ||
        !mesh_value_fits_float(ny) || !mesh_value_fits_float(nz))
        return 0;
    p = stl_skip_horizontal_ws(p, end);
    if (p != end)
        return 0;
    normal[0] = (float)nx;
    normal[1] = (float)ny;
    normal[2] = (float)nz;
    return 1;
}

/// @brief Parse an ASCII-STL `vertex x y z` line into `out_xyz[3]`.
/// @param start First nonblank byte of the bounded line.
/// @param end Trimmed one-past-end pointer.
/// @param out_xyz Three-float destination.
/// @return 1 on a well-formed line with three finite, float-range components and
///         no trailing junk; 0 otherwise.
static int stl_parse_vertex_line(const char *start, const char *end, float *out_xyz) {
    double x;
    double y;
    double z;
    const char *p = start;
    if ((size_t)(end - p) < 7u || strncmp(p, "vertex", 6) != 0 || (p[6] != ' ' && p[6] != '\t'))
        return 0;
    p = p + 6;
    if (!stl_parse_double(&p, end, &x) || !stl_parse_double(&p, end, &y) ||
        !stl_parse_double(&p, end, &z) || !mesh_value_fits_float(x) || !mesh_value_fits_float(y) ||
        !mesh_value_fits_float(z))
        return 0;
    p = stl_skip_horizontal_ws(p, end);
    if (p != end)
        return 0;
    out_xyz[0] = (float)x;
    out_xyz[1] = (float)y;
    out_xyz[2] = (float)z;
    return 1;
}

/// @brief Decode an ASCII-STL byte buffer into a Mesh3D.
///
/// Walks the text, recognising `facet normal …` blocks each
/// containing three `vertex …` lines. Same no-deduplication
/// rule as the binary loader.
/// @param data Complete ASCII STL byte buffer.
/// @param len Number of readable bytes in @p data.
/// @return New nonempty Mesh3D with recalculated normals, or NULL for malformed or failed input.
MESH3D_UNUSED_PRIVATE static void *stl_load_ascii(const uint8_t *data, size_t len) {
    enum {
        STL_ASCII_OUTSIDE_FACET = 0,
        STL_ASCII_EXPECT_OUTER_LOOP = 1,
        STL_ASCII_IN_LOOP = 2,
        STL_ASCII_EXPECT_ENDFACET = 3
    };

    void *mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;

    char *text = (char *)malloc(len + 1u);
    if (!text) {
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }
    memcpy(text, data, len);
    text[len] = '\0';

    const char *p = text;
    const char *end = text + len;
    float verts[9]; // 3 vertices × 3 components
    int vert_idx = 0;
    float normal[3] = {0.0f, 0.0f, 0.0f};
    int state = STL_ASCII_OUTSIDE_FACET;
    int parse_failed = 0;
    int emitted_triangles = 0;
    stl_vertex_cache_t vertex_cache = {0};
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    (void)stl_vertex_cache_init(&vertex_cache, 1024u);

    while (p < end) {
        const char *line = p;
        const char *line_end;
        while (p < end && *p != '\n')
            p++;
        line_end = stl_trim_line_end(line, p);
        line = stl_skip_horizontal_ws(line, line_end);
        if (p < end)
            p++;
        if (line == line_end)
            continue;

        switch (state) {
            case STL_ASCII_OUTSIDE_FACET:
                if (stl_line_starts_keyword(line, line_end, "solid") ||
                    stl_line_starts_keyword(line, line_end, "endsolid"))
                    continue;
                if (!stl_parse_facet_normal_line(line, line_end, normal)) {
                    parse_failed = 1;
                } else {
                    vert_idx = 0;
                    state = STL_ASCII_EXPECT_OUTER_LOOP;
                }
                break;
            case STL_ASCII_EXPECT_OUTER_LOOP:
                if (stl_line_equals(line, line_end, "outer loop")) {
                    state = STL_ASCII_IN_LOOP;
                } else {
                    parse_failed = 1;
                }
                break;
            case STL_ASCII_IN_LOOP:
                if (stl_line_starts_keyword(line, line_end, "vertex")) {
                    if (vert_idx >= 3 ||
                        !stl_parse_vertex_line(line, line_end, &verts[vert_idx * 3])) {
                        parse_failed = 1;
                    } else {
                        vert_idx++;
                    }
                } else if (stl_line_equals(line, line_end, "endloop")) {
                    if (vert_idx != 3)
                        parse_failed = 1;
                    else
                        state = STL_ASCII_EXPECT_ENDFACET;
                } else {
                    parse_failed = 1;
                }
                break;
            case STL_ASCII_EXPECT_ENDFACET:
                if (!stl_line_equals(line, line_end, "endfacet")) {
                    parse_failed = 1;
                } else {
                    uint32_t before = ((rt_mesh3d *)mesh)->index_count;
                    if (!stl_emit_triangle(mesh, &vertex_cache, verts, normal) ||
                        ((rt_mesh3d *)mesh)->build_failed)
                        parse_failed = 1;
                    if (((rt_mesh3d *)mesh)->index_count > before)
                        emitted_triangles++;
                    vert_idx = 0;
                    state = STL_ASCII_OUTSIDE_FACET;
                }
                break;
        }
        if (parse_failed)
            break;
    }

    if (parse_failed || state != STL_ASCII_OUTSIDE_FACET || emitted_triangles == 0 ||
        ((rt_mesh3d *)mesh)->build_failed) {
        free(text);
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        return NULL;
    }

    free(text);
    stl_vertex_cache_free(&vertex_cache);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_recalc_normals(mesh);
    return mesh;
}

/// @brief Parse an ASCII STL stream into a new Mesh3D.
/// @details Drives a small state machine over `facet`/`outer loop`/`vertex`/`endfacet` keywords,
///          accumulating each facet's three vertices (with the stored normal) into the mesh.
/// @param f Open stream positioned at the beginning of ASCII STL text.
/// @return New Mesh3D handle, or NULL on a malformed stream or allocation failure.
static void *stl_load_ascii_stream(FILE *f) {
    enum {
        STL_ASCII_OUTSIDE_FACET = 0,
        STL_ASCII_EXPECT_OUTER_LOOP = 1,
        STL_ASCII_IN_LOOP = 2,
        STL_ASCII_EXPECT_ENDFACET = 3
    };

    void *mesh;
    char *line_buf = NULL;
    size_t line_cap = 0;
    int line_status;
    float verts[9];
    int vert_idx = 0;
    float normal[3] = {0.0f, 0.0f, 0.0f};
    int state = STL_ASCII_OUTSIDE_FACET;
    int parse_failed = 0;
    int emitted_triangles = 0;
    stl_vertex_cache_t vertex_cache = {0};
    if (!f)
        return NULL;
    mesh = rt_mesh3d_new();
    if (!mesh)
        return NULL;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    (void)stl_vertex_cache_init(&vertex_cache, 1024u);
    while ((line_status = mesh3d_read_text_line(
                f, &line_buf, &line_cap, MESH3D_STL_ASCII_MAX_LINE_BYTES)) > 0) {
        const char *line = line_buf;
        const char *line_end = line_buf + strlen(line_buf);
        line_end = stl_trim_line_end(line, line_end);
        line = stl_skip_horizontal_ws(line, line_end);
        if (line == line_end)
            continue;
        switch (state) {
            case STL_ASCII_OUTSIDE_FACET:
                if (stl_line_starts_keyword(line, line_end, "solid") ||
                    stl_line_starts_keyword(line, line_end, "endsolid"))
                    continue;
                if (!stl_parse_facet_normal_line(line, line_end, normal)) {
                    parse_failed = 1;
                } else {
                    vert_idx = 0;
                    state = STL_ASCII_EXPECT_OUTER_LOOP;
                }
                break;
            case STL_ASCII_EXPECT_OUTER_LOOP:
                if (stl_line_equals(line, line_end, "outer loop"))
                    state = STL_ASCII_IN_LOOP;
                else
                    parse_failed = 1;
                break;
            case STL_ASCII_IN_LOOP:
                if (stl_line_starts_keyword(line, line_end, "vertex")) {
                    if (vert_idx >= 3 ||
                        !stl_parse_vertex_line(line, line_end, &verts[vert_idx * 3]))
                        parse_failed = 1;
                    else
                        vert_idx++;
                } else if (stl_line_equals(line, line_end, "endloop")) {
                    if (vert_idx != 3)
                        parse_failed = 1;
                    else
                        state = STL_ASCII_EXPECT_ENDFACET;
                } else {
                    parse_failed = 1;
                }
                break;
            case STL_ASCII_EXPECT_ENDFACET:
                if (!stl_line_equals(line, line_end, "endfacet")) {
                    parse_failed = 1;
                } else {
                    uint32_t before = ((rt_mesh3d *)mesh)->index_count;
                    if (!stl_emit_triangle(mesh, &vertex_cache, verts, normal) ||
                        ((rt_mesh3d *)mesh)->build_failed)
                        parse_failed = 1;
                    if (((rt_mesh3d *)mesh)->index_count > before)
                        emitted_triangles++;
                    vert_idx = 0;
                    state = STL_ASCII_OUTSIDE_FACET;
                }
                break;
        }
        if (parse_failed)
            break;
    }
    if (line_status == -2) {
        parse_failed = 1;
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE, "Mesh3D.FromSTL: line exceeds 1 MiB limit");
    } else if (line_status < 0) {
        parse_failed = 1;
    }
    free(line_buf);
    if (parse_failed || state != STL_ASCII_OUTSIDE_FACET || emitted_triangles == 0 ||
        ((rt_mesh3d *)mesh)->build_failed) {
        stl_vertex_cache_free(&vertex_cache);
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (rt_obj_release_check0(mesh))
            rt_obj_free(mesh);
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "Mesh3D.FromSTL: invalid ASCII STL geometry");
        return NULL;
    }
    stl_vertex_cache_free(&vertex_cache);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_recalc_normals(mesh);
    return mesh;
}

/// @brief Load a mesh from a binary or ASCII STL file.
/// @details STL files contain raw triangle soup (no shared vertices or UVs).
///          Each triangle has its own face normal. ASCII and binary formats are
///          auto-detected. UV coordinates are set to (0,0) for all vertices.
/// @param path File path to the .stl file (runtime string).
/// @return Mesh handle, or NULL on load failure.
void *rt_mesh3d_from_stl(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Mesh3D.FromSTL: path must not be null");
        return NULL;
    }
    const char *filepath = rt_string_cstr(path);
    if (!filepath) {
        rt_asset_error_end_load_failure();
        rt_trap("Mesh3D.FromSTL: invalid path");
        return NULL;
    }

    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Mesh3D.FromSTL: '%s' not found", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (rt_file_stdio_seek64(f, 0, SEEK_END) != 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Mesh3D.FromSTL: failed to seek '%s'", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    int64_t file_len = rt_file_stdio_tell64(f);
    if (file_len < 0 || file_len > STL_MAX_FILE_BYTES ||
        rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        rt_asset_error_setf(file_len > STL_MAX_FILE_BYTES ? RT_ASSET_ERROR_TOO_LARGE
                                                          : RT_ASSET_ERROR_UNREADABLE,
                            "Mesh3D.FromSTL: failed to read '%s'",
                            filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (file_len <= 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_BAD_MAGIC, "Mesh3D.FromSTL: '%s' is not a supported STL", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    void *mesh = NULL;
    if (file_len >= 84) {
        uint8_t header[84];
        if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
            fclose(f);
            rt_asset_error_setf(
                RT_ASSET_ERROR_UNREADABLE, "Mesh3D.FromSTL: failed to read '%s'", filepath);
            rt_asset_error_end_load_failure();
            return NULL;
        }
        uint32_t tri_count = stl_read_u32_le(header + 80);
        size_t expected_binary = 0;
        if (rt_untrusted_count_ok((int64_t)tri_count, 50u, (size_t)file_len - 84u) &&
            rt_alloc_count_ok_with_extra(tri_count, 50u, 84u))
            expected_binary = 84u + (size_t)tri_count * 50u;
        if (expected_binary != 0 && (size_t)file_len == expected_binary && tri_count > 0) {
            mesh = stl_load_binary_stream(f, tri_count);
            fclose(f);
            if (mesh)
                rt_asset_error_end_load_success();
            else {
                rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                    "Mesh3D.FromSTL: '%s' is not a supported binary STL",
                                    filepath);
                rt_asset_error_end_load_failure();
            }
            return mesh;
        }
        if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
            fclose(f);
            rt_asset_error_setf(
                RT_ASSET_ERROR_UNREADABLE, "Mesh3D.FromSTL: failed to seek '%s'", filepath);
            rt_asset_error_end_load_failure();
            return NULL;
        }
    }

    {
        char magic[5];
        if (fread(magic, 1, sizeof(magic), f) == sizeof(magic) && memcmp(magic, "solid", 5) == 0) {
            if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
                fclose(f);
                rt_asset_error_setf(
                    RT_ASSET_ERROR_UNREADABLE, "Mesh3D.FromSTL: failed to seek '%s'", filepath);
                rt_asset_error_end_load_failure();
                return NULL;
            }
            mesh = stl_load_ascii_stream(f);
            if (mesh) {
                fclose(f);
                rt_asset_error_end_load_success();
                return mesh;
            }
        }
        if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
            fclose(f);
            rt_asset_error_setf(
                RT_ASSET_ERROR_UNREADABLE, "Mesh3D.FromSTL: failed to seek '%s'", filepath);
            rt_asset_error_end_load_failure();
            return NULL;
        }
    }

    if (file_len >= 84 && ((file_len - 84) % 50) == 0) {
        uint64_t tri_count64 = (uint64_t)(file_len - 84) / 50u;
        if (tri_count64 > 0 && tri_count64 <= UINT32_MAX / 3u &&
            rt_file_stdio_seek64(f, 84, SEEK_SET) == 0) {
            mesh = stl_load_binary_stream(f, (uint32_t)tri_count64);
            fclose(f);
            if (mesh)
                rt_asset_error_end_load_success();
            else {
                rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                    "Mesh3D.FromSTL: '%s' is not a supported binary STL",
                                    filepath);
                rt_asset_error_end_load_failure();
            }
            return mesh;
        }
    }
    fclose(f);
    rt_asset_error_setf(
        RT_ASSET_ERROR_BAD_MAGIC, "Mesh3D.FromSTL: '%s' is not a supported STL", filepath);
    rt_asset_error_end_load_failure();
    return NULL;
}
