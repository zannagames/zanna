//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_textureasset3d_core.inc  (#included from
// rt_textureasset3d.c; not a standalone translation unit) Purpose: TextureAsset3D format info,
// container lifecycle, mip residency, alpha, and RGBA8 fallback
//===----------------------------------------------------------------------===//
/// @file
/// @brief Defines TextureAsset3D format metadata, ownership, residency, and core fallbacks.
///
/// This implementation fragment materializes the authoritative format table,
/// owns canonical source and container bytes, maintains parallel mip metadata,
/// decoded-Pixels, and native-view arrays, and implements transactional mip
/// residency. It also supplies common size, alpha, and raw-RGBA8 helpers used by
/// the format-specific decoder fragments included by `rt_textureasset3d.c`.
///
/// @note This file is included by `rt_textureasset3d.c` and is not a standalone
///       translation unit.

/// @brief Internal CPU fallback implementation selected by the authoritative format table.
typedef enum {
    /// No CPU fallback decoder is available.
    TEXTUREASSET3D_DECODER_NONE = 0,
    /// Copy uncompressed RGBA8 texels.
    TEXTUREASSET3D_DECODER_RGBA8,
    /// Decode BC1/DXT1 blocks.
    TEXTUREASSET3D_DECODER_BC1,
    /// Decode BC3/DXT5 blocks.
    TEXTUREASSET3D_DECODER_BC3,
    /// Decode single-channel BC4 blocks.
    TEXTUREASSET3D_DECODER_BC4,
    /// Decode two-channel BC5 blocks.
    TEXTUREASSET3D_DECODER_BC5,
    /// Decode unsigned-float BC6H blocks.
    TEXTUREASSET3D_DECODER_BC6H,
    /// Decode signed-float BC6H blocks.
    TEXTUREASSET3D_DECODER_BC6HS,
    /// Decode BC7 blocks.
    TEXTUREASSET3D_DECODER_BC7,
    /// Decode supported ETC2 RGBA8/EAC blocks.
    TEXTUREASSET3D_DECODER_ETC2,
    /// Decode supported ASTC LDR blocks.
    TEXTUREASSET3D_DECODER_ASTC,
    /// Transcode UASTC blocks to RGBA8.
    TEXTUREASSET3D_DECODER_UASTC,
} textureasset3d_decoder_kind;

/// @brief Persistent action used to reconstruct an evicted decoded mip.
typedef enum {
    TEXTUREASSET3D_FALLBACK_NONE = 0,    ///< Mip intentionally remains native/source-only.
    TEXTUREASSET3D_FALLBACK_DECODE = 1,  ///< Re-run the row-selected decoder from backing bytes.
    TEXTUREASSET3D_FALLBACK_CHECKER = 2, ///< Recreate the permissive checker substitution.
} textureasset3d_fallback_kind;

/// @brief Decoded properties of an authoritative Vulkan texture-format row.
/// @details In addition to block geometry this carries the CPU quality/decoder and native backend
///          mapping, making the row the only source of format behavior used by parser and renderer.
typedef struct {
    /// Inclusive first Vulkan format number represented by this row.
    uint32_t vk_format_first;
    /// Inclusive last Vulkan format number represented by this row.
    uint32_t vk_format_last;
    /// Stable runtime-facing format name.
    const char *name;
    /// Nonzero when payload data is block-compressed.
    int8_t compressed;
    /// One of the public RT_TEXTUREASSET3D_CPU_SUPPORT_* quality levels.
    int8_t cpu_support;
    /// Width in texels covered by one encoded block.
    int32_t block_width;
    /// Height in texels covered by one encoded block.
    int32_t block_height;
    /// Number of encoded bytes in one block.
    int32_t block_bytes;
    /// textureasset3d_decoder_kind selected for software fallback.
    int32_t decoder_kind;
    /// Backend-independent native upload format identifier.
    int32_t native_format_id;
    /// Backend capability bit required for native upload.
    int64_t backend_capability_bit;
} textureasset3d_format_info;

#define TEXTUREASSET3D_CPU_NONE RT_TEXTUREASSET3D_CPU_SUPPORT_NONE
#define TEXTUREASSET3D_CPU_PARTIAL RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL
#define TEXTUREASSET3D_CPU_FULL RT_TEXTUREASSET3D_CPU_SUPPORT_FULL
#define TEXTUREASSET3D_DECODER_NONE_VALUE TEXTUREASSET3D_DECODER_NONE
#define TEXTUREASSET3D_DECODER_RGBA8_VALUE TEXTUREASSET3D_DECODER_RGBA8
#define TEXTUREASSET3D_DECODER_BC1_VALUE TEXTUREASSET3D_DECODER_BC1
#define TEXTUREASSET3D_DECODER_BC3_VALUE TEXTUREASSET3D_DECODER_BC3
#define TEXTUREASSET3D_DECODER_BC4_VALUE TEXTUREASSET3D_DECODER_BC4
#define TEXTUREASSET3D_DECODER_BC5_VALUE TEXTUREASSET3D_DECODER_BC5
#define TEXTUREASSET3D_DECODER_BC6H_VALUE TEXTUREASSET3D_DECODER_BC6H
#define TEXTUREASSET3D_DECODER_BC6HS_VALUE TEXTUREASSET3D_DECODER_BC6HS
#define TEXTUREASSET3D_DECODER_BC7_VALUE TEXTUREASSET3D_DECODER_BC7
#define TEXTUREASSET3D_DECODER_ETC2_VALUE TEXTUREASSET3D_DECODER_ETC2
#define TEXTUREASSET3D_DECODER_ASTC_VALUE TEXTUREASSET3D_DECODER_ASTC
#define TEXTUREASSET3D_NATIVE_NONE RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE
#define TEXTUREASSET3D_NATIVE_BC1 RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1
#define TEXTUREASSET3D_NATIVE_BC3 RT_TEXTUREASSET3D_NATIVE_FORMAT_BC3
#define TEXTUREASSET3D_NATIVE_BC4 RT_TEXTUREASSET3D_NATIVE_FORMAT_BC4
#define TEXTUREASSET3D_NATIVE_BC5 RT_TEXTUREASSET3D_NATIVE_FORMAT_BC5
#define TEXTUREASSET3D_NATIVE_BC7 RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7
#define TEXTUREASSET3D_NATIVE_ETC2 RT_TEXTUREASSET3D_NATIVE_FORMAT_ETC2
#define TEXTUREASSET3D_NATIVE_ASTC RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC
#define TEXTUREASSET3D_BACKEND_NONE RT_TEXTUREASSET3D_BACKEND_CAP_NONE
#define TEXTUREASSET3D_BACKEND_BC1 RT_TEXTUREASSET3D_BACKEND_CAP_BC1
#define TEXTUREASSET3D_BACKEND_BC3 RT_TEXTUREASSET3D_BACKEND_CAP_BC3
#define TEXTUREASSET3D_BACKEND_BC4 RT_TEXTUREASSET3D_BACKEND_CAP_BC4
#define TEXTUREASSET3D_BACKEND_BC5 RT_TEXTUREASSET3D_BACKEND_CAP_BC5
#define TEXTUREASSET3D_BACKEND_BC7 RT_TEXTUREASSET3D_BACKEND_CAP_BC7
#define TEXTUREASSET3D_BACKEND_ETC2 RT_TEXTUREASSET3D_BACKEND_CAP_ETC2
#define TEXTUREASSET3D_BACKEND_ASTC RT_TEXTUREASSET3D_BACKEND_CAP_ASTC

/// @brief Immutable runtime materialization of rt_textureasset3d_formats.def.
static const textureasset3d_format_info textureasset3d_format_capabilities[] = {
#define RT_TEXTUREASSET3D_FORMAT_ROW(                                                              \
    first, last, row_name, is_compressed, bw, bh, bb, cpu, decoder, native_id, backend_cap)        \
    {first,                                                                                        \
     last,                                                                                         \
     row_name,                                                                                     \
     is_compressed,                                                                                \
     TEXTUREASSET3D_CPU_##cpu,                                                                     \
     bw,                                                                                           \
     bh,                                                                                           \
     bb,                                                                                           \
     TEXTUREASSET3D_DECODER_##decoder##_VALUE,                                                     \
     TEXTUREASSET3D_NATIVE_##native_id,                                                            \
     TEXTUREASSET3D_BACKEND_##backend_cap},
#include "rt_textureasset3d_formats.def"
#undef RT_TEXTUREASSET3D_FORMAT_ROW
};

#undef TEXTUREASSET3D_CPU_NONE
#undef TEXTUREASSET3D_CPU_PARTIAL
#undef TEXTUREASSET3D_CPU_FULL
#undef TEXTUREASSET3D_DECODER_NONE_VALUE
#undef TEXTUREASSET3D_DECODER_RGBA8_VALUE
#undef TEXTUREASSET3D_DECODER_BC1_VALUE
#undef TEXTUREASSET3D_DECODER_BC3_VALUE
#undef TEXTUREASSET3D_DECODER_BC4_VALUE
#undef TEXTUREASSET3D_DECODER_BC5_VALUE
#undef TEXTUREASSET3D_DECODER_BC6H_VALUE
#undef TEXTUREASSET3D_DECODER_BC6HS_VALUE
#undef TEXTUREASSET3D_DECODER_BC7_VALUE
#undef TEXTUREASSET3D_DECODER_ETC2_VALUE
#undef TEXTUREASSET3D_DECODER_ASTC_VALUE
#undef TEXTUREASSET3D_NATIVE_NONE
#undef TEXTUREASSET3D_NATIVE_BC1
#undef TEXTUREASSET3D_NATIVE_BC3
#undef TEXTUREASSET3D_NATIVE_BC4
#undef TEXTUREASSET3D_NATIVE_BC5
#undef TEXTUREASSET3D_NATIVE_BC7
#undef TEXTUREASSET3D_NATIVE_ETC2
#undef TEXTUREASSET3D_NATIVE_ASTC
#undef TEXTUREASSET3D_BACKEND_NONE
#undef TEXTUREASSET3D_BACKEND_BC1
#undef TEXTUREASSET3D_BACKEND_BC3
#undef TEXTUREASSET3D_BACKEND_BC4
#undef TEXTUREASSET3D_BACKEND_BC5
#undef TEXTUREASSET3D_BACKEND_BC7
#undef TEXTUREASSET3D_BACKEND_ETC2
#undef TEXTUREASSET3D_BACKEND_ASTC

/// @brief One mip level's location in the KTX2 byte stream (offset/length), its
///        uncompressed size, and pixel dimensions.
typedef struct {
    /// Byte offset of the encoded level within canonical backing.
    uint64_t offset;
    /// Stored byte length of the encoded level.
    uint64_t length;
    /// Expected byte length after supercompression is removed.
    uint64_t uncompressed_length;
    /// Level width in texels.
    uint32_t width;
    /// Level height in texels.
    uint32_t height;
} textureasset3d_mip;

/// @brief TextureAsset3D state: the mip table plus parallel arrays of decoded
///        Pixels mips and retained native payloads, format/block metadata, the
///        currently-resident mip window, and the cache identity/revision pair.
typedef struct {
    /// Runtime object vtable slot managed by the object system.
    void *vptr;
    /// Borrowed alias of the first currently resident decoded mip.
    void *pixels;
    /// Parallel array of owned decoded Pixels fallbacks, with nullable slots.
    void **mip_pixels;
    /// Parallel array of borrowed native-payload views into @ref source_backing.
    uint8_t **mip_payloads;
    /// Owned immutable canonical bytes used for native upload and reconstruction.
    uint8_t *source_backing;
    /// Owned exact original image/container bytes retained for lossless rebaking.
    uint8_t *container_backing;
    /// Parallel textureasset3d_fallback_kind values for rebuilding evicted mips.
    uint8_t *mip_fallback_kinds;
    /// Owned metadata table describing every allocated mip slot.
    textureasset3d_mip *mips;
    /// Base-level width in texels.
    int64_t width;
    /// Base-level height in texels.
    int64_t height;
    /// Logical number of loaded mip levels.
    int64_t mip_count;
    /// Allocated length of all parallel mip arrays.
    int64_t mip_capacity;
    /// First mip in the currently resident window.
    int64_t resident_mip_start;
    /// Number of consecutive mips in the resident window.
    int64_t resident_mip_count;
    /// Saturating encoded-or-decoded byte estimate for the resident window.
    int64_t resident_bytes;
    /// Saturating total retained backing and decoded-Pixels byte estimate.
    int64_t retained_bytes;
    /// Byte length of @ref source_backing.
    uint64_t source_backing_bytes;
    /// Byte length of @ref container_backing.
    uint64_t container_backing_bytes;
    /// Pixels generation at which the retained source container was valid.
    uint64_t container_pixels_generation;
    /// Base Pixels width associated with the retained source container.
    int64_t container_pixels_width;
    /// Base Pixels height associated with the retained source container.
    int64_t container_pixels_height;
    /// Normalized static name of the retained source container, or NULL.
    const char *container_kind;
    /// Stable runtime-facing decoded/native texture format name.
    const char *format;
    /// Static explanation of any degraded fallback behavior.
    const char *degraded_reason;
    /// Nonzero when the source format is block-compressed.
    int8_t compressed;
    /// Nonzero when only a degraded CPU representation is available.
    int8_t degraded;
    /// Native block width in texels.
    int32_t block_width;
    /// Native block height in texels.
    int32_t block_height;
    /// Native encoded byte size of one block.
    int32_t block_bytes;
    /// textureasset3d_decoder_kind used to reconstruct decoded mips.
    int32_t decoder_kind;
    /// Backend-independent native upload format identifier.
    int32_t native_format_id;
    /// Backend capability bit required for native upload.
    int64_t backend_capability_bit;
    /// Process-unique stable portion of the backend texture-cache key.
    uint64_t cache_identity;
    /// Nonzero revision invalidating cached native texture resources.
    uint64_t native_revision;
    /// Nonzero when @ref has_alpha_texels is exact for all loaded mips.
    int8_t alpha_metadata_known;
    /// Nonzero when a decoded texel with alpha below 255 was observed.
    int8_t has_alpha_texels;
    /// Nonzero when source-container validity follows base Pixels mutations.
    int8_t container_tracks_pixels;
} rt_textureasset3d;

static const uint8_t ktx2_identifier[12] = {
    0xAB,
    0x4B,
    0x54,
    0x58,
    0x20,
    0x32,
    0x30,
    0xBB,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
};

static uint64_t g_next_textureasset3d_cache_identity = 1;
static volatile uint64_t g_textureasset3d_supercompression_final_bytes = 0;
static volatile uint64_t g_textureasset3d_supercompression_peak_bytes = 0;

/// @brief Reconstruct one decoded mip from immutable canonical backing.
/// @details Defined after the format-specific codec includes. The forward declaration lets the
///          residency transaction remain in the core lifecycle section while selecting decoders
///          compiled later in the enclosing translation unit.
/// @param[in] asset Texture asset owning the canonical backing and mip metadata.
/// @param[in] mip Zero-based mip level to reconstruct.
/// @return A newly allocated Pixels fallback, or NULL when reconstruction is unavailable or fails.
static void *textureasset3d_decode_mip_from_backing(const rt_textureasset3d *asset, int64_t mip);
/// @brief Allocate the conspicuous checkerboard used when compressed fallback decoding fails.
/// @return A newly allocated Pixels object, or NULL on allocation failure.
static void *textureasset3d_make_decode_failure_checker(void);
/// @brief Recalculate retained backing and decoded-Pixels bytes for an asset.
/// @param[in] asset Texture asset whose current allocations are measured.
/// @return Saturating retained-byte count in the range 0 through INT64_MAX.
static int64_t textureasset3d_compute_retained_bytes(rt_textureasset3d *asset);

/// @brief Atomically allocate the next process-unique, nonzero cache identity.
/// @details 0 is reserved as "no key", so the loop retries past a wrapped-to-zero value.
/// @return A nonzero identity suitable for the stable portion of a backend cache key.
static uint64_t textureasset3d_next_cache_identity(void) {
    uint64_t id;
    do {
        id = rt_atomic_fetch_add_u64(
            &g_next_textureasset3d_cache_identity, UINT64_C(1), __ATOMIC_RELAXED);
    } while (id == 0);
    return id;
}

/// @brief Bump the native-payload revision so the backend cache key changes.
/// @details Wraps past 0 (a reserved value) to keep every revision nonzero.
/// @param[in,out] asset Texture asset whose native revision is incremented.
static void textureasset3d_bump_native_revision(rt_textureasset3d *asset) {
    if (!asset)
        return;
    if (asset->native_revision == UINT64_MAX)
        asset->native_revision = 1;
    else
        asset->native_revision++;
    if (asset->native_revision == 0)
        asset->native_revision = 1;
}

/// @brief Validate @p obj as a TextureAsset3D handle and return its typed pointer (NULL on
/// mismatch).
/// @param[in] obj Opaque runtime object to validate.
/// @return The typed asset pointer, or NULL when @p obj has the wrong runtime class.
static rt_textureasset3d *textureasset3d_checked(void *obj) {
    return (rt_textureasset3d *)rt_g3d_checked_or_null(obj, RT_G3D_TEXTUREASSET3D_CLASS_ID);
}

/// @brief Number of mip slots allocated for the asset's parallel mip arrays.
/// @param[in] asset Texture asset to inspect.
/// @return The nonnegative capacity clamped to TEXTUREASSET3D_MAX_MIP_COUNT.
static int64_t textureasset3d_allocated_mip_count(const rt_textureasset3d *asset) {
    if (!asset || asset->mip_capacity <= 0)
        return 0;
    if (asset->mip_capacity > (int64_t)TEXTUREASSET3D_MAX_MIP_COUNT)
        return (int64_t)TEXTUREASSET3D_MAX_MIP_COUNT;
    return asset->mip_capacity;
}

/// @brief Mip count safe for public access, clamped to the allocated mip table.
/// @param[in] asset Texture asset to inspect.
/// @return The validated logical mip count, or zero when metadata is absent or invalid.
static int64_t textureasset3d_safe_mip_count(const rt_textureasset3d *asset) {
    int64_t allocated = textureasset3d_allocated_mip_count(asset);
    if (!asset || !asset->mips || asset->mip_count <= 0 || allocated <= 0)
        return 0;
    return asset->mip_count < allocated ? asset->mip_count : allocated;
}

/// @brief Release a GC reference held in @p *slot if this is its last drop, then NULL it.
/// @param[in,out] slot Address of the owned reference slot to release and clear.
static void textureasset3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a decoded mip fallback only if it is still a live Pixels object.
/// @param[in,out] slot Address of a decoded-mip slot to validate, release, and clear.
static void textureasset3d_release_pixels_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_pixels_checked_impl_or_null(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    textureasset3d_release_ref(slot);
}

/// @brief GC finalizer for decoded mips, borrowed native views, canonical backing, and metadata.
/// @details Native mip pointers borrow slices of @c source_backing, so only the pointer table is
///          freed. The one backing allocation is then released exactly once.
/// @param[in,out] obj TextureAsset3D object whose owned resources are released.
static void textureasset3d_finalize(void *obj) {
    rt_textureasset3d *asset = (rt_textureasset3d *)obj;
    int64_t allocated;
    if (!asset)
        return;
    allocated = textureasset3d_allocated_mip_count(asset);
    asset->pixels = NULL;
    if (asset->mip_pixels) {
        for (int64_t i = 0; i < allocated; i++)
            textureasset3d_release_pixels_ref(&asset->mip_pixels[i]);
        free(asset->mip_pixels);
        asset->mip_pixels = NULL;
    }
    if (asset->mip_payloads) {
        free(asset->mip_payloads);
        asset->mip_payloads = NULL;
    }
    free(asset->source_backing);
    asset->source_backing = NULL;
    asset->source_backing_bytes = 0;
    free(asset->container_backing);
    asset->container_backing = NULL;
    asset->container_backing_bytes = 0;
    asset->container_kind = NULL;
    asset->container_tracks_pixels = 0;
    free(asset->mip_fallback_kinds);
    asset->mip_fallback_kinds = NULL;
    free(asset->mips);
    asset->mips = NULL;
    asset->mip_count = 0;
    asset->mip_capacity = 0;
    asset->retained_bytes = 0;
}

/// @brief Read a little-endian uint32 from @p p (KTX2 is little-endian on all hosts).
/// @param[in] p Pointer to at least four readable bytes.
/// @return The four bytes interpreted as an unsigned little-endian integer.
static uint32_t textureasset3d_read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Read a little-endian uint64 from @p p.
/// @param[in] p Pointer to at least eight readable bytes.
/// @return The eight bytes interpreted as an unsigned little-endian integer.
static uint64_t textureasset3d_read_u64le(const uint8_t *p) {
    uint64_t lo = textureasset3d_read_u32le(p);
    uint64_t hi = textureasset3d_read_u32le(p + 4);
    return lo | (hi << 32);
}

/// @brief Map a Vulkan format value through the authoritative capability table.
/// @details Inclusive row bounds are checked without arithmetic, avoiding range-wrap hazards.
///          Unknown formats return a compressed sentinel with no geometry, decoder, or backend
///          capability, causing expected-size validation to reject their levels deterministically.
/// @param vk_format Vulkan VkFormat numeric value stored by KTX2.
/// @return A copy of the matching immutable row, or the unknown sentinel.
static textureasset3d_format_info textureasset3d_format_from_vk(uint32_t vk_format) {
    for (size_t i = 0; i < sizeof(textureasset3d_format_capabilities) /
                               sizeof(textureasset3d_format_capabilities[0]);
         i++) {
        const textureasset3d_format_info *row = &textureasset3d_format_capabilities[i];
        if (vk_format >= row->vk_format_first && vk_format <= row->vk_format_last)
            return *row;
    }
    return (textureasset3d_format_info){0,
                                        0,
                                        "unknown",
                                        1,
                                        RT_TEXTUREASSET3D_CPU_SUPPORT_NONE,
                                        0,
                                        0,
                                        0,
                                        TEXTUREASSET3D_DECODER_NONE,
                                        RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE,
                                        RT_TEXTUREASSET3D_BACKEND_CAP_NONE};
}

/// @brief Compute the exact stored bytes required for one mip in a known format.
/// @param[in] format Authoritative format row defining block geometry and byte size.
/// @param[in] width Mip width in texels.
/// @param[in] height Mip height in texels.
/// @param[out] out_bytes Destination for the complete encoded byte count.
/// @return 1 on success, or 0 for invalid geometry, dimensions, output, or overflow.
static int textureasset3d_expected_mip_bytes(const textureasset3d_format_info *format,
                                             uint32_t width,
                                             uint32_t height,
                                             uint64_t *out_bytes) {
    uint64_t blocks_x;
    uint64_t blocks_y;
    if (!format || !out_bytes || width == 0 || height == 0 || format->block_width <= 0 ||
        format->block_height <= 0 || format->block_bytes <= 0)
        return 0;
    blocks_x =
        ((uint64_t)width + (uint64_t)format->block_width - 1u) / (uint64_t)format->block_width;
    blocks_y =
        ((uint64_t)height + (uint64_t)format->block_height - 1u) / (uint64_t)format->block_height;
    if (blocks_x != 0 && blocks_y > UINT64_MAX / blocks_x)
        return 0;
    if (blocks_x * blocks_y > UINT64_MAX / (uint64_t)format->block_bytes)
        return 0;
    *out_bytes = blocks_x * blocks_y * (uint64_t)format->block_bytes;
    return 1;
}

/// @brief Compute a mip level's dimension as base >> level, clamped to a minimum of 1.
/// @param[in] base Base-level dimension.
/// @param[in] level Zero-based mip level.
/// @return The shifted dimension, with zero clamped to one.
static uint32_t textureasset3d_mip_dimension(uint32_t base, uint32_t level) {
    uint32_t value = base >> level;
    return value > 0 ? value : 1;
}

/// @brief Add a byte count to an unsigned accumulator without wrapping.
/// @param[in,out] total Accumulator to update; NULL is ignored.
/// @param[in] value Value to add.
/// @post On overflow, @p total contains UINT64_MAX.
static void textureasset3d_add_saturating_u64(uint64_t *total, uint64_t value) {
    if (!total)
        return;
    if (UINT64_MAX - *total < value)
        *total = UINT64_MAX;
    else
        *total += value;
}

/// @brief Normalize one supported exact source-container kind to immutable static storage.
/// @param[in] kind Candidate container name, including the accepted `jpg` alias.
/// @return A canonical static name, or NULL for an unsupported or NULL kind.
static const char *textureasset3d_normalize_container_kind(const char *kind) {
    if (!kind)
        return NULL;
    if (strcmp(kind, "ktx2") == 0)
        return "ktx2";
    if (strcmp(kind, "png") == 0)
        return "png";
    if (strcmp(kind, "jpeg") == 0 || strcmp(kind, "jpg") == 0)
        return "jpeg";
    if (strcmp(kind, "gif") == 0)
        return "gif";
    if (strcmp(kind, "bmp") == 0)
        return "bmp";
    return NULL;
}

/// @brief Validate that a byte span begins with the selected supported container signature.
/// @param[in] data Candidate container bytes.
/// @param[in] size Number of readable bytes at @p data.
/// @param[in] kind Canonical container kind returned by textureasset3d_normalize_container_kind().
/// @return 1 when the span has the selected format's minimum signature, otherwise 0.
static int textureasset3d_container_magic_matches(const uint8_t *data,
                                                  uint64_t size,
                                                  const char *kind) {
    static const uint8_t png_magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || !kind)
        return 0;
    if (strcmp(kind, "ktx2") == 0)
        return size >= sizeof(ktx2_identifier) &&
               memcmp(data, ktx2_identifier, sizeof(ktx2_identifier)) == 0;
    if (strcmp(kind, "png") == 0)
        return size >= sizeof(png_magic) && memcmp(data, png_magic, sizeof(png_magic)) == 0;
    if (strcmp(kind, "jpeg") == 0)
        return size >= 4 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    if (strcmp(kind, "gif") == 0)
        return size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0);
    if (strcmp(kind, "bmp") == 0)
        return size >= 14 && data[0] == 'B' && data[1] == 'M';
    return 0;
}

/// @brief Copy a validated immutable source container onto a fully constructed asset.
/// @param[in,out] asset Destination asset whose prior container copy is replaced.
/// @param[in] data Exact source-container byte span.
/// @param[in] size Number of bytes in @p data.
/// @param[in] kind Supported container kind or accepted alias.
/// @return 1 when the container is validated and attached, otherwise 0.
static int textureasset3d_attach_source_container(rt_textureasset3d *asset,
                                                  const uint8_t *data,
                                                  uint64_t size,
                                                  const char *kind) {
    const char *normalized = textureasset3d_normalize_container_kind(kind);
    uint8_t *copy;
    rt_pixels_impl *base_pixels;
    if (!asset || !normalized || !data || size == 0 || size > TEXTUREASSET3D_MAX_FILE_BYTES ||
        size > SIZE_MAX || !textureasset3d_container_magic_matches(data, size, normalized)) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "TextureAsset3D: source container kind or magic is invalid");
        return 0;
    }
    copy = (uint8_t *)malloc((size_t)size);
    if (!copy) {
        rt_trap("TextureAsset3D: source-container allocation failed");
        return 0;
    }
    memcpy(copy, data, (size_t)size);
    free(asset->container_backing);
    asset->container_backing = copy;
    asset->container_backing_bytes = size;
    asset->container_kind = normalized;
    asset->container_tracks_pixels = 0;
    base_pixels = asset->mip_pixels && textureasset3d_safe_mip_count(asset) > 0
                      ? rt_pixels_checked_impl_or_null(asset->mip_pixels[0])
                      : NULL;
    if (base_pixels && base_pixels->width == asset->width && base_pixels->height == asset->height) {
        asset->container_tracks_pixels = 1;
        asset->container_pixels_generation = base_pixels->generation;
        asset->container_pixels_width = base_pixels->width;
        asset->container_pixels_height = base_pixels->height;
    }
    asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    return 1;
}

/// @brief Estimate the backing allocation occupied by one decoded Pixels mip.
/// @param[in] pixels Opaque Pixels object to validate and measure.
/// @return Width times height times four, zero for invalid dimensions, or UINT64_MAX on overflow.
static uint64_t textureasset3d_pixels_allocation_bytes(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl_or_null(pixels);
    uint64_t pixel_count;
    uint64_t data_bytes;
    uint64_t total;
    if (!p || p->width <= 0 || p->height <= 0)
        return 0;
    if ((uint64_t)p->width > UINT64_MAX / (uint64_t)p->height)
        return UINT64_MAX;
    pixel_count = (uint64_t)p->width * (uint64_t)p->height;
    if (pixel_count > UINT64_MAX / sizeof(uint32_t))
        return UINT64_MAX;
    data_bytes = pixel_count * (uint64_t)sizeof(uint32_t);
    total = data_bytes;
    return total;
}

/// @brief Scan one decoded mip for any non-opaque texel using Pixels' generation cache.
/// @param[in] pixels Opaque decoded Pixels object to inspect.
/// @return 1 when at least one texel has non-opaque alpha, otherwise 0.
static int textureasset3d_decoded_mip_has_alpha(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl_or_null(pixels);
    return rt_pixels_has_alpha_texels_cached(p) ? 1 : 0;
}

/// @brief Compute exact alpha metadata from decoded mip fallbacks.
/// @details Returns 1 only when every loaded mip has a decoded Pixels fallback, making the
///          resulting alpha classification exact. Native-only compressed assets without a decoded
///          fallback remain "unknown" instead of being classified by format name.
/// @param[in] mip_pixels Array of decoded Pixels objects, one per mip level.
/// @param[in] mip_count Number of entries in @p mip_pixels.
/// @param[out] out_has_alpha Optional destination set when any decoded mip contains transparency.
/// @return 1 when every mip was available and the result is exact, otherwise 0.
static int textureasset3d_compute_alpha_metadata(void **mip_pixels,
                                                 int64_t mip_count,
                                                 int8_t *out_has_alpha) {
    int has_alpha = 0;
    if (out_has_alpha)
        *out_has_alpha = 0;
    if (!mip_pixels || mip_count <= 0)
        return 0;
    for (int64_t i = 0; i < mip_count; i++) {
        if (!mip_pixels[i])
            return 0;
        if (textureasset3d_decoded_mip_has_alpha(mip_pixels[i]))
            has_alpha = 1;
    }
    if (out_has_alpha)
        *out_has_alpha = has_alpha ? 1 : 0;
    return 1;
}

/// @brief Recompute canonical backing plus currently decoded fallback bytes.
/// @details Native payload pointers are zero-copy views into @c source_backing and must not be
///          counted a second time. Decoded Pixels are counted only while resident/alive, so moving
///          the window visibly reduces retained process memory while immutable source bytes remain
///          sufficient to reconstruct an evicted level. The result saturates to INT64_MAX.
/// @param[in] asset Texture asset whose currently owned allocations are measured.
/// @return Saturating retained-byte count in the range 0 through INT64_MAX.
static int64_t textureasset3d_compute_retained_bytes(rt_textureasset3d *asset) {
    uint64_t total = 0;
    int64_t safe_mip_count = textureasset3d_safe_mip_count(asset);
    if (!asset || safe_mip_count <= 0)
        return 0;
    textureasset3d_add_saturating_u64(&total, asset->source_backing_bytes);
    textureasset3d_add_saturating_u64(&total, asset->container_backing_bytes);
    for (int64_t i = 0; i < safe_mip_count; i++) {
        if (asset->mip_pixels && asset->mip_pixels[i]) {
            textureasset3d_add_saturating_u64(
                &total, textureasset3d_pixels_allocation_bytes(asset->mip_pixels[i]));
        }
    }
    return total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
}

/// @brief Borrow an exact source container only while its associated decoded surface is unchanged.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @param[out] data Optional destination for the borrowed immutable container bytes.
/// @param[out] size Optional destination for the container byte count.
/// @param[out] kind Optional destination for the canonical static container-kind name.
/// @return 1 when an unchanged retained container is available, otherwise 0.
/// @note Returned pointers remain owned by @p obj and must not be freed or modified.
int8_t rt_textureasset3d_get_source_container(void *obj,
                                              const uint8_t **data,
                                              uint64_t *size,
                                              const char **kind) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    rt_pixels_impl *base_pixels = NULL;
    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    if (kind)
        *kind = NULL;
    if (!asset || !asset->container_backing || asset->container_backing_bytes == 0 ||
        !asset->container_kind)
        return 0;
    if (asset->container_tracks_pixels) {
        base_pixels = asset->mip_pixels && textureasset3d_safe_mip_count(asset) > 0
                          ? rt_pixels_checked_impl_or_null(asset->mip_pixels[0])
                          : NULL;
        if (base_pixels &&
            (base_pixels->generation != asset->container_pixels_generation ||
             base_pixels->width != asset->container_pixels_width ||
             base_pixels->height != asset->container_pixels_height ||
             base_pixels->width != asset->width || base_pixels->height != asset->height))
            return 0;
    }
    if (data)
        *data = asset->container_backing;
    if (size)
        *size = asset->container_backing_bytes;
    if (kind)
        *kind = asset->container_kind;
    return 1;
}

/// @brief Classify retained source provenance for bake-fidelity reporting.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return One of the RT_TEXTUREASSET3D_SOURCE_CONTAINER_* state values.
int64_t rt_textureasset3d_get_source_container_state(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    if (!asset || !asset->container_backing || asset->container_backing_bytes == 0 ||
        !asset->container_kind)
        return RT_TEXTUREASSET3D_SOURCE_CONTAINER_NONE;
    return rt_textureasset3d_get_source_container(obj, NULL, NULL, NULL)
               ? RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID
               : RT_TEXTUREASSET3D_SOURCE_CONTAINER_CHANGED_AFTER_IMPORT;
}

/// @brief Build a one-mip RGBA8 TextureAsset3D retaining exact encoded source bytes.
/// @param[in] pixels Valid decoded Pixels surface retained as mip zero.
/// @param[in] data Exact encoded source-container bytes to copy.
/// @param[in] size Number of bytes in @p data.
/// @param[in] kind Supported source-container kind or accepted alias.
/// @return A newly allocated TextureAsset3D, or NULL on validation or allocation failure.
void *rt_textureasset3d_wrap_encoded_pixels(void *pixels,
                                            const uint8_t *data,
                                            uint64_t size,
                                            const char *kind) {
    rt_pixels_impl *source = rt_pixels_checked_impl_or_null(pixels);
    const char *normalized = textureasset3d_normalize_container_kind(kind);
    textureasset3d_mip *mips = NULL;
    void **mip_pixels = NULL;
    uint8_t *fallback_kinds = NULL;
    uint8_t *container = NULL;
    rt_textureasset3d *asset;
    uint64_t pixel_bytes;
    if (!source || source->width <= 0 || source->height <= 0 || !source->data) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "TextureAsset3D: encoded source requires valid decoded Pixels");
        return NULL;
    }
    if (!normalized || !data || size == 0 || size > TEXTUREASSET3D_MAX_FILE_BYTES ||
        size > SIZE_MAX || !textureasset3d_container_magic_matches(data, size, normalized)) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "TextureAsset3D: source container kind or magic is invalid");
        return NULL;
    }
    if ((uint64_t)source->width > UINT32_MAX || (uint64_t)source->height > UINT32_MAX ||
        (uint64_t)source->width > UINT64_MAX / (uint64_t)source->height ||
        (uint64_t)source->width * (uint64_t)source->height > UINT64_MAX / sizeof(uint32_t)) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_TOO_LARGE,
                                    "TextureAsset3D: decoded source dimensions are too large");
        return NULL;
    }
    pixel_bytes = (uint64_t)source->width * (uint64_t)source->height * (uint64_t)sizeof(uint32_t);
    mips = (textureasset3d_mip *)calloc(1u, sizeof(*mips));
    mip_pixels = (void **)calloc(1u, sizeof(*mip_pixels));
    fallback_kinds = (uint8_t *)calloc(1u, sizeof(*fallback_kinds));
    container = (uint8_t *)malloc((size_t)size);
    if (!mips || !mip_pixels || !fallback_kinds || !container) {
        free(mips);
        free(mip_pixels);
        free(fallback_kinds);
        free(container);
        rt_trap("TextureAsset3D: encoded-source wrapper allocation failed");
        return NULL;
    }
    memcpy(container, data, (size_t)size);
    asset = (rt_textureasset3d *)rt_obj_new_i64(RT_G3D_TEXTUREASSET3D_CLASS_ID,
                                                (int64_t)sizeof(*asset));
    if (!asset) {
        free(mips);
        free(mip_pixels);
        free(fallback_kinds);
        free(container);
        rt_trap("TextureAsset3D: encoded-source object allocation failed");
        return NULL;
    }
    memset(asset, 0, sizeof(*asset));
    rt_obj_set_finalizer(asset, textureasset3d_finalize);
    rt_obj_retain_maybe(pixels);
    mip_pixels[0] = pixels;
    mips[0].length = pixel_bytes;
    mips[0].uncompressed_length = pixel_bytes;
    mips[0].width = (uint32_t)source->width;
    mips[0].height = (uint32_t)source->height;
    fallback_kinds[0] = TEXTUREASSET3D_FALLBACK_NONE;
    asset->pixels = pixels;
    asset->mip_pixels = mip_pixels;
    asset->mip_fallback_kinds = fallback_kinds;
    asset->mips = mips;
    asset->width = source->width;
    asset->height = source->height;
    asset->mip_count = 1;
    asset->mip_capacity = 1;
    asset->resident_mip_count = 1;
    asset->resident_bytes = pixel_bytes > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)pixel_bytes;
    asset->format = "rgba8";
    asset->degraded_reason = "";
    asset->block_width = 1;
    asset->block_height = 1;
    asset->block_bytes = 4;
    asset->cache_identity = textureasset3d_next_cache_identity();
    asset->native_revision = 1;
    asset->alpha_metadata_known = 1;
    asset->has_alpha_texels = textureasset3d_decoded_mip_has_alpha(pixels) ? 1 : 0;
    asset->container_backing = container;
    asset->container_backing_bytes = size;
    asset->container_kind = normalized;
    asset->container_tracks_pixels = 1;
    asset->container_pixels_generation = source->generation;
    asset->container_pixels_width = source->width;
    asset->container_pixels_height = source->height;
    asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    return asset;
}

/// @brief Transactionally reconstruct, publish, and evict decoded mips for a resident window.
/// @details The requested range is clamped to available levels. Every missing entering fallback
///          is decoded into a fixed staging array before any live slot or residency metadata is
///          changed. After staging succeeds, decoded Pixels outside the new range are released;
///          immutable canonical backing/native views remain. Resident byte accounting uses native
///          block bytes when available and decoded Pixels otherwise. A range change bumps the
///          backend revision only after publication. Negative inputs trap through @p api_name.
/// @param[in,out] asset Texture asset whose resident decoded window is replaced.
/// @param[in] first_mip First requested mip level; values beyond the table select an empty window.
/// @param[in] mip_count Requested number of consecutive resident levels.
/// @param[in] api_name Optional trap-message fallback for invalid negative ranges.
/// @return 1 on success, or 0 for a NULL asset or an unexpected reconstruction failure; the latter
///         leaves the prior live window and decoded slots unchanged.
static int textureasset3d_set_resident_mip_range_internal(rt_textureasset3d *asset,
                                                          int64_t first_mip,
                                                          int64_t mip_count,
                                                          const char *api_name) {
    uint64_t total = 0;
    void *staged[TEXTUREASSET3D_MAX_MIP_COUNT] = {0};
    int64_t old_start;
    int64_t old_count;
    int64_t safe_mip_count;
    int64_t clamped_mip_count;
    int can_evict;
    int reconstructed_base = 0;

    if (!asset)
        return 0;
    old_start = asset->resident_mip_start;
    old_count = asset->resident_mip_count;
    if (first_mip < 0 || mip_count < 0) {
        rt_trap(api_name ? api_name : "TextureAsset3D.SetResidentMipRange: negative mip range");
        return 0;
    }
    safe_mip_count = textureasset3d_safe_mip_count(asset);
    clamped_mip_count = safe_mip_count > 0 ? safe_mip_count : 0;
    if (mip_count == 0 || first_mip >= safe_mip_count || safe_mip_count <= 0) {
        first_mip = first_mip > clamped_mip_count ? clamped_mip_count : first_mip;
        mip_count = 0;
    } else if (mip_count > safe_mip_count - first_mip) {
        mip_count = safe_mip_count - first_mip;
    }

    can_evict = asset->source_backing && asset->source_backing_bytes > 0 &&
                asset->mip_fallback_kinds && asset->mip_pixels;

    /* Prepare every entering decoded fallback before the eviction commit. */
    if (can_evict) {
        for (int64_t i = 0; i < mip_count; i++) {
            int64_t absolute_mip = first_mip + i;
            uint8_t fallback_kind;
            if (asset->mip_pixels[absolute_mip])
                continue;
            fallback_kind = asset->mip_fallback_kinds[absolute_mip];
            if (fallback_kind == TEXTUREASSET3D_FALLBACK_DECODE)
                staged[absolute_mip] = textureasset3d_decode_mip_from_backing(asset, absolute_mip);
            else if (fallback_kind == TEXTUREASSET3D_FALLBACK_CHECKER)
                staged[absolute_mip] = textureasset3d_make_decode_failure_checker();
            if (fallback_kind != TEXTUREASSET3D_FALLBACK_NONE && !staged[absolute_mip]) {
                for (int64_t j = 0; j < safe_mip_count; j++)
                    textureasset3d_release_pixels_ref(&staged[j]);
                return 0;
            }
        }

        for (int64_t i = 0; i < safe_mip_count; i++) {
            int in_new_range = i >= first_mip && i - first_mip < mip_count;
            if (staged[i]) {
                if (i == 0)
                    reconstructed_base = 1;
                asset->mip_pixels[i] = staged[i];
                staged[i] = NULL;
            } else if (!in_new_range) {
                textureasset3d_release_pixels_ref(&asset->mip_pixels[i]);
            }
        }
    }

    if (reconstructed_base && asset->container_tracks_pixels && asset->mip_pixels[0]) {
        rt_pixels_impl *base_pixels = rt_pixels_checked_impl_or_null(asset->mip_pixels[0]);
        if (base_pixels) {
            asset->container_pixels_generation = base_pixels->generation;
            asset->container_pixels_width = base_pixels->width;
            asset->container_pixels_height = base_pixels->height;
        }
    }

    for (int64_t i = 0; i < mip_count; i++) {
        int64_t absolute_mip = first_mip + i;
        const textureasset3d_mip *mip = &asset->mips[absolute_mip];
        if (asset->mip_payloads && asset->mip_payloads[absolute_mip] && mip->length > 0) {
            textureasset3d_add_saturating_u64(&total, mip->length);
        } else if (asset->mip_pixels && asset->mip_pixels[absolute_mip]) {
            textureasset3d_add_saturating_u64(
                &total, textureasset3d_pixels_allocation_bytes(asset->mip_pixels[absolute_mip]));
        }
    }

    asset->resident_mip_start = first_mip;
    asset->resident_mip_count = mip_count;
    asset->resident_bytes = total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
    asset->pixels = (mip_count > 0 && asset->mip_pixels) ? asset->mip_pixels[first_mip] : NULL;
    asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    if (old_start != asset->resident_mip_start || old_count != asset->resident_mip_count)
        textureasset3d_bump_native_revision(asset);
    return 1;
}

/// @brief True when the currently resident mip window is inside the loaded mip table.
/// @param[in] asset Texture asset whose resident range is inspected.
/// @return 1 for a nonempty, wholly in-bounds range, otherwise 0.
static int textureasset3d_resident_range_valid(const rt_textureasset3d *asset) {
    int64_t safe_mip_count = textureasset3d_safe_mip_count(asset);
    if (!asset || safe_mip_count <= 0 || asset->resident_mip_start < 0 ||
        asset->resident_mip_count <= 0)
        return 0;
    if (asset->resident_mip_start >= safe_mip_count)
        return 0;
    return asset->resident_mip_count <= safe_mip_count - asset->resident_mip_start;
}

/// @brief True when the resident range contains at least one complete native payload.
/// @param[in] asset Texture asset whose resident native views are inspected.
/// @return 1 when a valid resident level has a nonempty native payload, otherwise 0.
static int textureasset3d_resident_range_has_native_payload(const rt_textureasset3d *asset) {
    if (!asset || !asset->compressed || !asset->mip_payloads || !asset->mips ||
        !textureasset3d_resident_range_valid(asset) || asset->block_width <= 0 ||
        asset->block_height <= 0 || asset->block_bytes <= 0)
        return 0;
    for (int64_t i = 0; i < asset->resident_mip_count; i++) {
        int64_t mip = asset->resident_mip_start + i;
        if (mip < 0 || mip >= textureasset3d_safe_mip_count(asset))
            continue;
        if (asset->mip_payloads[mip] && asset->mips[mip].length > 0 && asset->mips[mip].width > 0 &&
            asset->mips[mip].height > 0)
            return 1;
    }
    return 0;
}

/// @brief Read an entire file into a freshly malloc'd buffer (caller frees).
/// @details Rejects empty files and files larger than TEXTUREASSET3D_MAX_FILE_BYTES.
/// @param[in] path UTF-8 path of the file to read.
/// @param[out] out_size Optional destination for the returned byte count; cleared on entry.
/// @return Byte buffer with @p out_size set, or NULL on open/read/size failure.
/// @ownership The caller must free the returned buffer.
static uint8_t *textureasset3d_read_file_bytes(const char *path, size_t *out_size) {
    FILE *file;
    int64_t file_size;
    uint8_t *bytes;

    if (out_size)
        *out_size = 0;
    if (!path || !*path)
        return NULL;
    file = rt_file_stdio_open_utf8(path, "rb");
    if (!file)
        return NULL;
    if (textureasset3d_fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    file_size = (int64_t)textureasset3d_ftell(file);
    if (textureasset3d_fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    if (file_size <= 0 || (uint64_t)file_size > TEXTUREASSET3D_MAX_FILE_BYTES) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)file_size);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    if (fread(bytes, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    if (out_size)
        *out_size = (size_t)file_size;
    return bytes;
}

/// @brief Compute width*height*4 RGBA8 bytes with overflow checks.
/// @param[in] width Image width in texels.
/// @param[in] height Image height in texels.
/// @param[out] out_byte_count Optional destination for the computed byte count.
/// @return 1 with @p out_byte_count set, or 0 on zero dimension or multiplication overflow.
static int textureasset3d_rgba8_byte_count(uint32_t width,
                                           uint32_t height,
                                           uint64_t *out_byte_count) {
    uint64_t pixels;
    if (out_byte_count)
        *out_byte_count = 0;
    if (width == 0 || height == 0)
        return 0;
    if ((uint64_t)width > UINT64_MAX / (uint64_t)height)
        return 0;
    pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > UINT64_MAX / 4u)
        return 0;
    if (out_byte_count)
        *out_byte_count = pixels * 4u;
    return 1;
}

/// @brief Copy a raw RGBA8 mip from the file into a new Pixels object.
/// @details Validates the byte budget and that [offset, offset+needed) lies within @p size,
///          then repacks the source bytes into the Pixels RGBA word order.
/// @param[in] data Base address of the complete raw texture payload.
/// @param[in] size Number of readable bytes in @p data.
/// @param[in] width Width of the requested mip level in texels.
/// @param[in] height Height of the requested mip level in texels.
/// @param[in] offset Byte offset of the mip level within @p data.
/// @param[in] length Declared byte length of the mip level.
/// @return New Pixels handle, or NULL on bounds/budget failure.
static void *textureasset3d_decode_rgba8_fallback(const uint8_t *data,
                                                  size_t size,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint64_t offset,
                                                  uint64_t length) {
    uint64_t needed = 0;
    void *pixels;

    if (!textureasset3d_rgba8_byte_count(width, height, &needed))
        return NULL;
    if (needed > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES)
        return NULL;
    if (length < needed || offset > (uint64_t)size || needed > (uint64_t)size - offset)
        return NULL;

    pixels = rt_pixels_new((int64_t)width, (int64_t)height);
    if (!pixels) {
        rt_trap("TextureAsset3D.LoadKTX2: Pixels fallback allocation failed");
        return NULL;
    }
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint64_t index = ((uint64_t)y * (uint64_t)width + (uint64_t)x) * 4u;
            const uint8_t *p = data + offset + index;
            uint32_t rgba = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                            ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            rt_pixels_set_rgba(pixels, (int64_t)x, (int64_t)y, (int64_t)rgba);
        }
    }
    return pixels;
}

/// @brief Release a decoded mip array and all live Pixels references it owns.
/// @param[in,out] mip_pixels Array of nullable owned Pixels references.
/// @param[in] mip_count Number of slots in @p mip_pixels.
static void textureasset3d_release_mip_pixels(void **mip_pixels, int64_t mip_count);

/// @brief Allocate an 8x8 magenta/black checker Pixels fallback for failed compressed decodes.
/// @details The conspicuous pattern makes unsupported CPU texture blocks visible on software and
///          RenderTarget3D paths instead of producing an apparently blank material.
/// @return A newly allocated checkerboard Pixels object, or NULL on allocation failure.
static void *textureasset3d_make_decode_failure_checker(void) {
    void *pixels = rt_pixels_new(8, 8);
    if (!pixels) {
        rt_trap("TextureAsset3D.LoadKTX2: checker fallback allocation failed");
        return NULL;
    }
    for (int64_t y = 0; y < 8; y++) {
        for (int64_t x = 0; x < 8; x++) {
            int64_t rgba = (((x + y) & 1) == 0) ? INT64_C(0xFF00FFFF) : INT64_C(0x000000FF);
            rt_pixels_set_rgba(pixels, x, y, rgba);
        }
    }
    return pixels;
}

/// @brief Allocate a checker fallback for each mip slot after whole-texture CPU decode failure.
/// @param[in] mip_count Number of checkerboard Pixels objects to allocate.
/// @return An owned array of @p mip_count checkers, or NULL if validation or allocation fails.
/// @ownership The caller owns both the returned array and the references in its slots.
static void **textureasset3d_make_decode_failure_checker_mips(int64_t mip_count) {
    void **mip_pixels;

    if (mip_count <= 0)
        return NULL;
    mip_pixels = (void **)calloc((size_t)mip_count, sizeof(void *));
    if (!mip_pixels)
        return NULL;
    for (int64_t i = 0; i < mip_count; i++) {
        mip_pixels[i] = textureasset3d_make_decode_failure_checker();
        if (!mip_pixels[i]) {
            textureasset3d_release_mip_pixels(mip_pixels, mip_count);
            return NULL;
        }
    }
    return mip_pixels;
}

/// @brief Release every decoded Pixels mip in the array, then free the array itself.
/// @param[in,out] mip_pixels Array of nullable owned Pixels references.
/// @param[in] mip_count Number of slots in @p mip_pixels.
static void textureasset3d_release_mip_pixels(void **mip_pixels, int64_t mip_count) {
    if (!mip_pixels)
        return;
    for (int64_t i = 0; i < mip_count; i++)
        textureasset3d_release_pixels_ref(&mip_pixels[i]);
    free(mip_pixels);
}

/// @brief Free a native-payload view table without touching its borrowed backing slices.
/// @details Every non-NULL slot points into the TextureAsset3D's single canonical backing span.
///          The backing owner is released separately by the asset finalizer or parse cleanup.
/// @param[in,out] mip_payloads Owned pointer table whose slices remain borrowed.
/// @param[in] mip_count Number of slots in @p mip_payloads; retained for symmetric cleanup APIs.
static void textureasset3d_release_mip_payloads(uint8_t **mip_payloads, int64_t mip_count) {
    (void)mip_count;
    if (!mip_payloads)
        return;
    free(mip_payloads);
}

/// @brief Build per-mip native upload views over one immutable canonical backing span.
/// @details Bounds-checks every `[offset,length)` slice against @p size. No level bytes are copied;
///          the returned table alone is allocated and all slots borrow @p data for the asset's
///          lifetime. Zero-length mips leave a NULL slot.
/// @param[in] data Mutable-typed base of the asset-owned immutable canonical backing.
/// @param[in] size Number of readable bytes in @p data.
/// @param[in] mips Array of byte ranges for each mip level.
/// @param[in] mip_count Number of entries in @p mips and the returned table.
/// @return Caller-owned pointer table, or NULL on invalid bounds/allocation failure.
static uint8_t **textureasset3d_make_native_mip_payload_views(uint8_t *data,
                                                              size_t size,
                                                              const textureasset3d_mip *mips,
                                                              int64_t mip_count) {
    uint8_t **mip_payloads;

    if (!data || !mips || mip_count <= 0)
        return NULL;
    mip_payloads = (uint8_t **)calloc((size_t)mip_count, sizeof(uint8_t *));
    if (!mip_payloads)
        return NULL;
    for (int64_t i = 0; i < mip_count; i++) {
        const textureasset3d_mip *mip = &mips[i];
        if (mip->length == 0)
            continue;
        if (mip->offset > (uint64_t)size || mip->length > (uint64_t)size - mip->offset) {
            textureasset3d_release_mip_payloads(mip_payloads, mip_count);
            return NULL;
        }
        mip_payloads[i] = data + mip->offset;
    }
    return mip_payloads;
}

/// @brief Capture decoded RGBA8 Pixels mips into one immutable canonical backing allocation.
/// @details Used by BasisLZ after its codebook/slice decoder has produced Pixels. Each output level
///          is encoded as byte-order-independent R,G,B,A bytes and receives a packed offset/length
///          in @p mips. This one-time capture permits later residency eviction without retaining
///          BasisLZ decoder state or re-reading the source file.
/// @param[in] mip_pixels Complete decoded Pixels array.
/// @param[in,out] mips Mutable mip metadata receiving packed canonical ranges.
/// @param[in] mip_count Number of levels in both arrays.
/// @param[out] out_backing Receives the malloc-owned canonical span.
/// @param[out] out_size Receives its byte count.
/// @return 1 on success, 0 for invalid/missing Pixels, overflow, budget excess, or allocation
///         failure. Outputs are cleared before validation.
static int textureasset3d_capture_rgba8_backing(void **mip_pixels,
                                                textureasset3d_mip *mips,
                                                int64_t mip_count,
                                                uint8_t **out_backing,
                                                uint64_t *out_size) {
    uint64_t total = 0;
    uint8_t *backing;

    if (out_backing)
        *out_backing = NULL;
    if (out_size)
        *out_size = 0;
    if (!mip_pixels || !mips || mip_count <= 0 || !out_backing || !out_size)
        return 0;
    for (int64_t i = 0; i < mip_count; i++) {
        uint64_t level_bytes = 0;
        rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(mip_pixels[i]);
        if (!pixels || pixels->width != (int64_t)mips[i].width ||
            pixels->height != (int64_t)mips[i].height ||
            !textureasset3d_rgba8_byte_count(mips[i].width, mips[i].height, &level_bytes) ||
            level_bytes > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES ||
            total > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES - level_bytes)
            return 0;
        total += level_bytes;
    }
    backing = (uint8_t *)malloc(total ? (size_t)total : 1u);
    if (!backing)
        return 0;
    total = 0;
    for (int64_t i = 0; i < mip_count; i++) {
        rt_pixels_impl *pixels = rt_pixels_checked_impl_or_null(mip_pixels[i]);
        uint64_t level_bytes = (uint64_t)mips[i].width * (uint64_t)mips[i].height * 4u;
        mips[i].offset = total;
        mips[i].length = level_bytes;
        mips[i].uncompressed_length = level_bytes;
        for (uint32_t y = 0; y < mips[i].height; y++) {
            for (uint32_t x = 0; x < mips[i].width; x++) {
                uint32_t rgba = pixels->data[(uint64_t)y * mips[i].width + x];
                uint8_t *dst = backing + total + ((uint64_t)y * mips[i].width + x) * 4u;
                dst[0] = (uint8_t)(rgba >> 24);
                dst[1] = (uint8_t)(rgba >> 16);
                dst[2] = (uint8_t)(rgba >> 8);
                dst[3] = (uint8_t)rgba;
            }
        }
        total += level_bytes;
    }
    *out_backing = backing;
    *out_size = total;
    return 1;
}

/// @brief Decode all mips of a raw RGBA8 texture into a Pixels array.
/// @param[in] data Base address of the complete raw texture payload.
/// @param[in] size Number of readable bytes in @p data.
/// @param[in] mips Array describing the source range and dimensions of each mip level.
/// @param[in] mip_count Number of entries in @p mips.
/// @return A newly allocated array of Pixels objects, or NULL if any mip fails.
/// @ownership The caller owns the returned array and every Pixels reference it contains.
static void **textureasset3d_decode_rgba8_mips(const uint8_t *data,
                                               size_t size,
                                               const textureasset3d_mip *mips,
                                               int64_t mip_count) {
    void **mip_pixels;

    if (!data || !mips || mip_count <= 0)
        return NULL;
    mip_pixels = (void **)calloc((size_t)mip_count, sizeof(void *));
    if (!mip_pixels)
        return NULL;
    for (int64_t i = 0; i < mip_count; i++) {
        mip_pixels[i] = textureasset3d_decode_rgba8_fallback(
            data, size, mips[i].width, mips[i].height, mips[i].offset, mips[i].length);
        if (!mip_pixels[i]) {
            textureasset3d_release_mip_pixels(mip_pixels, mip_count);
            return NULL;
        }
    }
    return mip_pixels;
}
