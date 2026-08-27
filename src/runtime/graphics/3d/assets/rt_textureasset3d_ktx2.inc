//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_textureasset3d_ktx2.inc
// Purpose: Implement the TextureAsset3D KTX2 parser and public query API.
//   Included by rt_textureasset3d.c.
// Key invariants:
//   - KTX2 ranges and mip geometry are validated before indexed access.
//   - Format fallback selection comes from the authoritative format table.
// Ownership/Lifetime:
//   - TextureAsset3D owns its immutable canonical source backing and mip table.
//   - Decoded Pixels results are new GC-managed objects returned to the caller.
// Links: rt_textureasset3d.c, rt_textureasset3d_internal.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements KTX2 loading and the public TextureAsset3D query surface.
///
/// This implementation fragment validates KTX2 headers, level tables, format
/// geometry, supercompression, and canonical backing ownership. It selects CPU
/// fallbacks through the authoritative format table, applies strict or
/// permissive degradation policy, constructs TextureAsset3D instances, and
/// exposes their format, residency, alpha, and native-upload metadata.
///
/// @note This file is included by `rt_textureasset3d.c` and is not a standalone
///       translation unit.

/// @brief Decode one mip from an asset's immutable canonical backing using its table-selected
/// decoder.
/// @details The asset retains exact packed ranges in @c mips. This helper validates the requested
///          index/range through the existing per-format decoder and returns a new Pixels object;
///          it never mutates residency or source state. UASTC remains internally distinguishable
///          after its public normalized format changes to rgba8.
/// @param[in] asset Valid TextureAsset3D implementation with canonical backing.
/// @param[in] mip Absolute mip index.
/// @return New GC-managed Pixels fallback, or NULL when the index/source/decoder is unavailable or
///         the level uses a valid-but-partially-unsupported block mode.
static void *textureasset3d_decode_mip_from_backing(const rt_textureasset3d *asset, int64_t mip) {
    const textureasset3d_mip *level;
    const uint8_t *data;
    size_t size;

    if (!asset || !asset->source_backing || asset->source_backing_bytes > SIZE_MAX ||
        !asset->mips || mip < 0 || mip >= textureasset3d_safe_mip_count(asset))
        return NULL;
    level = &asset->mips[mip];
    data = asset->source_backing;
    size = (size_t)asset->source_backing_bytes;
    switch ((textureasset3d_decoder_kind)asset->decoder_kind) {
        case TEXTUREASSET3D_DECODER_RGBA8:
            return textureasset3d_decode_rgba8_fallback(
                data, size, level->width, level->height, level->offset, level->length);
        case TEXTUREASSET3D_DECODER_BC1:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_bc1_block_adapter,
                "TextureAsset3D: BC1 mip reconstruction allocation failed");
        case TEXTUREASSET3D_DECODER_BC3:
            return textureasset3d_decode_bc3_fallback(
                data, size, level->width, level->height, level->offset, level->length);
        case TEXTUREASSET3D_DECODER_BC4:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_bc4_block_adapter,
                "TextureAsset3D: BC4 mip reconstruction allocation failed");
        case TEXTUREASSET3D_DECODER_BC5:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_bc5_block_adapter,
                "TextureAsset3D: BC5 mip reconstruction allocation failed");
        case TEXTUREASSET3D_DECODER_BC6H:
        case TEXTUREASSET3D_DECODER_BC6HS:
            return textureasset3d_decode_bc6h_fallback(data,
                                                       size,
                                                       level->width,
                                                       level->height,
                                                       level->offset,
                                                       level->length,
                                                       asset->decoder_kind ==
                                                           TEXTUREASSET3D_DECODER_BC6HS);
        case TEXTUREASSET3D_DECODER_BC7:
            return textureasset3d_decode_bc7_fallback(
                data, size, level->width, level->height, level->offset, level->length);
        case TEXTUREASSET3D_DECODER_ETC2:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_etc2_block_adapter,
                "TextureAsset3D: ETC2 mip reconstruction allocation failed");
        case TEXTUREASSET3D_DECODER_ASTC:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_astc_block_adapter,
                "TextureAsset3D: ASTC mip reconstruction allocation failed");
        case TEXTUREASSET3D_DECODER_UASTC:
            return textureasset3d_decode_compressed_fallback(
                data,
                size,
                level->width,
                level->height,
                level->offset,
                level->length,
                asset->block_width,
                asset->block_height,
                asset->block_bytes,
                textureasset3d_decode_uastc_block_adapter,
                "TextureAsset3D: UASTC mip reconstruction allocation failed");
        default:
            return NULL;
    }
}

/// @brief Probe every table-supported mip without retaining all decoded surfaces at once.
/// @details Each level is decoded independently to validate partial-decoder coverage. Mip zero is
///          retained as the immediately drawable fallback; successful later probes are released
///          and reconstructed from canonical backing only when their residency window is selected.
///          This bounds import-time decoded memory to mip zero plus one probe instead of the sum of
///          the complete mip chain. NULL fallback-kind slots preserve genuine partial misses.
/// @param[in] prototype Temporary asset view containing backing, mips, geometry, and decoder kind.
/// @param[in] mip_count Validated number of levels.
/// @param[out] fallback_kinds Per-level reconstruction policy table to populate.
/// @return Newly allocated Pixels pointer table retaining at most mip zero, or NULL when no level
///         decoded/allocation failed.
static void **textureasset3d_probe_capability_mips(const rt_textureasset3d *prototype,
                                                   int64_t mip_count,
                                                   uint8_t *fallback_kinds) {
    void **mip_pixels;
    int decoded_any = 0;
    if (!prototype || mip_count <= 0 || prototype->decoder_kind == TEXTUREASSET3D_DECODER_NONE)
        return NULL;
    if (!prototype->source_backing || prototype->source_backing_bytes > SIZE_MAX || !fallback_kinds)
        return NULL;
    mip_pixels = (void **)calloc((size_t)mip_count, sizeof(*mip_pixels));
    if (!mip_pixels) {
        rt_trap("TextureAsset3D.LoadKTX2: fallback table allocation failed");
        return NULL;
    }
    for (int64_t i = 0; i < mip_count; ++i) {
        void *decoded = textureasset3d_decode_mip_from_backing(prototype, i);
        if (!decoded)
            continue;
        fallback_kinds[i] = TEXTUREASSET3D_FALLBACK_DECODE;
        decoded_any = 1;
        if (i == 0)
            mip_pixels[0] = decoded;
        else
            textureasset3d_release_pixels_ref(&decoded);
    }
    if (!decoded_any) {
        free(mip_pixels);
        return NULL;
    }
    return mip_pixels;
}

/// @brief Stable human-readable detail for a capability row's CPU decode failure.
/// @param[in] decoder Table-selected decoder kind.
/// @return Static diagnostic text used in the existing load warning/error.
static const char *textureasset3d_decoder_failure_detail(int32_t decoder) {
    switch ((textureasset3d_decoder_kind)decoder) {
        case TEXTUREASSET3D_DECODER_BC1:
            return "corrupt BC1 block payload";
        case TEXTUREASSET3D_DECODER_BC3:
            return "corrupt BC3 block payload";
        case TEXTUREASSET3D_DECODER_BC4:
            return "corrupt BC4 block payload";
        case TEXTUREASSET3D_DECODER_BC5:
            return "corrupt BC5 block payload";
        case TEXTUREASSET3D_DECODER_BC6H:
        case TEXTUREASSET3D_DECODER_BC6HS:
            return "corrupt BC6H block payload";
        case TEXTUREASSET3D_DECODER_BC7:
            return "unsupported reserved BC7 mode or corrupt block payload";
        case TEXTUREASSET3D_DECODER_ETC2:
            return "unsupported ETC2 color mode or corrupt block payload";
        case TEXTUREASSET3D_DECODER_ASTC:
            return "unsupported ASTC block type or corrupt block payload";
        case TEXTUREASSET3D_DECODER_UASTC:
            return "reserved UASTC mode or corrupt block payload";
        default:
            return "unsupported or corrupt compressed blocks";
    }
}

/// @brief Parse a KTX2 byte stream into a fully-populated TextureAsset3D.
/// @details Validates the identifier and dimensionality, reads the level index, builds the
///          mip table with bounds checks, then — for uncompressed level data — produces an
///          RGBA8/BC3/BC7/ETC2/ASTC software Pixels fallback and/or retains native compressed
///          payloads.
///          Seeds the cache identity and makes all mips resident. Malformed or unsupported
///          input reports through rt_asset_error (recoverable: callers receive NULL and can
///          query Assets3D.GetLastLoadError); only allocation failures trap.
/// @param[in] data Complete caller-owned KTX2 byte stream.
/// @param[in] size Number of readable bytes in @p data.
/// @param[in] api_name Public entry-point name used to contextualize diagnostics.
/// @param[in] strict Nonzero to reject CPU decode misses instead of substituting checkers.
/// @return New TextureAsset3D handle, or NULL on validation/allocation failure.
static void *textureasset3d_parse_ktx2(const uint8_t *data,
                                       size_t size,
                                       const char *api_name,
                                       int strict) {
    uint32_t vk_format;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t pixel_depth;
    uint32_t layer_count;
    uint32_t face_count;
    uint32_t level_count;
    uint32_t supercompression_scheme;
    uint64_t level_table_bytes;
    textureasset3d_format_info format;
    textureasset3d_mip *mips = NULL;
    int64_t mip_count;
    void **mip_pixels = NULL;
    uint8_t **mip_payloads = NULL;
    uint8_t *source_backing = NULL;
    uint8_t *fallback_kinds = NULL;
    uint64_t source_backing_bytes = 0;
    int attempted_cpu_decode = 0;
    int degraded = 0;
    const char *decode_failure_detail = NULL;
    rt_textureasset3d *asset;

    if (!api_name)
        api_name = "TextureAsset3D.LoadKTX2";
    if (!data || size < TEXTUREASSET3D_KTX2_HEADER_SIZE ||
        memcmp(data, ktx2_identifier, sizeof(ktx2_identifier)) != 0) {
        rt_asset_error_setf(RT_ASSET_ERROR_BAD_MAGIC, "%s: not a KTX2 texture", api_name);
        return NULL;
    }

    vk_format = textureasset3d_read_u32le(data + 12);
    pixel_width = textureasset3d_read_u32le(data + 20);
    pixel_height = textureasset3d_read_u32le(data + 24);
    pixel_depth = textureasset3d_read_u32le(data + 28);
    layer_count = textureasset3d_read_u32le(data + 32);
    face_count = textureasset3d_read_u32le(data + 36);
    level_count = textureasset3d_read_u32le(data + 40);
    supercompression_scheme = textureasset3d_read_u32le(data + 44);

    if (pixel_width == 0 || pixel_height == 0 || pixel_width > INT32_MAX ||
        pixel_height > INT32_MAX) {
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: invalid dimensions", api_name);
        return NULL;
    }
    if (pixel_depth > 1 || layer_count > 1 || face_count != 1) {
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNSUPPORTED, "%s: unsupported KTX2 dimensionality", api_name);
        return NULL;
    }
    /* Scheme 0 = none, 1 = BasisLZ/ETC1S (decoded by the dedicated path below),
     * 2 = Zstandard, 3 = ZLIB (both decompressed per level below). */
    if (supercompression_scheme > 3) {
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                            "%s: unsupported KTX2 supercompression scheme %u",
                            api_name,
                            (unsigned)supercompression_scheme);
        return NULL;
    }
    if (level_count == 0) {
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNSUPPORTED, "%s: implicit mip generation is unsupported", api_name);
        return NULL;
    }
    mip_count = (int64_t)level_count;
    if (mip_count > (int64_t)TEXTUREASSET3D_MAX_MIP_COUNT) {
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED, "%s: unsupported mip count", api_name);
        return NULL;
    }
    if (!rt_count_fits_bytes(level_count,
                             TEXTUREASSET3D_KTX2_LEVEL_ENTRY_SIZE,
                             UINT64_MAX - TEXTUREASSET3D_KTX2_HEADER_SIZE)) {
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: level index overflow", api_name);
        return NULL;
    }
    level_table_bytes = TEXTUREASSET3D_KTX2_HEADER_SIZE +
                        (uint64_t)level_count * TEXTUREASSET3D_KTX2_LEVEL_ENTRY_SIZE;
    if (level_table_bytes > (uint64_t)size) {
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: truncated level index", api_name);
        return NULL;
    }

    /* BasisLZ/ETC1S levels have no vkFormat and carry their own codebook segment;
     * they decode straight to RGBA8 Pixels through the dedicated path. */
    if (supercompression_scheme == 1) {
        asset = (rt_textureasset3d *)textureasset3d_parse_ktx2_basislz(
            data, size, api_name, pixel_width, pixel_height, level_count);
        if (asset && !textureasset3d_attach_source_container(asset, data, (uint64_t)size, "ktx2")) {
            textureasset3d_release_ref((void **)&asset);
            return NULL;
        }
        return asset;
    }

    format = textureasset3d_format_from_vk(vk_format);
    /* UASTC LDR 4x4 also carries vkFormat 0; it is identified by the Data Format
     * Descriptor's color model (166). Level payloads are 128-bit blocks that the
     * generic block-decode path unpacks to RGBA8 (no native GPU upload format). */
    int format_is_uastc = 0;
    if (vk_format == 0) {
        uint32_t dfd_offset = textureasset3d_read_u32le(data + 48);
        uint32_t dfd_length = textureasset3d_read_u32le(data + 52);
        if (dfd_length >= 16u && (uint64_t)dfd_offset <= (uint64_t)size &&
            (uint64_t)dfd_length <= (uint64_t)size - dfd_offset && data[dfd_offset + 12u] == 166u) {
            format_is_uastc = 1;
            format = (textureasset3d_format_info){0,
                                                  0,
                                                  "uastc",
                                                  1,
                                                  RT_TEXTUREASSET3D_CPU_SUPPORT_PARTIAL,
                                                  4,
                                                  4,
                                                  16,
                                                  TEXTUREASSET3D_DECODER_UASTC,
                                                  RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE,
                                                  RT_TEXTUREASSET3D_BACKEND_CAP_NONE};
        }
    }
    mips = (textureasset3d_mip *)calloc((size_t)mip_count, sizeof(textureasset3d_mip));
    if (!mips) {
        rt_trap("TextureAsset3D.LoadKTX2: mip table allocation failed");
        return NULL;
    }
    for (int64_t i = 0; i < mip_count; i++) {
        mips[i].width = textureasset3d_mip_dimension(pixel_width, (uint32_t)i);
        mips[i].height = textureasset3d_mip_dimension(pixel_height, (uint32_t)i);
        if (level_count > 0) {
            const uint8_t *entry = data + TEXTUREASSET3D_KTX2_HEADER_SIZE +
                                   (uint64_t)i * TEXTUREASSET3D_KTX2_LEVEL_ENTRY_SIZE;
            mips[i].offset = textureasset3d_read_u64le(entry);
            mips[i].length = textureasset3d_read_u64le(entry + 8u);
            mips[i].uncompressed_length = textureasset3d_read_u64le(entry + 16u);
            /* The offset must stay inside the file even for zero-length levels:
             * `data + offset` is still formed below, and an unchecked 64-bit
             * offset would create an out-of-bounds pointer. */
            if (mips[i].offset > (uint64_t)size ||
                mips[i].length > (uint64_t)size - mips[i].offset) {
                free(mips);
                rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT, "%s: truncated mip payload", api_name);
                return NULL;
            }
            {
                uint64_t expected_bytes = 0;
                if (!textureasset3d_expected_mip_bytes(
                        &format, mips[i].width, mips[i].height, &expected_bytes)) {
                    free(mips);
                    rt_asset_error_setf(
                        RT_ASSET_ERROR_CORRUPT, "%s: invalid mip payload length", api_name);
                    return NULL;
                }
                if (supercompression_scheme == 0) {
                    /* Uncompressed levels: stored length must match exactly, and
                     * the file's uncompressed_length must be 0 (KTX2's "same as
                     * level length" sentinel) or exactly expected_bytes. */
                    if (mips[i].length != expected_bytes ||
                        (mips[i].uncompressed_length != 0 &&
                         mips[i].uncompressed_length != expected_bytes)) {
                        free(mips);
                        rt_asset_error_setf(
                            RT_ASSET_ERROR_CORRUPT, "%s: invalid mip payload length", api_name);
                        return NULL;
                    }
                } else {
                    /* Supercompressed levels: the stored uncompressed size must
                     * match the format's expectation; the compressed length is
                     * validated by the decompressor. */
                    if (mips[i].uncompressed_length != expected_bytes) {
                        free(mips);
                        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                            "%s: inconsistent uncompressed level length",
                                            api_name);
                        return NULL;
                    }
                }
            }
        }
    }

    /* Own every final level in one packed canonical span. Supercompressed levels decode directly
     * into their assigned slices, so no whole-level output allocation coexists merely for a copy.
     * Scheme 0 still copies source-file bytes once because filesystem/asset input is caller-owned.
     */
    {
        uint64_t total = 0;
        uint64_t cursor = 0;
        for (int64_t i = 0; i < mip_count; i++) {
            uint64_t final_length =
                supercompression_scheme == 0 ? mips[i].length : mips[i].uncompressed_length;
            if (final_length == 0 || final_length > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES ||
                total > TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES - final_length) {
                free(mips);
                rt_asset_error_setf(
                    RT_ASSET_ERROR_TOO_LARGE, "%s: canonical mip backing is too large", api_name);
                return NULL;
            }
            total += final_length;
        }
        source_backing = (uint8_t *)malloc(total ? (size_t)total : 1u);
        if (!source_backing) {
            free(mips);
            rt_trap("TextureAsset3D.LoadKTX2: canonical mip backing allocation failed");
            return NULL;
        }
        source_backing_bytes = total;
        if (supercompression_scheme != 0) {
            rt_atomic_store_u64(
                &g_textureasset3d_supercompression_final_bytes, total, __ATOMIC_RELEASE);
            rt_atomic_store_u64(
                &g_textureasset3d_supercompression_peak_bytes, total, __ATOMIC_RELEASE);
        }
        for (int64_t i = 0; i < mip_count; i++) {
            uint64_t stored_offset = mips[i].offset;
            uint64_t stored_length = mips[i].length;
            uint64_t final_length =
                supercompression_scheme == 0 ? stored_length : mips[i].uncompressed_length;
            int ok = 1;
            if (supercompression_scheme == 0) {
                memcpy(source_backing + cursor, data + stored_offset, (size_t)final_length);
            } else if (supercompression_scheme == 2) {
                ok = rt_zstd_decompress_into(data + stored_offset,
                                             (size_t)stored_length,
                                             source_backing + cursor,
                                             (size_t)final_length);
            } else {
                ok = rt_compress_inflate_zlib_into(data + stored_offset,
                                                   (size_t)stored_length,
                                                   source_backing + cursor,
                                                   (size_t)final_length);
            }
            if (!ok) {
                free(source_backing);
                free(mips);
                rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                    "%s: level %lld failed %s decompression",
                                    api_name,
                                    (long long)i,
                                    supercompression_scheme == 2 ? "Zstandard" : "ZLIB");
                return NULL;
            }
            mips[i].offset = cursor;
            mips[i].length = final_length;
            mips[i].uncompressed_length = final_length;
            cursor += final_length;
        }
    }

    fallback_kinds = (uint8_t *)calloc((size_t)mip_count, sizeof(*fallback_kinds));
    if (!fallback_kinds) {
        free(source_backing);
        free(mips);
        rt_trap("TextureAsset3D.LoadKTX2: fallback-state allocation failed");
        return NULL;
    }
    {
        rt_textureasset3d prototype;
        memset(&prototype, 0, sizeof(prototype));
        prototype.source_backing = source_backing;
        prototype.source_backing_bytes = source_backing_bytes;
        prototype.mips = mips;
        prototype.mip_count = mip_count;
        prototype.mip_capacity = mip_count;
        prototype.block_width = format.block_width;
        prototype.block_height = format.block_height;
        prototype.block_bytes = format.block_bytes;
        prototype.decoder_kind = format.decoder_kind;
        attempted_cpu_decode = format.cpu_support != RT_TEXTUREASSET3D_CPU_SUPPORT_NONE;
        decode_failure_detail = textureasset3d_decoder_failure_detail(format.decoder_kind);
        mip_pixels = textureasset3d_probe_capability_mips(&prototype, mip_count, fallback_kinds);
    }
    if (format.compressed && attempted_cpu_decode && !mip_pixels) {
        if (strict) {
            free(fallback_kinds);
            free(source_backing);
            free(mips);
            rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                                "%s: %s CPU texture decode failed (%s); strict loading forbids "
                                "checker fallback",
                                api_name,
                                format.name,
                                decode_failure_detail);
            return NULL;
        }
        mip_pixels = textureasset3d_make_decode_failure_checker_mips(mip_count);
        if (!mip_pixels) {
            free(fallback_kinds);
            free(source_backing);
            free(mips);
            rt_trap("TextureAsset3D.LoadKTX2: checker fallback allocation failed");
            return NULL;
        }
        degraded = 1;
        memset(fallback_kinds, TEXTUREASSET3D_FALLBACK_CHECKER, (size_t)mip_count);
        rt_asset_error_add_warningf("%s: %s CPU texture decode failed (%s); using 8x8 "
                                    "magenta/black checker fallback",
                                    api_name,
                                    format.name,
                                    decode_failure_detail);
    }
    if (format.compressed && format.native_format_id != RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE) {
        mip_payloads = textureasset3d_make_native_mip_payload_views(
            source_backing, (size_t)source_backing_bytes, mips, mip_count);
        if (!mip_payloads) {
            textureasset3d_release_mip_pixels(mip_pixels, mip_count);
            free(fallback_kinds);
            free(source_backing);
            free(mips);
            rt_trap("TextureAsset3D.LoadKTX2: native mip payload allocation failed");
            return NULL;
        }
    }
    if (format_is_uastc) {
        /* No GPU consumes UASTC blocks directly. Keep its source ranges/decoder for residency
         * reconstruction while exposing the successfully decoded texture as normalized RGBA8. */
        format.name = "rgba8";
        format.compressed = 0;
        format.native_format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
        format.backend_capability_bit = RT_TEXTUREASSET3D_BACKEND_CAP_NONE;
    }

    asset = (rt_textureasset3d *)rt_obj_new_i64(RT_G3D_TEXTUREASSET3D_CLASS_ID,
                                                (int64_t)sizeof(rt_textureasset3d));
    if (!asset) {
        textureasset3d_release_mip_pixels(mip_pixels, mip_count);
        textureasset3d_release_mip_payloads(mip_payloads, mip_count);
        free(fallback_kinds);
        free(source_backing);
        free(mips);
        rt_trap("TextureAsset3D.LoadKTX2: allocation failed");
        return NULL;
    }
    memset(asset, 0, sizeof(*asset));
    rt_obj_set_finalizer(asset, textureasset3d_finalize);
    asset->mip_pixels = mip_pixels;
    asset->mip_payloads = mip_payloads;
    asset->source_backing = source_backing;
    asset->source_backing_bytes = source_backing_bytes;
    asset->mip_fallback_kinds = fallback_kinds;
    asset->mips = mips;
    asset->width = (int64_t)pixel_width;
    asset->height = (int64_t)pixel_height;
    asset->mip_count = mip_count;
    asset->mip_capacity = mip_count;
    asset->format = format.name;
    asset->degraded_reason = degraded ? "cpu_decode_failed" : "";
    asset->compressed = format.compressed ? 1 : 0;
    asset->degraded = degraded ? 1 : 0;
    asset->block_width = format.block_width;
    asset->block_height = format.block_height;
    asset->block_bytes = format.block_bytes;
    asset->decoder_kind = format.decoder_kind;
    asset->native_format_id = format.native_format_id;
    asset->backend_capability_bit = format.backend_capability_bit;
    asset->cache_identity = textureasset3d_next_cache_identity();
    asset->native_revision = 1;
    asset->alpha_metadata_known =
        textureasset3d_compute_alpha_metadata(mip_pixels, mip_count, &asset->has_alpha_texels) ? 1
                                                                                               : 0;
    asset->resident_mip_start = 0;
    asset->resident_mip_count = mip_count;
    asset->pixels = mip_pixels ? mip_pixels[0] : NULL;
    {
        uint64_t resident_bytes = 0;
        for (int64_t i = 0; i < mip_count; ++i)
            textureasset3d_add_saturating_u64(&resident_bytes, mips[i].length);
        asset->resident_bytes =
            resident_bytes > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)resident_bytes;
    }
    asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    if (!textureasset3d_attach_source_container(asset, data, (uint64_t)size, "ktx2")) {
        textureasset3d_release_ref((void **)&asset);
        return NULL;
    }
    return asset;
}

/// @brief Decode a caller-owned KTX2 byte stream into a TextureAsset3D.
/// @details Returns NULL with a recoverable rt_asset_error on malformed or
///          unsupported input (query via Assets3D.GetLastLoadError); traps
///          only on caller misuse (NULL data) and allocation failure.
/// @param[in] data Complete borrowed KTX2 byte stream.
/// @param[in] size Number of readable bytes in @p data.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2_memory(const uint8_t *data, uint64_t size) {
    void *asset;

    if (!data) {
        rt_trap("TextureAsset3D.LoadKTX2Memory: invalid payload");
        return NULL;
    }
    rt_asset_error_begin_load();
    if (size == 0 || size > (uint64_t)SIZE_MAX || size > (uint64_t)TEXTUREASSET3D_MAX_FILE_BYTES) {
        rt_asset_error_setf(RT_ASSET_ERROR_TOO_LARGE,
                            "TextureAsset3D.LoadKTX2Memory: invalid payload size");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, (size_t)size, "TextureAsset3D.LoadKTX2Memory", 0);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Strictly decode a caller-owned KTX2 byte span without checker substitution.
/// @details Mirrors @ref rt_textureasset3d_load_ktx2_memory validation and load-error lifecycle,
///          but passes strict policy into the shared parser. The input remains borrowed and may be
///          released immediately after return because every accepted level is copied/decoded into
///          canonical asset-owned backing.
/// @param[in] data Complete borrowed KTX2 byte stream.
/// @param[in] size Number of readable bytes in @p data.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2_memory_strict(const uint8_t *data, uint64_t size) {
    void *asset;

    if (!data) {
        rt_trap("TextureAsset3D.LoadKTX2MemoryStrict: invalid payload");
        return NULL;
    }
    rt_asset_error_begin_load();
    if (size == 0 || size > (uint64_t)SIZE_MAX || size > (uint64_t)TEXTUREASSET3D_MAX_FILE_BYTES) {
        rt_asset_error_setf(RT_ASSET_ERROR_TOO_LARGE,
                            "TextureAsset3D.LoadKTX2MemoryStrict: invalid payload size");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, (size_t)size, "TextureAsset3D.LoadKTX2MemoryStrict", 1);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Load a KTX2 texture from a filesystem path.
/// @details Returns NULL with a recoverable rt_asset_error when the file is
///          missing, unreadable, malformed, or unsupported; traps only on a
///          NULL/empty path (caller misuse) and allocation failure.
/// @param[in] path Runtime string naming a filesystem KTX2 file.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2(rt_string path) {
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    size_t size = 0;
    uint8_t *data;
    void *asset;

    if (!cpath || !*cpath) {
        rt_trap("TextureAsset3D.LoadKTX2: invalid path");
        return NULL;
    }
    rt_asset_error_begin_load();
    data = textureasset3d_read_file_bytes(cpath, &size);
    if (!data) {
        rt_asset_error_setf(
            RT_ASSET_ERROR_NOT_FOUND, "TextureAsset3D.LoadKTX2: cannot read '%s'", cpath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, size, "TextureAsset3D.LoadKTX2", 0);
    free(data);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Strictly load KTX2 from a filesystem path without checker substitution.
/// @details File acquisition, limits, canonical backing ownership, and recoverable error behavior
///          match the compatibility loader. A partially supported decoder that cannot decode any
///          mip causes a NULL result with an unsupported-data diagnostic.
/// @param[in] path Runtime string naming a filesystem KTX2 file.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2_strict(rt_string path) {
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    size_t size = 0;
    uint8_t *data;
    void *asset;

    if (!cpath || !*cpath) {
        rt_trap("TextureAsset3D.LoadKTX2Strict: invalid path");
        return NULL;
    }
    rt_asset_error_begin_load();
    data = textureasset3d_read_file_bytes(cpath, &size);
    if (!data) {
        rt_asset_error_setf(
            RT_ASSET_ERROR_NOT_FOUND, "TextureAsset3D.LoadKTX2Strict: cannot read '%s'", cpath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, size, "TextureAsset3D.LoadKTX2Strict", 1);
    free(data);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Load a KTX2 texture from a packed asset path (rt_asset registry).
/// @details Returns NULL with a recoverable rt_asset_error when the asset is
///          missing, malformed, or unsupported; traps only on a NULL path.
/// @param[in] path Packed-asset registry path.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2_asset(rt_string path) {
    size_t size = 0;
    uint8_t *data;
    void *asset;

    if (!path) {
        rt_trap("TextureAsset3D.LoadKTX2Asset: invalid path");
        return NULL;
    }
    rt_asset_error_begin_load();
    data = rt_asset_load_raw(path, &size);
    if (!data) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND,
                            "TextureAsset3D.LoadKTX2Asset: asset not found");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, size, "TextureAsset3D.LoadKTX2Asset", 0);
    free(data);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Strictly load KTX2 bytes through the packed-asset registry.
/// @details The raw asset buffer is always released before return. Missing assets and malformed
///          input remain recoverable; recognized blocks outside partial CPU decoder coverage are
///          rejected instead of becoming a checker asset.
/// @param[in] path Packed-asset registry path.
/// @return A newly allocated TextureAsset3D, or NULL on recoverable load failure.
void *rt_textureasset3d_load_ktx2_asset_strict(rt_string path) {
    size_t size = 0;
    uint8_t *data;
    void *asset;

    if (!path) {
        rt_trap("TextureAsset3D.LoadKTX2AssetStrict: invalid path");
        return NULL;
    }
    rt_asset_error_begin_load();
    data = rt_asset_load_raw(path, &size);
    if (!data) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND,
                            "TextureAsset3D.LoadKTX2AssetStrict: asset not found");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    asset = textureasset3d_parse_ktx2(data, size, "TextureAsset3D.LoadKTX2AssetStrict", 1);
    free(data);
    if (asset)
        rt_asset_error_end_load_success();
    else
        rt_asset_error_end_load_failure();
    return asset;
}

/// @brief Return NONE/PARTIAL/FULL CPU fallback quality from the authoritative format table.
/// @param[in] format_name Stable format name to query.
/// @return The matching RT_TEXTUREASSET3D_CPU_SUPPORT_* level, or NONE if absent or ambiguous.
int32_t rt_textureasset3d_cpu_format_support_level(const char *format_name) {
    int32_t support = RT_TEXTUREASSET3D_CPU_SUPPORT_NONE;

    if (!format_name)
        return support;
    for (size_t i = 0; i < sizeof(textureasset3d_format_capabilities) /
                               sizeof(textureasset3d_format_capabilities[0]);
         i++) {
        const textureasset3d_format_info *row = &textureasset3d_format_capabilities[i];
        if (strcmp(row->name, format_name) != 0)
            continue;
        if (support == RT_TEXTUREASSET3D_CPU_SUPPORT_NONE)
            support = row->cpu_support;
        else if (support != row->cpu_support)
            return RT_TEXTUREASSET3D_CPU_SUPPORT_NONE;
    }
    return support;
}

/// @brief Legacy boolean CPU-support query derived from the table's quality value.
/// @param[in] format_name Stable format name to query.
/// @return 1 when the format has partial or full CPU support, otherwise 0.
int8_t rt_textureasset3d_cpu_supports_format(const char *format_name) {
    return rt_textureasset3d_cpu_format_support_level(format_name) !=
                   RT_TEXTUREASSET3D_CPU_SUPPORT_NONE
               ? 1
               : 0;
}

/// @brief Return true when the runtime KTX2 parser can produce CPU texture payloads.
/// @return Always 1 because KTX2 CPU loading is compiled into this runtime.
int8_t rt_textureasset3d_cpu_supports_ktx2(void) {
    return 1;
}

/// @brief Number of rows in the immutable authoritative format table.
/// @return The number of indices accepted by rt_textureasset3d_get_format_capability().
int64_t rt_textureasset3d_get_format_capability_count(void) {
    return (int64_t)(sizeof(textureasset3d_format_capabilities) /
                     sizeof(textureasset3d_format_capabilities[0]));
}

/// @brief Copy one authoritative capability row into caller-owned diagnostics storage.
/// @param[in] index Zero-based row index.
/// @param[out] out_capability Destination cleared before validation and filled on success.
/// @return 1 when @p index and @p out_capability are valid, otherwise 0.
int8_t rt_textureasset3d_get_format_capability(
    int64_t index, rt_textureasset3d_format_capability *out_capability) {
    const textureasset3d_format_info *row;

    if (out_capability)
        memset(out_capability, 0, sizeof(*out_capability));
    if (!out_capability || index < 0 || index >= rt_textureasset3d_get_format_capability_count())
        return 0;
    row = &textureasset3d_format_capabilities[index];
    out_capability->vk_format_first = row->vk_format_first;
    out_capability->vk_format_last = row->vk_format_last;
    out_capability->name = row->name;
    out_capability->compressed = row->compressed;
    out_capability->cpu_support = row->cpu_support;
    out_capability->block_width = row->block_width;
    out_capability->block_height = row->block_height;
    out_capability->block_bytes = row->block_bytes;
    out_capability->native_format_id = row->native_format_id;
    out_capability->backend_capability_bit = row->backend_capability_bit;
    return 1;
}

/// @brief Clear direct-to-backing allocation telemetry for a deterministic next parse.
void rt_textureasset3d_test_reset_supercompression_allocation_telemetry(void) {
    rt_atomic_store_u64(
        &g_textureasset3d_supercompression_final_bytes, UINT64_C(0), __ATOMIC_RELEASE);
    rt_atomic_store_u64(
        &g_textureasset3d_supercompression_peak_bytes, UINT64_C(0), __ATOMIC_RELEASE);
}

/// @brief Return final canonical bytes recorded by the latest supercompressed parse.
/// @return The atomically recorded final canonical-backing byte count.
uint64_t rt_textureasset3d_test_get_supercompression_final_bytes(void) {
    return rt_atomic_load_u64(&g_textureasset3d_supercompression_final_bytes, __ATOMIC_ACQUIRE);
}

/// @brief Return peak live final-destination bytes recorded by that parse.
/// @return The atomically recorded peak destination byte count.
uint64_t rt_textureasset3d_test_get_supercompression_peak_bytes(void) {
    return rt_atomic_load_u64(&g_textureasset3d_supercompression_peak_bytes, __ATOMIC_ACQUIRE);
}

/// @brief Texture width in pixels (0 if the handle is invalid).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Positive base-level width, or 0 for an invalid handle or dimension.
int64_t rt_textureasset3d_get_width(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return (asset && asset->width > 0) ? asset->width : 0;
}

/// @brief Texture height in pixels (0 if the handle is invalid).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Positive base-level height, or 0 for an invalid handle or dimension.
int64_t rt_textureasset3d_get_height(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return (asset && asset->height > 0) ? asset->height : 0;
}

/// @brief Number of mip levels (0 if the handle is invalid).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Validated mip count clamped to the allocated metadata table.
int64_t rt_textureasset3d_get_mip_count(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return textureasset3d_safe_mip_count(asset);
}

/// @brief Runtime format name ("rgba8"/"bc3"/"bc7"/"astc"/"etc2"/"unknown"; "" if invalid).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Runtime string containing the stable format name, or an empty string if invalid.
rt_string rt_textureasset3d_get_format(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return rt_const_cstr((asset && asset->format) ? asset->format : "");
}

/// @brief Whether the texture stores native block-compressed mip payloads.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return 1 for a valid compressed asset, otherwise 0.
int8_t rt_textureasset3d_get_compressed(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return (asset && asset->compressed) ? 1 : 0;
}

/// @brief Whether permissive loading substituted the visible checker fallback.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return 1 for a valid degraded asset, otherwise 0.
int8_t rt_textureasset3d_get_degraded(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return asset && asset->degraded ? 1 : 0;
}

/// @brief Stable degradation reason token, or an empty string for normal/invalid assets.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Runtime string containing the degradation token or an empty string.
rt_string rt_textureasset3d_get_degraded_reason(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return rt_const_cstr(asset && asset->degraded && asset->degraded_reason ? asset->degraded_reason
                                                                            : "");
}

/// @brief First mip index currently marked resident.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Clamped first resident mip index, or 0 for invalid metadata.
int64_t rt_textureasset3d_get_resident_mip_start(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    int64_t safe_mip_count = textureasset3d_safe_mip_count(asset);
    if (!asset || safe_mip_count <= 0 || asset->resident_mip_start < 0)
        return 0;
    if (asset->resident_mip_start > safe_mip_count)
        return safe_mip_count;
    return asset->resident_mip_start;
}

/// @brief Number of mip levels currently marked resident.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Number of levels in a valid resident range, otherwise 0.
int64_t rt_textureasset3d_get_resident_mip_count(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return textureasset3d_resident_range_valid(asset) ? asset->resident_mip_count : 0;
}

/// @brief Total byte size of the currently-resident mips (saturated to INT64_MAX).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Nonnegative resident byte estimate, or 0 for invalid state.
int64_t rt_textureasset3d_get_resident_bytes(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    if (!textureasset3d_resident_range_valid(asset) || asset->resident_bytes < 0)
        return 0;
    return asset->resident_bytes;
}

/// @brief Total byte size of all decoded/native mip payloads retained by the asset.
/// @details This is intentionally different from `ResidentBytes`: the resident
///          range describes the active upload window, while retained bytes reflects
///          process memory held so the upload window can move without reloading.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Nonnegative retained byte estimate, recomputed if cached metadata is invalid.
int64_t rt_textureasset3d_get_retained_bytes(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    if (!asset)
        return 0;
    if (asset->retained_bytes < 0)
        asset->retained_bytes = textureasset3d_compute_retained_bytes(asset);
    return asset->retained_bytes > 0 ? asset->retained_bytes : 0;
}

/// @brief Set the resident mip window (traps on a negative range); see the internal helper.
/// @param[in,out] obj Opaque TextureAsset3D handle whose resident window is replaced.
/// @param[in] first_mip First requested mip level.
/// @param[in] mip_count Requested number of consecutive levels.
void rt_textureasset3d_set_resident_mip_range(void *obj, int64_t first_mip, int64_t mip_count) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    textureasset3d_set_resident_mip_range_internal(
        asset, first_mip, mip_count, "TextureAsset3D.SetResidentMipRange: negative mip range");
}

/// @brief Borrow the decoded Pixels object for the first resident mip (NULL if none/invalid).
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Borrowed decoded Pixels handle for the first resident level, or NULL.
void *rt_textureasset3d_get_pixels(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    if (!textureasset3d_resident_range_valid(asset) || !asset->mip_pixels)
        return NULL;
    return asset->mip_pixels[asset->resident_mip_start];
}

/// @brief Return whether this asset has exact load-time alpha metadata.
/// @details Exact metadata is available when decoded Pixels fallbacks were produced for all mips.
///          Native-only payloads without decoded fallbacks intentionally report unknown so Canvas3D
///          does not assume alpha solely from an alpha-capable compressed format.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return 1 when alpha classification is exact for every loaded mip, otherwise 0.
int8_t rt_textureasset3d_alpha_metadata_known(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return (asset && asset->alpha_metadata_known) ? 1 : 0;
}

/// @brief Return whether exact alpha metadata found any non-opaque texel.
/// @details Returns 0 for both "known opaque" and "unknown"; callers that need to distinguish those
///          states should first call `rt_textureasset3d_alpha_metadata_known`.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return 1 when exact metadata records a non-opaque texel, otherwise 0.
int8_t rt_textureasset3d_has_alpha_texels(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return (asset && asset->alpha_metadata_known && asset->has_alpha_texels) ? 1 : 0;
}

/// @brief Compute the backend texture-cache key for this asset's resident native blocks.
/// @details Folds the asset's cache identity, native revision, and resident mip range into a
///          single hash (FNV offset basis seed, golden-ratio mix per term). Returns 0 when there
///          is nothing to cache (no native payloads or no resident mips); never returns 0
///          otherwise.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Nonzero cache key for a resident native payload, or 0 when none is cacheable.
uint64_t rt_textureasset3d_get_native_cache_key(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    uint64_t signature = 1469598103934665603ull;

    if (!textureasset3d_resident_range_has_native_payload(asset))
        return 0;
    signature ^=
        asset->cache_identity + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    signature ^=
        asset->native_revision + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    signature ^= (uint64_t)asset->resident_mip_start + 0x9e3779b97f4a7c15ull + (signature << 6) +
                 (signature >> 2);
    signature ^= (uint64_t)asset->resident_mip_count + 0x9e3779b97f4a7c15ull + (signature << 6) +
                 (signature >> 2);
    return signature ? signature : 1;
}

/// @brief Return the asset's process-unique cache identity (0 for a non-asset handle).
/// @details Unlike `rt_textureasset3d_get_native_cache_key`, the identity is stable across
///          resident-window and revision changes, so callers tracking per-asset state over
///          time (e.g., Canvas3D texture streaming) can key on it without losing history
///          every time they move the window themselves.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Process-unique stable identity, or 0 for an invalid handle.
uint64_t rt_textureasset3d_get_cache_identity(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return asset ? asset->cache_identity : 0;
}

/// @brief Return the authoritative row's normalized native format id.
/// @details The value is captured at load time from the same table that selected block geometry
///          and fallback decoding, eliminating the former name-based mapping switch.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Native format identifier, or RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE.
int32_t rt_textureasset3d_get_native_format_id(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return asset && asset->compressed ? asset->native_format_id
                                      : RT_TEXTUREASSET3D_NATIVE_FORMAT_NONE;
}

/// @brief Return the same format row's backend capability bit for upload selection.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @return Required backend capability bit, or RT_TEXTUREASSET3D_BACKEND_CAP_NONE.
int64_t rt_textureasset3d_get_native_capability_bit(void *obj) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);
    return asset && asset->compressed ? asset->backend_capability_bit
                                      : RT_TEXTUREASSET3D_BACKEND_CAP_NONE;
}

/// @brief Borrow a retained native compressed mip's bytes and geometry for backend upload.
/// @details Reports the payload pointer, byte size, pixel dimensions, and block geometry for the
///          given absolute @p mip. Only valid for compressed assets with retained native payloads.
/// @param[in] obj Opaque TextureAsset3D handle.
/// @param[in] mip Absolute zero-based mip index.
/// @param[out] out_data Optional destination for the borrowed encoded payload pointer.
/// @param[out] out_bytes Optional destination for the encoded payload byte count.
/// @param[out] out_width Optional destination for the mip width in texels.
/// @param[out] out_height Optional destination for the mip height in texels.
/// @param[out] out_block_width Optional destination for the block footprint width.
/// @param[out] out_block_height Optional destination for the block footprint height.
/// @param[out] out_block_bytes Optional destination for the encoded bytes per block.
/// @return 1 with the out-params set, 0 if the mip is out of range or has no native payload.
/// @note The returned byte pointer remains owned by @p obj and must not be freed or modified.
int rt_textureasset3d_get_native_mip_info(void *obj,
                                          int64_t mip,
                                          const uint8_t **out_data,
                                          uint64_t *out_bytes,
                                          int32_t *out_width,
                                          int32_t *out_height,
                                          int32_t *out_block_width,
                                          int32_t *out_block_height,
                                          int32_t *out_block_bytes) {
    rt_textureasset3d *asset = textureasset3d_checked(obj);

    if (out_data)
        *out_data = NULL;
    if (out_bytes)
        *out_bytes = 0;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
    if (out_block_width)
        *out_block_width = 0;
    if (out_block_height)
        *out_block_height = 0;
    if (out_block_bytes)
        *out_block_bytes = 0;
    int64_t safe_mip_count = textureasset3d_safe_mip_count(asset);
    if (!asset || !asset->compressed || !asset->mip_payloads || !asset->mips || mip < 0 ||
        mip >= safe_mip_count || !asset->mip_payloads[mip] || asset->mips[mip].length == 0 ||
        asset->mips[mip].width == 0 || asset->mips[mip].height == 0 ||
        asset->mips[mip].width > (uint32_t)INT32_MAX ||
        asset->mips[mip].height > (uint32_t)INT32_MAX || asset->block_width <= 0 ||
        asset->block_height <= 0 || asset->block_bytes <= 0)
        return 0;

    if (out_data)
        *out_data = asset->mip_payloads[mip];
    if (out_bytes)
        *out_bytes = asset->mips[mip].length;
    if (out_width)
        *out_width = (int32_t)asset->mips[mip].width;
    if (out_height)
        *out_height = (int32_t)asset->mips[mip].height;
    if (out_block_width)
        *out_block_width = asset->block_width;
    if (out_block_height)
        *out_block_height = asset->block_height;
    if (out_block_bytes)
        *out_block_bytes = asset->block_bytes;
    return 1;
}
