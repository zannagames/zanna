//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_codec.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF image-codec, data-URI/base64, byte I/O, and
// buffer-view access helpers
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_codec.inc
 * @brief Provides byte, URI, image-codec, preload-bundle, and buffer-view helpers for glTF import.
 * @details This implementation fragment is included by `rt_gltf.c`. It owns the importer's
 *          overflow-safe binary readers, GLB document slicing, embedded-data decoding, external
 *          URI normalization, image staging, preload dependency transfer, and transparent
 *          buffer-view lookup. The helpers deliberately validate all untrusted sizes and offsets
 *          before allocating memory or publishing borrowed spans.
 */

//===----------------------------------------------------------------------===//
// Buffer management
//===----------------------------------------------------------------------===//

/// @brief Owned byte storage for one materialized glTF buffer.
typedef struct {
    /// Heap-allocated buffer contents.
    uint8_t *data;
    /// Number of readable bytes at @ref data.
    size_t len;
} gltf_buffer_t;

/// @brief Type tag describing the payload owned by a preload dependency.
typedef enum {
    /// Raw external or embedded glTF buffer bytes.
    GLTF_PRELOAD_DEP_BUFFER = 1,
    /// Encoded image bytes awaiting decode.
    GLTF_PRELOAD_DEP_IMAGE = 2,
    /// Decoded `VGRA` staging blob awaiting main-thread upload.
    GLTF_PRELOAD_DEP_IMAGE_RGBA = 3,
    /// Packed, worker-decoded mesh primitive.
    GLTF_PRELOAD_DEP_MESH_POD = 4,
    /// Packed, worker-decoded morph-target data.
    GLTF_PRELOAD_DEP_MORPH_POD = 5,
    /// Runtime Pixels object created from staged RGBA data.
    GLTF_PRELOAD_DEP_IMAGE_PIXELS = 6,
    /// Runtime texture object retaining its encoded source bytes.
    GLTF_PRELOAD_DEP_IMAGE_SOURCE = 7
} gltf_preload_dependency_kind_t;

/// Magic word at the start of a packed mesh preload blob.
#define GLTF_PRELOAD_MESH_POD_MAGIC 0x504D4756u /* "VGMP" little-endian */
/* Version 2 grew the header from 32 to 40 bytes to carry the per-primitive skin-import
 * diagnostics (truncated-influence / out-of-range-joint vertex counts) from the worker
 * decode to the committing thread, where warnings can be recorded. The POD only ever
 * lives in memory between a preload worker and the same process's loader, so the bump
 * has no persistence implications. */
#define GLTF_PRELOAD_MESH_POD_VERSION 2u
/// Byte size of the current packed mesh preload header.
#define GLTF_PRELOAD_MESH_POD_HEADER_SIZE 40u
/// Packed mesh flag indicating decoded vertex normals are present.
#define GLTF_PRELOAD_MESH_POD_HAS_NORMALS 0x1u
/// Packed mesh flag indicating primary texture coordinates are present.
#define GLTF_PRELOAD_MESH_POD_HAS_UV0 0x2u
/// Packed mesh flag indicating decoded tangents are present.
#define GLTF_PRELOAD_MESH_POD_HAS_TANGENTS 0x4u
/// Packed mesh flag indicating joint indices and weights are present.
#define GLTF_PRELOAD_MESH_POD_HAS_SKINNING 0x8u
/// Packed mesh flag indicating secondary texture coordinates are present.
#define GLTF_PRELOAD_MESH_POD_HAS_UV1 0x10u

/// Magic word at the start of a packed morph-target preload blob.
#define GLTF_PRELOAD_MORPH_POD_MAGIC 0x544D4756u /* "VGMT" little-endian */
/// Current in-memory packed morph-target format version.
#define GLTF_PRELOAD_MORPH_POD_VERSION 1u
/// Byte size of the packed morph-target header.
#define GLTF_PRELOAD_MORPH_POD_HEADER_SIZE 32u
/// Byte size of one packed morph-target record.
#define GLTF_PRELOAD_MORPH_POD_RECORD_SIZE 32u
/// Packed morph flag indicating position deltas are present.
#define GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS 0x1u
/// Packed morph flag indicating normal deltas are present.
#define GLTF_PRELOAD_MORPH_POD_HAS_NORMALS 0x2u
/// Packed morph flag indicating tangent deltas are present.
#define GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS 0x4u

/// @brief One staged preload dependency: an owned key (path), raw bytes and/or a decoded
///        runtime object, how many bytes have been incrementally prepared, and its kind tag.
typedef struct {
    /// Owned lookup key, normally a normalized path or generated primitive key.
    char *path;
    /// Owned raw or packed payload, depending on @ref kind.
    uint8_t *data;
    /// Retained decoded runtime object, when the dependency has been materialized.
    void *object;
    /// Number of bytes stored at @ref data.
    size_t len;
    /// RGBA pixel bytes already copied during incremental preparation.
    size_t prepared;
    /// Payload representation and ownership discriminator.
    gltf_preload_dependency_kind_t kind;
} gltf_preload_dependency_t;

/// @brief Borrowed byte span for a buffer used during worker-side preload decoding.
typedef struct {
    /// First byte of the buffer.
    const uint8_t *data;
    /// Number of readable bytes.
    size_t len;
} gltf_preload_buffer_ref_t;

/// @brief Validated worker-side description of one glTF bufferView.
typedef struct {
    /// Index of the parent buffer.
    int buffer;
    /// Byte offset into the parent buffer.
    size_t byte_offset;
    /// Number of bytes in the view.
    size_t byte_length;
    /// Optional interleaved element stride.
    size_t byte_stride;
    /// Non-zero when every parsed field passed validation.
    int valid;
} gltf_preload_buffer_view_ref_t;

/// @brief Compact worker-side description of a glTF accessor and optional sparse overlay.
typedef struct {
    /// Referenced base bufferView index, or a negative value for sparse-only data.
    int view;
    /// Byte offset within the base view.
    size_t byte_offset;
    /// glTF numeric component-type constant.
    int comp_type;
    /// Number of components per logical element.
    int comp_count;
    /// Number of logical elements.
    int count;
    /// Whether integer values are normalized during float conversion.
    int8_t normalized;
    /// Whether sparse indices and replacement values are present.
    int8_t has_sparse;
    /// BufferView containing sparse element indices.
    int sparse_indices_view;
    /// Byte offset into the sparse-index view.
    size_t sparse_indices_offset;
    /// BufferView containing sparse replacement values.
    int sparse_values_view;
    /// Byte offset into the sparse-values view.
    size_t sparse_values_offset;
    /// Number of sparse replacements.
    int sparse_count;
    /// glTF component type of each sparse index.
    int sparse_index_comp_type;
    /// Non-zero when the complete accessor description is usable.
    int valid;
} gltf_preload_accessor_ref_t;

/// @brief Owned root bytes and staged dependencies produced by asynchronous glTF preload.
struct rt_gltf_preload_bundle {
    /// Owned plain glTF or GLB root document bytes.
    uint8_t *root_data;
    /// Number of bytes in @ref root_data.
    size_t root_size;
    /// Growable array of owned dependency payloads.
    gltf_preload_dependency_t *dependencies;
    /// Number of initialized dependency entries.
    size_t dependency_count;
    /// Allocated capacity of @ref dependencies.
    size_t dependency_capacity;
};

/// @brief Validated runtime accessor view used by all typed glTF reads.
typedef struct {
    /// Borrowed first byte of the base accessor data.
    const uint8_t *data;
    /// Number of logical elements.
    int32_t count;
    /// Byte distance between consecutive base elements.
    int32_t stride;
    /// glTF numeric component-type constant.
    int32_t comp_type;
    /// Number of components per logical element.
    int32_t comp_count;
    /// Whether integer components are normalized when converted to floats.
    int8_t normalized;
    /// Borrowed sparse-index data, or NULL when no sparse overlay exists.
    const uint8_t *sparse_indices;
    /// Borrowed sparse replacement-value data.
    const uint8_t *sparse_values;
    /// Number of sparse replacement entries.
    int32_t sparse_count;
    /// glTF component type used by sparse indices.
    int32_t sparse_index_comp_type;
    /// Byte distance between sparse indices.
    int32_t sparse_index_stride;
    /// Byte distance between sparse replacement elements.
    int32_t sparse_value_stride;
} gltf_accessor_view_t;

/// @brief Format the preload dependency key for a mesh primitive.
static void gltf_preload_mesh_key(int mesh_index, int primitive_index, char *out, size_t out_cap);
/// @brief Format the preload dependency key for a primitive's morph-target payload.
static void gltf_preload_morph_key(int mesh_index, int primitive_index, char *out, size_t out_cap);
/// @brief Read an unaligned little-endian 32-bit unsigned integer.
static uint32_t gltf_read_u32_le(const uint8_t *p);
/// @brief Multiply two sizes without allowing overflow.
static int gltf_checked_mul_size(size_t a, size_t b, size_t *out);
/// @brief Validate a staged RGBA blob and obtain its dimensions and payload size.
static int gltf_preload_rgba_blob_pixel_bytes(const uint8_t *blob,
                                              size_t len,
                                              uint32_t *out_width,
                                              uint32_t *out_height,
                                              size_t *out_pixel_bytes);

/// @brief Compute a checked geometric capacity for preload staging arrays.
/// @param current Current element capacity.
/// @param required Minimum element capacity required by the caller.
/// @param initial First capacity to try when @p current is zero.
/// @param elem_size Byte size of one array element.
/// @param out_capacity Receives either the unchanged current capacity or a safe grown capacity.
/// @return Non-zero when @p required fits the reported capacity; zero for invalid input or
///         overflow.
static int gltf_preload_next_capacity_size(
    size_t current, size_t required, size_t initial, size_t elem_size, size_t *out_capacity) {
    size_t next;
    if (out_capacity)
        *out_capacity = current;
    if (!out_capacity || elem_size == 0u || required == 0u)
        return 0;
    if (current >= required)
        return 1;
    next = current > 0u ? current : initial;
    if (next == 0u)
        next = 1u;
    while (next < required) {
        size_t prev = next;
        if (next > SIZE_MAX / 2u)
            next = required;
        else
            next *= 2u;
        if (next <= prev && next < required)
            return 0;
    }
    if (next > SIZE_MAX / elem_size)
        return 0;
    *out_capacity = next;
    return 1;
}

/// @brief Int-capacity wrapper for JSON-indexed preload ref tables.
/// @param current Current non-negative element capacity.
/// @param required Positive minimum element capacity.
/// @param initial Initial capacity used for an empty table.
/// @param elem_size Byte size of one table element.
/// @param out_capacity Receives the checked capacity representable as an `int`.
/// @return Non-zero on success, or zero for invalid input, overflow, or an `INT_MAX` excess.
static int gltf_preload_next_capacity_int(
    int current, int required, int initial, size_t elem_size, int *out_capacity) {
    size_t next = 0u;
    if (out_capacity)
        *out_capacity = current;
    if (!out_capacity || current < 0 || required <= 0)
        return 0;
    if (current >= required)
        return 1;
    if (!gltf_preload_next_capacity_size(
            (size_t)current, (size_t)required, (size_t)initial, elem_size, &next) ||
        next > (size_t)INT_MAX)
        return 0;
    *out_capacity = (int)next;
    return 1;
}

/// @brief Duplicate a NUL-terminated string into a malloc'd copy (NULL in -> NULL out).
/// @param text Source C string, or NULL.
/// @return Heap-allocated copy to free, or NULL for a null source or allocation failure.
static char *gltf_strdup_cstr(const char *text) {
    size_t len;
    char *copy;
    if (!text)
        return NULL;
    len = strlen(text);
    copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

/// @brief Release a GC reference held in @p *slot if this is its last drop, then NULL it.
/// @param slot Address of the retained runtime-object slot to release.
static void gltf_preload_release_object(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Free one dependency's owned path, raw bytes, and decoded object, then zero it.
/// @param dep Dependency entry to clear; may be NULL.
static void gltf_preload_dependency_clear(gltf_preload_dependency_t *dep) {
    if (!dep)
        return;
    free(dep->path);
    free(dep->data);
    gltf_preload_release_object(&dep->object);
    memset(dep, 0, sizeof(*dep));
}

/// @brief Free a preload bundle: its root JSON bytes, every staged dependency, and the bundle.
/// @param bundle Bundle to destroy; may be NULL.
void rt_gltf_preload_bundle_free(rt_gltf_preload_bundle *bundle) {
    if (!bundle)
        return;
    free(bundle->root_data);
    bundle->root_data = NULL;
    for (size_t i = 0; i < bundle->dependency_count; ++i)
        gltf_preload_dependency_clear(&bundle->dependencies[i]);
    free(bundle->dependencies);
    free(bundle);
}

/// @brief Number of staged dependencies in the bundle (0 if NULL).
/// @param bundle Bundle to inspect; may be NULL.
/// @return Current number of staged dependency entries.
size_t rt_gltf_preload_bundle_dependency_count(const rt_gltf_preload_bundle *bundle) {
    return bundle ? bundle->dependency_count : 0u;
}

/// @brief Count staged dependencies that are decoded RGBA images.
/// @param bundle Bundle to inspect; may be NULL.
/// @return Number of dependencies tagged `GLTF_PRELOAD_DEP_IMAGE_RGBA`.
size_t rt_gltf_preload_bundle_decoded_image_count(const rt_gltf_preload_bundle *bundle) {
    size_t count = 0;
    if (!bundle)
        return 0u;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        if (bundle->dependencies[i].kind == GLTF_PRELOAD_DEP_IMAGE_RGBA)
            count++;
    }
    return count;
}

/// @brief Total still-unprepared RGBA pixel bytes across decoded-image dependencies.
/// @details Excludes the 12-byte "VGRA" header and any bytes already prepared via the slice
///          API; saturates at SIZE_MAX. Lets a caller budget remaining main-thread upload work.
/// @param bundle Bundle whose decoded-image staging bytes are totaled; may be NULL.
/// @return Remaining RGBA payload bytes, saturated at `SIZE_MAX`.
size_t rt_gltf_preload_bundle_decoded_image_bytes(const rt_gltf_preload_bundle *bundle) {
    size_t bytes = 0;
    if (!bundle)
        return 0u;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        if (bundle->dependencies[i].kind == GLTF_PRELOAD_DEP_IMAGE_RGBA) {
            size_t len = 0u;
            size_t prepared = bundle->dependencies[i].prepared;
            if (!gltf_preload_rgba_blob_pixel_bytes(
                    bundle->dependencies[i].data, bundle->dependencies[i].len, NULL, NULL, &len))
                continue;
            if (prepared < len)
                len -= prepared;
            else
                len = 0u;
            if (SIZE_MAX - bytes < len)
                return SIZE_MAX;
            bytes += len;
        }
    }
    return bytes;
}

/// @brief Validate a "VGRA" blob header and report its width, height, and pixel byte count.
/// @param blob Candidate blob beginning with the 12-byte `VGRA` header.
/// @param len Number of readable bytes at @p blob.
/// @param out_width Optional destination for the decoded width.
/// @param out_height Optional destination for the decoded height.
/// @param out_pixel_bytes Optional destination for the tightly packed RGBA payload size.
/// @return 1 with the out-params set, or 0 on a malformed/too-short blob (out-params zeroed).
static int gltf_preload_rgba_blob_pixel_bytes(const uint8_t *blob,
                                              size_t len,
                                              uint32_t *out_width,
                                              uint32_t *out_height,
                                              size_t *out_pixel_bytes) {
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    size_t pixel_bytes;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
    if (out_pixel_bytes)
        *out_pixel_bytes = 0;
    if (!blob || len < 12u)
        return 0;
    if (blob[0] != 'V' || blob[1] != 'G' || blob[2] != 'R' || blob[3] != 'A')
        return 0;
    width = gltf_read_u32_le(blob + 4);
    height = gltf_read_u32_le(blob + 8);
    if (width == 0 || height == 0)
        return 0;
    if (!gltf_checked_mul_size((size_t)width, (size_t)height, &pixel_count) ||
        !gltf_checked_mul_size(pixel_count, 4u, &pixel_bytes) || pixel_bytes > len - 12u)
        return 0;
    if (out_width)
        *out_width = width;
    if (out_height)
        *out_height = height;
    if (out_pixel_bytes)
        *out_pixel_bytes = pixel_bytes;
    return 1;
}

/// @brief Size (<= @p max_bytes) of the next decoded-image slice that would be prepared.
/// @details Returns a 4-byte-aligned (whole-pixel) byte count for the first decoded-RGBA
///          dependency with work remaining, letting the caller pace upload without mutating
///          state. Returns 0 when nothing is pending or a blob is malformed.
/// @param bundle Bundle containing staged decoded images.
/// @param max_bytes Requested upper bound for the next upload slice.
/// @return Whole-pixel-aligned next-slice byte count, or zero when no work is available.
size_t rt_gltf_preload_bundle_next_decoded_image_slice_bytes(const rt_gltf_preload_bundle *bundle,
                                                             size_t max_bytes) {
    if (!bundle || max_bytes == 0u)
        return 0u;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        const gltf_preload_dependency_t *dep = &bundle->dependencies[i];
        size_t pixel_bytes;
        size_t remaining;
        size_t slice;
        if (dep->kind != GLTF_PRELOAD_DEP_IMAGE_RGBA)
            continue;
        if (!gltf_preload_rgba_blob_pixel_bytes(dep->data, dep->len, NULL, NULL, &pixel_bytes))
            return 0u;
        if (dep->prepared >= pixel_bytes)
            continue;
        remaining = pixel_bytes - dep->prepared;
        slice = remaining < max_bytes ? remaining : max_bytes;
        if (slice < 4u)
            slice = remaining < 4u ? remaining : 4u;
        return slice - (slice % 4u);
    }
    return 0u;
}

/// @brief Incrementally upload up to @p max_bytes of the next decoded image into its Pixels object.
/// @details Lazily allocates the destination Pixels on first slice, copies a whole-pixel-aligned
///          run of RGBA texels, and advances the prepared cursor. When a blob finishes it marks the
///          surface dirty, frees the staging bytes, and retags the dependency as IMAGE_PIXELS. This
///          spreads large texture uploads across frames to avoid a single main-thread stall.
/// @param bundle Bundle whose first pending decoded image is advanced.
/// @param max_bytes Maximum number of RGBA bytes to copy this call.
/// @return Bytes prepared this call (0 when nothing remains or allocation fails).
size_t rt_gltf_preload_bundle_prepare_decoded_image_slice(rt_gltf_preload_bundle *bundle,
                                                          size_t max_bytes) {
    if (!bundle || max_bytes == 0u)
        return 0u;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        gltf_preload_dependency_t *dep = &bundle->dependencies[i];
        uint32_t width;
        uint32_t height;
        size_t pixel_bytes;
        size_t remaining;
        size_t copy_bytes;
        size_t first_pixel;
        size_t copy_pixels;
        rt_pixels_impl *pixels;
        if (dep->kind != GLTF_PRELOAD_DEP_IMAGE_RGBA)
            continue;
        if (!gltf_preload_rgba_blob_pixel_bytes(dep->data, dep->len, &width, &height, &pixel_bytes))
            return 0u;
        if (dep->prepared >= pixel_bytes) {
            dep->prepared = pixel_bytes;
            continue;
        }
        if (!dep->object) {
            dep->object = pixels_alloc((int64_t)width, (int64_t)height);
            if (!dep->object)
                return 0u;
        }
        remaining = pixel_bytes - dep->prepared;
        copy_bytes = remaining < max_bytes ? remaining : max_bytes;
        if (copy_bytes < 4u)
            copy_bytes = remaining < 4u ? remaining : 4u;
        copy_bytes -= copy_bytes % 4u;
        if (copy_bytes == 0u)
            return 0u;
        first_pixel = dep->prepared / 4u;
        copy_pixels = copy_bytes / 4u;
        pixels = (rt_pixels_impl *)dep->object;
        for (size_t pi = 0; pi < copy_pixels; ++pi) {
            const uint8_t *sp = dep->data + 12u + dep->prepared + pi * 4u;
            pixels->data[first_pixel + pi] = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) |
                                             ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
        }
        dep->prepared += copy_bytes;
        if (dep->prepared >= pixel_bytes) {
            pixels_touch(pixels);
            free(dep->data);
            dep->data = NULL;
            dep->len = 0u;
            dep->prepared = 0u;
            dep->kind = GLTF_PRELOAD_DEP_IMAGE_PIXELS;
        }
        return copy_bytes;
    }
    return 0u;
}

/// @brief Count staged dependencies that are packed mesh POD blobs.
/// @param bundle Bundle to inspect; may be NULL.
/// @return Number of dependencies tagged `GLTF_PRELOAD_DEP_MESH_POD`.
size_t rt_gltf_preload_bundle_decoded_mesh_count(const rt_gltf_preload_bundle *bundle) {
    size_t count = 0;
    if (!bundle)
        return 0u;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        if (bundle->dependencies[i].kind == GLTF_PRELOAD_DEP_MESH_POD)
            count++;
    }
    return count;
}

/// @brief Append a dependency (taking ownership of @p data) under key @p path, growing the array.
/// @details Doubles capacity as needed and copies the key string. On any failure the caller
///          retains ownership of @p data (it is not stored).
/// @param bundle Destination preload bundle.
/// @param path Stable dependency lookup key to copy.
/// @param kind Payload kind stored in the new entry.
/// @param data Owned payload transferred only on success.
/// @param len Number of bytes at @p data.
/// @return 1 on success, 0 on invalid args or allocation failure.
static int gltf_preload_bundle_add_dependency(rt_gltf_preload_bundle *bundle,
                                              const char *path,
                                              gltf_preload_dependency_kind_t kind,
                                              uint8_t *data,
                                              size_t len) {
    gltf_preload_dependency_t *grown;
    char *path_copy;
    if (!bundle || !path || !data)
        return 0;
    if (bundle->dependency_count == bundle->dependency_capacity) {
        size_t next_capacity;
        if (!gltf_preload_next_capacity_size(bundle->dependency_capacity,
                                             bundle->dependency_count + 1u,
                                             8u,
                                             sizeof(*bundle->dependencies),
                                             &next_capacity))
            return 0;
        grown = (gltf_preload_dependency_t *)realloc(bundle->dependencies,
                                                     next_capacity * sizeof(*bundle->dependencies));
        if (!grown)
            return 0;
        memset(grown + bundle->dependency_capacity,
               0,
               (next_capacity - bundle->dependency_capacity) * sizeof(*grown));
        bundle->dependencies = grown;
        bundle->dependency_capacity = next_capacity;
    }
    path_copy = gltf_strdup_cstr(path);
    if (!path_copy)
        return 0;
    bundle->dependencies[bundle->dependency_count].path = path_copy;
    bundle->dependencies[bundle->dependency_count].data = data;
    bundle->dependencies[bundle->dependency_count].object = NULL;
    bundle->dependencies[bundle->dependency_count].len = len;
    bundle->dependencies[bundle->dependency_count].prepared = 0u;
    bundle->dependencies[bundle->dependency_count].kind = kind;
    bundle->dependency_count++;
    return 1;
}

/// @brief Find a dependency by (kind, path), remove it, and hand its raw bytes to the caller.
/// @details Transfers ownership of the returned buffer (caller frees) and compacts the array.
/// @param bundle Bundle to search and mutate.
/// @param path Dependency key to match exactly.
/// @param kind Required dependency kind.
/// @param out_len Optional destination for the transferred byte length; cleared on no match.
/// @return The dependency's data with @p out_len set, or NULL if no match.
static uint8_t *gltf_preload_bundle_take_dependency(rt_gltf_preload_bundle *bundle,
                                                    const char *path,
                                                    gltf_preload_dependency_kind_t kind,
                                                    size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!bundle || !path)
        return NULL;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        gltf_preload_dependency_t *dep = &bundle->dependencies[i];
        if (dep->kind == kind && dep->path && strcmp(dep->path, path) == 0) {
            uint8_t *data = dep->data;
            if (out_len)
                *out_len = dep->len;
            free(dep->path);
            if (i + 1u < bundle->dependency_count) {
                memmove(dep,
                        dep + 1u,
                        (bundle->dependency_count - i - 1u) * sizeof(*bundle->dependencies));
            }
            bundle->dependency_count--;
            memset(
                &bundle->dependencies[bundle->dependency_count], 0, sizeof(*bundle->dependencies));
            return data;
        }
    }
    return NULL;
}

/// @brief Find a dependency by (kind, path), remove it, and hand its decoded object to the caller.
/// @details Frees the dependency's raw bytes and key, compacts the array, and transfers ownership
///          of the returned runtime object. Returns NULL if no match.
/// @param bundle Bundle to search and mutate.
/// @param path Dependency key to match exactly.
/// @param kind Required object-bearing dependency kind.
/// @return Transferred runtime object, or NULL when no matching entry exists.
static void *gltf_preload_bundle_take_object_dependency(rt_gltf_preload_bundle *bundle,
                                                        const char *path,
                                                        gltf_preload_dependency_kind_t kind) {
    if (!bundle || !path)
        return NULL;
    for (size_t i = 0; i < bundle->dependency_count; ++i) {
        gltf_preload_dependency_t *dep = &bundle->dependencies[i];
        if (dep->kind == kind && dep->path && strcmp(dep->path, path) == 0) {
            void *object = dep->object;
            free(dep->path);
            free(dep->data);
            dep->object = NULL;
            if (i + 1u < bundle->dependency_count) {
                memmove(dep,
                        dep + 1u,
                        (bundle->dependency_count - i - 1u) * sizeof(*bundle->dependencies));
            }
            bundle->dependency_count--;
            memset(
                &bundle->dependencies[bundle->dependency_count], 0, sizeof(*bundle->dependencies));
            return object;
        }
    }
    return NULL;
}

/// @brief Read a little-endian uint32 from an unaligned byte pointer.
/// @details Uses individual byte loads and explicit shifts to avoid undefined behaviour
///   from unaligned access on strict-alignment platforms. Used for parsing the 12-byte
///   GLB (binary glTF) header and chunk-header magic/length fields.
/// @param p Pointer to at least four readable bytes.
/// @return Host-endian unsigned value represented by those bytes.
static uint32_t gltf_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Borrowed JSON/BIN spans extracted from one validated glTF root document.
/// @details The spans point into the caller-owned root byte buffer. Plain JSON documents set
///   `is_glb` to zero and leave the BIN span empty; GLB documents set it to one and may carry one
///   optional BIN chunk after the required JSON chunk.
typedef struct gltf_root_document_view {
    /// Borrowed first byte of the JSON document or GLB JSON chunk.
    uint8_t *json;
    /// Exact JSON span length, including GLB chunk padding when present.
    size_t json_len;
    /// Borrowed first byte of the optional GLB BIN chunk.
    uint8_t *bin;
    /// Exact BIN chunk length.
    size_t bin_len;
    /// Non-zero when the root bytes used the GLB container.
    int is_glb;
} gltf_root_document_view;

/// @brief Validate a plain glTF JSON document or the complete GLB 2.0 chunk sequence.
/// @details GLB input must contain exactly one non-empty JSON chunk first and at most one BIN
///   chunk second. Chunk alignment, declared length, bounds, duplicate/unknown chunks, truncated
///   headers, and trailing bytes are rejected before any span is published. Literal NUL bytes are
///   rejected in both plain and GLB JSON so downstream length-aware parsing cannot be truncated by
///   a C-string conversion. The returned spans borrow @p data and remain valid only while it lives.
/// @param data Root glTF/GLB bytes.
/// @param len Exact byte length of @p data.
/// @param out Receives a fully initialized borrowed document view on success.
/// @return 1 for a valid root document, otherwise 0 with @p out cleared.
static int gltf_parse_root_document_view(uint8_t *data, size_t len, gltf_root_document_view *out) {
    static const uint32_t json_chunk_type = UINT32_C(0x4E4F534A);
    static const uint32_t bin_chunk_type = UINT32_C(0x004E4942);
    gltf_root_document_view parsed;
    size_t pos;
    int chunk_index;
    memset(&parsed, 0, sizeof(parsed));
    if (out)
        memset(out, 0, sizeof(*out));
    if (!data || !out || len == 0 || len > (size_t)UINT32_MAX)
        return 0;

    if (len >= 4u && data[0] == 0x67 && data[1] == 0x6C && data[2] == 0x54 && data[3] == 0x46) {
        if (len < 12u || gltf_read_u32_le(data + 4u) != 2u ||
            gltf_read_u32_le(data + 8u) != (uint32_t)len)
            return 0;
        parsed.is_glb = 1;
        pos = 12u;
        chunk_index = 0;
        while (pos < len) {
            uint32_t chunk_len;
            uint32_t chunk_type;
            if (len - pos < 8u)
                return 0;
            chunk_len = gltf_read_u32_le(data + pos);
            chunk_type = gltf_read_u32_le(data + pos + 4u);
            pos += 8u;
            if ((chunk_len & 3u) != 0u || (size_t)chunk_len > len - pos)
                return 0;
            if (chunk_index == 0) {
                if (chunk_type != json_chunk_type || chunk_len == 0u)
                    return 0;
                parsed.json = data + pos;
                parsed.json_len = (size_t)chunk_len;
                if (memchr(parsed.json, '\0', parsed.json_len) != NULL)
                    return 0;
            } else if (chunk_index == 1) {
                if (chunk_type != bin_chunk_type)
                    return 0;
                parsed.bin = data + pos;
                parsed.bin_len = (size_t)chunk_len;
            } else {
                return 0;
            }
            pos += (size_t)chunk_len;
            chunk_index++;
        }
        if (pos != len || chunk_index < 1 || !parsed.json)
            return 0;
    } else {
        if (memchr(data, '\0', len) != NULL)
            return 0;
        parsed.json = data;
        parsed.json_len = len;
    }
    *out = parsed;
    return 1;
}

/// @brief Overflow-safe size_t addition: sets *out = a+b and returns 1, or returns 0 on overflow.
/// @details Guards all byte-range arithmetic in the accessor/buffer-view bounds checks so that
///   a malformed glTF with exaggerated offsets or lengths can't wrap around to a valid-looking
///   in-bounds address. Returns 0 also when @p out is NULL.
/// @param a First addend.
/// @param b Second addend.
/// @param out Receives the sum when representable.
/// @return Non-zero on success; zero when @p out is NULL or the addition overflows.
static int gltf_checked_add_size(size_t a, size_t b, size_t *out) {
    if (!out || a > SIZE_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

/// @brief Overflow-safe size_t multiplication: sets *out = a*b and returns 1, or returns 0 on
/// overflow.
/// @details Uses the `b > SIZE_MAX / a` guard (avoiding the multiply itself when `a != 0`) to
///   detect the overflow before it happens. Used alongside `gltf_checked_add_size` to
///   safely compute `stride * count` and `comp_size * comp_count` bounds checks.
/// @param a First factor.
/// @param b Second factor.
/// @param out Receives the product when representable.
/// @return Non-zero on success; zero when @p out is NULL or the multiplication overflows.
static int gltf_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Read a little-endian uint16 from @p p.
/// @param p Pointer to at least two readable bytes.
/// @return Host-endian unsigned value represented by those bytes.
static uint16_t gltf_read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/// @brief Read a little-endian int32 from @p p (two's-complement reinterpretation of the u32).
/// @param p Pointer to at least four readable bytes.
/// @return Host-endian signed 32-bit value represented by those bytes.
static int32_t gltf_read_i32_le(const uint8_t *p) {
    return (int32_t)gltf_read_u32_le(p);
}

/// @brief Write @p v as a little-endian uint32 into @p p.
/// @param p Destination with space for four bytes.
/// @param v Host-endian value to encode.
static void gltf_write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/// @brief Read a little-endian IEEE-754 float32 from @p p (bit-copied, no aliasing).
/// @param p Pointer to at least four readable bytes.
/// @return Float whose object representation is encoded at @p p.
static float gltf_read_f32_le(const uint8_t *p) {
    uint32_t bits = gltf_read_u32_le(p);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/// @brief Write @p value as a little-endian IEEE-754 float32 into @p p.
/// @param p Destination with space for four bytes.
/// @param value Float value whose object representation is encoded.
static void gltf_write_f32_le(uint8_t *p, float value) {
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    gltf_write_u32_le(p, bits);
}

/// @brief Case-insensitive substring search: is @p needle present anywhere in @p text?
/// @details An empty needle matches; used to spot a MIME token within a longer type string.
/// @param text NUL-terminated ASCII string to search.
/// @param needle NUL-terminated ASCII substring to find.
/// @return Non-zero when @p needle occurs in @p text ignoring ASCII case.
static int gltf_ascii_has_token_i(const char *text, const char *needle) {
    size_t needle_len;
    if (!text || !needle)
        return 0;
    needle_len = strlen(needle);
    if (needle_len == 0)
        return 1;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (gltf_ascii_ieq_n(text + i, needle, needle_len))
            return 1;
    }
    return 0;
}

/// @brief Whether @p mime_or_name denotes a BMP image (by MIME token or .bmp extension).
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero for a recognized BMP declaration or suffix.
static int gltf_preload_image_is_bmp(const char *mime_or_name) {
    size_t len;
    if (!mime_or_name)
        return 0;
    if (gltf_ascii_has_token_i(mime_or_name, "image/bmp") ||
        gltf_ascii_has_token_i(mime_or_name, "image/x-ms-bmp"))
        return 1;
    len = strlen(mime_or_name);
    return len >= 4u && gltf_ascii_ieq_n(mime_or_name + len - 4u, ".bmp", 4u);
}

/// @brief Whether @p mime_or_name denotes a PNG image (by MIME token or .png extension).
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero for a recognized PNG declaration or suffix.
static int gltf_preload_image_is_png(const char *mime_or_name) {
    size_t len;
    if (!mime_or_name)
        return 0;
    if (gltf_ascii_has_token_i(mime_or_name, "image/png"))
        return 1;
    len = strlen(mime_or_name);
    return len >= 4u && gltf_ascii_ieq_n(mime_or_name + len - 4u, ".png", 4u);
}

/// @brief Whether @p mime_or_name denotes a JPEG image (by MIME token or .jpg/.jpeg extension).
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero for a recognized JPEG declaration or suffix.
static int gltf_preload_image_is_jpeg(const char *mime_or_name) {
    size_t len;
    if (!mime_or_name)
        return 0;
    if (gltf_ascii_has_token_i(mime_or_name, "image/jpeg") ||
        gltf_ascii_has_token_i(mime_or_name, "image/jpg"))
        return 1;
    len = strlen(mime_or_name);
    return (len >= 4u && gltf_ascii_ieq_n(mime_or_name + len - 4u, ".jpg", 4u)) ||
           (len >= 5u && gltf_ascii_ieq_n(mime_or_name + len - 5u, ".jpeg", 5u));
}

/// @brief Whether @p mime_or_name denotes a GIF image (by MIME token or .gif extension).
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero for a recognized GIF declaration or suffix.
static int gltf_preload_image_is_gif(const char *mime_or_name) {
    size_t len;
    if (!mime_or_name)
        return 0;
    if (gltf_ascii_has_token_i(mime_or_name, "image/gif"))
        return 1;
    len = strlen(mime_or_name);
    return len >= 4u && gltf_ascii_ieq_n(mime_or_name + len - 4u, ".gif", 4u);
}

/// @brief Whether the image format is one the preloader can decode (PNG/BMP/JPEG/GIF).
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero when a preload decoder exists for the declared format.
static int gltf_preload_image_is_supported_format(const char *mime_or_name) {
    return gltf_preload_image_is_png(mime_or_name) || gltf_preload_image_is_bmp(mime_or_name) ||
           gltf_preload_image_is_jpeg(mime_or_name) || gltf_preload_image_is_gif(mime_or_name);
}

/// @brief Whether @p mime_or_name denotes a KTX2 texture asset.
/// @param mime_or_name MIME declaration or resource name to classify.
/// @return Non-zero for a recognized KTX2 declaration or suffix.
static int gltf_image_is_ktx2(const char *mime_or_name) {
    size_t len;
    if (!mime_or_name)
        return 0;
    if (gltf_ascii_has_token_i(mime_or_name, "image/ktx2") ||
        gltf_ascii_has_token_i(mime_or_name, "model/ktx2") ||
        gltf_ascii_has_token_i(mime_or_name, "application/ktx2"))
        return 1;
    len = strlen(mime_or_name);
    return len >= 5u && gltf_ascii_ieq_n(mime_or_name + len - 5u, ".ktx2", 5u);
}

/// @brief Identify one supported exact image container from its magic bytes.
/// @details The returned spelling is the normalized kind accepted by TextureAsset3D's private
///          source-container bridge. A filename or MIME declaration is deliberately not trusted:
///          the exact bytes determine whether provenance can safely be retained.
/// @param data Encoded image bytes to inspect.
/// @param len Number of readable bytes at @p data.
/// @return Static normalized container name, or NULL when the magic is unsupported.
static const char *gltf_image_container_kind(const uint8_t *data, size_t len) {
    static const uint8_t ktx2_magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t png_magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data)
        return NULL;
    if (len >= sizeof(ktx2_magic) && memcmp(data, ktx2_magic, sizeof(ktx2_magic)) == 0)
        return "ktx2";
    if (len >= sizeof(png_magic) && memcmp(data, png_magic, sizeof(png_magic)) == 0)
        return "png";
    if (len >= 4u && data[0] == 0xFFu && data[1] == 0xD8u && data[2] == 0xFFu)
        return "jpeg";
    if (len >= 6u && (memcmp(data, "GIF87a", 6u) == 0 || memcmp(data, "GIF89a", 6u) == 0))
        return "gif";
    if (len >= 14u && data[0] == 'B' && data[1] == 'M')
        return "bmp";
    return NULL;
}

/// @brief Replace decoded Pixels with a TextureAsset3D that also owns the exact encoded bytes.
/// @details Ownership of @p decoded is consumed only when wrapping succeeds or a recognized
///          source container cannot be retained. KTX2 already decodes directly to a source-backed
///          TextureAsset3D and therefore passes through unchanged.
/// @param decoded Retained decoded Pixels or texture object.
/// @param data Exact encoded source bytes.
/// @param len Number of encoded bytes at @p data.
/// @return The unchanged object, a new source-preserving wrapper, or NULL if wrapping fails.
static void *gltf_preserve_decoded_image_source(void *decoded, const uint8_t *data, size_t len) {
    const char *kind = gltf_image_container_kind(data, len);
    void *wrapped;
    if (!decoded || !kind || strcmp(kind, "ktx2") == 0)
        return decoded;
    if (!rt_pixels_checked_impl_or_null(decoded))
        return decoded;
    wrapped = rt_textureasset3d_wrap_encoded_pixels(decoded, data, (uint64_t)len, kind);
    if (!wrapped) {
        gltf_preload_release_object(&decoded);
        return NULL;
    }
    gltf_preload_release_object(&decoded);
    return wrapped;
}

/// @brief Decode KTX2 payloads under trap recovery so malformed textures fail the import cleanly.
/// @param data KTX2 container bytes.
/// @param data_len Number of readable bytes at @p data.
/// @return Newly retained texture asset, or NULL when validation, decoding, or trap recovery fails.
static void *gltf_decode_ktx2_payload(const uint8_t *data, size_t data_len) {
    void *asset = NULL;
    jmp_buf recovery;
    if (!data || data_len == 0)
        return NULL;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0)
        asset = rt_textureasset3d_load_ktx2_memory(data, (uint64_t)data_len);
    rt_trap_clear_recovery();
    return asset;
}

/// @brief Decode an uncompressed 24/32-bpp BMP into an owned "VGRA" RGBA blob.
/// @details Parses the BMP/DIB headers, handles bottom-up vs. top-down (negative height) row
///          order and the 4-byte row-stride padding, and swizzles BGR(A) to RGBA. Every size
///          computation is overflow-checked. The output blob is `V`,`G`,`R`,`A` + LE width +
///          LE height + tightly-packed RGBA8 rows (caller frees @p out_blob).
/// @param data Encoded BMP bytes.
/// @param len Number of readable bytes at @p data.
/// @param out_blob Receives the owned `VGRA` blob on success and is cleared on failure.
/// @param out_len Receives the total blob byte length and is cleared on failure.
/// @return 1 on success, 0 on a malformed/unsupported BMP or allocation failure.
static int gltf_decode_bmp_to_rgba_blob(const uint8_t *data,
                                        size_t len,
                                        uint8_t **out_blob,
                                        size_t *out_len) {
    uint32_t pixel_offset;
    uint32_t dib_size;
    int32_t width_i;
    int32_t height_i;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t width;
    uint32_t height;
    size_t row_stride;
    size_t image_rows;
    size_t pixel_bytes;
    size_t blob_len;
    uint8_t *blob;
    int top_down = 0;

    if (out_blob)
        *out_blob = NULL;
    if (out_len)
        *out_len = 0;
    if (!data || len < 54u || !out_blob || !out_len)
        return 0;
    if (data[0] != 'B' || data[1] != 'M')
        return 0;

    pixel_offset = gltf_read_u32_le(data + 10);
    dib_size = gltf_read_u32_le(data + 14);
    if (dib_size < 40u || pixel_offset >= len)
        return 0;
    width_i = gltf_read_i32_le(data + 18);
    height_i = gltf_read_i32_le(data + 22);
    planes = gltf_read_u16_le(data + 26);
    bit_count = gltf_read_u16_le(data + 28);
    compression = gltf_read_u32_le(data + 30);
    if (width_i <= 0 || height_i == 0 || planes != 1 || compression != 0 ||
        (bit_count != 24 && bit_count != 32))
        return 0;
    if (height_i < 0) {
        if (height_i == INT32_MIN)
            return 0;
        top_down = 1;
        height_i = -height_i;
    }
    width = (uint32_t)width_i;
    height = (uint32_t)height_i;
    if (!gltf_checked_mul_size((size_t)width, (size_t)bit_count, &row_stride))
        return 0;
    if (!gltf_checked_add_size(row_stride, 31u, &row_stride))
        return 0;
    row_stride = (row_stride / 32u) * 4u;
    if (!gltf_checked_mul_size(row_stride, (size_t)height, &image_rows))
        return 0;
    if ((size_t)pixel_offset > len || image_rows > len - (size_t)pixel_offset)
        return 0;
    if (!gltf_checked_mul_size((size_t)width, (size_t)height, &pixel_bytes) ||
        !gltf_checked_mul_size(pixel_bytes, 4u, &pixel_bytes) ||
        !gltf_checked_add_size(12u, pixel_bytes, &blob_len))
        return 0;
    blob = (uint8_t *)malloc(blob_len);
    if (!blob)
        return 0;
    blob[0] = 'V';
    blob[1] = 'G';
    blob[2] = 'R';
    blob[3] = 'A';
    gltf_write_u32_le(blob + 4, width);
    gltf_write_u32_le(blob + 8, height);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t src_y = top_down ? y : (height - 1u - y);
        const uint8_t *row = data + (size_t)pixel_offset + (size_t)src_y * row_stride;
        uint8_t *dst = blob + 12u + (size_t)y * (size_t)width * 4u;
        for (uint32_t x = 0; x < width; ++x) {
            if (bit_count == 32) {
                const uint8_t *sp = row + (size_t)x * 4u;
                dst[x * 4u + 0u] = sp[2];
                dst[x * 4u + 1u] = sp[1];
                dst[x * 4u + 2u] = sp[0];
                dst[x * 4u + 3u] = sp[3];
            } else {
                const uint8_t *sp = row + (size_t)x * 3u;
                dst[x * 4u + 0u] = sp[2];
                dst[x * 4u + 1u] = sp[1];
                dst[x * 4u + 2u] = sp[0];
                dst[x * 4u + 3u] = 0xFFu;
            }
        }
    }
    *out_blob = blob;
    *out_len = blob_len;
    return 1;
}

/// @brief Pack a 0xRRGGBBAA pixel array into an owned "VGRA" RGBA blob (overflow-checked).
/// @details Shared sink for the PNG/JPEG/GIF decoders, which all emit packed 32-bit pixels.
/// @param pixels Source pixel array in `0xRRGGBBAA` order.
/// @param width_i Positive image width.
/// @param height_i Positive image height.
/// @param out_blob Receives the owned `VGRA` blob on success and is cleared on failure.
/// @param out_len Receives the total blob byte length and is cleared on failure.
/// @return Non-zero on success; zero for invalid dimensions, overflow, or allocation failure.
static int gltf_rgba32_to_rgba_blob(const uint32_t *pixels,
                                    int64_t width_i,
                                    int64_t height_i,
                                    uint8_t **out_blob,
                                    size_t *out_len) {
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    size_t pixel_bytes;
    size_t blob_len;
    uint8_t *blob;
    if (out_blob)
        *out_blob = NULL;
    if (out_len)
        *out_len = 0;
    if (!pixels || !out_blob || !out_len || width_i <= 0 || height_i <= 0 ||
        (uint64_t)width_i > UINT32_MAX || (uint64_t)height_i > UINT32_MAX)
        return 0;
    width = (uint32_t)width_i;
    height = (uint32_t)height_i;
    if (!gltf_checked_mul_size((size_t)width, (size_t)height, &pixel_count) ||
        !gltf_checked_mul_size(pixel_count, 4u, &pixel_bytes) ||
        !gltf_checked_add_size(12u, pixel_bytes, &blob_len))
        return 0;
    blob = (uint8_t *)malloc(blob_len);
    if (!blob)
        return 0;
    blob[0] = 'V';
    blob[1] = 'G';
    blob[2] = 'R';
    blob[3] = 'A';
    gltf_write_u32_le(blob + 4, width);
    gltf_write_u32_le(blob + 8, height);
    for (size_t i = 0; i < pixel_count; ++i) {
        uint32_t rgba = pixels[i];
        uint8_t *dst = blob + 12u + i * 4u;
        dst[0] = (uint8_t)((rgba >> 24) & 0xFFu);
        dst[1] = (uint8_t)((rgba >> 16) & 0xFFu);
        dst[2] = (uint8_t)((rgba >> 8) & 0xFFu);
        dst[3] = (uint8_t)(rgba & 0xFFu);
    }
    *out_blob = blob;
    *out_len = blob_len;
    return 1;
}

/// @brief Decode an encoded image payload (PNG/BMP/JPEG/GIF) into an owned "VGRA" RGBA blob.
/// @details Dispatches on @p mime_or_name to the matching codec; PNG/JPEG/GIF go through the
///          shared rt_*_decode helpers then gltf_rgba32_to_rgba_blob, BMP uses the inline decoder.
/// @param mime_or_name MIME declaration or filename selecting the decoder.
/// @param data Encoded image bytes.
/// @param data_len Number of readable bytes at @p data.
/// @param out_blob Receives an owned `VGRA` blob on success and is cleared on failure.
/// @param out_len Receives the complete blob length and is cleared on failure.
/// @return 1 on success, 0 for an unsupported format or any decode failure.
static int gltf_decode_image_payload_to_rgba_blob(const char *mime_or_name,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  uint8_t **out_blob,
                                                  size_t *out_len) {
    if (out_blob)
        *out_blob = NULL;
    if (out_len)
        *out_len = 0;
    if (!mime_or_name || !data || data_len == 0 || !out_blob || !out_len)
        return 0;
    if (gltf_preload_image_is_png(mime_or_name)) {
        uint32_t *rgba32 = NULL;
        int64_t width = 0;
        int64_t height = 0;
        int ok = 0;
        if (rt_png_decode_buffer_rgba32(data, data_len, &rgba32, &width, &height))
            ok = gltf_rgba32_to_rgba_blob(rgba32, width, height, out_blob, out_len);
        free(rgba32);
        return ok;
    }
    if (gltf_preload_image_is_bmp(mime_or_name))
        return gltf_decode_bmp_to_rgba_blob(data, data_len, out_blob, out_len);
    if (gltf_preload_image_is_jpeg(mime_or_name)) {
        uint32_t *rgba32 = NULL;
        int64_t width = 0;
        int64_t height = 0;
        int ok = 0;
        if (rt_jpeg_decode_buffer_rgba32(data, data_len, &rgba32, &width, &height))
            ok = gltf_rgba32_to_rgba_blob(rgba32, width, height, out_blob, out_len);
        free(rgba32);
        return ok;
    }
    if (gltf_preload_image_is_gif(mime_or_name)) {
        uint32_t *rgba32 = NULL;
        int width = 0;
        int height = 0;
        int ok = 0;
        if (rt_gif_decode_memory_first_rgba32(data, data_len, &rgba32, &width, &height))
            ok = gltf_rgba32_to_rgba_blob(
                rgba32, (int64_t)width, (int64_t)height, out_blob, out_len);
        free(rgba32);
        return ok;
    }
    return 0;
}

/// @brief Build a runtime Pixels object from a "VGRA" RGBA blob (inverse of the blob encoders).
/// @details Validates the magic and header, bounds-checks the payload, then packs the RGBA
///          bytes into the Pixels 0xRRGGBBAA words and marks the surface dirty.
/// @param blob `VGRA` staging blob to decode; ownership remains with the caller.
/// @param len Number of readable bytes at @p blob.
/// @return New Pixels handle, or NULL on a malformed blob or allocation failure.
static void *gltf_pixels_from_rgba_blob(uint8_t *blob, size_t len) {
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    size_t pixel_bytes;
    rt_pixels_impl *pixels;
    const uint8_t *src;
    if (!blob || len < 12u)
        return NULL;
    if (blob[0] != 'V' || blob[1] != 'G' || blob[2] != 'R' || blob[3] != 'A')
        return NULL;
    width = gltf_read_u32_le(blob + 4);
    height = gltf_read_u32_le(blob + 8);
    if (width == 0 || height == 0)
        return NULL;
    if (!gltf_checked_mul_size((size_t)width, (size_t)height, &pixel_count) ||
        !gltf_checked_mul_size(pixel_count, 4u, &pixel_bytes) || pixel_bytes > len - 12u)
        return NULL;
    pixels = pixels_alloc((int64_t)width, (int64_t)height);
    if (!pixels)
        return NULL;
    src = blob + 12u;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t *sp = src + i * 4u;
        pixels->data[i] = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) |
                          ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
    }
    if (pixel_count > 0)
        pixels_touch(pixels);
    return pixels;
}

/// @brief Read a non-negative integer field from a JSON object as a `size_t`.
/// @details Calls `jint` with the supplied default, then rejects negative values (which indicate
///   a malformed glTF or missing field) by returning 0. Used for `byteOffset`, `byteLength`,
///   and `byteStride` fields where negative values would cause pointer arithmetic to underflow.
/// @param obj  JSON object to read from.
/// @param key  Field name.
/// @param def  Default `size_t` value when the field is absent.
/// @param out  Receives the validated value; not written on failure.
/// @return 1 on success, 0 if the value is negative or @p out is NULL.
static int gltf_nonnegative_size(void *obj, const char *key, size_t def, size_t *out) {
    int64_t raw;
    if (!out)
        return 0;
    raw = jint(obj, key, (int64_t)def);
    if (raw < 0)
        return 0;
    *out = (size_t)raw;
    return 1;
}

/// @brief Sanitize a decoded glTF resource URI into a safe relative path in @p out.
/// @details Path-traversal guard for external buffers/images: rejects absolute paths,
///   Windows drive letters (`X:`), URI scheme separators, forbids any `..` segment,
///   and collapses `.` and redundant separators, emitting forward-slash-joined segments.
/// @param decoded_uri Percent-decoded URI path to normalize.
/// @param out Destination buffer for the safe relative path.
/// @param out_cap Capacity of @p out including its terminator.
/// @return Non-zero if a non-empty safe path was written; 0 (with out[0]='\0') if the URI
///   is empty, unsafe, or would overflow @p out_cap.
static int gltf_normalize_relative_uri(const char *decoded_uri, char *out, size_t out_cap) {
    const char *p;
    size_t out_len = 0;
    int wrote_segment = 0;
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!decoded_uri || decoded_uri[0] == '\0')
        return 0;
    if (decoded_uri[0] == '/' || decoded_uri[0] == '\\')
        return 0;
    if (strchr(decoded_uri, ':') || strstr(decoded_uri, "://"))
        return 0;

    p = decoded_uri;
    while (*p) {
        const char *seg;
        size_t seg_len;
        while (*p == '/' || *p == '\\')
            p++;
        seg = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        seg_len = (size_t)(p - seg);
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.'))
            continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.')
            return 0;
        if (wrote_segment) {
            if (out_len + 1 >= out_cap)
                return 0;
            out[out_len++] = '/';
        }
        if (seg_len >= out_cap || out_len > out_cap - 1 - seg_len)
            return 0;
        memcpy(out + out_len, seg, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
        wrote_segment = 1;
    }
    return wrote_segment;
}

/// @brief Canonical base64 alphabet indexed by decoded six-bit value.
static const char gltf_base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Value of a hex digit (0-15), or -1 if @p ch is not a hex digit. Used for %XX URI escapes.
/// @param ch Candidate ASCII hexadecimal digit.
/// @return Numeric digit value from zero through fifteen, or -1 when invalid.
static int gltf_data_uri_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/// @brief ASCII whitespace test (space, tab, CR, LF, form-feed, vertical-tab); locale-independent.
/// @param ch Character to classify.
/// @return Non-zero for one of the six ASCII whitespace characters.
static int gltf_ascii_isspace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

/// @brief Case-insensitive compare of the first @p len bytes of @p a and @p b (ASCII only).
/// @param a First byte sequence.
/// @param b Second byte sequence.
/// @param len Number of bytes to compare.
/// @return 1 if equal ignoring ASCII case, 0 otherwise.
static int gltf_ascii_ieq_n(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return 1;
}

/// @brief Whether a data-URI metadata segment contains a ";base64" token.
/// @details Scans the semicolon-separated parameters between the MIME type and the comma,
///          matching the literal "base64" parameter case-insensitively.
/// @param meta Metadata bytes following `data:` and preceding the comma.
/// @param len Number of metadata bytes.
/// @return Non-zero when a standalone case-insensitive `base64` parameter is present.
static int gltf_data_uri_meta_has_base64(const char *meta, size_t len) {
    size_t start = 0;
    while (start < len) {
        size_t end = start;
        while (end < len && meta[end] != ';')
            end++;
        if (end > start && end - start == 6 && gltf_ascii_ieq_n(meta + start, "base64", 6))
            return 1;
        start = end + 1;
    }
    return 0;
}

/// @brief Validate an optional MIME type from a glTF data URI metadata prefix.
/// @details glTF data URIs are expected to carry binary base64 payloads. When a MIME type is
///          present before the first `;` or `,`, require the conservative `type/subtype` token
///          shape and reject whitespace/control characters so malformed metadata does not get
///          silently treated as opaque binary.
/// @param mime MIME-type bytes without parameters.
/// @param len Number of bytes in @p mime.
/// @return Non-zero when the MIME type is empty or a valid ASCII media type token.
static int gltf_data_uri_mime_is_valid(const char *mime, size_t len) {
    int saw_slash = 0;
    if (!mime)
        return len == 0;
    if (len == 0)
        return 1;
    if (mime[0] == '/' || mime[len - 1u] == '/')
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)mime[i];
        int token_char = isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '&' ||
                         ch == '-' || ch == '^' || ch == '_' || ch == '.' || ch == '+' || ch == '|';
        if (ch == '/') {
            if (saw_slash)
                return 0;
            saw_slash = 1;
            continue;
        }
        if (!token_char)
            return 0;
    }
    return saw_slash;
}

/// @brief Percent-decode (%XX) a URI string into a freshly allocated byte buffer (caller frees).
/// @details Decodes %XX escapes to bytes and copies all other characters verbatim. Malformed
///          escapes reject the URI instead of leaving a literal `%` in the payload. The buffer is
///          sized to the input length (an upper bound, since decoding only shrinks).
/// @param src Encoded source bytes.
/// @param len Number of source bytes to decode.
/// @param out_data Receives the heap-allocated decoded byte buffer.
/// @param out_len Receives the number of decoded bytes.
/// @return 1 with @p out_data / @p out_len set, or 0 on bad args or allocation failure.
static int gltf_percent_decode_bytes(const char *src,
                                     size_t len,
                                     uint8_t **out_data,
                                     size_t *out_len) {
    uint8_t *decoded;
    size_t write = 0;
    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (!src || !out_data || !out_len)
        return 0;
    decoded = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!decoded)
        return 0;
    for (size_t read = 0; read < len; ++read) {
        if (src[read] == '%') {
            int hi;
            int lo;
            if (read + 2 >= len) {
                free(decoded);
                return 0;
            }
            hi = gltf_data_uri_hex_digit(src[read + 1]);
            lo = gltf_data_uri_hex_digit(src[read + 2]);
            if (hi < 0 || lo < 0) {
                free(decoded);
                return 0;
            }
            decoded[write++] = (uint8_t)((hi << 4) | lo);
            read += 2;
            continue;
        }
        decoded[write++] = (uint8_t)src[read];
    }
    *out_data = decoded;
    *out_len = write;
    return 1;
}

/// @brief Decode a single base64 character to its 0-63 value (-2 for '=', -1 for invalid).
/// @param c Candidate base64 digit or padding character.
/// @return Six-bit digit value, -2 for padding, or -1 for an invalid character.
static int gltf_base64_digit_value(char c) {
    const char *p = strchr(gltf_base64_chars, c);
    if (p)
        return (int)(p - gltf_base64_chars);
    if (c == '=')
        return -2;
    return -1;
}

/// @brief Decode a base64 string of `len` characters into raw bytes (caller `free`s).
/// @details ASCII whitespace is ignored, while padding is accepted only in the final quartet.
/// @param data Base64 text, not necessarily NUL-terminated.
/// @param len Number of source characters.
/// @param out_len Optional destination for the decoded byte count; cleared before decoding.
/// @return Heap-allocated decoded bytes, including a unique allocation for empty input, or NULL
///         for malformed data or allocation failure.
static uint8_t *gltf_base64_decode(const char *data, size_t len, size_t *out_len) {
    char *compact = NULL;
    const char *src;
    size_t compact_len = 0;
    if (out_len)
        *out_len = 0;
    if (!data)
        return NULL;
    for (size_t i = 0; i < len; ++i) {
        if (!gltf_ascii_isspace(data[i]))
            compact_len++;
    }
    if (compact_len != len) {
        compact = (char *)malloc(compact_len > 0 ? compact_len : 1);
        if (!compact)
            return NULL;
        compact_len = 0;
        for (size_t i = 0; i < len; ++i) {
            if (!gltf_ascii_isspace(data[i]))
                compact[compact_len++] = data[i];
        }
        src = compact;
    } else {
        src = data;
    }
    len = compact_len;
    if (len == 0) {
        uint8_t *empty = (uint8_t *)malloc(1);
        if (out_len)
            *out_len = 0;
        free(compact);
        return empty;
    }
    if (len % 4 != 0) {
        free(compact);
        return NULL;
    }

    size_t olen = (len / 4) * 3;
    if (src[len - 1] == '=')
        olen--;
    if (len > 1 && src[len - 2] == '=')
        olen--;

    uint8_t *output = (uint8_t *)malloc(olen > 0 ? olen : 1);
    if (!output) {
        free(compact);
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        int a = gltf_base64_digit_value(src[i++]);
        int b = gltf_base64_digit_value(src[i++]);
        int c = gltf_base64_digit_value(src[i++]);
        int d = gltf_base64_digit_value(src[i++]);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            free(output);
            free(compact);
            return NULL;
        }
        if ((c == -2 && d != -2) || ((c == -2 || d == -2) && i < len)) {
            free(output);
            free(compact);
            return NULL;
        }
        if (c == -2)
            c = 0;
        if (d == -2)
            d = 0;
        {
            uint32_t triple =
                ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
            if (j < olen)
                output[j++] = (uint8_t)((triple >> 16) & 0xFF);
            if (j < olen)
                output[j++] = (uint8_t)((triple >> 8) & 0xFF);
            if (j < olen)
                output[j++] = (uint8_t)(triple & 0xFF);
        }
    }

    if (out_len)
        *out_len = olen;
    free(compact);
    return output;
}

/// @brief Parse a `data:[<mediatype>][;base64],<data>` URI inline-embedded in glTF JSON.
///
/// Recognises the base64 marker and strips the MIME-type prefix.
/// Used for inline texture / buffer data so a single .gltf file
/// can be self-contained without separate .bin sidecars.
/// @param uri NUL-terminated data URI to parse.
/// @param mime_buf Optional destination for the media type.
/// @param mime_buf_cap Capacity of @p mime_buf including its terminator.
/// @param out_data Receives the heap-allocated decoded payload.
/// @param out_len Receives the decoded payload length.
/// @return 1 on success (writes decoded bytes + MIME type), 0 on malformed URI.
static int gltf_parse_data_uri(
    const char *uri, char *mime_buf, size_t mime_buf_cap, uint8_t **out_data, size_t *out_len) {
    const char *comma;
    const char *payload;
    uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0;
    size_t mime_len = 0;
    int is_base64 = 0;
    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (mime_buf && mime_buf_cap > 0)
        mime_buf[0] = '\0';
    if (!out_data || !out_len)
        return 0;
    if (!uri || strncmp(uri, "data:", 5) != 0)
        return 0;
    comma = strchr(uri, ',');
    if (!comma)
        return 0;
    payload = comma + 1;

    {
        const char *meta = uri + 5;
        size_t meta_len = (size_t)(comma - meta);
        const char *semi = memchr(meta, ';', meta_len);
        if (semi) {
            mime_len = (size_t)(semi - meta);
            is_base64 = gltf_data_uri_meta_has_base64(meta, meta_len);
        } else {
            mime_len = meta_len;
        }
        if (!is_base64 || !gltf_data_uri_mime_is_valid(meta, mime_len))
            return 0;
        if (mime_buf && mime_buf_cap > 0 && mime_len > 0) {
            if (mime_len >= mime_buf_cap)
                mime_len = mime_buf_cap - 1;
            memcpy(mime_buf, meta, mime_len);
            mime_buf[mime_len] = '\0';
        }
    }

    if (!gltf_percent_decode_bytes(
            payload, strlen(payload), &decoded_payload, &decoded_payload_len))
        return 0;
    *out_data = gltf_base64_decode((const char *)decoded_payload, decoded_payload_len, out_len);
    free(decoded_payload);
    return *out_data != NULL;
}

/// @brief Copy a data-URI's MIME type (the part between "data:" and the first ';' or ',') into
///        @p mime_buf, NUL-terminated and truncated to @p mime_buf_cap. Empties the buffer if
///        absent.
/// @param uri Data URI whose metadata prefix is inspected.
/// @param mime_buf Destination buffer, cleared when no type is available.
/// @param mime_buf_cap Capacity of @p mime_buf including its terminator.
static void gltf_data_uri_copy_mime(const char *uri, char *mime_buf, size_t mime_buf_cap) {
    const char *comma;
    const char *meta;
    const char *semi;
    size_t meta_len;
    size_t mime_len;
    if (mime_buf && mime_buf_cap > 0)
        mime_buf[0] = '\0';
    if (!uri || strncmp(uri, "data:", 5) != 0 || !mime_buf || mime_buf_cap == 0)
        return;
    comma = strchr(uri, ',');
    if (!comma)
        return;
    meta = uri + 5;
    meta_len = (size_t)(comma - meta);
    semi = memchr(meta, ';', meta_len);
    mime_len = semi ? (size_t)(semi - meta) : meta_len;
    if (mime_len == 0)
        return;
    if (mime_len >= mime_buf_cap)
        mime_len = mime_buf_cap - 1u;
    memcpy(mime_buf, meta, mime_len);
    mime_buf[mime_len] = '\0';
}

/// @brief EXT_meshopt_compression: decoded shadow buffers, indexed by bufferView.
/// @details Built once per load (after buffers materialize, before accessors resolve)
///          and consulted by gltf_get_buffer_view_data so every downstream consumer
///          (accessors, sparse tables, images) reads decompressed bytes transparently.
///          The active table is thread-scoped for the duration of one load, mirroring
///          the rt_gltf_load_options pattern; preload workers never install one.
typedef struct {
    /// Owned decoded bytes indexed by bufferView, or NULL for uncompressed views.
    uint8_t **view_data; /* per bufferView: owned decoded bytes, or NULL (not compressed) */
    /// Decoded byte length parallel to @ref view_data.
    size_t *view_len;    /* decoded byte length (byteStride * count) */
    /// Number of slots in the parallel arrays.
    int64_t view_count;
} gltf_meshopt_table_t;

/// @brief Thread-scoped decoded meshopt table consulted during the active import.
static RT_THREAD_LOCAL const gltf_meshopt_table_t *g_gltf_meshopt_views = NULL;

/// @brief Resolve a buffer-view index to its `(data, length)` slice into the underlying buffer.
///
/// glTF separates buffers (raw bytes) from bufferViews (named
/// offset+length slices). Most accessors reference data through a
/// bufferView, so this helper centralises the indirection.
/// @param root glTF JSON root containing the `bufferViews` array.
/// @param view_idx BufferView index to resolve.
/// @param buffers Materialized parent-buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param out_len Optional destination for the resolved slice length.
/// @return Borrowed first byte of the view, a stable sentinel for an empty null-backed view, or
///         NULL when the view is invalid or out of bounds.
static const uint8_t *gltf_get_buffer_view_data(
    void *root, int64_t view_idx, gltf_buffer_t *buffers, int buf_count, size_t *out_len) {
    static const uint8_t empty_buffer_view_byte = 0;
    void *views = jarr(root, "bufferViews");
    size_t byte_offset;
    size_t byte_length;
    size_t byte_end;
    if (!views || view_idx < 0 || view_idx >= jarr_len(views))
        return NULL;
    void *view = rt_seq_get(views, view_idx);
    if (!view)
        return NULL;
    /* Compressed views bypass the parent buffer entirely — the parent may be a
     * data-less placeholder fallback per the EXT_meshopt_compression spec. */
    if (g_gltf_meshopt_views && view_idx < g_gltf_meshopt_views->view_count &&
        g_gltf_meshopt_views->view_data[view_idx]) {
        if (out_len)
            *out_len = g_gltf_meshopt_views->view_len[view_idx];
        return g_gltf_meshopt_views->view_data[view_idx];
    }
    int buf_idx = (int)jint(view, "buffer", -1);
    if (buf_idx < 0 || buf_idx >= buf_count)
        return NULL;
    if (!gltf_nonnegative_size(view, "byteOffset", 0, &byte_offset) ||
        !gltf_nonnegative_size(view, "byteLength", 0, &byte_length))
        return NULL;
    if (!gltf_checked_add_size(byte_offset, byte_length, &byte_end))
        return NULL;
    if (byte_end > buffers[buf_idx].len)
        return NULL;
    if (out_len)
        *out_len = byte_length;
    if (byte_length == 0)
        return buffers[buf_idx].data ? buffers[buf_idx].data + byte_offset
                                     : &empty_buffer_view_byte;
    if (!buffers[buf_idx].data)
        return NULL;
    return buffers[buf_idx].data + byte_offset;
}
