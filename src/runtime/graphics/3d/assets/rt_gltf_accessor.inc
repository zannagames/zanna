//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_accessor.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF accessor binding, sparse handling, and component
// decode/read
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_accessor.inc
 * @brief Binds glTF accessors to validated byte views, applies sparse overrides,
 *        decodes component types, and validates primitive attribute/topology data.
 *
 * Accessor views borrow dense and sparse bytes from the load-local buffer tables.
 * All declared offsets, strides, counts, component layouts, and sparse indices are
 * range checked before readers may access them. This fragment also centralizes skin
 * influence truncation diagnostics and recoverable triangle emission.
 */

/// @brief Return the byte size of one glTF accessor component by its GL-enum component type.
/// @details glTF uses the original GL integer constants for component types:
///   5120 = BYTE (1), 5121 = UNSIGNED_BYTE (1), 5122 = SHORT (2), 5123 = UNSIGNED_SHORT (2),
///   5125 = UNSIGNED_INT (4), 5126 = FLOAT (4). Returns 0 for unrecognised values so callers
///   can treat 0 as an error sentinel.
/// @param comp_type glTF componentType GL enum.
/// @return Component width in bytes, or zero when unsupported.
static int gltf_component_size(int comp_type) {
    switch (comp_type) {
        case 5120:
        case 5121:
            return 1;
        case 5122:
        case 5123:
            return 2;
        case 5125:
        case 5126:
            return 4;
        default:
            return 0;
    }
}

/// @brief Return the number of components for a glTF accessor `"type"` string.
/// @details Maps the glTF type name to its component count: "SCALAR"=1, "VEC2"=2, "VEC3"=3,
///   "VEC4"=4, "MAT2"=4, "MAT3"=9, "MAT4"=16.
/// @param acc_type Accessor type string.
/// @return Component count, or zero for NULL/unrecognized input.
static int gltf_component_count(const char *acc_type) {
    if (!acc_type)
        return 0;
    if (strcmp(acc_type, "SCALAR") == 0)
        return 1;
    if (strcmp(acc_type, "VEC2") == 0)
        return 2;
    if (strcmp(acc_type, "VEC3") == 0)
        return 3;
    if (strcmp(acc_type, "VEC4") == 0)
        return 4;
    if (strcmp(acc_type, "MAT2") == 0)
        return 4;
    if (strcmp(acc_type, "MAT3") == 0)
        return 9;
    if (strcmp(acc_type, "MAT4") == 0)
        return 16;
    return 0;
}

static int gltf_validate_sparse_indices(const gltf_accessor_view_t *view);

/// @brief Initialize accessor scalar layout and extract its buffer-view binding metadata.
/// @param acc Parsed accessor JSON object.
/// @param[out] out View initialized with count, packed stride, component type/count,
///                 and normalized flag.
/// @param[out] out_bv_idx Receives the bufferView index, or -1 when absent.
/// @param[out] out_byte_offset_acc Receives the non-negative accessor byteOffset.
/// @param[out] out_comp_size Receives the component width in bytes.
/// @return Non-zero for a valid positive count and recognized layout; otherwise zero.
static int gltf_accessor_init_view(void *acc,
                                   gltf_accessor_view_t *out,
                                   int64_t *out_bv_idx,
                                   size_t *out_byte_offset_acc,
                                   int *out_comp_size) {
    int comp_type;
    int comp_size;
    int comp_count;
    const char *acc_type;
    int64_t count_raw;
    memset(out, 0, sizeof(*out));
    count_raw = jint(acc, "count", 0);
    if (count_raw <= 0 || count_raw > INT32_MAX)
        return 0;
    if (!gltf_nonnegative_size(acc, "byteOffset", 0, out_byte_offset_acc))
        return 0;
    comp_type = (int)jint(acc, "componentType", 5126);
    comp_size = gltf_component_size(comp_type);
    acc_type = jstr(acc, "type");
    comp_count = gltf_component_count(acc_type);
    if (comp_size <= 0 || comp_count <= 0)
        return 0;
    out->count = (int32_t)count_raw;
    out->stride = comp_size * comp_count;
    out->comp_type = comp_type;
    out->comp_count = comp_count;
    out->normalized = jbool(acc, "normalized", 0) ? 1 : 0;
    *out_bv_idx = jint(acc, "bufferView", -1);
    *out_comp_size = comp_size;
    return 1;
}

/// @brief Bind an accessor view to validated dense bufferView bytes.
/// @details A negative @p bv_idx represents a sparse-only accessor and succeeds without
///          dense data. Meshopt-decoded shadow views take precedence over fallback buffers.
/// @param root Parsed glTF root JSON object.
/// @param bv_idx BufferView index, or a negative value for no dense payload.
/// @param byte_offset_acc Accessor-local byte offset.
/// @param buffers Loaded raw buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param comp_size Component width in bytes.
/// @param comp_count Components per accessor element.
/// @param[in,out] out Initialized accessor view receiving stride and borrowed data.
/// @return Non-zero on success, or zero for invalid indices, bounds, alignment,
///         stride, count, or unavailable bytes.
static int gltf_accessor_bind_buffer_view(void *root,
                                          int64_t bv_idx,
                                          size_t byte_offset_acc,
                                          gltf_buffer_t *buffers,
                                          int buf_count,
                                          int comp_size,
                                          int comp_count,
                                          gltf_accessor_view_t *out) {
    void *views = jarr(root, "bufferViews");
    void *bv;
    int buf_idx;
    size_t byte_offset_bv;
    size_t byte_length_bv;
    size_t byte_stride;
    size_t elem_size;
    size_t view_end;
    size_t last_offset;
    size_t last_span;
    size_t required_view_len;
    const uint8_t *view_base;
    if (bv_idx < 0)
        return 1;
    if (!views || bv_idx >= jarr_len(views))
        return 0;
    bv = rt_seq_get(views, (int64_t)bv_idx);
    if (!bv)
        return 0;
    buf_idx = (int)jint(bv, "buffer", 0);
    if (!gltf_nonnegative_size(bv, "byteOffset", 0, &byte_offset_bv) ||
        !gltf_nonnegative_size(bv, "byteLength", 0, &byte_length_bv) ||
        !gltf_nonnegative_size(bv, "byteStride", 0, &byte_stride))
        return 0;
    /* EXT_meshopt_compression: compressed views bind against the decoded shadow
     * buffer; the parent (buffer, byteOffset) range may reference a data-less
     * placeholder fallback, so the raw-buffer bounds checks below don't apply. */
    if (g_gltf_meshopt_views && bv_idx < g_gltf_meshopt_views->view_count &&
        g_gltf_meshopt_views->view_data[bv_idx]) {
        view_base = g_gltf_meshopt_views->view_data[bv_idx];
        if (byte_length_bv != g_gltf_meshopt_views->view_len[bv_idx])
            return 0;
    } else {
        if (!buffers || buf_count <= 0 || buf_idx < 0 || buf_idx >= buf_count)
            return 0;
        if (!gltf_checked_add_size(byte_offset_bv, byte_length_bv, &view_end) ||
            view_end > buffers[buf_idx].len)
            return 0;
        view_base = buffers[buf_idx].data ? buffers[buf_idx].data + byte_offset_bv : NULL;
    }

    elem_size = (size_t)comp_size * (size_t)comp_count;
    if (byte_stride > 0) {
        if (byte_stride < elem_size || byte_stride > 252u || byte_stride % (size_t)comp_size != 0)
            return 0;
        out->stride = (int32_t)byte_stride;
    } else {
        if (elem_size > INT32_MAX)
            return 0;
        out->stride = (int32_t)elem_size;
    }
    if (byte_offset_acc > byte_length_bv)
        return 0;
    if (!rt_untrusted_count_ok((int64_t)out->count, elem_size, byte_length_bv - byte_offset_acc))
        return 0;
    if (out->count <= 0) /* re-assert locally: count-1 below underflows size_t when count==0 */
        return 0;
    if (!gltf_checked_mul_size((size_t)(out->count - 1), (size_t)out->stride, &last_offset) ||
        !gltf_checked_add_size(byte_offset_acc, last_offset, &last_span) ||
        !gltf_checked_add_size(last_span, elem_size, &required_view_len))
        return 0;
    if (required_view_len > byte_length_bv)
        return 0;
    out->data = view_base ? view_base + byte_offset_acc : NULL;
    return 1;
}

/// @brief Bind and validate an accessor's optional sparse index/value payloads.
/// @param root Parsed glTF root JSON object.
/// @param acc Parsed accessor JSON object.
/// @param buffers Loaded raw buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param comp_size Dense/sparse component width in bytes.
/// @param comp_count Components per accessor element.
/// @param[in,out] out Initialized view receiving borrowed sparse ranges.
/// @return Non-zero when sparse metadata is absent or valid; otherwise zero.
static int gltf_accessor_bind_sparse(void *root,
                                     void *acc,
                                     gltf_buffer_t *buffers,
                                     int buf_count,
                                     int comp_size,
                                     int comp_count,
                                     gltf_accessor_view_t *out) {
    void *sparse = jget(acc, "sparse");
    int64_t sparse_count_raw = sparse ? jint(sparse, "count", 0) : 0;
    int32_t sparse_count =
        (sparse_count_raw > 0 && sparse_count_raw <= INT32_MAX) ? (int32_t)sparse_count_raw : 0;
    if (!sparse)
        return 1;
    if (sparse_count_raw <= 0 || sparse_count_raw > INT32_MAX || sparse_count_raw > out->count)
        return 0;
    if (sparse_count <= 0)
        return 1;
    {
        void *indices = jget(sparse, "indices");
        void *values = jget(sparse, "values");
        int64_t indices_view = jint(indices, "bufferView", -1);
        int64_t values_view = jint(values, "bufferView", -1);
        size_t indices_offset;
        size_t values_offset;
        int index_comp_type = (int)jint(indices, "componentType", 0);
        int index_comp_size = gltf_component_size(index_comp_type);
        size_t index_len = 0;
        size_t value_len = 0;
        size_t index_bytes = 0;
        size_t sparse_elem_size = 0;
        size_t value_bytes = 0;
        size_t index_end = 0;
        size_t value_end = 0;
        const uint8_t *index_data =
            gltf_get_buffer_view_data(root, indices_view, buffers, buf_count, &index_len);
        const uint8_t *value_data =
            gltf_get_buffer_view_data(root, values_view, buffers, buf_count, &value_len);
        if (!index_data || !value_data || index_comp_size <= 0)
            return 0;
        if (index_comp_type != 5121 && index_comp_type != 5123 && index_comp_type != 5125)
            return 0;
        if (!gltf_nonnegative_size(indices, "byteOffset", 0, &indices_offset) ||
            !gltf_nonnegative_size(values, "byteOffset", 0, &values_offset))
            return 0;
        if (indices_offset > index_len || values_offset > value_len)
            return 0;
        if (!rt_untrusted_count_ok(
                (int64_t)sparse_count, (size_t)index_comp_size, index_len - indices_offset))
            return 0;
        if (!gltf_checked_mul_size((size_t)sparse_count, (size_t)index_comp_size, &index_bytes) ||
            !gltf_checked_add_size(indices_offset, index_bytes, &index_end) ||
            index_end > index_len)
            return 0;
        if (!gltf_checked_mul_size((size_t)comp_size, (size_t)comp_count, &sparse_elem_size) ||
            !rt_untrusted_count_ok(
                (int64_t)sparse_count, sparse_elem_size, value_len - values_offset) ||
            !gltf_checked_mul_size((size_t)sparse_count, sparse_elem_size, &value_bytes) ||
            !gltf_checked_add_size(values_offset, value_bytes, &value_end) || value_end > value_len)
            return 0;
        if (sparse_elem_size > INT32_MAX)
            return 0;
        out->sparse_indices = index_data + indices_offset;
        out->sparse_values = value_data + values_offset;
        out->sparse_count = sparse_count;
        out->sparse_index_comp_type = index_comp_type;
        out->sparse_index_stride = index_comp_size;
        out->sparse_value_stride = (int32_t)sparse_elem_size;
        return gltf_validate_sparse_indices(out);
    }
}

/// @brief Resolve a glTF accessor into a fully validated typed byte view.
/// @param root Parsed glTF root JSON object.
/// @param accessor_idx Zero-based accessor index.
/// @param buffers Loaded raw buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param[out] out Receives borrowed dense/sparse byte ranges and scalar layout.
/// @return Non-zero on success, or zero for invalid JSON, bounds, component layout,
///         or sparse metadata.
static int gltf_get_accessor_view(void *root,
                                  int64_t accessor_idx,
                                  gltf_buffer_t *buffers,
                                  int buf_count,
                                  gltf_accessor_view_t *out) {
    void *accessors = jarr(root, "accessors");
    void *acc;
    int64_t bv_idx;
    size_t byte_offset_acc;
    int comp_size;
    if (!out || !accessors || accessor_idx < 0 || accessor_idx >= jarr_len(accessors))
        return 0;
    acc = rt_seq_get(accessors, accessor_idx);
    if (!acc)
        return 0;
    if (!gltf_accessor_init_view(acc, out, &bv_idx, &byte_offset_acc, &comp_size))
        return 0;
    if (bv_idx < 0 && !jget(acc, "sparse"))
        return 0;
    if (!gltf_accessor_bind_buffer_view(
            root, bv_idx, byte_offset_acc, buffers, buf_count, comp_size, out->comp_count, out))
        return 0;
    return gltf_accessor_bind_sparse(
        root, acc, buffers, buf_count, comp_size, out->comp_count, out);
}

/// @brief Validate sparse accessor metadata globally before importers may skip primitives.
/// @param root Parsed glTF root JSON object.
/// @param buffers Loaded raw buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @return Non-zero when every non-empty sparse accessor resolves validly; otherwise zero.
static int gltf_validate_sparse_accessors(void *root, gltf_buffer_t *buffers, int buf_count) {
    void *accessors = jarr(root, "accessors");
    int64_t accessor_count = jarr_len(accessors);
    for (int64_t i = 0; i < accessor_count; i++) {
        void *acc = rt_seq_get(accessors, i);
        void *sparse = jget(acc, "sparse");
        if (sparse && jint(sparse, "count", 0) > 0) {
            gltf_accessor_view_t view;
            if (!gltf_get_accessor_view(root, i, buffers, buf_count, &view))
                return 0;
        }
    }
    return 1;
}

/// @brief Read a little-endian unsigned 16-bit integer.
/// @param src Pointer to at least two readable bytes.
/// @return Decoded value.
static uint16_t gltf_read_le_u16(const uint8_t *src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

/// @brief Read a little-endian signed 16-bit integer.
/// @param src Pointer to at least two readable bytes.
/// @return Decoded value.
static int16_t gltf_read_le_i16(const uint8_t *src) {
    return (int16_t)gltf_read_le_u16(src);
}

/// @brief Read a little-endian unsigned 32-bit integer.
/// @param src Pointer to at least four readable bytes.
/// @return Decoded value.
static uint32_t gltf_read_le_u32(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

/// @brief Read a little-endian IEEE-754 float through an alias-safe bit copy.
/// @param src Pointer to at least four readable bytes.
/// @return Decoded float bit pattern.
static float gltf_read_le_f32(const uint8_t *src) {
    uint32_t bits = gltf_read_le_u32(src);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/// @brief Decode one glTF accessor component from raw bytes to a float.
/// @details Handles all six glTF component types. For integer types, when @p normalized is
///   non-zero, the value is linearly mapped to [−1, 1] (signed) or [0, 1] (unsigned) per the
///   glTF spec §3.6.2.4:
///     - INT8:  max(value/127, −1) to preserve the symmetric range at the cost of −128 → −1.
///     - UINT8:  value/255.
///     - INT16:  max(value/32767, −1).
///     - UINT16: value/65535.
///     - UINT32: value/4294967295 (double precision to avoid float precision loss).
///     - FLOAT:  returned as-is regardless of the normalized flag.
///   Used when reading attribute accessors (positions, normals, UVs, joint weights, etc.).
/// @param src        Pointer to the first byte of the component in the buffer.
/// @param comp_type  glTF component type integer (5120–5126).
/// @param normalized Non-zero to apply normalization; 0 to return the raw numeric value.
/// @return The decoded float value, or 0.0f for unknown component types.
static float gltf_decode_component_f32(const uint8_t *src, int comp_type, int normalized) {
    if (!src)
        return 0.0f;
    switch (comp_type) {
        case 5120: {
            int8_t value = (int8_t)src[0];
            if (!normalized)
                return (float)value;
            if (value == INT8_MIN)
                return -1.0f;
            return (float)value / 127.0f;
        }
        case 5121: {
            uint8_t value = src[0];
            return normalized ? (float)value / 255.0f : (float)value;
        }
        case 5122: {
            int16_t value = gltf_read_le_i16(src);
            if (!normalized)
                return (float)value;
            if (value == INT16_MIN)
                return -1.0f;
            return (float)value / 32767.0f;
        }
        case 5123: {
            uint16_t value = gltf_read_le_u16(src);
            return normalized ? (float)value / 65535.0f : (float)value;
        }
        case 5125: {
            uint32_t value = gltf_read_le_u32(src);
            return normalized ? (float)((double)value / 4294967295.0) : (float)value;
        }
        case 5126: {
            float value = gltf_read_le_f32(src);
            return isfinite(value) ? value : 0.0f;
        }
        default:
            return 0.0f;
    }
}

/// @brief Decode one glTF accessor component from raw bytes to a uint32.
/// @details Used for reading index accessors (SCALAR UNSIGNED_INT/SHORT/BYTE) and joint-index
///   accessors (UNSIGNED_BYTE, UNSIGNED_SHORT). Signed integer types clamp negative values to 0
///   so that malformed data can't produce garbage joint indices or out-of-range triangle indices.
///   FLOAT is intentionally rejected for uint32 reads because glTF indices and joint indices are
///   integer component streams. Normalization is never applied for uint32 reads.
/// @param src        Pointer to the first byte of the component.
/// @param comp_type  glTF component type integer (5120–5126).
/// @return Raw unsigned integer value, or 0u for unknown component types.
static uint32_t gltf_decode_component_u32(const uint8_t *src, int comp_type) {
    if (!src)
        return 0u;
    switch (comp_type) {
        case 5120: {
            int8_t value = (int8_t)src[0];
            return value < 0 ? 0u : (uint32_t)value;
        }
        case 5121: {
            uint8_t value = src[0];
            return (uint32_t)value;
        }
        case 5122: {
            int16_t value = gltf_read_le_i16(src);
            return value < 0 ? 0u : (uint32_t)value;
        }
        case 5123: {
            uint16_t value = gltf_read_le_u16(src);
            return (uint32_t)value;
        }
        case 5125: {
            return gltf_read_le_u32(src);
        }
        default:
            return 0u;
    }
}

/// @brief Binary-search the accessor's sparse-index array for @p element_idx.
/// @details glTF sparse accessors store a sorted list of (index, value) pairs that override
///   specific elements in the base buffer. To resolve element N: first check the base buffer
///   data (if present), then binary-search for N in the sparse indices array; if found, the
///   corresponding sparse value overrides the base. This function performs the binary search
///   and returns the position in the sparse array, not the element value itself.
///   Returns -1 when @p element_idx is not among the sparse overrides (i.e. the base data applies).
/// @param view         Accessor view with a non-NULL sparse_indices pointer.
/// @param element_idx  Zero-based logical element index to look up.
/// @return Sparse array slot (≥0) if found, -1 if not overridden.
static int32_t gltf_accessor_sparse_value_index(const gltf_accessor_view_t *view,
                                                int32_t element_idx) {
    int32_t lo;
    int32_t hi;
    if (!view || !view->sparse_indices || view->sparse_count <= 0)
        return -1;
    lo = 0;
    hi = view->sparse_count - 1;
    while (lo <= hi) {
        int32_t i = lo + (hi - lo) / 2;
        uint32_t sparse_index = gltf_decode_component_u32(
            view->sparse_indices + (size_t)i * (size_t)view->sparse_index_stride,
            view->sparse_index_comp_type);
        if (sparse_index == (uint32_t)element_idx)
            return i;
        if (sparse_index < (uint32_t)element_idx)
            lo = i + 1;
        else
            hi = i - 1;
    }
    return -1;
}

/// @brief Validate sparse accessor indices before sparse reads rely on binary search.
/// @details glTF requires sparse indices to be strictly increasing and in accessor range. Without
///   this guard duplicate or unsorted indices silently produce inconsistent overrides because the
///   runtime lookup performs a binary search over the sparse index table.
/// @param view Accessor view containing optional sparse indices.
/// @return Non-zero when no sparse data exists or all indices are strictly increasing
///         and in range; otherwise zero.
static int gltf_validate_sparse_indices(const gltf_accessor_view_t *view) {
    uint32_t previous = 0u;
    if (!view || !view->sparse_indices || view->sparse_count <= 0)
        return 1;
    for (int32_t i = 0; i < view->sparse_count; i++) {
        uint32_t sparse_index = gltf_decode_component_u32(
            view->sparse_indices + (size_t)i * (size_t)view->sparse_index_stride,
            view->sparse_index_comp_type);
        if (sparse_index >= (uint32_t)view->count)
            return 0;
        if (i > 0 && sparse_index <= previous)
            return 0;
        previous = sparse_index;
    }
    return 1;
}

/// @brief Read one element from an accessor view into a float array, applying sparse overrides.
/// @details First zeros @p out_components output slots, then — if the base buffer is present —
///   reads up to `min(view->comp_count, out_components)` components from the strided base data
///   via `gltf_decode_component_f32`. Finally, if a sparse override exists for @p element_idx
///   (detected by `gltf_accessor_sparse_value_index`), the sparse values are decoded over the
///   same component slots. The two-phase approach correctly handles the case where the base buffer
///   is NULL (accessor with sparse overrides only, no bufferView).
/// @param view            Accessor view to read from; may have a NULL base data pointer.
/// @param element_idx     Zero-based logical element index.
/// @param out             Output array; receives up to @p out_components floats.
/// @param out_components  Number of float slots in @p out; may be less than view->comp_count.
static void gltf_accessor_read_f32(const gltf_accessor_view_t *view,
                                   int32_t element_idx,
                                   float *out,
                                   int32_t out_components) {
    if (!out || out_components <= 0) {
        return;
    }
    for (int32_t i = 0; i < out_components; i++)
        out[i] = 0.0f;
    if (!view || element_idx < 0 || element_idx >= view->count)
        return;
    int comp_size = gltf_component_size(view->comp_type);
    if (comp_size <= 0)
        return;
    int32_t limit = view->comp_count < out_components ? view->comp_count : out_components;
    if (view->data) {
        const uint8_t *base = view->data + (size_t)element_idx * (size_t)view->stride;
        for (int32_t i = 0; i < limit; i++)
            out[i] = gltf_decode_component_f32(
                base + (size_t)i * (size_t)comp_size, view->comp_type, view->normalized);
    }
    {
        int32_t sparse_slot = gltf_accessor_sparse_value_index(view, element_idx);
        if (sparse_slot >= 0) {
            const uint8_t *sparse =
                view->sparse_values + (size_t)sparse_slot * (size_t)view->sparse_value_stride;
            for (int32_t i = 0; i < limit; i++)
                out[i] = gltf_decode_component_f32(
                    sparse + (size_t)i * (size_t)comp_size, view->comp_type, view->normalized);
        }
    }
}

/// @brief Read one element from an accessor view into a uint32 array, applying sparse overrides.
/// @details Same two-phase (base + sparse-override) strategy as `gltf_accessor_read_f32`, but
///   decodes via `gltf_decode_component_u32` instead. Used for index buffers (SCALAR UINT/USHORT/
///   UBYTE accessor type) and joint-index attributes (UNSIGNED_BYTE/UNSIGNED_SHORT VEC4).
/// @param view            Accessor view to read from.
/// @param element_idx     Zero-based logical element index.
/// @param out             Output array; receives up to @p out_components uint32 values.
/// @param out_components  Number of uint32 slots in @p out.
static void gltf_accessor_read_u32(const gltf_accessor_view_t *view,
                                   int32_t element_idx,
                                   uint32_t *out,
                                   int32_t out_components) {
    if (!out || out_components <= 0) {
        return;
    }
    for (int32_t i = 0; i < out_components; i++)
        out[i] = 0u;
    if (!view || element_idx < 0 || element_idx >= view->count)
        return;
    int comp_size = gltf_component_size(view->comp_type);
    if (comp_size <= 0)
        return;
    int32_t limit = view->comp_count < out_components ? view->comp_count : out_components;
    if (view->data) {
        const uint8_t *base = view->data + (size_t)element_idx * (size_t)view->stride;
        for (int32_t i = 0; i < limit; i++)
            out[i] =
                gltf_decode_component_u32(base + (size_t)i * (size_t)comp_size, view->comp_type);
    }
    {
        int32_t sparse_slot = gltf_accessor_sparse_value_index(view, element_idx);
        if (sparse_slot >= 0) {
            const uint8_t *sparse =
                view->sparse_values + (size_t)sparse_slot * (size_t)view->sparse_value_stride;
            for (int32_t i = 0; i < limit; i++)
                out[i] = gltf_decode_component_u32(sparse + (size_t)i * (size_t)comp_size,
                                                   view->comp_type);
        }
    }
}

/// @brief Per-primitive skin-import diagnostics: how many vertices lost source
///        influence data on the way into the fixed 4-slot vertex record.
typedef struct {
    /// Vertices with more than four meaningful influences folded to the strongest four.
    uint32_t truncated_vertices;    /**< vertices with >4 meaningful influences folded to top-4 */
    /// Vertices containing a meaningful joint outside the runtime draw-palette limit.
    uint32_t out_of_range_vertices; /**< vertices whose meaningful joints >= VGFX3D_MAX_BONES */
} gltf_skin_import_stats_t;

/// @brief Emit load warnings for skin data the importer had to drop or truncate.
/// @details Both the synchronous mesh import and the preloaded-POD commit paths funnel
///          their per-primitive stats through here so the two paths produce identical
///          AssetDiagnostics3D warnings. No-op when the stats are all zero.
/// @param mesh_index Zero-based source mesh index used in diagnostics.
/// @param primitive_index Zero-based primitive index within the source mesh.
/// @param stats Accumulated per-primitive skin import statistics.
static void gltf_emit_skin_import_warnings(int mesh_index,
                                           int primitive_index,
                                           const gltf_skin_import_stats_t *stats) {
    if (!stats)
        return;
    if (stats->truncated_vertices > 0u) {
        rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_TRUNCATED_INFLUENCE_VERTICES,
                                       (int64_t)stats->truncated_vertices);
        rt_asset_error_add_warningf(
            "glTF mesh %d primitive %d: %u vertices carried more than 4 bone influences; "
            "weakest influences dropped and weights renormalized",
            mesh_index,
            primitive_index,
            stats->truncated_vertices);
    }
    if (stats->out_of_range_vertices > 0u) {
        rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_OUT_OF_RANGE_JOINT_VERTICES,
                                       (int64_t)stats->out_of_range_vertices);
        rt_asset_error_add_warningf(
            "glTF mesh %d primitive %d: %u vertices referenced joints beyond the %d-bone "
            "limit; those influences were dropped",
            mesh_index,
            primitive_index,
            stats->out_of_range_vertices,
            (int)VGFX3D_MAX_BONES);
    }
}

/// @brief Insert a `(joint, weight)` influence into a fixed 4-slot vertex influence
///        record, keeping the top four weights.
/// @details GPU skinning palettes cap per-vertex influences at 4 slots, but glTF meshes
///          can carry more influences via JOINTS_0 + JOINTS_1 accessor pairs. This
///          helper is called once per (joint, weight) pair from every contributing
///          accessor, folding all of them into the same 4-tuple. Policy:
///            1. Matching joint already in a slot → sum weights (handles duplicate
///               joints reported across JOINTS_0 and JOINTS_1 for the same bone).
///            2. Empty slot available (weight ~= 0) → fill it.
///            3. All slots full and the incoming weight is larger than the smallest
///               slot → evict the weakest. Otherwise the incoming weight is dropped.
///          Inputs whose weight is negligible/non-finite (<= 1e-8) are silently dropped
///          so garbage in the source accessors can't corrupt the influence record.
///          Meaningful influences that are still lost are reported through the flags:
///          a joint index past VGFX3D_MAX_BONES raises @p out_of_range_io, and a fifth
///          meaningful influence hitting a full record raises @p truncated_io (whether
///          the incoming or the weakest resident influence is the one dropped).
/// @param joint Source joint index from a glTF JOINTS_* accessor.
/// @param weight Source weight from the matching WEIGHTS_* accessor.
/// @param out_joints 4-element joint-index array, mutated in place.
/// @param out_weights 4-element weight array, mutated in place.
/// @param truncated_io Optional flag set to 1 when a meaningful influence is folded away.
/// @param out_of_range_io Optional flag set to 1 when a meaningful joint is out of range.
/// @param joint_limit Exclusive accepted joint-index bound.
static void gltf_add_top_joint_influence_limit(uint32_t joint,
                                               float weight,
                                               uint32_t *out_joints,
                                               float *out_weights,
                                               int *truncated_io,
                                               int *out_of_range_io,
                                               uint32_t joint_limit) {
    int replace = -1;
    if (!out_joints || !out_weights || !isfinite(weight) || weight <= 1e-8f)
        return;
    if (joint >= joint_limit) {
        if (out_of_range_io)
            *out_of_range_io = 1;
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (out_weights[i] > 1e-8f && out_joints[i] == joint) {
            out_weights[i] += weight;
            return;
        }
        if (replace < 0 && out_weights[i] <= 1e-8f)
            replace = i;
    }
    if (replace < 0) {
        if (truncated_io)
            *truncated_io = 1;
        replace = 0;
        for (int i = 1; i < 4; i++) {
            if (out_weights[i] < out_weights[replace])
                replace = i;
        }
        if (weight <= out_weights[replace])
            return;
    }
    out_joints[replace] = joint;
    out_weights[replace] = weight;
}

/// @brief Merge one influence using the runtime draw-palette bone limit.
/// @param joint Source joint index.
/// @param weight Source influence weight.
/// @param[in,out] out_joints Four-element strongest-joint array.
/// @param[in,out] out_weights Four-element parallel weight array.
/// @param[in,out] truncated_io Optional flag set when a meaningful influence is dropped.
/// @param[in,out] out_of_range_io Optional flag set for an out-of-range meaningful joint.
static void gltf_add_top_joint_influence(uint32_t joint,
                                         float weight,
                                         uint32_t *out_joints,
                                         float *out_weights,
                                         int *truncated_io,
                                         int *out_of_range_io) {
    gltf_add_top_joint_influence_limit(
        joint, weight, out_joints, out_weights, truncated_io, out_of_range_io, VGFX3D_MAX_BONES);
}

/// @brief Clamp negatives and renormalize a 4-element vertex weight tuple to sum 1.
/// @details glTF assets are *meant* to ship normalized, but some exporters produce
///          weights that sum to slightly less or more than 1 (accumulated rounding
///          error, or after this loader's own top-4 truncation throws out some
///          influence). Per-vertex weight sums that diverge from 1 cause the skinned
///          mesh to bloat or shrink, so we clean up after the accessor walk:
///            1. Any negative weight (exporter bug or bad sparse override) is clamped
///               to 0 and dropped from the sum.
///            2. Non-zero sum → scale to 1.
///            3. Sum below threshold → leave the tuple alone (a zero-weight vertex is
///               a valid "this vertex is unrigged" signal).
/// @param[in,out] weights Four-element influence tuple.
static void gltf_normalize_joint_influences(float *weights) {
    float sum = 0.0f;
    if (!weights)
        return;
    for (int i = 0; i < 4; i++) {
        if (isfinite(weights[i]) && weights[i] > 0.0f)
            sum += weights[i];
        else
            weights[i] = 0.0f;
    }
    if (sum > 1e-8f) {
        for (int i = 0; i < 4; i++)
            weights[i] /= sum;
    }
}

/// @brief Validate and emit one triangle to the mesh if all three indices form an area.
/// @details Duplicate-index, collinear-position, and out-of-range triangles are silently
///   dropped. This is a safety net for glTF files that include degenerate geometry (exported by
///   tools that strip vertices but leave the index buffer unchanged). Returns 1 if the triangle
///   was emitted, 0 if it was rejected.
/// @param[in,out] mesh Runtime Mesh3D receiving a valid triangle.
/// @param vertex_count Number of vertices addressable by the primitive.
/// @param i0 First candidate vertex index.
/// @param i1 Second candidate vertex index.
/// @param i2 Third candidate vertex index.
/// @return Non-zero when the triangle is accepted and appended; otherwise zero.
static int gltf_emit_triangle(
    void *mesh, uint32_t vertex_count, uint32_t i0, uint32_t i1, uint32_t i2) {
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
        return 0;
    /* Delegate the degeneracy test to the mesh's own predicate so the filter and
     * AddTriangle agree exactly. A separate loader-side epsilon (the old fixed
     * 1e-20 area check) let scale-relative slivers through and trapped imports
     * of real-world low-poly character models. */
    if (!rt_mesh3d_triangle_indices_valid(mesh, (int64_t)i0, (int64_t)i1, (int64_t)i2))
        return 0;
    rt_mesh3d_add_triangle(mesh, (int64_t)i0, (int64_t)i1, (int64_t)i2);
    return !((rt_mesh3d *)mesh)->build_failed;
}

/// @brief Read a triangle index from an optional index accessor, or use the element position
/// directly.
/// @details When @p view is NULL the primitive has no index buffer, so the element index itself
///   is used as the vertex index (sequential triangles). This mirrors how GL_TRIANGLES works
///   with `glDrawArrays` vs `glDrawElements`.
/// @param view Optional scalar unsigned index accessor.
/// @param element_idx Logical index-stream element.
/// @return Decoded vertex index or @p element_idx for an unindexed primitive.
static uint32_t gltf_read_index(const gltf_accessor_view_t *view, int32_t element_idx) {
    if (!view)
        return (uint32_t)element_idx;
    uint32_t value = 0;
    gltf_accessor_read_u32(view, element_idx, &value, 1);
    return value;
}

/// @brief Test whether an accessor view has dense data or sparse substitution payload.
/// @param view Accessor view.
/// @return Non-zero when readable dense or sparse bytes are present.
static int gltf_accessor_has_payload(const gltf_accessor_view_t *view) {
    return view &&
           (view->data || (view->sparse_count > 0 && view->sparse_indices && view->sparse_values));
}

/// @brief Test whether an accessor is a scalar unsigned index stream.
/// @param view Accessor view.
/// @return Non-zero for SCALAR unsigned byte/short/int layouts.
static int gltf_accessor_is_indices(const gltf_accessor_view_t *view) {
    return view && view->comp_count == 1 &&
           (view->comp_type == 5121 || view->comp_type == 5123 || view->comp_type == 5125);
}

/// @brief Test whether an accessor uses an 8/16-bit integer component type.
/// @param view Accessor view.
/// @return Non-zero for signed/unsigned byte or short components.
static int gltf_accessor_is_quantized_int(const gltf_accessor_view_t *view) {
    return view && (view->comp_type == 5120 || view->comp_type == 5121 || view->comp_type == 5122 ||
                    view->comp_type == 5123);
}

/// @brief True if the accessor is a valid POSITION stream: FLOAT VEC3, or a
///        KHR_mesh_quantization 8/16-bit integer VEC3 (normalized or plain).
/// @param view Accessor view.
/// @return Non-zero when the layout is accepted for POSITION.
static int gltf_accessor_is_position(const gltf_accessor_view_t *view) {
    if (!view || view->comp_count != 3)
        return 0;
    return view->comp_type == 5126 || gltf_accessor_is_quantized_int(view);
}

/// @brief True if the accessor is a valid NORMAL stream: FLOAT VEC3, or a
///        KHR_mesh_quantization normalized signed byte/short VEC3.
/// @param view Accessor view.
/// @return Non-zero when the layout is accepted for NORMAL.
static int gltf_accessor_is_normal(const gltf_accessor_view_t *view) {
    if (!view || view->comp_count < 3)
        return 0;
    if (view->comp_type == 5126)
        return view->comp_count == 3;
    return view->normalized && (view->comp_type == 5120 || view->comp_type == 5122);
}

/// @brief True if the accessor is a valid TANGENT stream (@p min_components..4): FLOAT,
///        or a KHR_mesh_quantization normalized signed byte/short.
/// @param view Accessor view.
/// @param min_components Minimum accepted component count.
/// @return Non-zero when the layout is accepted for TANGENT.
static int gltf_accessor_is_tangent(const gltf_accessor_view_t *view, int min_components) {
    if (!view || view->comp_count < min_components || view->comp_count > 4)
        return 0;
    if (view->comp_type == 5126)
        return 1;
    return view->normalized && (view->comp_type == 5120 || view->comp_type == 5122);
}

/// @brief True if the accessor is a valid UV stream (≥2 components, FLOAT or a
///        KHR_mesh_quantization 8/16-bit integer, normalized or plain).
/// @param view Accessor view.
/// @return Non-zero when the layout is accepted for a texture coordinate.
static int gltf_accessor_is_texcoord(const gltf_accessor_view_t *view) {
    return view && view->comp_count >= 2 &&
           (view->comp_type == 5126 || gltf_accessor_is_quantized_int(view));
}

/// @brief True if the accessor is a valid vertex-color stream (3-4 components, FLOAT or
///   normalized u8/u16).
/// @param view Accessor view.
/// @return Non-zero when the layout is accepted for COLOR_0.
static int gltf_accessor_is_color(const gltf_accessor_view_t *view) {
    return view && view->comp_count >= 3 && view->comp_count <= 4 &&
           (view->comp_type == 5126 ||
            (view->normalized && (view->comp_type == 5121 || view->comp_type == 5123)));
}

/// @brief Test whether an accessor is a valid joint-index stream.
/// @param view Accessor view.
/// @return Non-zero for at least four unsigned byte/short components.
static int gltf_accessor_is_joints(const gltf_accessor_view_t *view) {
    return view && view->comp_count >= 4 && (view->comp_type == 5121 || view->comp_type == 5123);
}

/// @brief Test whether an accessor is a valid skin-weight stream.
/// @param view Accessor view.
/// @return Non-zero for at least four float or normalized unsigned byte/short components.
static int gltf_accessor_is_weights(const gltf_accessor_view_t *view) {
    return view && view->comp_count >= 4 &&
           (view->comp_type == 5126 ||
            (view->normalized && (view->comp_type == 5121 || view->comp_type == 5123)));
}

/// @brief Test whether every value in a float array is finite.
/// @param values Float array.
/// @param count Number of elements to inspect.
/// @return Non-zero when @p values is non-NULL, @p count is non-negative, and all
///         elements are finite.
static int gltf_f32_array_is_finite(const float *values, int32_t count) {
    if (!values || count < 0)
        return 0;
    for (int32_t i = 0; i < count; i++) {
        if (!isfinite(values[i]))
            return 0;
    }
    return 1;
}

/// @brief Normalize a finite vec3 in place.
/// @param[in,out] v Three-component vector.
/// @return 1 when the vector had a useful finite length, 0 for zero/NaN/Inf input.
static int gltf_normalize_vec3f_in_place(float *v) {
    double len;
    if (!v || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    len = sqrt((double)v[0] * (double)v[0] + (double)v[1] * (double)v[1] +
               (double)v[2] * (double)v[2]);
    if (!isfinite(len) || len <= 1e-12)
        return 0;
    v[0] = (float)((double)v[0] / len);
    v[1] = (float)((double)v[1] / len);
    v[2] = (float)((double)v[2] / len);
    return 1;
}

/// @brief Normalize a glTF tangent vec4 xyz and reduce handedness to ±1.
/// @param[in,out] tangent Four-component tangent `(xyz, handedness)`.
/// @return 1 when xyz was usable; 0 when tangent space should be recomputed.
static int gltf_sanitize_tangent4(float *tangent) {
    if (!tangent || !isfinite(tangent[3]))
        return 0;
    tangent[3] = tangent[3] < 0.0f ? -1.0f : 1.0f;
    return gltf_normalize_vec3f_in_place(tangent);
}

/// @brief Triangle count yielded by a glTF primitive topology: mode 4 = TRIANGLES
///   (count/3), 5/6 = TRIANGLE_STRIP/FAN (count-2). Returns 0 for too few indices and
///   -1 for non-triangle modes (points/lines).
/// @param mode glTF primitive topology mode.
/// @param index_count Number of logical indices/vertices in the draw.
/// @return Triangle count, zero for insufficient data, or -1 for unsupported non-triangle modes.
static int32_t gltf_primitive_triangle_count(int64_t mode, int32_t index_count) {
    if (index_count <= 0)
        return 0;
    if (mode == 4)
        return index_count / 3;
    if (mode == 5 || mode == 6)
        return index_count >= 3 ? index_count - 2 : 0;
    return -1;
}

/// @brief Convert glTF primitive topology indices to triangles and append them to a mesh.
/// @details Handles the three glTF triangle-topology modes:
///   - mode 4 = GL_TRIANGLES: every three consecutive indices form one triangle.
///   - mode 5 = GL_TRIANGLE_STRIP: each new vertex extends the strip; winding alternates
///     on odd indices (i+1, i, i+2 instead of i, i+1, i+2) to preserve clockwise order.
///   - mode 6 = GL_TRIANGLE_FAN: a fixed first vertex (the fan center) combined with each
///     consecutive pair of subsequent vertices.
///   Other topology modes (POINTS, LINES, LINE_STRIP, LINE_LOOP — modes 0-3) are not
///   supported and return 0. @p index_view may be NULL for unindexed draws.
/// @param mesh         Zanna mesh object to receive the triangles.
/// @param mode         glTF primitive topology mode (4, 5, or 6).
/// @param index_view   Optional index buffer accessor; NULL for non-indexed draw.
/// @param vertex_count Total number of vertices in the primitive (used for bounds-checking).
/// @return 1 if at least one triangle was emitted, 0 on unsupported topology or no data.
static int gltf_append_primitive_indices(void *mesh,
                                         int64_t mode,
                                         const gltf_accessor_view_t *index_view,
                                         int32_t vertex_count) {
    int32_t count = index_view ? index_view->count : vertex_count;
    int32_t emitted = 0;
    if (count <= 0)
        return 0;
    if (mode == 4) {
        for (int32_t i = 0; i + 2 < count; i += 3) {
            emitted += gltf_emit_triangle(mesh,
                                          (uint32_t)vertex_count,
                                          gltf_read_index(index_view, i),
                                          gltf_read_index(index_view, i + 1),
                                          gltf_read_index(index_view, i + 2));
        }
        return emitted > 0;
    }
    if (mode == 5) {
        for (int32_t i = 0; i + 2 < count; i++) {
            uint32_t i0 = gltf_read_index(index_view, i + 0);
            uint32_t i1 = gltf_read_index(index_view, i + 1);
            uint32_t i2 = gltf_read_index(index_view, i + 2);
            if ((i & 1) != 0) {
                uint32_t tmp = i0;
                i0 = i1;
                i1 = tmp;
            }
            emitted += gltf_emit_triangle(mesh, (uint32_t)vertex_count, i0, i1, i2);
        }
        return emitted > 0;
    }
    if (mode == 6) {
        uint32_t base = gltf_read_index(index_view, 0);
        for (int32_t i = 1; i + 1 < count; i++) {
            emitted += gltf_emit_triangle(mesh,
                                          (uint32_t)vertex_count,
                                          base,
                                          gltf_read_index(index_view, i),
                                          gltf_read_index(index_view, i + 1));
        }
        return emitted > 0;
    }
    return 0;
}

/// @brief Release a runtime reference slot and set it to NULL.
/// @param[in,out] slot Address of the reference to release.
static void gltf_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Drop a transient GC reference taken during the load.
/// @param obj Transient runtime object, such as the parsed JSON map; NULL is accepted.
static void gltf_release_local(void *obj) {
    void *tmp = obj;
    gltf_release_ref(&tmp);
}
