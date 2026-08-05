//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_parse.inc
// Purpose: FBX typed-node storage, bounded binary parsing, and indexed graph queries.
// Key invariants:
//   - Every byte/property/node read is checked against the source and aggregate load budgets.
//   - Query helpers accept missing parents and malformed properties without dereferencing them.
// Ownership/Lifetime:
//   - Parsed nodes and indexes belong to the including loader context until its teardown.
//   - Returned child/property pointers borrow storage from that parsed node tree.
// Links: rt_fbx_loader.c, rt_fbx_loader_geometry.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_parse.inc
 * @brief Implements bounded FBX binary parsing, typed node storage, texture-source
 *        decoding, path hardening, and indexed object lookup.
 *
 * Every binary cursor operation is bounds checked, property/node allocations are
 * charged to the aggregate load context, and compressed arrays are decoded through
 * the strict shared zlib implementation. Texture helpers reject unsafe relative paths
 * and preserve supported encoded source containers. Parsed nodes own their property,
 * child, name, and object-index allocations until recursive teardown.
 */

/*==========================================================================
 * Binary reader helpers
 *=========================================================================*/

/// @brief Bounds-checked cursor and version state for an FBX binary byte stream.
typedef struct {
    /// Borrowed source byte buffer.
    const uint8_t *data;
    /// Total readable source length.
    size_t len;
    /// Current byte offset.
    size_t pos;
    /// FBX file version read from the binary header.
    uint32_t version;
    /// Non-zero for FBX 7500+ 64-bit node headers.
    int is_64bit; /* version >= 7500 */
    /// Sticky malformed-input flag set by failed reads.
    int error;
    /// Per-load budget and telemetry context.
    fbx_load_context_t *context;
} fbx_reader_t;

/// @brief Treat POSIX roots, UNC-style roots, and `C:` prefixes as absolute paths.
/// @param path Candidate native or FBX-authored path.
/// @return Non-zero when @p path has an absolute-root form; otherwise zero.
static int fbx_is_absolute_path(const char *path) {
    if (!path || !*path)
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

/// @brief Copy a path while normalizing separators to `/`.
/// @param[out] dst Destination buffer, always NUL-terminated when its size is nonzero.
/// @param dst_size Capacity of @p dst in bytes.
/// @param src Source path; NULL is treated as an empty string.
static void fbx_normalize_path(char *dst, size_t dst_size, const char *src) {
    size_t di = 0;
    if (!dst || dst_size == 0)
        return;
    if (!src)
        src = "";
    while (*src && di + 1 < dst_size) {
        char ch = *src++;
        dst[di++] = (ch == '\\') ? '/' : ch;
    }
    dst[di] = '\0';
}

/// @brief Normalize a path into a malloc-owned string with `/` separators.
/// @details Used by external texture resolution so long but valid FBX texture references are
///          bounded by `RT_FBX_MAX_TEXTURE_PATH_BYTES` instead of a small stack buffer.
/// @param src Source path; NULL is normalized as an empty string.
/// @return Malloc-owned normalized path, or NULL on allocation failure or overlong input.
static char *fbx_normalize_path_alloc(const char *src) {
    size_t len;
    char *out;
    if (!src)
        src = "";
    len = strlen(src);
    if (len > RT_FBX_MAX_TEXTURE_PATH_BYTES)
        return NULL;
    out = (char *)malloc(len + 1u);
    if (!out)
        return NULL;
    fbx_normalize_path(out, len + 1u, src);
    return out;
}

/// @brief Return the final component of a path without allocating.
/// @param path Path containing either slash style; NULL is accepted.
/// @return Borrowed pointer within @p path after the final separator, or an empty
///         static string when @p path is NULL.
static const char *fbx_path_basename(const char *path) {
    const char *last = path;
    if (!path)
        return "";
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return last;
}

/// @brief Extract the directory portion of @p path into a malloc-owned string.
/// @param path Source file path.
/// @return Directory path using `/` separators, an empty string when no directory exists, or NULL.
static char *fbx_parent_dir_alloc(const char *path) {
    char *normalized;
    char *out;
    const char *last_sep;
    size_t dir_len;
    normalized = fbx_normalize_path_alloc(path);
    if (!normalized)
        return NULL;
    last_sep = strrchr(normalized, '/');
    if (!last_sep) {
        normalized[0] = '\0';
        return normalized;
    }
    dir_len = (size_t)(last_sep - normalized);
    if (dir_len == 0 && normalized[0] == '/')
        dir_len = 1;
    out = (char *)malloc(dir_len + 1u);
    if (!out) {
        free(normalized);
        return NULL;
    }
    memcpy(out, normalized, dir_len);
    out[dir_len] = '\0';
    free(normalized);
    return out;
}

/// @brief Join a directory and leaf filename using `/` into a malloc-owned path.
/// @param dir Directory prefix; NULL or empty returns a normalized copy of @p leaf.
/// @param leaf Non-empty filename or relative path to append.
/// @return Malloc-owned joined path, or NULL for empty/unsafe/overlong input.
static char *fbx_join_path_alloc(const char *dir, const char *leaf) {
    char *clean_leaf;
    char *clean_dir;
    char *out;
    size_t dir_len;
    size_t leaf_len;
    if (!leaf || !*leaf)
        return NULL;
    clean_leaf = fbx_normalize_path_alloc(leaf);
    if (!clean_leaf) {
        rt_asset_error_add_warning("FBX.Load: texture filename is too long; texture skipped");
        return NULL;
    }
    leaf_len = strlen(clean_leaf);
    if (leaf_len == 0 || leaf_len > RT_FBX_MAX_TEXTURE_PATH_BYTES) {
        free(clean_leaf);
        return NULL;
    }
    if (!dir || !*dir) {
        return clean_leaf;
    }
    clean_dir = fbx_normalize_path_alloc(dir);
    if (!clean_dir) {
        free(clean_leaf);
        return NULL;
    }
    dir_len = strlen(clean_dir);
    int needs_sep = dir_len > 0 && clean_dir[dir_len - 1u] != '/';
    if (dir_len > RT_FBX_MAX_TEXTURE_PATH_BYTES || dir_len > SIZE_MAX - leaf_len - 2u ||
        dir_len + (needs_sep ? 1u : 0u) + leaf_len > RT_FBX_MAX_TEXTURE_PATH_BYTES) {
        free(clean_dir);
        free(clean_leaf);
        rt_asset_error_add_warning("FBX.Load: joined texture path is too long; texture skipped");
        return NULL;
    }
    out = (char *)malloc(dir_len + (needs_sep ? 1u : 0u) + leaf_len + 1u);
    if (!out) {
        free(clean_dir);
        free(clean_leaf);
        return NULL;
    }
    memcpy(out, clean_dir, dir_len);
    if (needs_sep)
        out[dir_len++] = '/';
    memcpy(out + dir_len, clean_leaf, leaf_len + 1u);
    free(clean_dir);
    free(clean_leaf);
    return out;
}

/// @brief Sanitize a texture reference into a safe malloc-owned relative path.
/// @details Path-traversal guard: normalizes separators, rejects absolute paths, URI schemes, and
///          any `..` segment, then collapses `.` and redundant slashes into `/`-joined segments.
/// @param src Untrusted texture reference from the FBX document.
/// @return Malloc-owned safe relative path, or NULL if empty, unsafe, overlong, or unavailable.
static char *fbx_normalize_relative_texture_ref_alloc(const char *src) {
    char *normalized;
    char *out;
    const char *p;
    size_t out_len = 0;
    size_t out_cap;
    int wrote_segment = 0;
    if (!src || !*src)
        return NULL;
    normalized = fbx_normalize_path_alloc(src);
    if (!normalized)
        return NULL;
    if (!*normalized || fbx_is_absolute_path(normalized) || strchr(normalized, ':')) {
        free(normalized);
        return NULL;
    }
    for (const char *q = normalized; *q; ++q) {
        unsigned char ch = (unsigned char)*q;
        if (ch < 0x20u || ch == 0x7fu) {
            free(normalized);
            return NULL;
        }
    }
    out_cap = strlen(normalized) + 1u;
    out = (char *)malloc(out_cap);
    if (!out) {
        free(normalized);
        return NULL;
    }
    p = normalized;
    out[0] = '\0';
    while (*p) {
        const char *seg;
        size_t seg_len;
        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        seg_len = (size_t)(p - seg);
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.'))
            continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            free(normalized);
            free(out);
            return NULL;
        }
        if (wrote_segment) {
            if (out_len + 1 >= out_cap) {
                free(normalized);
                free(out);
                return NULL;
            }
            out[out_len++] = '/';
        }
        if (seg_len >= out_cap || out_len > out_cap - 1 - seg_len) {
            free(normalized);
            free(out);
            return NULL;
        }
        memcpy(out + out_len, seg, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
        wrote_segment = 1;
    }
    free(normalized);
    if (!wrote_segment) {
        free(out);
        return NULL;
    }
    return out;
}

/// @brief Return non-zero for URI-style paths such as file:/, http:, or data:.
/// @details A Windows drive prefix (`C:`) is not a URI scheme and is handled as an absolute path.
/// @param path Candidate path or URI string.
/// @return Non-zero when a colon before the first separator forms a URI scheme.
static int fbx_has_uri_scheme(const char *path) {
    const char *p;
    if (!path || !*path)
        return 0;
    for (p = path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            return 0;
        if (*p == ':') {
            if (p == path + 1 && isalpha((unsigned char)path[0]))
                return 0;
            return p > path;
        }
    }
    return 0;
}

/// @brief Return whether absolute FBX texture references may be loaded directly.
/// @details Absolute paths can expose arbitrary local files when loading untrusted FBX assets, so
///          they are disabled by default. Set ZANNA_FBX_ALLOW_ABSOLUTE_TEXTURES=1 to opt in.
/// @return Non-zero when absolute texture loading is explicitly enabled.
static int fbx_absolute_texture_paths_allowed(void) {
    const char *value = getenv("ZANNA_FBX_ALLOW_ABSOLUTE_TEXTURES");
    return value && value[0] == '1' && value[1] == '\0';
}

static void *fbx_decode_texture_payload(const char *name, const uint8_t *data, size_t len);

/// @brief Read one bounded external texture so its exact source bytes can survive import.
/// @param path Native UTF-8 texture file path.
/// @param[out] out_len Receives the exact source length; initialized to zero.
/// @return Newly allocated byte buffer on success, or NULL for invalid input, I/O,
///         size-limit, or read failure. Allocation exhaustion traps.
static uint8_t *fbx_read_texture_bytes(const char *path, size_t *out_len) {
    FILE *file;
    int64_t file_size;
    uint8_t *data;
    if (out_len)
        *out_len = 0;
    if (!path || !out_len)
        return NULL;
    file = rt_file_stdio_open_utf8(path, "rb");
    if (!file)
        return NULL;
    if (rt_file_stdio_seek64(file, 0, SEEK_END) != 0 ||
        (file_size = rt_file_stdio_tell64(file)) <= 0 ||
        (uint64_t)file_size > RT_FBX_MAX_TEXTURE_FILE_BYTES ||
        (uint64_t)file_size > (uint64_t)SIZE_MAX || rt_file_stdio_seek64(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)file_size);
    if (!data) {
        fclose(file);
        rt_trap("FBX.Load: external texture allocation failed");
        return NULL;
    }
    if (fread(data, 1u, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_len = (size_t)file_size;
    return data;
}

/// @brief Decode a bounded external texture while retaining its exact source container.
/// @param path Native UTF-8 texture path used for reading and codec hints.
/// @return Newly allocated Pixels/TextureAsset3D object on success, or NULL when the
///         file cannot be decoded.
static void *fbx_texture_load_cstr(const char *path) {
    uint8_t *data;
    size_t data_len = 0;
    void *texture = NULL;
    data = fbx_read_texture_bytes(path, &data_len);
    if (data) {
        texture = fbx_decode_texture_payload(path, data, data_len);
        free(data);
        if (texture)
            return texture;
    }
    /* Preserve the established decoder fallback for unusual but supported filesystem paths. */
    rt_string runtime_path = rt_const_cstr(path);
    void *pixels = rt_pixels_load(runtime_path);
    rt_string_unref(runtime_path);
    return pixels;
}

/// @brief Resolve and load a local texture reference with a basename fallback.
/// @details URI references are rejected. Absolute references are used only under the
///          explicit environment opt-in; safe relative paths are resolved beside the FBX,
///          followed by a basename-only exporter-compatibility attempt.
/// @param fbx_path Source FBX path whose directory anchors relative references.
/// @param texture_ref Untrusted texture path authored in the FBX.
/// @return Newly allocated decoded texture object on success, or NULL when resolution
///         or decoding fails.
static void *fbx_try_load_texture_path(const char *fbx_path, const char *texture_ref) {
    char *raw_ref = NULL;
    char *normalized_ref = NULL;
    char *dir = NULL;
    char *candidate = NULL;
    const char *basename;
    void *pixels = NULL;

    if (!texture_ref || !*texture_ref)
        return NULL;
    raw_ref = fbx_normalize_path_alloc(texture_ref);
    if (!raw_ref) {
        rt_asset_error_add_warning("FBX.Load: texture reference is too long; texture skipped");
        return NULL;
    }
    if (!*raw_ref || fbx_has_uri_scheme(raw_ref))
        goto done;
    dir = fbx_parent_dir_alloc(fbx_path);
    if (!dir)
        goto done;
    if (fbx_is_absolute_path(raw_ref) && fbx_absolute_texture_paths_allowed()) {
        pixels = fbx_texture_load_cstr(raw_ref);
        if (pixels)
            goto done;
    }

    normalized_ref = fbx_normalize_relative_texture_ref_alloc(raw_ref);
    if (normalized_ref) {
        if (*dir) {
            candidate = fbx_join_path_alloc(dir, normalized_ref);
            if (candidate)
                pixels = fbx_texture_load_cstr(candidate);
        } else {
            pixels = fbx_texture_load_cstr(normalized_ref);
        }
        if (pixels)
            goto done;
        free(candidate);
        candidate = NULL;
    }

    basename = fbx_path_basename(raw_ref);
    if (!basename || !*basename)
        goto done;
    if (normalized_ref && strcmp(basename, normalized_ref) == 0 && !*dir)
        goto done;

    if (*dir) {
        candidate = fbx_join_path_alloc(dir, basename);
        if (!candidate)
            goto done;
    } else {
        candidate = fbx_normalize_path_alloc(basename);
        if (!candidate)
            goto done;
    }
    pixels = fbx_texture_load_cstr(candidate);

done:
    free(candidate);
    free(dir);
    free(normalized_ref);
    free(raw_ref);
    return pixels;
}

/// @brief Case-insensitive test (ASCII) for whether @p text ends with @p suffix.
/// @param text Candidate NUL-terminated ASCII string.
/// @param suffix Suffix to compare.
/// @return Non-zero on a case-insensitive suffix match; otherwise zero.
static int fbx_ascii_ends_with_i(const char *text, const char *suffix) {
    size_t text_len;
    size_t suffix_len;
    if (!text || !suffix)
        return 0;
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len)
        return 0;
    text += text_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

/// @brief Perform a case-insensitive ASCII substring search.
/// @param text Haystack string.
/// @param needle Substring to locate; an empty string always matches.
/// @return Non-zero when @p needle occurs in @p text; otherwise zero.
static int fbx_ascii_contains_i(const char *text, const char *needle) {
    size_t needle_len;
    if (!text || !needle)
        return 0;
    needle_len = strlen(needle);
    if (needle_len == 0)
        return 1;
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_len)
            return 1;
    }
    return 0;
}

/// @brief Compare two ASCII strings case-insensitively.
/// @param a First string.
/// @param b Second string.
/// @return Non-zero when both non-NULL strings are equal ignoring ASCII case.
static int fbx_ascii_equals_i(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

/// @brief Build a Pixels object from a width×height RGBA32 buffer (copied), with
///   overflow-checked sizing.
/// @param rgba32 Source packed RGBA pixel buffer.
/// @param width Positive image width in pixels.
/// @param height Positive image height in pixels.
/// @return Newly allocated Pixels object on success, or NULL for invalid dimensions,
///         arithmetic overflow, or allocation failure.
static void *fbx_pixels_from_rgba32(uint32_t *rgba32, int64_t width, int64_t height) {
    rt_pixels_impl *pixels;
    size_t pixel_count;
    size_t pixel_bytes;
    if (!rgba32 || width <= 0 || height <= 0)
        return NULL;
    if ((uint64_t)width > SIZE_MAX || (uint64_t)height > SIZE_MAX)
        return NULL;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return NULL;
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t))
        return NULL;
    pixel_bytes = pixel_count * sizeof(uint32_t);
    pixels = pixels_alloc(width, height);
    if (!pixels)
        return NULL;
    memcpy(pixels->data, rgba32, pixel_bytes);
    if (pixel_count > 0)
        pixels_touch(pixels);
    return pixels;
}

/// @brief Sniff a PNG payload by its eight-byte signature.
/// @param data Candidate encoded bytes.
/// @param len Number of readable bytes.
/// @return Non-zero when the PNG signature matches.
static int fbx_payload_looks_png(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    return data && len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

/// @brief Sniff a JPEG payload by its 0xFFD8 start-of-image marker.
/// @param data Candidate encoded bytes.
/// @param len Number of readable bytes.
/// @return Non-zero when the JPEG start-of-image marker matches.
static int fbx_payload_looks_jpeg(const uint8_t *data, size_t len) {
    return data && len >= 2u && data[0] == 0xff && data[1] == 0xd8;
}

/// @brief Sniff a GIF payload by its "GIF87a"/"GIF89a" header.
/// @param data Candidate encoded bytes.
/// @param len Number of readable bytes.
/// @return Non-zero when a supported GIF version header matches.
static int fbx_payload_looks_gif(const uint8_t *data, size_t len) {
    return data && len >= 6u && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0);
}

/// @brief Identify supported encoded texture bytes independently of filename hints.
/// @param data Candidate encoded bytes.
/// @param len Number of readable bytes.
/// @return Static lowercase container name for KTX2, PNG, JPEG, GIF, or BMP;
///         otherwise NULL.
static const char *fbx_texture_container_kind(const uint8_t *data, size_t len) {
    static const uint8_t ktx2_magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    if (data && len >= sizeof(ktx2_magic) && memcmp(data, ktx2_magic, sizeof(ktx2_magic)) == 0)
        return "ktx2";
    if (fbx_payload_looks_png(data, len))
        return "png";
    if (data && len >= 4u && data[0] == 0xFFu && data[1] == 0xD8u && data[2] == 0xFFu)
        return "jpeg";
    if (fbx_payload_looks_gif(data, len))
        return "gif";
    if (data && len >= 14u && data[0] == 'B' && data[1] == 'M')
        return "bmp";
    return NULL;
}

/// @brief Wrap decoded Pixels with exact encoded source bytes.
/// @details When @p kind is known, consumes the caller's @p pixels reference whether
///          wrapping succeeds or fails. With an unknown kind, returns @p pixels unchanged.
/// @param pixels Decoded Pixels object.
/// @param data Original encoded payload bytes.
/// @param len Encoded payload length.
/// @param kind Static container identifier, or NULL.
/// @return Newly allocated TextureAsset3D wrapper, the original Pixels object when
///         @p kind is NULL, or NULL on wrapper failure.
static void *fbx_preserve_texture_source(void *pixels,
                                         const uint8_t *data,
                                         size_t len,
                                         const char *kind) {
    void *texture;
    if (!pixels || !kind)
        return pixels;
    texture = rt_textureasset3d_wrap_encoded_pixels(pixels, data, (uint64_t)len, kind);
    if (!texture) {
        fbx_release_ref(&pixels);
        return NULL;
    }
    fbx_release_ref(&pixels);
    return texture;
}

/// @brief Decode an embedded texture payload into a Pixels object, selecting the codec by
///   magic bytes or by @p name's extension.
/// @param name Optional untrusted filename hint used only when signatures are insufficient.
/// @param data Encoded image payload.
/// @param len Number of encoded bytes.
/// @return Newly allocated Pixels or TextureAsset3D object on success, or NULL when
///         the container is unsupported or decoding fails.
static void *fbx_decode_texture_payload(const char *name, const uint8_t *data, size_t len) {
    uint32_t *rgba32 = NULL;
    int64_t width = 0;
    int64_t height = 0;
    void *pixels = NULL;
    const char *kind;
    if (!data || len == 0)
        return NULL;
    kind = fbx_texture_container_kind(data, len);
    if (kind && strcmp(kind, "ktx2") == 0)
        return rt_textureasset3d_load_ktx2_memory(data, (uint64_t)len);
    if (fbx_payload_looks_png(data, len) || fbx_ascii_ends_with_i(name, ".png")) {
        if (rt_png_decode_buffer_rgba32(data, len, &rgba32, &width, &height))
            pixels = fbx_pixels_from_rgba32(rgba32, width, height);
        free(rgba32);
        return fbx_preserve_texture_source(pixels, data, len, kind);
    }
    if (fbx_payload_looks_jpeg(data, len) || fbx_ascii_ends_with_i(name, ".jpg") ||
        fbx_ascii_ends_with_i(name, ".jpeg")) {
        if (rt_jpeg_decode_buffer_rgba32(data, len, &rgba32, &width, &height))
            pixels = fbx_pixels_from_rgba32(rgba32, width, height);
        free(rgba32);
        return fbx_preserve_texture_source(pixels, data, len, kind);
    }
    if (fbx_payload_looks_gif(data, len) || fbx_ascii_ends_with_i(name, ".gif")) {
        int gif_width = 0;
        int gif_height = 0;
        if (rt_gif_decode_memory_first_rgba32(data, len, &rgba32, &gif_width, &gif_height))
            pixels = fbx_pixels_from_rgba32(rgba32, (int64_t)gif_width, (int64_t)gif_height);
        free(rgba32);
        return fbx_preserve_texture_source(pixels, data, len, kind);
    }
    if ((kind && strcmp(kind, "bmp") == 0) || fbx_ascii_ends_with_i(name, ".bmp")) {
        pixels = rt_asset_decode_typed("embedded.bmp", data, len);
        return fbx_preserve_texture_source(pixels, data, len, kind);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// FBX is little-endian; v >= 7500 promotes lengths/offsets to 64-bit
// (`is_64bit` flag). Short reads are marked as hard parse errors so corrupt
// assets do not surface as partially imported runtime content.
// ---------------------------------------------------------------------------

/// @brief Test whether a reader cursor has consumed the entire input.
/// @param r Reader state.
/// @return Non-zero when @p r is positioned at or beyond its source length.
static int fbx_eof(const fbx_reader_t *r) {
    return r->pos >= r->len;
}

/// @brief Ensure a requested byte count remains in the reader.
/// @param[in,out] r Reader whose sticky error/cursor state is updated on failure.
/// @param n Number of bytes required from the current cursor.
/// @return Non-zero when the bytes are available; otherwise zero after marking an error
///         and moving the cursor to EOF.
static int fbx_require(fbx_reader_t *r, size_t n) {
    if (!r || r->error)
        return 0;
    if (r->pos > r->len || n > r->len - r->pos) {
        r->error = 1;
        r->pos = r->len;
        return 0;
    }
    return 1;
}

/// @brief Read one unsigned byte.
/// @param[in,out] r Reader cursor.
/// @return Decoded byte, or zero after marking a short read.
static uint8_t fbx_u8(fbx_reader_t *r) {
    if (!fbx_require(r, 1))
        return 0;
    return r->data[r->pos++];
}

/// @brief Read a little-endian unsigned 16-bit integer.
/// @param[in,out] r Reader cursor.
/// @return Decoded value, or zero after marking a short read.
static uint16_t fbx_u16(fbx_reader_t *r) {
    if (!fbx_require(r, 2))
        return 0;
    uint16_t v = (uint16_t)r->data[r->pos] | ((uint16_t)r->data[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

/// @brief Read a little-endian unsigned 32-bit integer.
/// @param[in,out] r Reader cursor.
/// @return Decoded value, or zero after marking a short read.
static uint32_t fbx_u32(fbx_reader_t *r) {
    if (!fbx_require(r, 4))
        return 0;
    uint32_t v = (uint32_t)r->data[r->pos] | ((uint32_t)r->data[r->pos + 1] << 8) |
                 ((uint32_t)r->data[r->pos + 2] << 16) | ((uint32_t)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

/// @brief Read a little-endian signed 32-bit integer.
/// @param[in,out] r Reader cursor.
/// @return Decoded value, or zero after marking a short read.
static int32_t fbx_i32(fbx_reader_t *r) {
    return (int32_t)fbx_u32(r);
}

/// @brief Read a little-endian uint64 (composed of two u32 halves).
/// @param[in,out] r Reader cursor.
/// @return Decoded value, or zero after marking a short read.
static uint64_t fbx_u64(fbx_reader_t *r) {
    if (!fbx_require(r, 8))
        return 0;
    uint64_t lo = fbx_u32(r);
    uint64_t hi = fbx_u32(r);
    return lo | (hi << 32);
}

/// @brief Read a little-endian int64.
/// @param[in,out] r Reader cursor.
/// @return Decoded value, or zero after marking a short read.
static int64_t fbx_i64(fbx_reader_t *r) {
    return (int64_t)fbx_u64(r);
}

/// @brief Read an IEEE 754 single-precision float (memcpy bit-pattern; safe for aliasing).
/// @param[in,out] r Reader cursor.
/// @return Decoded float bit pattern, or positive zero after marking a short read.
static float fbx_f32(fbx_reader_t *r) {
    uint32_t bits = fbx_u32(r);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/// @brief Read an IEEE 754 double-precision float.
/// @param[in,out] r Reader cursor.
/// @return Decoded double bit pattern, or positive zero after marking a short read.
static double fbx_f64(fbx_reader_t *r) {
    uint64_t bits = fbx_u64(r);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

/// @brief Advance a reader cursor by a checked byte count.
/// @param[in,out] r Reader cursor and sticky error state.
/// @param n Number of bytes to skip.
static void fbx_skip(fbx_reader_t *r, size_t n) {
    if (!fbx_require(r, n))
        return;
    r->pos += n;
}

/*==========================================================================
 * FBX node tree
 *=========================================================================*/

/* Maximum children of one FBX record. Exporters place one record per object under a single
 * `Objects` node, so mocap animation packages routinely run to tens of thousands: a 218-clip
 * package measures 67,056 (39,783 AnimationCurve + 26,596 AnimationCurveNode + 218 stacks +
 * 218 layers + rig records). This bound only keeps a single array within int32 and one
 * allocation sane; aggregate retained memory stays governed by the per-load byte budget
 * (`fbx_budget_charge`, `RT_FBX_DEFAULT_LOAD_BUDGET_BYTES`), which every reserved slot is
 * charged against before allocation. */
#define FBX_MAX_CHILDREN 1048576
#define FBX_MAX_NODE_DEPTH 512
#define FBX_MAX_NODE_PROPS 4096
#define FBX_MAX_PROPERTY_BYTES (64u * 1024u * 1024u)
#define FBX_MAX_COMPRESSED_ARRAY_BYTES (256u * 1024u * 1024u)
#define FBX_MAX_DECOMPRESSED_ARRAY_BYTES (256u * 1024u * 1024u)

/// @brief Owned typed value for one parsed FBX node property.
typedef struct {
    /// FBX scalar/string/raw/array type code.
    char type; /* C/Y/I/L/F/D/S/R/b/i/l/f/d */

    /// Storage selected by @ref type.
    union {
        /// Boolean (`C`) scalar.
        int8_t bool_val;
        /// Signed 16-bit (`Y`) scalar.
        int16_t i16;
        /// Signed 32-bit (`I`) scalar.
        int32_t i32;
        /// Signed 64-bit (`L`) scalar.
        int64_t i64;
        /// Single-precision (`F`) scalar.
        float f32;
        /// Double-precision (`D`) scalar.
        double f64;

        /// Owned string (`S`) payload and explicit byte length.
        struct {
            /// NUL-terminated allocation; embedded NUL bytes may precede the terminator.
            char *str;
            /// Authored payload length, excluding the synthesized terminator.
            uint32_t len;
        } string;

        /// Owned untyped raw (`R`) payload.
        struct {
            /// Exact raw bytes, or NULL for an empty payload.
            uint8_t *data;
            /// Number of raw bytes.
            uint32_t len;
        } raw;

        /// Owned decoded lowercase-array payload.
        struct {
            /// Native-endian element buffer.
            void *data;
            /// Number of elements.
            uint32_t count;
            /// Original lowercase FBX element-type code.
            char elem_type;
        } array;
    } v;
} fbx_prop_t;

struct fbx_object_index;

/// @brief Owned parsed FBX node with growable typed properties and child nodes.
typedef struct fbx_node {
    /// NUL-terminated structural node name.
    char *name;
    /// Exact structural-name byte length.
    uint32_t name_len;
    /// Owned property array.
    fbx_prop_t *props;
    /// Number of initialized properties.
    int32_t prop_count;
    /// Allocated property capacity.
    int32_t prop_capacity;
    /// Owned child-node array.
    struct fbx_node *children;
    /// Number of initialized children.
    int32_t child_count;
    /// Allocated child capacity.
    int32_t child_capacity;
    /// Optional object-ID hash index owned by the top-level `Objects` node.
    struct fbx_object_index *object_index;
} fbx_node_t;

static fbx_node_t *fbx_find_object_by_id(fbx_node_t *objects, int64_t id);
static void fbx_free_object_index(struct fbx_object_index *index);

/// @brief Assign an exact, NUL-free structural name to an empty FBX node.
/// @details Structural node names (`Objects`, `Geometry`, `Vertices`, and similar) are carried with
///          an explicit byte length even though a trailing NUL is allocated for existing query
///          helpers. The allocation is charged before it is attempted and embedded NUL bytes are
///          rejected so binary and ASCII trees obey the same lookup rules.
/// @param node Destination node whose current name must be NULL.
/// @param data Name bytes; may be NULL only when @p length is zero.
/// @param length Exact byte count, excluding the synthesized trailing NUL.
/// @param context Active load context charged for the allocation.
/// @return Nonzero on success, zero on invalid input, overflow, budget exhaustion, or allocation
///         failure.
static int fbx_node_set_name(fbx_node_t *node,
                             const char *data,
                             size_t length,
                             fbx_load_context_t *context) {
    char *name;
    if (!node || node->name || length > UINT32_MAX || (length > 0 && !data) ||
        (length > 0 && memchr(data, '\0', length) != NULL) || length == SIZE_MAX ||
        !fbx_budget_charge(context, (uint64_t)length + 1u, 1u)) {
        return 0;
    }
    name = (char *)malloc(length + 1u);
    if (!name)
        return 0;
    if (length > 0)
        memcpy(name, data, length);
    name[length] = '\0';
    node->name = name;
    node->name_len = (uint32_t)length;
    return 1;
}

/// @brief Grow a node's child array while charging only newly reserved slots.
/// @details Capacity grows geometrically up to @ref FBX_MAX_CHILDREN. Existing child addresses may
///          change, so callers must obtain the returned array slot only after this helper succeeds.
///          Newly exposed storage is cleared to make failure cleanup deterministic.
/// @param node Node whose child storage should accommodate one more entry.
/// @param context Active load context charged for the capacity delta.
/// @return Nonzero when at least one unused child slot is available.
static int fbx_node_reserve_child(fbx_node_t *node, fbx_load_context_t *context) {
    int32_t new_capacity;
    fbx_node_t *grown;
    size_t old_bytes;
    size_t new_bytes;
    if (!node)
        return 0;
    if (node->child_count < node->child_capacity)
        return 1;
    if (node->child_count >= FBX_MAX_CHILDREN) {
        if (context)
            context->structural_limit_exceeded = 1;
        return 0;
    }
    if (node->child_count < 0 || node->child_capacity < 0 || node->child_capacity > INT32_MAX / 2) {
        return 0;
    }
    new_capacity = node->child_capacity == 0 ? 8 : node->child_capacity * 2;
    if (new_capacity > FBX_MAX_CHILDREN)
        new_capacity = FBX_MAX_CHILDREN;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown) ||
        !fbx_budget_charge(
            context, (uint64_t)(new_capacity - node->child_capacity), sizeof(*grown))) {
        return 0;
    }
    old_bytes = (size_t)node->child_capacity * sizeof(*grown);
    new_bytes = (size_t)new_capacity * sizeof(*grown);
    grown = (fbx_node_t *)realloc(node->children, new_bytes);
    if (!grown)
        return 0;
    memset((uint8_t *)grown + old_bytes, 0, new_bytes - old_bytes);
    node->children = grown;
    node->child_capacity = new_capacity;
    return 1;
}

/// @brief Grow a node's property array while charging only newly reserved slots.
/// @details The binary parser normally reserves its exact declared count; the ASCII parser uses
///          this geometric helper because a line's property count is not known until tokenization.
///          Newly exposed slots are cleared for safe recursive rollback.
/// @param node Node whose property storage should accommodate one more entry.
/// @param context Active load context charged for the capacity delta.
/// @return Nonzero when at least one unused property slot is available.
static int fbx_node_reserve_property(fbx_node_t *node, fbx_load_context_t *context) {
    int32_t new_capacity;
    fbx_prop_t *grown;
    size_t old_bytes;
    size_t new_bytes;
    if (!node)
        return 0;
    if (node->prop_count < node->prop_capacity)
        return 1;
    if (node->prop_count >= FBX_MAX_NODE_PROPS) {
        if (context)
            context->structural_limit_exceeded = 1;
        return 0;
    }
    if (node->prop_count < 0 || node->prop_capacity < 0 || node->prop_capacity > INT32_MAX / 2) {
        return 0;
    }
    new_capacity = node->prop_capacity == 0 ? 4 : node->prop_capacity * 2;
    if (new_capacity > FBX_MAX_NODE_PROPS)
        new_capacity = FBX_MAX_NODE_PROPS;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown) ||
        !fbx_budget_charge(
            context, (uint64_t)(new_capacity - node->prop_capacity), sizeof(*grown))) {
        return 0;
    }
    old_bytes = (size_t)node->prop_capacity * sizeof(*grown);
    new_bytes = (size_t)new_capacity * sizeof(*grown);
    grown = (fbx_prop_t *)realloc(node->props, new_bytes);
    if (!grown)
        return 0;
    memset((uint8_t *)grown + old_bytes, 0, new_bytes - old_bytes);
    node->props = grown;
    node->prop_capacity = new_capacity;
    return 1;
}

/*==========================================================================
 * Array decompression (strict shared RFC 1950 decoder)
 *=========================================================================*/

/// @brief Inflate a zlib-wrapped FBX array property to raw bytes.
///
/// FBX wraps array data in standard zlib. The shared decoder validates CMF/FLG,
/// dictionary prohibition, exact DEFLATE consumption/output length, and Adler-32,
/// while writing directly into the one budgeted destination allocation.
/// @param data Compressed zlib-wrapped bytes.
/// @param comp_len Number of compressed bytes.
/// @param count Declared element count.
/// @param elem_size Byte width of one decoded element.
/// @param[in,out] context Loader context charged for the exact output buffer.
/// @return Newly-allocated buffer (caller `free`s) or NULL on failure.
static void *fbx_decompress_array(const uint8_t *data,
                                  uint32_t comp_len,
                                  uint32_t count,
                                  uint32_t elem_size,
                                  fbx_load_context_t *context) {
    size_t expected;
    uint8_t *inflated;
    if (comp_len < 6)
        return NULL;
    if (comp_len > FBX_MAX_COMPRESSED_ARRAY_BYTES)
        return NULL;
    if (!rt_untrusted_count_ok((int64_t)count, (size_t)elem_size, FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
        return NULL;
    expected = (size_t)count * (size_t)elem_size;
    if (!fbx_budget_charge(context, expected ? expected : 1u, 1u))
        return NULL;
    inflated = (uint8_t *)malloc(expected ? expected : 1u);
    if (!inflated)
        return NULL;
    if (!rt_compress_inflate_zlib_into(data, comp_len, inflated, expected)) {
        free(inflated);
        return NULL;
    }
    return inflated;
}

/*==========================================================================
 * Property parsing
 *=========================================================================*/

/// @brief Parse one FBX node property into `*prop`.
///
/// FBX property types are encoded as a single ASCII type-byte:
///   - Y/I/L/F/D — scalar int16/int32/int64/float/double
///   - C/b      — bool/raw-byte
///   - S/R      — string / raw byte array
///   - lowercase i/l/f/d — array (with optional zlib compression)
/// Arrays carry a (count, encoding, compressed_len) header before
/// the data; if `encoding == 1` we hand the bytes to
/// `fbx_decompress_array`.
/// @param[in,out] r Binary reader positioned at the property type byte.
/// @param[out] prop Zeroed destination that takes ownership of any decoded allocation.
/// @return Zero on success, or -1 for malformed input, an unknown type, budget
///         rejection, or allocation/decompression failure.
static int fbx_parse_property(fbx_reader_t *r, fbx_prop_t *prop) {
    if (fbx_eof(r))
        return -1;
    prop->type = (char)fbx_u8(r);
    if (r->error)
        return -1;
    switch (prop->type) {
        case 'C':
            prop->v.bool_val = (int8_t)fbx_u8(r);
            break;
        case 'Y':
            prop->v.i16 = (int16_t)fbx_u16(r);
            break;
        case 'I':
            prop->v.i32 = fbx_i32(r);
            break;
        case 'L':
            prop->v.i64 = fbx_i64(r);
            break;
        case 'F':
            prop->v.f32 = fbx_f32(r);
            break;
        case 'D':
            prop->v.f64 = fbx_f64(r);
            break;
        case 'S':
        case 'R': {
            uint32_t len = fbx_u32(r);
            if (len > FBX_MAX_PROPERTY_BYTES)
                return -1;
            if (!fbx_require(r, len))
                return -1;
            if (prop->type == 'S') {
                if (!fbx_budget_charge(r->context, (uint64_t)len + 1u, 1u))
                    return -1;
                prop->v.string.str = (char *)malloc(len + 1);
                if (!prop->v.string.str)
                    return -1;
                memcpy(prop->v.string.str, r->data + r->pos, len);
                prop->v.string.str[len] = '\0';
                prop->v.string.len = len;
            } else {
                /* Only allocate for a non-empty payload: malloc(0) may return NULL, which
                 * would be indistinguishable from OOM. A zero-length raw property keeps a
                 * NULL data pointer with len 0 (readers guard on len), mirroring the string
                 * path which always allocates len+1. */
                prop->v.raw.data = NULL;
                if (len > 0) {
                    if (!fbx_budget_charge(r->context, len, 1u))
                        return -1;
                    prop->v.raw.data = (uint8_t *)malloc(len);
                    if (!prop->v.raw.data)
                        return -1;
                    memcpy(prop->v.raw.data, r->data + r->pos, len);
                }
                prop->v.raw.len = len;
            }
            fbx_skip(r, len);
            break;
        }
        case 'b':
        case 'i':
        case 'l':
        case 'f':
        case 'd': {
            uint32_t count = fbx_u32(r);
            uint32_t encoding = fbx_u32(r);
            uint32_t comp_len = fbx_u32(r);
            if (!fbx_require(r, comp_len))
                return -1;

            uint32_t elem_size = 0;
            switch (prop->type) {
                case 'b':
                    elem_size = 1;
                    break;
                case 'i':
                    elem_size = 4;
                    break;
                case 'l':
                    elem_size = 8;
                    break;
                case 'f':
                    elem_size = 4;
                    break;
                case 'd':
                    elem_size = 8;
                    break;
            }

            prop->v.array.count = count;
            prop->v.array.elem_type = prop->type;
            if (!rt_untrusted_count_ok((int64_t)count,
                                       (size_t)elem_size,
                                       encoding == 0 ? (size_t)comp_len
                                                     : FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
                return -1;
            size_t expected = (size_t)count * (size_t)elem_size;
            if (encoding == 1) {
                prop->v.array.data =
                    fbx_decompress_array(r->data + r->pos, comp_len, count, elem_size, r->context);
                if (count > 0 && !prop->v.array.data)
                    return -1;
            } else if (encoding == 0) {
                if (comp_len != expected)
                    return -1;
                if (!fbx_budget_charge(r->context, expected ? expected : 1u, 1u))
                    return -1;
                prop->v.array.data = malloc(expected ? expected : 1u);
                if (expected > 0 && !prop->v.array.data)
                    return -1;
                if (expected > 0)
                    memcpy(prop->v.array.data, r->data + r->pos, expected);
            } else {
                return -1;
            }
            fbx_skip(r, comp_len);
            break;
        }
        default:
            return -1;
    }
    return 0;
}

/*==========================================================================
 * Node parsing (recursive)
 *=========================================================================*/

/// @brief Release any heap allocation owned by a parsed property.
/// @param[in,out] p Property whose string, raw, or array payload is released.
static void fbx_free_prop(fbx_prop_t *p) {
    switch (p->type) {
        case 'S':
            free(p->v.string.str);
            break;
        case 'R':
            free(p->v.raw.data);
            break;
        case 'b':
        case 'i':
        case 'l':
        case 'f':
        case 'd':
            free(p->v.array.data);
            break;
        default:
            break;
    }
}

/// @brief Recursively release an FBX node tree and reset its root node.
/// @param[in,out] n Node whose properties, children, name, and object index are released;
///                  NULL is accepted as a no-op.
static void fbx_free_node(fbx_node_t *n) {
    if (!n)
        return;
    for (int32_t i = 0; i < n->prop_count; i++)
        fbx_free_prop(&n->props[i]);
    free(n->props);
    for (int32_t i = 0; i < n->child_count; i++)
        fbx_free_node(&n->children[i]);
    free(n->children);
    free(n->name);
    fbx_free_object_index(n->object_index);
    memset(n, 0, sizeof(*n));
}

/// @brief Parse one FBX node (header + properties + children) at the cursor.
///
/// Each node header carries `end_offset`, `prop_count`,
/// `prop_list_len`, and a name. After consuming the properties we
/// recurse on child nodes until we reach the sentinel "null record"
/// (an all-zero header). The `is_64bit` flag widens the offset
/// fields from 4 to 8 bytes for FBX 7500+.
/// @param[in,out] r Reader positioned at a non-sentinel node header.
/// @param[out] node Destination node, initialized before parsing.
/// @param depth Current recursion depth used to enforce @ref FBX_MAX_NODE_DEPTH.
/// @return Zero on success, or -1 when the node is malformed, exceeds limits/budget,
///         or cannot be allocated.
static int fbx_parse_node(fbx_reader_t *r, fbx_node_t *node, int depth) {
    uint8_t encoded_name_len;
    uint64_t props_start;
    uint64_t props_end;

    if (depth > FBX_MAX_NODE_DEPTH) {
        if (r && r->context)
            r->context->structural_limit_exceeded = 1;
        return -1;
    }
    memset(node, 0, sizeof(fbx_node_t));

    uint64_t end_offset = r->is_64bit ? fbx_u64(r) : fbx_u32(r);
    uint64_t num_props = r->is_64bit ? fbx_u64(r) : fbx_u32(r);
    uint64_t prop_list_len = r->is_64bit ? fbx_u64(r) : fbx_u32(r);

    encoded_name_len = fbx_u8(r);
    if (r->error)
        return -1;

    if (end_offset == 0 && num_props == 0 && encoded_name_len == 0)
        return -1; /* null record (sentinel) */

    if (end_offset > r->len || end_offset < r->pos)
        return -1;
    if (!fbx_require(r, encoded_name_len))
        return -1;
    if (!fbx_node_set_name(node, (const char *)(r->data + r->pos), encoded_name_len, r->context)) {
        return -1;
    }
    fbx_skip(r, encoded_name_len);

    props_start = (uint64_t)r->pos;
    if (props_start > end_offset || prop_list_len > end_offset - props_start)
        return -1;
    props_end = props_start + prop_list_len;
    if (props_end > r->len)
        return -1;
    if (num_props > FBX_MAX_NODE_PROPS) {
        if (r->context)
            r->context->structural_limit_exceeded = 1;
        return -1;
    }
    if ((size_t)num_props > SIZE_MAX / sizeof(fbx_prop_t))
        return -1;
    if (num_props > 0 && !rt_untrusted_count_ok((int64_t)num_props, 1u, (size_t)prop_list_len))
        return -1;

    /* Parse properties */
    if (num_props > 0) {
        if (!fbx_budget_charge(r->context, num_props, sizeof(fbx_prop_t)))
            return -1;
        node->props = (fbx_prop_t *)calloc((size_t)num_props, sizeof(fbx_prop_t));
        if (!node->props)
            return -1;
        node->prop_capacity = (int32_t)num_props;
        for (uint64_t i = 0; i < num_props; i++) {
            if ((uint64_t)r->pos >= props_end) {
                node->prop_count = (int32_t)i;
                return -1;
            }
            if (fbx_parse_property(r, &node->props[i]) < 0) {
                node->prop_count = (int32_t)i;
                return -1;
            }
            if ((uint64_t)r->pos > props_end) {
                node->prop_count = (int32_t)(i + 1);
                return -1;
            }
        }
        node->prop_count = (int32_t)num_props;
    }
    if ((uint64_t)r->pos < props_end)
        fbx_skip(r, (size_t)(props_end - (uint64_t)r->pos));
    if (r->error || (uint64_t)r->pos != props_end)
        return -1;

    /* Parse children (until end_offset or null record) */
    while (r->pos < (size_t)end_offset && !fbx_eof(r)) {
        /* Check for null record sentinel */
        size_t sentinel_size = r->is_64bit ? 25 : 13;
        if (r->pos + sentinel_size <= r->len) {
            int is_null = 1;
            for (size_t i = 0; i < sentinel_size; i++)
                if (r->data[r->pos + i] != 0) {
                    is_null = 0;
                    break;
                }
            if (is_null) {
                fbx_skip(r, sentinel_size);
                break;
            }
        }

        if (!fbx_node_reserve_child(node, r->context))
            return -1;

        fbx_node_t *child = &node->children[node->child_count];
        if (fbx_parse_node(r, child, depth + 1) < 0) {
            fbx_free_node(child);
            return -1;
        }
        node->child_count++;
    }

    /* Ensure we're at end_offset */
    if ((uint64_t)r->pos > end_offset)
        return -1;
    if (r->pos < (size_t)end_offset)
        r->pos = (size_t)end_offset;

    return 0;
}

/*==========================================================================
 * Node tree query helpers
 *=========================================================================*/

/// @brief Linear-search `parent->children` for the first child named `name`.
/// FBX node trees are small (few hundred nodes) so the O(n) cost is fine.
/// @param parent Parent node whose direct children are searched.
/// @param name Exact case-sensitive structural name.
/// @return Borrowed pointer to the first matching child, or NULL.
static fbx_node_t *fbx_find_child(fbx_node_t *parent, const char *name) {
    if (!parent || !name)
        return NULL;
    for (int32_t i = 0; i < parent->child_count; i++)
        if (strcmp(parent->children[i].name, name) == 0)
            return &parent->children[i];
    return NULL;
}

/// @brief Coerce an integer-like property to signed 64-bit.
/// @param node Node containing the property.
/// @param idx Zero-based property index.
/// @return Converted `Y`, `I`, `L`, or `C` value, or zero for invalid input/type.
static int64_t fbx_prop_i64(fbx_node_t *node, int idx) {
    if (!node || idx < 0 || idx >= node->prop_count)
        return 0;
    fbx_prop_t *p = &node->props[idx];
    switch (p->type) {
        case 'L':
            return p->v.i64;
        case 'I':
            return p->v.i32;
        case 'Y':
            return p->v.i16;
        case 'C':
            return p->v.bool_val;
        default:
            return 0;
    }
}

/// @brief Coerce a numeric property to double.
/// @param node Node containing the property.
/// @param idx Zero-based property index.
/// @return Converted scalar value, or zero for invalid input/type.
static double fbx_prop_f64(fbx_node_t *node, int idx) {
    if (!node || idx < 0 || idx >= node->prop_count)
        return 0.0;
    fbx_prop_t *p = &node->props[idx];
    if (p->type == 'D')
        return p->v.f64;
    if (p->type == 'F')
        return p->v.f32;
    if (p->type == 'L')
        return (double)p->v.i64;
    if (p->type == 'I')
        return (double)p->v.i32;
    if (p->type == 'Y')
        return (double)p->v.i16;
    if (p->type == 'C')
        return (double)p->v.bool_val;
    return 0.0;
}

static const char *fbx_prop_str(fbx_node_t *node, int idx);

/// @brief Coerce property `idx` of `node` to a boolean-like value.
/// @details FBX exporters encode flags as `C`, integer, or occasionally float properties inside
///   `Properties70`. Treats non-zero numeric values and string "true"/"1" as enabled.
/// @param node Node containing the property.
/// @param idx Zero-based property index.
/// @return Non-zero when the supported numeric/string encoding is truthy; otherwise zero.
static int fbx_prop_truthy(fbx_node_t *node, int idx) {
    fbx_prop_t *p;
    const char *text;
    if (!node || idx < 0 || idx >= node->prop_count)
        return 0;
    p = &node->props[idx];
    switch (p->type) {
        case 'C':
            return p->v.bool_val ? 1 : 0;
        case 'Y':
        case 'I':
        case 'L':
            return fbx_prop_i64(node, idx) != 0;
        case 'F':
        case 'D':
            return fabs(fbx_prop_f64(node, idx)) > 1e-12;
        case 'S':
            text = fbx_prop_str(node, idx);
            return (strcmp(text, "true") == 0 || strcmp(text, "True") == 0 ||
                    strcmp(text, "TRUE") == 0 || strcmp(text, "1") == 0);
        default:
            return 0;
    }
}

/// @brief Obtain a borrowed C-string view of a string property.
/// @param node Node containing the property.
/// @param idx Zero-based property index.
/// @return Borrowed NUL-terminated string for an `S` property, or an empty static string.
static const char *fbx_prop_str(fbx_node_t *node, int idx) {
    if (!node || idx < 0 || idx >= node->prop_count)
        return "";
    fbx_prop_t *p = &node->props[idx];
    if (p->type == 'S' && p->v.string.str)
        return p->v.string.str;
    return "";
}

/// @brief Strip the `Namespace::` prefix from an FBX object name and copy the remainder
/// into `out`. FBX stores names as `Model::Hips`, `Geometry::CubeMesh`, etc., where the
/// prefix is loader metadata that authors in Blender / Maya / Houdini never see. Walks
/// the full string to find the *last* `::` so names that legitimately contain double
/// colons in a sub-namespace still strip correctly. NULL-safe (treats NULL input as
/// empty), output is always NUL-terminated, truncates when the stripped name doesn't
/// fit in `out_size`.
/// @param raw_name NUL-terminated authored object name; NULL is treated as empty.
/// @param[out] out Destination buffer for the decoded display name.
/// @param out_size Capacity of @p out in bytes.
static void fbx_decode_object_name(const char *raw_name, char *out, size_t out_size) {
    const char *start = raw_name ? raw_name : "";
    const char *end = start;
    const char *scope = NULL;
    size_t len;

    if (!out || out_size == 0)
        return;
    while (*end)
        end++;
    for (const char *p = start; p + 1 < end; p++) {
        if (p[0] == ':' && p[1] == ':')
            scope = p;
    }
    if (scope)
        start = scope + 2;
    len = (size_t)(end - start);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

/// @brief Borrow an exact display-name span from a length-carrying FBX string property.
/// @details The first embedded NUL terminates the human-readable portion (binary FBX commonly
///          appends a NUL/type suffix). The last `::` namespace separator before that terminator is
///          stripped. No fixed buffer or C-string scan is used, so long equal-prefix names remain
///          distinct and malformed suffix bytes cannot cause an overread.
/// @param node Node containing the object-name property.
/// @param property_index String property index, normally one.
/// @param out_data Receives a borrowed pointer into the property allocation.
/// @param out_len Receives the exact stripped display-name byte count.
/// @return Nonzero when a non-empty string property view was produced.
static int fbx_object_name_view(fbx_node_t *node,
                                int property_index,
                                const char **out_data,
                                size_t *out_len) {
    const char *data;
    size_t len;
    size_t readable_len;
    size_t start = 0;

    if (out_data)
        *out_data = "";
    if (out_len)
        *out_len = 0;
    if (!node || !out_data || !out_len || property_index < 0 ||
        property_index >= node->prop_count || node->props[property_index].type != 'S' ||
        !node->props[property_index].v.string.str) {
        return 0;
    }
    data = node->props[property_index].v.string.str;
    len = node->props[property_index].v.string.len;
    readable_len = 0;
    while (readable_len < len && data[readable_len] != '\0')
        readable_len++;
    for (size_t i = 0; i + 1u < readable_len; i++) {
        if (data[i] == ':' && data[i + 1u] == ':')
            start = i + 2u;
    }
    *out_data = data + start;
    *out_len = readable_len - start;
    return *out_len > 0;
}

/// @brief Create an exact-length runtime string for one FBX object's decoded display name.
/// @details See @ref fbx_object_name_view for namespace and embedded-NUL handling. The returned
///          string follows ordinary runtime-string ownership and must be unreferenced by callers.
/// @param node Source FBX object node.
/// @param property_index Name property index.
/// @return Runtime string containing the exact display name, or an owned empty string.
static rt_string fbx_object_name_string(fbx_node_t *node, int property_index) {
    const char *data = "";
    size_t len = 0;
    (void)fbx_object_name_view(node, property_index, &data, &len);
    return rt_string_from_bytes(data, len);
}

/// @brief One open-addressed numeric object-id slot.
typedef struct {
    /// Numeric FBX object identifier.
    int64_t id;
    /// Borrowed direct child of the indexed `Objects` node.
    fbx_node_t *node;
    /// Non-zero when this slot terminates neither an empty probe nor initialization.
    int8_t occupied;
} fbx_object_index_slot_t;

/// @brief Load-local numeric object-id index owned by the parsed `Objects` node.
struct fbx_object_index {
    /// Owned power-of-two open-addressing table.
    fbx_object_index_slot_t *slots;
    /// Number of allocated slots.
    size_t capacity;
    /// Borrowed load context used for lookup-probe telemetry.
    fbx_load_context_t *context;
};

/// @brief Mix one signed FBX object id into a uniformly distributed 64-bit hash.
/// @param id Numeric FBX object id.
/// @return Deterministic mixed hash value.
static uint64_t fbx_hash_object_id(int64_t id) {
    uint64_t value = (uint64_t)id;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

/// @brief Release an object-ID index without touching the nodes it borrows.
/// @param[in,out] index Index allocation to release; NULL is accepted as a no-op.
static void fbx_free_object_index(struct fbx_object_index *index) {
    if (!index)
        return;
    free(index->slots);
    free(index);
}

/// @brief Build the load-local numeric-id index for direct children of `Objects`.
/// @details Capacity is a power of two at no more than 50% load. Duplicate IDs are rejected rather
///          than making object identity depend on traversal order. Nodes without an integer-like
///          first property are not indexable and remain available to ordinary typed traversal.
/// @param objects Parsed top-level `Objects` node.
/// @param context Active load context charged before both index allocations.
/// @return Nonzero on success, zero on duplicate IDs, overflow, budget exhaustion, or allocation
///         failure.
static int fbx_build_object_index(fbx_node_t *objects, fbx_load_context_t *context) {
    struct fbx_object_index *index;
    size_t indexed_count = 0;
    size_t capacity = 8u;

    if (!objects || objects->object_index)
        return objects && objects->object_index;
    for (int32_t i = 0; i < objects->child_count; i++) {
        fbx_node_t *node = &objects->children[i];
        if (node->prop_count > 0 && (node->props[0].type == 'L' || node->props[0].type == 'I' ||
                                     node->props[0].type == 'Y' || node->props[0].type == 'C')) {
            indexed_count++;
        }
    }
    while (capacity < indexed_count * 2u) {
        if (capacity > SIZE_MAX / 2u)
            return 0;
        capacity *= 2u;
    }
    if (!fbx_budget_charge(context, 1u, sizeof(*index)) ||
        !fbx_budget_charge(context, capacity, sizeof(fbx_object_index_slot_t))) {
        return 0;
    }
    index = (struct fbx_object_index *)calloc(1, sizeof(*index));
    if (!index)
        return 0;
    index->slots = (fbx_object_index_slot_t *)calloc(capacity, sizeof(*index->slots));
    if (!index->slots) {
        free(index);
        return 0;
    }
    index->capacity = capacity;
    index->context = context;
    for (int32_t i = 0; i < objects->child_count; i++) {
        fbx_node_t *node = &objects->children[i];
        size_t slot;
        int64_t id;
        if (node->prop_count <= 0 || (node->props[0].type != 'L' && node->props[0].type != 'I' &&
                                      node->props[0].type != 'Y' && node->props[0].type != 'C')) {
            continue;
        }
        id = fbx_prop_i64(node, 0);
        slot = (size_t)fbx_hash_object_id(id) & (capacity - 1u);
        while (index->slots[slot].occupied) {
            if (index->slots[slot].id == id) {
                fbx_free_object_index(index);
                return 0;
            }
            slot = (slot + 1u) & (capacity - 1u);
        }
        index->slots[slot].occupied = 1;
        index->slots[slot].id = id;
        index->slots[slot].node = node;
    }
    objects->object_index = index;
    return 1;
}

/// @brief Resolve an object through the load-local numeric-id hash index.
/// @details A linear fallback is retained only for isolated synthetic callers that did not build
///          an index; production loads always build one before extraction. Probe telemetry counts
///          each examined hash slot.
/// @param objects Parsed `Objects` node.
/// @param id Numeric FBX object id.
/// @return Borrowed matching node, or NULL.
static fbx_node_t *fbx_find_object_by_id(fbx_node_t *objects, int64_t id) {
    struct fbx_object_index *index;
    size_t slot;
    if (!objects)
        return NULL;
    index = objects->object_index;
    if (!index || !index->slots || index->capacity == 0) {
        for (int32_t i = 0; i < objects->child_count; i++) {
            fbx_node_t *obj = &objects->children[i];
            if (obj->prop_count >= 1 && fbx_prop_i64(obj, 0) == id)
                return obj;
        }
        return NULL;
    }
    slot = (size_t)fbx_hash_object_id(id) & (index->capacity - 1u);
    for (size_t probes = 0; probes < index->capacity; probes++) {
        fbx_object_index_slot_t *entry = &index->slots[slot];
        fbx_record_lookup_probes(index->context, 1u);
        if (!entry->occupied)
            return NULL;
        if (entry->id == id)
            return entry->node;
        slot = (slot + 1u) & (index->capacity - 1u);
    }
    return NULL;
}

#include "rt_fbx_loader_connections.inc"
