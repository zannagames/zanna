//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_io.c
// Purpose: Image I/O: BMP load/save, GIF load, and format dispatch.
// Key invariants:
//   - Decode failures return NULL and report asset diagnostics.
//   - Generic loading detects PNG, JPEG, BMP, and GIF from file signatures,
//     not extensions.
//   - BMP decoding accepts only uncompressed 24-bit BITMAPINFOHEADER data,
//     bounds decoded pixels, and normalizes top-down or bottom-up BGR rows to
//     opaque canonical RGBA.
//   - BMP saving discards source alpha, writes a bottom-up 24-bit stream to a
//     sibling temporary file, and replaces the destination only after a
//     complete flush and close.
//   - Save paths keep their historical integer success/failure contract.
// Ownership/Lifetime:
//   - Loaded Pixels objects are GC-managed and owned by the caller.
//   - Temporary decode buffers are released before return.
// Links: src/runtime/graphics/2d/rt_pixels.h (public API),
//        src/runtime/graphics/2d/rt_pixels_io_internal.h (codec helpers),
//        src/runtime/graphics/2d/rt_pixels_png.c (PNG codec),
//        src/runtime/graphics/2d/rt_pixels_jpeg.c (JPEG codec),
//        src/runtime/graphics/3d/assets/rt_asset_error.h (load diagnostics)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BMP I/O, first-frame GIF loading, and image dispatch.
///
/// Loaders own one asset-error context apiece so delegated generic loads retain
/// the decoder's precise diagnostic. File parsing uses explicit little-endian
/// fields and checked offsets rather than host structure layout. Save commits
/// are transactional at the destination-path level.

#include "rt_pixels_io_internal.h"

#include "rt_asset_error.h"
#include "rt_file_stdio.h"
#include "rt_trap.h"

#define BMP_MAX_PIXELS ((size_t)64u * 1024u * 1024u)

//=============================================================================
// BMP Image I/O
//=============================================================================

// BMP file format structures (packed)
#pragma pack(push, 1)

/// @brief In-memory values from the 14-byte BMP file header.
/// @details Serialization and parsing are performed field by field, so the
///          packed layout is descriptive rather than used for raw file I/O.
typedef struct bmp_file_header {
    uint8_t magic[2];     // 'B', 'M'
    uint32_t file_size;   // Total file size
    uint16_t reserved1;   // 0
    uint16_t reserved2;   // 0
    uint32_t data_offset; // Offset to pixel data
} bmp_file_header;

/// @brief In-memory values from a 40-byte Windows `BITMAPINFOHEADER`.
/// @details Only the uncompressed, single-plane, 24-bit subset is accepted by
///          this translation unit.
typedef struct bmp_info_header {
    uint32_t header_size;      // 40 for BITMAPINFOHEADER
    int32_t width;             // Image width
    int32_t height;            // Image height (positive = bottom-up)
    uint16_t planes;           // 1
    uint16_t bit_count;        // Bits per pixel (24 for RGB)
    uint32_t compression;      // 0 = BI_RGB (uncompressed)
    uint32_t image_size;       // Can be 0 for uncompressed
    int32_t x_pels_per_meter;  // Horizontal resolution
    int32_t y_pels_per_meter;  // Vertical resolution
    uint32_t colors_used;      // 0 = default
    uint32_t colors_important; // 0 = all
} bmp_info_header;

#pragma pack(pop)

/// @brief Read a little-endian unsigned 16-bit integer from a byte buffer.
///
/// @param p Pointer to at least two bytes.
/// @return The decoded host-endian value.
static uint16_t bmp_read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/// @brief Read a little-endian unsigned 32-bit integer from a byte buffer.
///
/// @param p Pointer to at least four bytes.
/// @return The decoded host-endian value.
static uint32_t bmp_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Read a little-endian signed 32-bit integer from a byte buffer.
///
/// @param p Pointer to at least four bytes.
/// @return The decoded host-endian signed value.
static int32_t bmp_read_i32_le(const uint8_t *p) {
    return (int32_t)bmp_read_u32_le(p);
}

/// @brief Store a 16-bit value in little-endian byte order.
///
/// @param p Pointer to at least two output bytes.
/// @param value Host-endian value to encode.
static void bmp_write_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

/// @brief Store a 32-bit value in little-endian byte order.
///
/// @param p Pointer to at least four output bytes.
/// @param value Host-endian value to encode.
static void bmp_write_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/// @brief Store a signed 32-bit value in little-endian byte order.
///
/// @param p Pointer to at least four output bytes.
/// @param value Host-endian signed value to encode.
static void bmp_write_i32_le(uint8_t *p, int32_t value) {
    bmp_write_u32_le(p, (uint32_t)value);
}

/// @brief Read a BMP file header without depending on host struct layout or endian.
///
/// @param f Open binary stream positioned at the file header.
/// @param out Receives the decoded header on success.
/// @return 1 on success, 0 on short read or invalid arguments.
static int bmp_read_file_header(FILE *f, bmp_file_header *out) {
    uint8_t buf[14];
    if (!f || !out || !px_read_exact(f, buf, sizeof(buf)))
        return 0;
    out->magic[0] = buf[0];
    out->magic[1] = buf[1];
    out->file_size = bmp_read_u32_le(buf + 2);
    out->reserved1 = bmp_read_u16_le(buf + 6);
    out->reserved2 = bmp_read_u16_le(buf + 8);
    out->data_offset = bmp_read_u32_le(buf + 10);
    return 1;
}

/// @brief Read a BMP BITMAPINFOHEADER without raw-struct deserialization.
///
/// @param f Open binary stream positioned at the info header.
/// @param out Receives the decoded header on success.
/// @return 1 on success, 0 on short read or invalid arguments.
static int bmp_read_info_header(FILE *f, bmp_info_header *out) {
    uint8_t buf[40];
    if (!f || !out || !px_read_exact(f, buf, sizeof(buf)))
        return 0;
    out->header_size = bmp_read_u32_le(buf + 0);
    out->width = bmp_read_i32_le(buf + 4);
    out->height = bmp_read_i32_le(buf + 8);
    out->planes = bmp_read_u16_le(buf + 12);
    out->bit_count = bmp_read_u16_le(buf + 14);
    out->compression = bmp_read_u32_le(buf + 16);
    out->image_size = bmp_read_u32_le(buf + 20);
    out->x_pels_per_meter = bmp_read_i32_le(buf + 24);
    out->y_pels_per_meter = bmp_read_i32_le(buf + 28);
    out->colors_used = bmp_read_u32_le(buf + 32);
    out->colors_important = bmp_read_u32_le(buf + 36);
    return 1;
}

/// @brief Write a BMP file header in canonical little-endian byte order.
///
/// @param f Open binary stream positioned where the file header should be written.
/// @param hdr Header values to encode.
/// @return 1 on success, 0 on write failure or invalid arguments.
static int bmp_write_file_header(FILE *f, const bmp_file_header *hdr) {
    uint8_t buf[14];
    if (!f || !hdr)
        return 0;
    buf[0] = hdr->magic[0];
    buf[1] = hdr->magic[1];
    bmp_write_u32_le(buf + 2, hdr->file_size);
    bmp_write_u16_le(buf + 6, hdr->reserved1);
    bmp_write_u16_le(buf + 8, hdr->reserved2);
    bmp_write_u32_le(buf + 10, hdr->data_offset);
    return px_write_exact(f, buf, sizeof(buf));
}

/// @brief Write a BMP BITMAPINFOHEADER in canonical little-endian byte order.
///
/// @param f Open binary stream positioned where the info header should be written.
/// @param hdr Header values to encode.
/// @return 1 on success, 0 on write failure or invalid arguments.
static int bmp_write_info_header(FILE *f, const bmp_info_header *hdr) {
    uint8_t buf[40];
    if (!f || !hdr)
        return 0;
    bmp_write_u32_le(buf + 0, hdr->header_size);
    bmp_write_i32_le(buf + 4, hdr->width);
    bmp_write_i32_le(buf + 8, hdr->height);
    bmp_write_u16_le(buf + 12, hdr->planes);
    bmp_write_u16_le(buf + 14, hdr->bit_count);
    bmp_write_u32_le(buf + 16, hdr->compression);
    bmp_write_u32_le(buf + 20, hdr->image_size);
    bmp_write_i32_le(buf + 24, hdr->x_pels_per_meter);
    bmp_write_i32_le(buf + 28, hdr->y_pels_per_meter);
    bmp_write_u32_le(buf + 32, hdr->colors_used);
    bmp_write_u32_le(buf + 36, hdr->colors_important);
    return px_write_exact(f, buf, sizeof(buf));
}

/// @brief Decode a 24-bit uncompressed BMP file into a Pixels object.
///
/// Supports the most common BMP variant: BITMAPINFOHEADER, 24bpp,
/// no compression. Handles both bottom-up (positive height — the
/// historical default) and top-down (negative height) row layouts.
/// Source rows are 4-byte aligned; we strip the BGR→RGBA conversion
/// in the inner loop and force alpha = 255 since 24bpp BMP has no
/// alpha channel. Caps width/height at 32768 to bound memory.
/// @param path Runtime string handle naming the UTF-8 input path; null or an
///        invalid string traps and closes the load context as failed.
/// @return GC-managed `rt_pixels_impl*` on success, NULL on any
///         failure (file open, magic mismatch, unsupported variant,
///         short read).
void *rt_pixels_load_bmp(void *path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadBmp: path must not be null");
        return NULL;
    }

    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadBmp: invalid path");
        return NULL;
    }

    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Pixels.LoadBmp: '%s' not found", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    uint8_t *row_buf = NULL;
    rt_pixels_impl *pixels = NULL;
    int success = 0;

    // Read file header
    bmp_file_header file_hdr;
    if (!bmp_read_file_header(f, &file_hdr))
        goto bmp_cleanup;

    // Check magic
    if (file_hdr.magic[0] != 'B' || file_hdr.magic[1] != 'M')
        goto bmp_cleanup;

    // Read info header
    bmp_info_header info_hdr;
    if (!bmp_read_info_header(f, &info_hdr))
        goto bmp_cleanup;

    // Only support BITMAPINFOHEADER, 24-bit, uncompressed BMP.
    if (info_hdr.header_size != sizeof(bmp_info_header) || info_hdr.bit_count != 24 ||
        info_hdr.compression != 0)
        goto bmp_cleanup;

    int32_t width = info_hdr.width;
    int32_t height = info_hdr.height;
    int bottom_up = 1;

    // Handle negative height (top-down)
    if (height < 0) {
        if (height == INT32_MIN)
            goto bmp_cleanup;
        height = -height;
        bottom_up = 0;
    }

    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
        goto bmp_cleanup;
    if ((size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > BMP_MAX_PIXELS)
        goto bmp_cleanup;

    // Calculate row padding (rows must be 4-byte aligned)
    size_t row_payload = 0;
    if (!px_mul_size((size_t)width, 3, &row_payload))
        goto bmp_cleanup;
    size_t row_size = (row_payload + 3u) & ~(size_t)3u;
    size_t data_size = 0;
    if (!px_mul_size(row_size, (size_t)height, &data_size))
        goto bmp_cleanup;

    uint64_t min_data_offset = (uint64_t)sizeof(bmp_file_header) + (uint64_t)info_hdr.header_size;
    uint64_t data_offset = (uint64_t)file_hdr.data_offset;
    if (data_offset < min_data_offset || data_offset > (uint64_t)INT64_MAX)
        goto bmp_cleanup;
    if (file_hdr.file_size != 0) {
        uint64_t declared_size = (uint64_t)file_hdr.file_size;
        if (data_offset > declared_size || (uint64_t)data_size > declared_size - data_offset)
            goto bmp_cleanup;
    }
    int64_t current_pos = px_ftell(f);
    if (current_pos < 0)
        goto bmp_cleanup;
    if (px_fseek(f, 0, SEEK_END) != 0)
        goto bmp_cleanup;
    int64_t actual_size = px_ftell(f);
    if (actual_size < 0)
        goto bmp_cleanup;
    if (file_hdr.file_size != 0 && (uint64_t)file_hdr.file_size > (uint64_t)actual_size)
        goto bmp_cleanup;
    if (data_offset > (uint64_t)actual_size ||
        (uint64_t)data_size > (uint64_t)actual_size - data_offset)
        goto bmp_cleanup;
    if (px_fseek(f, current_pos, SEEK_SET) != 0)
        goto bmp_cleanup;

    // Allocate row buffer
    row_buf = (uint8_t *)malloc(row_size);
    if (!row_buf)
        goto bmp_cleanup;

    // Create pixels
    pixels = pixels_alloc(width, height);
    if (!pixels)
        goto bmp_cleanup;

    // Seek to pixel data
    if (px_fseek(f, (int64_t)file_hdr.data_offset, SEEK_SET) != 0)
        goto bmp_cleanup;

    // Read pixel data
    for (int32_t y = 0; y < height; y++) {
        if (!px_read_exact(f, row_buf, row_size))
            goto bmp_cleanup;

        // Determine destination row (bottom-up reverses row order)
        int32_t dst_y = bottom_up ? (height - 1 - y) : y;
        uint32_t *dst_row = pixels->data + dst_y * width;

        // Convert BGR to RGBA
        for (int32_t x = 0; x < width; x++) {
            uint8_t b = row_buf[x * 3 + 0];
            uint8_t g = row_buf[x * 3 + 1];
            uint8_t r = row_buf[x * 3 + 2];
            // Pack as 0xRRGGBBAA (alpha = 255 for opaque)
            dst_row[x] = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
        }
    }
    success = 1;

bmp_cleanup:
    if (!success && pixels) {
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        pixels = NULL;
    }
    free(row_buf);
    fclose(f);
    if (pixels) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_setf_if_empty(
            RT_ASSET_ERROR_CORRUPT, "Pixels.LoadBmp: '%s' is not a supported BMP", filepath);
        rt_asset_error_end_load_failure();
    }
    return pixels;
}

/// @brief Save a Pixels object as a 24-bit uncompressed BMP file.
///
/// Writes a standard bottom-up BMP (positive height). Refuses
/// oversized images where the resulting file would overflow the
/// 32-bit `bfSize` field. Pads each row to a 4-byte multiple and discards the
/// source alpha channel. Output is written to a temporary sibling and replaces
/// the requested path only after all bytes are flushed and the stream closes;
/// failed attempts unlink that temporary file.
/// @param pixels Opaque source Pixels handle. Null returns zero; a nonnull
///        invalid handle traps and returns zero.
/// @param path Runtime string handle naming the UTF-8 destination. Null or an
///        invalid string returns zero without creating a file.
/// @return 1 on success, 0 on any failure (open, oversized, write).
int64_t rt_pixels_save_bmp(void *pixels, void *path) {
    if (!pixels || !path)
        return 0;

    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.SaveBmp: invalid pixels");
    if (!p)
        return 0;
    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath)
        return 0;

    if (p->width <= 0 || p->height <= 0 || p->width > INT32_MAX || p->height > INT32_MAX)
        return 0;

    int32_t width = (int32_t)p->width;
    int32_t height = (int32_t)p->height;

    // Calculate row padding
    size_t row_payload = 0;
    if (!px_mul_size((size_t)width, 3, &row_payload))
        return 0;
    size_t row_size = (row_payload + 3u) & ~(size_t)3u;
    size_t padding = row_size - row_payload;

    // Calculate file size (guard against uint32 overflow for very large images)
    uint64_t data_size_u64 = (uint64_t)row_size * (uint64_t)height;
    if (data_size_u64 > (uint64_t)0xFFFFFFC9u) // UINT32_MAX - 54
        return 0;
    uint32_t data_size = (uint32_t)data_size_u64;
    uint32_t file_size = 54 + data_size; // 14 + 40 + data

    char *tmp_path = NULL;
    FILE *f = rt_file_stdio_open_temp_for_replace_utf8(filepath, &tmp_path);
    if (!f)
        return 0;
    uint8_t *row_buf = NULL;
    int result = 0;

    // Write file header
    bmp_file_header file_hdr = {
        .magic = {'B', 'M'},
        .file_size = file_size,
        .reserved1 = 0,
        .reserved2 = 0,
        .data_offset = 54,
    };
    if (!bmp_write_file_header(f, &file_hdr))
        goto bmp_save_cleanup;

    // Write info header
    bmp_info_header info_hdr = {
        .header_size = 40,
        .width = width,
        .height = height, // Positive = bottom-up
        .planes = 1,
        .bit_count = 24,
        .compression = 0,
        .image_size = data_size,
        .x_pels_per_meter = 2835, // ~72 DPI
        .y_pels_per_meter = 2835,
        .colors_used = 0,
        .colors_important = 0,
    };
    if (!bmp_write_info_header(f, &info_hdr))
        goto bmp_save_cleanup;

    // Allocate row buffer
    row_buf = (uint8_t *)calloc(1, row_size);
    if (!row_buf)
        goto bmp_save_cleanup;

    // Write pixel data (bottom-up)
    for (int32_t y = height - 1; y >= 0; y--) {
        uint32_t *src_row = p->data + y * width;

        // Convert RGBA to BGR
        for (int32_t x = 0; x < width; x++) {
            uint32_t pixel = src_row[x];
            // Pixel format is 0xRRGGBBAA
            row_buf[x * 3 + 0] = (uint8_t)((pixel >> 8) & 0xFF);  // B
            row_buf[x * 3 + 1] = (uint8_t)((pixel >> 16) & 0xFF); // G
            row_buf[x * 3 + 2] = (uint8_t)((pixel >> 24) & 0xFF); // R
        }

        // Zero padding bytes
        for (size_t i = 0; i < padding; i++)
            row_buf[row_payload + i] = 0;

        if (!px_write_exact(f, row_buf, row_size)) {
            goto bmp_save_cleanup;
        }
    }

    result = 1;

bmp_save_cleanup:
    free(row_buf);
    if (!rt_file_stdio_flush_sync_close(f))
        result = 0;
    if (result) {
        result = rt_file_stdio_replace_utf8(tmp_path, filepath) ? 1 : 0;
    }
    if (!result && tmp_path)
        (void)rt_file_stdio_unlink_utf8(tmp_path);
    free(tmp_path);
    return result;
}

//=============================================================================
// GIF Loading (first frame convenience wrapper)
//=============================================================================

#include "rt_gif.h"

/// @brief Decode a GIF file into a Pixels object (first frame only).
///
/// LZW-decompresses the indexed-color image stream and resolves
/// each index against the Global Color Table. Always reads only
/// the first frame — animated GIFs collapse to their initial
/// snapshot. Used for sprite-sheet and texture loading; for full
/// animation use the dedicated GIF decoder elsewhere.
/// @details Ownership of the first decoded frame's Pixels handle transfers to
///          the caller. Every additional frame handle and the decoder-owned
///          frame array are released before return. The loader distinguishes
///          missing files from corrupt or empty decoder output in the active
///          asset diagnostic.
/// @param path Runtime string handle naming the UTF-8 input path; null or an
///        invalid string traps.
/// @return Pixels on success, NULL on file/format failure.
void *rt_pixels_load_gif(void *path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadGif: path must not be null");
        return NULL;
    }

    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadGif: invalid path");
        return NULL;
    }

    gif_frame_t *frames = NULL;
    int frame_count = 0, w = 0, h = 0;
    if (gif_decode_file(filepath, &frames, &frame_count, &w, &h) <= 0) {
        FILE *probe = rt_file_stdio_open_utf8(filepath, "rb");
        if (probe) {
            fclose(probe);
            rt_asset_error_setf(
                RT_ASSET_ERROR_CORRUPT, "Pixels.LoadGif: '%s' is not a supported GIF", filepath);
        } else {
            rt_asset_error_setf(
                RT_ASSET_ERROR_NOT_FOUND, "Pixels.LoadGif: '%s' not found", filepath);
        }
        rt_asset_error_end_load_failure();
        return NULL;
    }

    // Defend against a decoder that returns a positive status but no usable frame.
    if (!frames || frame_count < 1 || !frames[0].pixels) {
        if (frames) {
            for (int i = 0; i < frame_count; i++) {
                if (frames[i].pixels && rt_obj_release_check0(frames[i].pixels))
                    rt_obj_free(frames[i].pixels);
            }
            free(frames);
        }
        rt_asset_error_setf(
            RT_ASSET_ERROR_CORRUPT, "Pixels.LoadGif: '%s' produced no frames", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    // Take ownership of the first frame's Pixels, free the rest
    void *result = frames[0].pixels;
    for (int i = 1; i < frame_count; i++) {
        // Release extra frames (GC-managed)
        if (frames[i].pixels) {
            if (rt_obj_release_check0(frames[i].pixels))
                rt_obj_free(frames[i].pixels);
        }
    }
    free(frames);
    rt_asset_error_end_load_success();
    return result;
}

//=============================================================================
// Auto-detect loader
//=============================================================================

/// @brief Load a supported image by inspecting its file signature.
/// @details Opens the UTF-8 path once to probe up to eight bytes, then
///          delegates PNG, JPEG, BMP, or GIF data to the corresponding
///          format loader. Delegates own their asset-error contexts; wrapper
///          failures create a context only for null or invalid paths, missing
///          or unreadable files, and unrecognized magic. File extensions are
///          ignored.
/// @param path Runtime string handle naming the UTF-8 input path; null or an
///        invalid string traps.
/// @return A caller-owned GC-managed Pixels handle, or null with an asset
///         diagnostic on probe or decode failure.
void *rt_pixels_load(void *path) {
    /* Do NOT open a load context spanning the delegation below: each format loader
     * (rt_pixels_load_png/jpeg/bmp/gif) opens and closes its OWN begin/end load
     * context. Wrapping the delegation in another begin/end nests them, so the
     * delegate's inner end_load clears the state the outer end then misreports.
     * Each wrapper-own error therefore gets its own begin/end pair, and the
     * dispatch path returns the delegate result directly. */
    if (!path) {
        rt_asset_error_begin_load();
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.Load: path must not be null");
        return NULL;
    }

    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath) {
        rt_asset_error_begin_load();
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.Load: invalid path");
        return NULL;
    }

    FILE *af = rt_file_stdio_open_utf8(filepath, "rb");
    if (!af) {
        rt_asset_error_begin_load();
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Pixels.Load: '%s' not found", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    uint8_t hdr[8];
    size_t n = fread(hdr, 1, 8, af);
    int read_error = ferror(af) != 0;
    fclose(af);
    if (read_error) {
        rt_asset_error_begin_load();
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Pixels.Load: failed to read '%s'", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (n >= 8 && hdr[0] == 137 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G')
        return rt_pixels_load_png(path);
    if (n >= 2 && hdr[0] == 0xFF && hdr[1] == 0xD8)
        return rt_pixels_load_jpeg(path);
    if (n >= 2 && hdr[0] == 'B' && hdr[1] == 'M')
        return rt_pixels_load_bmp(path);
    if (n >= 3 && hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F')
        return rt_pixels_load_gif(path);

    rt_asset_error_begin_load();
    rt_asset_error_setf(
        RT_ASSET_ERROR_BAD_MAGIC, "Pixels.Load: '%s' is not a supported image", filepath);
    rt_asset_error_end_load_failure();
    return NULL;
}
