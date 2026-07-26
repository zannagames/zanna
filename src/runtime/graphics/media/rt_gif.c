//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_gif.c
// Purpose: GIF87a/89a decoder with LZW decompression and multi-frame animation.
// Key invariants:
//   - LZW code table limited to 4096 entries (12-bit codes maximum)
//   - Interlaced images use 4-pass row reordering
//   - Disposal method 3 (restore-to-previous) saves/restores canvas state
//   - All output pixels are RGBA (0xRRGGBBAA); transparency via alpha=0
// Ownership/Lifetime:
//   - Each decoded frame produces a GC-managed Pixels object via pixels_alloc
//   - The gif_frame_t array itself is malloc'd; caller frees it
// Links: src/runtime/graphics/media/rt_gif.h (public API),
//        src/runtime/graphics/2d/rt_pixels_internal.h (Pixels allocation)
//
//===----------------------------------------------------------------------===//

#include "rt_gif.h"

#include "rt_bytes.h"
#include "rt_file_ext.h"
#include "rt_object.h"
#include "rt_pixels_internal.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// GIF file reader
//===----------------------------------------------------------------------===//

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} gif_reader_t;

#define GIF_MAX_CANVAS_PIXELS ((size_t)64u * 1024u * 1024u)
#define GIF_MAX_FILE_BYTES (INT64_C(100) * 1024 * 1024)
#define GIF_MAX_DECODED_FRAME_BYTES ((size_t)512u * 1024u * 1024u)
#define GIF_MAX_LZW_SUB_BLOCK_BYTES ((size_t)64u * 1024u * 1024u)
#define GIF_PREVIOUS_CANVAS_RETAIN_PIXELS ((size_t)2u * 1024u * 1024u)

/// @brief Install a temporary runtime-trap recovery target for decoder file I/O.
/// @param buf Jump buffer that receives a recovered trap.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Clear the current thread's temporary runtime-trap recovery target.
void rt_trap_clear_recovery(void);

/// @brief Read a GIF file through the runtime file API into decoder-owned memory.
/// @details The decoder frees the returned buffer on every exit path. Reading
///          through `rt_io_file_read_all_bytes` preserves that ownership and
///          uses the runtime's cross-platform UTF-8 path handling. File I/O
///          traps are recovered locally so `gif_decode_file` keeps its legacy
///          return-0-on-open-failure contract.
/// @param filepath UTF-8 filesystem path.
/// @param out_len Receives the byte count on success.
/// @return malloc-owned bytes, or NULL on I/O, size, or allocation failure.
static uint8_t *gif_read_file_bytes(const char *filepath, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!filepath || !out_len)
        return NULL;

    rt_string path = rt_string_from_bytes(filepath, strlen(filepath));
    if (!path)
        return NULL;

    void *volatile bytes = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        rt_trap_clear_recovery();
        rt_string_unref(path);
        return NULL;
    }
    bytes = rt_io_file_read_all_bytes(path);
    rt_trap_clear_recovery();
    rt_string_unref(path);
    if (!bytes)
        return NULL;

    int64_t len_i64 = rt_bytes_len(bytes);
    if (len_i64 <= 0 || len_i64 > GIF_MAX_FILE_BYTES || (uint64_t)len_i64 > (uint64_t)SIZE_MAX) {
        if (rt_obj_release_check0(bytes))
            rt_obj_free(bytes);
        return NULL;
    }

    size_t len = (size_t)len_i64;
    const uint8_t *src = rt_bytes_data_const(bytes);
    uint8_t *copy = src ? (uint8_t *)malloc(len) : NULL;
    if (copy)
        memcpy(copy, src, len);
    if (rt_obj_release_check0(bytes))
        rt_obj_free(bytes);
    if (!copy)
        return NULL;

    *out_len = len;
    return copy;
}

#define GIF_MAX_LZW_MIN_CODE_SIZE 8

typedef enum {
    GIF_DECODE_FAILURE_NONE = 0,
    GIF_DECODE_FAILURE_GENERIC = 1,
    GIF_DECODE_FAILURE_TOO_LARGE = 2,
} gif_decode_failure_t;

/// @brief Validate the GIF LZW minimum code size used by all GIF decode paths.
/// @details GIF image data permits root code sizes from 2 through 8. Keeping this check shared
///          prevents the first-frame and full-animation decoders from accepting different inputs.
/// @param min_code_size Raw value read just before the image data sub-blocks.
/// @return 1 when @p min_code_size is supported by the decoder.
static int gif_lzw_min_code_size_is_valid(int min_code_size) {
    return min_code_size >= 2 && min_code_size <= GIF_MAX_LZW_MIN_CODE_SIZE;
}

/// @brief Validate a GIF logical screen size and compute its pixel count.
/// @details Keeps all GIF decode paths on the same overflow and memory-budget
///          policy before allocating canvas-sized buffers.
/// @param screen_w Logical screen width read from the GIF descriptor.
/// @param screen_h Logical screen height read from the GIF descriptor.
/// @param out_pixels Receives the checked pixel count on success.
/// @return 1 when the dimensions are supported, 0 otherwise.
static int gif_checked_canvas_size(int screen_w, int screen_h, size_t *out_pixels) {
    size_t pixels;
    if (!out_pixels)
        return 0;
    *out_pixels = 0;
    if (screen_w <= 0 || screen_h <= 0 || screen_w > 32768 || screen_h > 32768)
        return 0;
    if ((size_t)screen_w > SIZE_MAX / (size_t)screen_h)
        return 0;
    pixels = (size_t)screen_w * (size_t)screen_h;
    if (pixels == 0 || pixels > GIF_MAX_CANVAS_PIXELS || pixels > SIZE_MAX / sizeof(uint32_t))
        return 0;
    *out_pixels = pixels;
    return 1;
}

/// @brief Checked `size_t` multiplication for GIF decoder allocation sizing.
/// @details Keeps rectangle snapshot and frame-buffer byte-count calculations
///          from overflowing before the allocation size is passed to malloc or
///          realloc.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives `a * b` on success.
/// @return 1 when the product is representable, 0 otherwise.
static int gif_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Release the Pixels handles stored in a partially decoded GIF frame array.
/// @details Used only on decoder failure paths before the caller receives the
///          frame array. Successfully returned arrays remain caller-owned.
/// @param frames Frame array allocated by the GIF decoder; may be NULL.
/// @param frame_count Number of initialized entries in @p frames.
static void gif_release_decoded_frames(gif_frame_t *frames, int frame_count) {
    if (!frames || frame_count <= 0)
        return;
    for (int i = 0; i < frame_count; i++) {
        if (frames[i].pixels && rt_obj_release_check0(frames[i].pixels))
            rt_obj_free(frames[i].pixels);
        frames[i].pixels = NULL;
    }
}

/// @brief Check whether another full-canvas GIF frame snapshot fits the decoded-output budget.
/// @details Animated GIF frames returned by this decoder are full RGBA canvas snapshots. This
///          cumulative budget prevents compact LZW streams from expanding into unbounded retained
///          frame memory while still allowing multi-frame animations under the configured cap.
/// @param decoded_bytes Bytes already committed to returned frame snapshots.
/// @param frame_bytes Bytes needed by the next full-canvas snapshot.
/// @return 1 if the next frame is allowed, 0 on overflow or budget exhaustion.
static int gif_decoded_frame_budget_allows(size_t decoded_bytes, size_t frame_bytes) {
    if (frame_bytes == 0 || frame_bytes > GIF_MAX_DECODED_FRAME_BYTES)
        return 0;
    if (decoded_bytes > SIZE_MAX - frame_bytes)
        return 0;
    return decoded_bytes + frame_bytes <= GIF_MAX_DECODED_FRAME_BYTES;
}

/// @brief Copy bytes from a bounded GIF reader and advance its cursor.
/// @param r Reader whose current position is consumed.
/// @param buf Writable destination for @p count bytes.
/// @param count Number of bytes to copy.
/// @return 1 on success, or 0 for invalid input or insufficient remaining data.
static int gif_read(gif_reader_t *r, void *buf, size_t count) {
    if (!r || !buf || r->pos > r->len || count > r->len - r->pos)
        return 0;
    memcpy(buf, r->data + r->pos, count);
    r->pos += count;
    return 1;
}

/// @brief Read one unsigned byte and advance the GIF reader.
/// @param r Valid reader to consume.
/// @return Byte value in [0, 255], or -1 at end of data.
static int gif_read_u8(gif_reader_t *r) {
    if (r->pos >= r->len)
        return -1;
    return r->data[r->pos++];
}

/// @brief Read one little-endian 16-bit value and advance the GIF reader.
/// @param r Reader to consume.
/// @return Unsigned value in [0, 65535], or -1 for invalid input or underflow.
static int gif_read_u16_le(gif_reader_t *r) {
    if (!r || r->pos > r->len || 2u > r->len - r->pos)
        return -1;
    int val = r->data[r->pos] | (r->data[r->pos + 1] << 8);
    r->pos += 2;
    return val;
}

/// @brief Skip a sequence of GIF data sub-blocks.
/// @details Each sub-block starts with a length byte and the sequence ends
///          with a zero-length terminator. Truncated payloads fail without
///          advancing the cursor past the buffer.
/// @param r Reader positioned at the first sub-block length byte.
/// @return 1 when a terminator was found, 0 on truncation or invalid input.
static int gif_skip_sub_blocks(gif_reader_t *r) {
    if (!r)
        return 0;
    while (r->pos < r->len) {
        int block_size = gif_read_u8(r);
        if (block_size <= 0)
            return block_size == 0; // block terminator (0x00)
        if ((size_t)block_size > r->len - r->pos)
            return 0;
        r->pos += (size_t)block_size;
    }
    return 0;
}

//===----------------------------------------------------------------------===//
// LZW Decompressor
//===----------------------------------------------------------------------===//

#define LZW_MAX_TABLE_SIZE 4096
#define LZW_MAX_BITS 12

typedef struct {
    uint16_t prefix; // index of prefix entry, or 0xFFFF for root entries
    uint8_t suffix;  // last byte of this string
    uint16_t length; // total string length
} lzw_entry_t;

typedef struct {
    lzw_entry_t table[LZW_MAX_TABLE_SIZE];
    int table_size;
    int min_code_size;
    int code_size;
    int clear_code;
    int end_code;
    int next_code;

    // Bit reader state
    uint32_t bitbuf;
    int bits_left;
    const uint8_t *block_data;
    size_t block_len;
    size_t block_pos;
} lzw_state_t;

/// @brief Concatenate a terminated GIF data-sub-block sequence.
/// @details Grows a decoder-owned byte buffer up to @ref GIF_MAX_LZW_SUB_BLOCK_BYTES and requires a
///          zero-length terminator. The reader is left after the terminator on success.
/// @param r Reader positioned at the first sub-block length byte.
/// @param out_len Required destination for the concatenated byte count.
/// @return Malloc-owned concatenated data, or NULL for truncation, budget, overflow, or allocation
///         failure.
static uint8_t *gif_read_sub_blocks(gif_reader_t *r, size_t *out_len) {
    size_t cap = 256;
    size_t len = 0;
    int saw_terminator = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return NULL;

    while (r->pos < r->len) {
        int block_size = gif_read_u8(r);
        if (block_size == 0) {
            saw_terminator = 1;
            break;
        }
        if (block_size < 0)
            break;
        if ((size_t)block_size > SIZE_MAX - len) {
            free(buf);
            return NULL;
        }
        size_t needed = len + (size_t)block_size;
        if (needed > GIF_MAX_LZW_SUB_BLOCK_BYTES) {
            free(buf);
            return NULL;
        }
        if (needed > cap) {
            if (needed > SIZE_MAX / 2) {
                free(buf);
                return NULL;
            }
            cap = needed * 2;
            uint8_t *new_buf = (uint8_t *)realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        if (!gif_read(r, buf + len, (size_t)block_size)) {
            free(buf);
            return NULL;
        }
        len += (size_t)block_size;
    }

    if (!saw_terminator) {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/// @brief Initialize LZW state and its root color-index entries.
/// @param s Writable decompressor state.
/// @param min_code_size Valid GIF root code width in [2, 8].
/// @param data Concatenated borrowed LZW bytes.
/// @param len Length of @p data in bytes.
static void lzw_init(lzw_state_t *s, int min_code_size, const uint8_t *data, size_t len) {
    s->min_code_size = min_code_size;
    s->clear_code = 1 << min_code_size;
    s->end_code = s->clear_code + 1;
    s->code_size = min_code_size + 1;
    s->next_code = s->end_code + 1;
    s->table_size = s->next_code;

    // Initialize root entries (one per color index)
    for (int i = 0; i < s->clear_code; i++) {
        s->table[i].prefix = 0xFFFF;
        s->table[i].suffix = (uint8_t)i;
        s->table[i].length = 1;
    }

    s->bitbuf = 0;
    s->bits_left = 0;
    s->block_data = data;
    s->block_len = len;
    s->block_pos = 0;
}

/// @brief Read the next variable-width code from an LZW bit stream.
/// @param s Mutable decompressor and bit-reader state.
/// @return Decoded non-negative code, or -1 when insufficient input bits remain.
static int lzw_read_code(lzw_state_t *s) {
    while (s->bits_left < s->code_size) {
        if (s->block_pos >= s->block_len)
            return -1;
        s->bitbuf |= (uint32_t)s->block_data[s->block_pos++] << s->bits_left;
        s->bits_left += 8;
    }
    int code = (int)(s->bitbuf & ((1u << s->code_size) - 1));
    s->bitbuf >>= s->code_size;
    s->bits_left -= s->code_size;
    return code;
}

/// @brief Expand one LZW dictionary code into the output index buffer.
/// @details Walks prefix links backward while writing from the end of the reserved string span, so
///          the resulting color indices retain forward order.
/// @param s Initialized LZW dictionary state.
/// @param code Dictionary code to expand.
/// @param out Writable output buffer.
/// @param out_cap Capacity of @p out in bytes.
/// @param out_pos In/out cursor advanced by the emitted string length.
/// @return 0 on success, or -1 for invalid state, code, dictionary links, or output bounds.
static int lzw_emit_string(
    lzw_state_t *s, int code, uint8_t *out, size_t out_cap, size_t *out_pos) {
    if (!s || !out || !out_pos || *out_pos > out_cap || code < 0 || code >= s->table_size)
        return -1;
    int len = s->table[code].length;
    if ((size_t)len > out_cap - *out_pos)
        return -1;
    // Write backwards then the order is correct
    size_t pos = *out_pos + (size_t)len - 1;
    int c = code;
    for (int i = 0; i < len; i++) {
        if (c < 0 || c >= s->table_size)
            return -1;
        out[pos--] = s->table[c].suffix;
        c = (int)s->table[c].prefix;
        if (c == 0xFFFF)
            break;
    }
    *out_pos += (size_t)len;
    return 0;
}

/// @brief Resolve the first color index in an LZW dictionary string.
/// @param s Initialized LZW dictionary state.
/// @param code Dictionary code whose prefix chain is followed.
/// @return Root suffix byte, or 0 when the code or prefix chain is invalid.
static uint8_t lzw_first_byte(lzw_state_t *s, int code) {
    while (code >= 0 && code < s->table_size && s->table[code].prefix != 0xFFFF)
        code = (int)s->table[code].prefix;
    return (code >= 0 && code < s->table_size) ? s->table[code].suffix : 0;
}

/// @brief Decompress a complete GIF LZW stream into color indices.
/// @details Handles clear/end codes, dynamic code-width growth through 12 bits, and the KwKwK
///          special case. Success requires an explicit end code and exactly @p expected_pixels
///          output bytes.
/// @param min_code_size GIF root code width in [2, 8].
/// @param data Concatenated LZW input bytes.
/// @param data_len Length of @p data in bytes.
/// @param expected_pixels Exact number of indices required by the image rectangle.
/// @param out_len Required destination receiving @p expected_pixels on success.
/// @return Malloc-owned color-index buffer, or NULL for malformed input, size overflow, or
///         allocation failure.
static uint8_t *lzw_decompress(int min_code_size,
                               const uint8_t *data,
                               size_t data_len,
                               size_t expected_pixels,
                               size_t *out_len) {
    if (!gif_lzw_min_code_size_is_valid(min_code_size))
        return NULL;
    if (expected_pixels == 0 || expected_pixels > SIZE_MAX - 256)
        return NULL;

    lzw_state_t state;
    lzw_init(&state, min_code_size, data, data_len);

    size_t cap = expected_pixels + 256;
    uint8_t *output = (uint8_t *)malloc(cap);
    if (!output)
        return NULL;
    size_t pos = 0;

    int prev_code = -1;
    int saw_end = 0;
    int failed = 0;
    while (1) {
        int code = lzw_read_code(&state);
        if (code < 0) {
            failed = 1;
            break;
        }
        if (code == state.end_code) {
            saw_end = 1;
            break;
        }

        if (code == state.clear_code) {
            // Reset table
            state.code_size = state.min_code_size + 1;
            state.next_code = state.end_code + 1;
            state.table_size = state.next_code;
            prev_code = -1;
            continue;
        }

        if (code < state.table_size) {
            // Code is in table — emit it
            if (lzw_emit_string(&state, code, output, cap, &pos) != 0) {
                failed = 1;
                break;
            }

            // Add new entry: prev_string + first_byte_of_current_string
            if (prev_code >= 0 && state.next_code < LZW_MAX_TABLE_SIZE) {
                state.table[state.next_code].prefix = (uint16_t)prev_code;
                state.table[state.next_code].suffix = lzw_first_byte(&state, code);
                state.table[state.next_code].length = state.table[prev_code].length + 1;
                state.next_code++;
                state.table_size = state.next_code;
                // Grow code size if needed
                if (state.next_code >= (1 << state.code_size) && state.code_size < LZW_MAX_BITS)
                    state.code_size++;
            }
        } else if (code == state.next_code && prev_code >= 0) {
            // KwKwK special case: code not yet in table
            uint8_t first = lzw_first_byte(&state, prev_code);
            if (lzw_emit_string(&state, prev_code, output, cap, &pos) != 0) {
                failed = 1;
                break;
            }
            if (pos >= cap) {
                failed = 1;
                break;
            }
            output[pos++] = first;

            // Add new entry
            if (state.next_code < LZW_MAX_TABLE_SIZE) {
                state.table[state.next_code].prefix = (uint16_t)prev_code;
                state.table[state.next_code].suffix = first;
                state.table[state.next_code].length = state.table[prev_code].length + 1;
                state.next_code++;
                state.table_size = state.next_code;
                if (state.next_code >= (1 << state.code_size) && state.code_size < LZW_MAX_BITS)
                    state.code_size++;
            }
        } else {
            failed = 1;
            break; // invalid code
        }

        prev_code = code;
    }

    if (failed || !saw_end || pos != expected_pixels) {
        free(output);
        return NULL;
    }
    *out_len = pos;
    return output;
}

//===----------------------------------------------------------------------===//
// GIF Decoder
//===----------------------------------------------------------------------===//

/// @brief Interlace pass parameters: start row, row step.
static const int gif_interlace_start[4] = {0, 4, 2, 1};
static const int gif_interlace_step[4] = {8, 8, 4, 2};

/// @brief Convert GIF Graphic Control Extension delay units to display milliseconds.
/// @details The file stores delay in centiseconds. A zero or one-centisecond delay is commonly
///          authored accidentally and causes busy animation loops, so the decoder follows browser
///          practice by normalizing those tiny values to a conservative 100 ms fallback.
/// @param delay_cs Encoded frame delay in centiseconds.
/// @return Display delay in milliseconds, saturated at `INT_MAX`.
static int gif_delay_ms_from_centiseconds(int delay_cs) {
    if (delay_cs <= 1)
        return 100;
    if (delay_cs > INT_MAX / 10)
        return INT_MAX;
    return delay_cs * 10;
}

/// @brief Map a decoded GIF row to its display row, accounting for four-pass interlacing.
/// @details Non-interlaced images use row order directly. Interlaced images are stored by pass, so
///          this helper centralizes the row remap for the full-animation and first-frame decoders.
/// @param decoded_row Sequential row index in the decoded image data.
/// @param image_height Image-rectangle height in rows.
/// @param interlaced Nonzero when GIF four-pass row ordering is active.
/// @return Display-relative row index for the decoded row.
static int gif_actual_image_row(int decoded_row, int image_height, int interlaced) {
    int row_in_pass;
    if (!interlaced)
        return decoded_row;
    row_in_pass = decoded_row;
    for (int pass = 0; pass < 4; pass++) {
        int pass_rows = (image_height - gif_interlace_start[pass] + gif_interlace_step[pass] - 1) /
                        gif_interlace_step[pass];
        if (row_in_pass < pass_rows)
            return gif_interlace_start[pass] + row_in_pass * gif_interlace_step[pass];
        row_in_pass -= pass_rows;
    }
    return decoded_row;
}

/// @brief Decode a GIF file from disk into an array of RGBA frames.
/// @details Reads the entire file into memory, then dispatches to the
///          in-memory decoder path (the same one used by ZPAK-embedded GIFs).
///          On success, @p out_frames is malloc'd and the caller must free it
///          (each contained Pixels object is GC-managed and released via
///          rt_obj_release_check0). Failure modes (NULL paths, read failure,
///          truncated files) all return 0 and leave outputs untouched.
/// @param filepath        Filesystem path to the .gif file. Must be non-NULL.
/// @param out_frames      Out: malloc'd array of decoded frames. Required.
/// @param out_frame_count Out: number of frames written. Required.
/// @param out_width       Out: canvas width in pixels. Optional (NULL-safe).
/// @param out_height      Out: canvas height in pixels. Optional (NULL-safe).
/// @return 1 on success, 0 on any error.
int gif_decode_file(const char *filepath,
                    gif_frame_t **out_frames,
                    int *out_frame_count,
                    int *out_width,
                    int *out_height) {
    if (!filepath || !out_frames || !out_frame_count)
        return 0;

    size_t file_len = 0;
    uint8_t *file_data = gif_read_file_bytes(filepath, &file_len);
    if (!file_data)
        return 0;

    gif_reader_t reader = {file_data, file_len, 0};
    gif_reader_t *r = &reader;

    // Verify GIF signature
    uint8_t sig[6];
    if (!gif_read(r, sig, 6) || (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0)) {
        free(file_data);
        return 0;
    }

    // Logical screen descriptor
    int screen_w = gif_read_u16_le(r);
    int screen_h = gif_read_u16_le(r);
    size_t canvas_size = 0;
    if (!gif_checked_canvas_size(screen_w, screen_h, &canvas_size)) {
        free(file_data);
        return 0;
    }

    int packed = gif_read_u8(r);
    if (packed < 0) {
        free(file_data);
        return 0;
    }
    int has_gct = (packed >> 7) & 1;
    int gct_size_field = packed & 0x07;
    int bg_color_index = gif_read_u8(r);
    int aspect = gif_read_u8(r); // pixel aspect ratio (ignored)
    if (bg_color_index < 0 || aspect < 0) {
        free(file_data);
        return 0;
    }

    // Global color table
    uint8_t gct[256 * 3];
    int gct_count = 0;
    memset(gct, 0, sizeof(gct));
    if (has_gct) {
        gct_count = 1 << (gct_size_field + 1);
        if (!gif_read(r, gct, (size_t)gct_count * 3)) {
            free(file_data);
            return 0;
        }
    }

    // Allocate frame array (grow as needed)
    int frame_cap = 16;
    int frame_count = 0;
    gif_decode_failure_t decode_failed = GIF_DECODE_FAILURE_NONE;
    size_t decoded_frame_bytes = 0;
    size_t frame_snapshot_bytes = canvas_size * sizeof(uint32_t);
    gif_frame_t *frames = (gif_frame_t *)calloc((size_t)frame_cap, sizeof(gif_frame_t));
    if (!frames) {
        free(file_data);
        return 0;
    }

    // Canvas for frame compositing (RGBA)
    uint32_t *canvas = (uint32_t *)calloc(canvas_size, sizeof(uint32_t));
    uint32_t *prev_canvas = NULL;
    size_t prev_canvas_capacity = 0;
    if (!canvas) {
        free(canvas);
        free(frames);
        free(file_data);
        return 0;
    }

    // Fill canvas with background color
    uint32_t bg_rgba = 0x00000000;
    if (has_gct && bg_color_index < gct_count) {
        bg_rgba = ((uint32_t)gct[bg_color_index * 3] << 24) |
                  ((uint32_t)gct[bg_color_index * 3 + 1] << 16) |
                  ((uint32_t)gct[bg_color_index * 3 + 2] << 8) | 0xFF;
    }
    for (size_t i = 0; i < canvas_size; i++)
        canvas[i] = bg_rgba;

    // Current GCE state
    int gce_delay_ms = 0;
    int gce_dispose = 0;
    int gce_transparent = -1;
    int prev_rect_x0 = 0;
    int prev_rect_y0 = 0;
    int prev_rect_x1 = 0;
    int prev_rect_y1 = 0;
    int gce_valid = 0;

    // Process blocks
    while (r->pos < r->len) {
        int block_type = gif_read_u8(r);
        if (block_type < 0 || block_type == 0x3B) // trailer
            break;

        if (block_type == 0x21) {
            // Extension block
            int ext_label = gif_read_u8(r);
            if (ext_label < 0) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }
            if (ext_label == 0xF9) {
                // Graphics Control Extension
                int block_size = gif_read_u8(r);
                if (block_size != 4) {
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
                int gce_packed = gif_read_u8(r);
                int delay;
                int trans_idx;
                if (gce_packed < 0) {
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
                gce_dispose = (gce_packed >> 2) & 0x07;
                int has_transparent = gce_packed & 0x01;
                delay = gif_read_u16_le(r);
                trans_idx = gif_read_u8(r);
                if (delay < 0 || trans_idx < 0) {
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
                gce_delay_ms = gif_delay_ms_from_centiseconds(delay);
                gce_transparent = has_transparent ? trans_idx : -1;
                gce_valid = 1;
                if (gif_read_u8(r) != 0) { // block terminator
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
            } else {
                // Skip other extensions
                if (!gif_skip_sub_blocks(r)) {
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
            }
            continue;
        }

        if (block_type == 0x2C) {
            // Image descriptor
            int img_left = gif_read_u16_le(r);
            int img_top = gif_read_u16_le(r);
            int img_w = gif_read_u16_le(r);
            int img_h = gif_read_u16_le(r);
            int img_packed = gif_read_u8(r);
            if (img_left < 0 || img_top < 0 || img_w < 0 || img_h < 0 || img_packed < 0) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }
            int has_lct = (img_packed >> 7) & 1;
            int interlaced = (img_packed >> 6) & 1;
            int lct_size_field = img_packed & 0x07;

            // Local color table (overrides GCT for this frame)
            uint8_t lct[256 * 3];
            int lct_count = 0;
            const uint8_t *color_table = gct;
            int color_count = gct_count;
            if (has_lct) {
                lct_count = 1 << (lct_size_field + 1);
                if (!gif_read(r, lct, (size_t)lct_count * 3)) {
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
                color_table = lct;
                color_count = lct_count;
            }
            if (color_count <= 0) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }

            // LZW minimum code size
            int min_code_size = gif_read_u8(r);
            if (!gif_lzw_min_code_size_is_valid(min_code_size)) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }
            if (img_w <= 0 || img_h <= 0 || img_left > screen_w - img_w ||
                img_top > screen_h - img_h)
                goto skip_image_data;

            // Read LZW sub-blocks
            size_t lzw_data_len = 0;
            uint8_t *lzw_data = gif_read_sub_blocks(r, &lzw_data_len);
            if (!lzw_data) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }

            // Decompress LZW
            if ((size_t)img_w > SIZE_MAX / (size_t)img_h) {
                free(lzw_data);
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }
            size_t pixel_count = (size_t)img_w * (size_t)img_h;
            size_t index_len = 0;
            uint8_t *indices =
                lzw_decompress(min_code_size, lzw_data, lzw_data_len, pixel_count, &index_len);
            free(lzw_data);
            if (!indices) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }

            // Save only the affected canvas rectangle for dispose method 3.
            prev_rect_x0 = img_left < 0 ? 0 : img_left;
            prev_rect_y0 = img_top < 0 ? 0 : img_top;
            prev_rect_x1 = img_left + img_w;
            prev_rect_y1 = img_top + img_h;
            if (prev_rect_x1 > screen_w)
                prev_rect_x1 = screen_w;
            if (prev_rect_y1 > screen_h)
                prev_rect_y1 = screen_h;
            if (prev_rect_x1 < prev_rect_x0)
                prev_rect_x1 = prev_rect_x0;
            if (prev_rect_y1 < prev_rect_y0)
                prev_rect_y1 = prev_rect_y0;
            if (gce_dispose == 3) {
                size_t row_pixels = (size_t)(prev_rect_x1 - prev_rect_x0);
                size_t rect_h = (size_t)(prev_rect_y1 - prev_rect_y0);
                size_t needed_pixels = 0;
                if (row_pixels > 0 && rect_h > 0 &&
                    (!gif_checked_mul_size(row_pixels, rect_h, &needed_pixels) ||
                     needed_pixels > SIZE_MAX / sizeof(uint32_t))) {
                    free(indices);
                    decode_failed = GIF_DECODE_FAILURE_GENERIC;
                    break;
                }
                if (prev_canvas_capacity > GIF_PREVIOUS_CANVAS_RETAIN_PIXELS &&
                    needed_pixels < GIF_PREVIOUS_CANVAS_RETAIN_PIXELS / 2u) {
                    free(prev_canvas);
                    prev_canvas = NULL;
                    prev_canvas_capacity = 0;
                }
                if (needed_pixels > prev_canvas_capacity) {
                    uint32_t *grown =
                        (uint32_t *)realloc(prev_canvas, needed_pixels * sizeof(uint32_t));
                    if (!grown) {
                        free(indices);
                        decode_failed = GIF_DECODE_FAILURE_GENERIC;
                        break;
                    }
                    prev_canvas = grown;
                    prev_canvas_capacity = needed_pixels;
                }
                for (int y = prev_rect_y0; y < prev_rect_y1; y++) {
                    size_t src_off = (size_t)y * (size_t)screen_w + (size_t)prev_rect_x0;
                    size_t dst_off = (size_t)(y - prev_rect_y0) * row_pixels;
                    memcpy(prev_canvas + dst_off, canvas + src_off, row_pixels * sizeof(uint32_t));
                }
            }

            // Apply decoded pixels to canvas
            size_t idx = 0;
            int invalid_color_index = 0;
            for (int y = 0; y < img_h && idx < index_len; y++) {
                // De-interlace: map logical row y to actual row
                int actual_y = gif_actual_image_row(y, img_h, interlaced);

                int canvas_y = img_top + actual_y;
                if (canvas_y < 0 || canvas_y >= screen_h) {
                    idx += (size_t)img_w;
                    continue;
                }

                for (int x = 0; x < img_w && idx < index_len; x++, idx++) {
                    int canvas_x = img_left + x;
                    if (canvas_x < 0 || canvas_x >= screen_w)
                        continue;

                    int color_idx = indices[idx];
                    if (gce_transparent >= 0 && color_idx == gce_transparent)
                        continue; // transparent — don't overwrite canvas

                    if (color_idx < color_count) {
                        uint32_t rgba = ((uint32_t)color_table[color_idx * 3] << 24) |
                                        ((uint32_t)color_table[color_idx * 3 + 1] << 16) |
                                        ((uint32_t)color_table[color_idx * 3 + 2] << 8) | 0xFF;
                        canvas[canvas_y * screen_w + canvas_x] = rgba;
                    } else {
                        invalid_color_index = 1;
                        break;
                    }
                }
                if (invalid_color_index)
                    break;
            }
            free(indices);
            if (invalid_color_index) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }

            // Create Pixels object for this frame (snapshot of current canvas).
            // Each animated GIF frame is materialised as a full screen-sized RGBA snapshot and
            // every frame is retained simultaneously, so peak memory is O(frames * canvas). This
            // is inherent to the "each frame is a complete image" contract and is bounded by the
            // budget check below; callers needing only the first frame should use
            // rt_pixels_load_gif (first-frame only), which avoids the per-frame snapshot cost.
            if (!gif_decoded_frame_budget_allows(decoded_frame_bytes, frame_snapshot_bytes)) {
                decode_failed = GIF_DECODE_FAILURE_TOO_LARGE;
                break;
            }
            rt_pixels_impl *px = pixels_alloc((int64_t)screen_w, (int64_t)screen_h);
            if (px) {
                memcpy(px->data, canvas, canvas_size * sizeof(uint32_t));

                // Grow frames array if needed
                if (frame_count >= frame_cap) {
                    if (frame_cap > INT32_MAX / 2 ||
                        (size_t)(frame_cap * 2) > SIZE_MAX / sizeof(gif_frame_t)) {
                        if (rt_obj_release_check0(px))
                            rt_obj_free(px);
                        decode_failed = GIF_DECODE_FAILURE_GENERIC;
                        break;
                    }
                    frame_cap *= 2;
                    gif_frame_t *new_frames =
                        (gif_frame_t *)realloc(frames, (size_t)frame_cap * sizeof(gif_frame_t));
                    if (!new_frames) {
                        if (rt_obj_release_check0(px))
                            rt_obj_free(px);
                        decode_failed = GIF_DECODE_FAILURE_GENERIC;
                        break;
                    }
                    frames = new_frames;
                }

                frames[frame_count].pixels = px;
                frames[frame_count].delay_ms = gce_valid ? gce_delay_ms : 100;
                frames[frame_count].dispose_method = gce_dispose;
                frame_count++;
                decoded_frame_bytes += frame_snapshot_bytes;
            } else {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }

            // Apply disposal method for next frame
            switch (gce_dispose) {
                case 2: // Restore to background
                {
                    int y0 = img_top < 0 ? 0 : img_top;
                    int y1 = img_top + img_h;
                    int x0 = img_left < 0 ? 0 : img_left;
                    int x1 = img_left + img_w;
                    if (y1 > screen_h)
                        y1 = screen_h;
                    if (x1 > screen_w)
                        x1 = screen_w;
                    for (int y = y0; y < y1; y++) {
                        uint32_t *row = canvas + (size_t)y * (size_t)screen_w + (size_t)x0;
                        for (int x = x0; x < x1; x++)
                            *row++ = bg_rgba;
                    }
                } break;
                case 3: // Restore to previous
                    if (prev_canvas && prev_rect_x1 > prev_rect_x0 && prev_rect_y1 > prev_rect_y0) {
                        size_t row_pixels = (size_t)(prev_rect_x1 - prev_rect_x0);
                        for (int y = prev_rect_y0; y < prev_rect_y1; y++) {
                            size_t dst_off = (size_t)y * (size_t)screen_w + (size_t)prev_rect_x0;
                            size_t src_off = (size_t)(y - prev_rect_y0) * row_pixels;
                            memcpy(canvas + dst_off,
                                   prev_canvas + src_off,
                                   row_pixels * sizeof(uint32_t));
                        }
                    }
                    break;
                default: // 0 or 1: do not dispose (keep canvas as-is)
                    break;
            }

            // Reset GCE for next frame
            gce_valid = 0;
            gce_delay_ms = 0;
            gce_dispose = 0;
            gce_transparent = -1;
            continue;

        skip_image_data:
            if (!gif_skip_sub_blocks(r)) {
                decode_failed = GIF_DECODE_FAILURE_GENERIC;
                break;
            }
            gce_valid = 0;
            gce_delay_ms = 0;
            gce_dispose = 0;
            gce_transparent = -1;
            continue;
        }

        // Unknown top-level block types have no generic length field; fail closed.
        decode_failed = GIF_DECODE_FAILURE_GENERIC;
        break;
    }

    free(canvas);
    free(prev_canvas);
    free(file_data);

    if (decode_failed != GIF_DECODE_FAILURE_NONE || frame_count == 0) {
        gif_release_decoded_frames(frames, frame_count);
        free(frames);
        return 0;
    }

    *out_frames = frames;
    *out_frame_count = frame_count;
    if (out_width)
        *out_width = screen_w;
    if (out_height)
        *out_height = screen_h;
    return frame_count;
}

/// @brief Decode the first renderable GIF image from a memory buffer into RGBA32 pixels.
/// @details Validates GIF87a/GIF89a structure, canvas and file-size budgets, palettes, image bounds,
///          LZW termination, and palette indices. Extension blocks before the first image are
///          honored for transparency; later animation frames and disposal are not decoded. Output
///          pointers are initialized to NULL/zero before validation.
/// @param data Read-only GIF file bytes.
/// @param len Accessible byte length of @p data, limited to 100 MiB.
/// @param out_pixels Required destination receiving a malloc-owned full-canvas RGBA32 buffer.
/// @param out_width Optional destination receiving the logical screen width.
/// @param out_height Optional destination receiving the logical screen height.
/// @return 1 after decoding the first valid image, otherwise 0 with initialized outputs.
int rt_gif_decode_memory_first_rgba32(
    const uint8_t *data, size_t len, uint32_t **out_pixels, int *out_width, int *out_height) {
    gif_reader_t reader;
    gif_reader_t *r = &reader;
    uint8_t sig[6];
    int screen_w;
    int screen_h;
    int packed;
    int has_gct;
    int gct_size_field;
    int bg_color_index;
    uint8_t gct[256 * 3];
    int gct_count = 0;
    size_t canvas_size;
    uint32_t *canvas = NULL;
    uint32_t bg_rgba = 0x00000000;
    int gce_delay_ms = 0;
    int gce_dispose = 0;
    int gce_transparent = -1;
    int gce_valid = 0;

    if (out_pixels)
        *out_pixels = NULL;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
    if (!data || len == 0 || len > 100u * 1024u * 1024u || !out_pixels)
        return 0;

    reader.data = data;
    reader.len = len;
    reader.pos = 0;

    if (!gif_read(r, sig, 6) || (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0))
        return 0;

    screen_w = gif_read_u16_le(r);
    screen_h = gif_read_u16_le(r);
    if (!gif_checked_canvas_size(screen_w, screen_h, &canvas_size))
        return 0;

    packed = gif_read_u8(r);
    if (packed < 0)
        return 0;
    has_gct = (packed >> 7) & 1;
    gct_size_field = packed & 0x07;
    bg_color_index = gif_read_u8(r);
    if (bg_color_index < 0 || gif_read_u8(r) < 0)
        return 0;

    memset(gct, 0, sizeof(gct));
    if (has_gct) {
        gct_count = 1 << (gct_size_field + 1);
        if (!gif_read(r, gct, (size_t)gct_count * 3))
            return 0;
    }

    canvas = (uint32_t *)calloc(canvas_size, sizeof(uint32_t));
    if (!canvas)
        return 0;

    if (has_gct && bg_color_index < gct_count) {
        bg_rgba = ((uint32_t)gct[bg_color_index * 3] << 24) |
                  ((uint32_t)gct[bg_color_index * 3 + 1] << 16) |
                  ((uint32_t)gct[bg_color_index * 3 + 2] << 8) | 0xFF;
    }
    for (size_t i = 0; i < canvas_size; i++)
        canvas[i] = bg_rgba;

    while (r->pos < r->len) {
        int block_type = gif_read_u8(r);
        if (block_type < 0 || block_type == 0x3B)
            break;

        if (block_type == 0x21) {
            int ext_label = gif_read_u8(r);
            if (ext_label < 0)
                break;
            if (ext_label == 0xF9) {
                int block_size = gif_read_u8(r);
                if (block_size != 4)
                    break;
                int gce_packed = gif_read_u8(r);
                int delay;
                int trans_idx;
                if (gce_packed < 0)
                    break;
                gce_dispose = (gce_packed >> 2) & 0x07;
                delay = gif_read_u16_le(r);
                trans_idx = gif_read_u8(r);
                if (delay < 0 || trans_idx < 0)
                    break;
                gce_delay_ms = gif_delay_ms_from_centiseconds(delay);
                gce_transparent = (gce_packed & 0x01) ? trans_idx : -1;
                gce_valid = 1;
                if (gif_read_u8(r) != 0)
                    break;
            } else {
                if (!gif_skip_sub_blocks(r))
                    break;
            }
            continue;
        }

        if (block_type == 0x2C) {
            int img_left = gif_read_u16_le(r);
            int img_top = gif_read_u16_le(r);
            int img_w = gif_read_u16_le(r);
            int img_h = gif_read_u16_le(r);
            int img_packed = gif_read_u8(r);
            int has_lct;
            int interlaced;
            int lct_size_field;
            uint8_t lct[256 * 3];
            int lct_count = 0;
            const uint8_t *color_table = gct;
            int color_count = gct_count;
            int min_code_size;
            size_t lzw_data_len = 0;
            uint8_t *lzw_data = NULL;
            size_t pixel_count;
            size_t index_len = 0;
            uint8_t *indices = NULL;
            size_t idx = 0;
            int invalid_color_index = 0;

            if (img_left < 0 || img_top < 0 || img_w < 0 || img_h < 0 || img_packed < 0)
                break;
            has_lct = (img_packed >> 7) & 1;
            interlaced = (img_packed >> 6) & 1;
            lct_size_field = img_packed & 0x07;
            if (has_lct) {
                lct_count = 1 << (lct_size_field + 1);
                if (!gif_read(r, lct, (size_t)lct_count * 3))
                    break;
                color_table = lct;
                color_count = lct_count;
            }
            if (color_count <= 0)
                break;

            min_code_size = gif_read_u8(r);
            if (!gif_lzw_min_code_size_is_valid(min_code_size))
                break;
            if (img_w <= 0 || img_h <= 0 || img_left > screen_w - img_w ||
                img_top > screen_h - img_h)
                goto skip_image_data;
            if ((size_t)img_w > SIZE_MAX / (size_t)img_h)
                goto skip_image_data;

            lzw_data = gif_read_sub_blocks(r, &lzw_data_len);
            if (!lzw_data)
                break;
            pixel_count = (size_t)img_w * (size_t)img_h;
            indices =
                lzw_decompress(min_code_size, lzw_data, lzw_data_len, pixel_count, &index_len);
            free(lzw_data);
            if (!indices)
                break;

            for (int y = 0; y < img_h && idx < index_len; y++) {
                int actual_y = gif_actual_image_row(y, img_h, interlaced);

                int canvas_y = img_top + actual_y;
                if (canvas_y < 0 || canvas_y >= screen_h) {
                    idx += (size_t)img_w;
                    continue;
                }
                for (int x = 0; x < img_w && idx < index_len; x++, idx++) {
                    int canvas_x = img_left + x;
                    int color_idx;
                    if (canvas_x < 0 || canvas_x >= screen_w)
                        continue;
                    color_idx = indices[idx];
                    if (gce_transparent >= 0 && color_idx == gce_transparent)
                        continue;
                    if (color_idx < color_count) {
                        uint32_t rgba = ((uint32_t)color_table[color_idx * 3] << 24) |
                                        ((uint32_t)color_table[color_idx * 3 + 1] << 16) |
                                        ((uint32_t)color_table[color_idx * 3 + 2] << 8) | 0xFF;
                        canvas[canvas_y * screen_w + canvas_x] = rgba;
                    } else {
                        invalid_color_index = 1;
                        break;
                    }
                }
                if (invalid_color_index)
                    break;
            }
            free(indices);
            if (invalid_color_index) {
                free(canvas);
                return 0;
            }
            *out_pixels = canvas;
            if (out_width)
                *out_width = screen_w;
            if (out_height)
                *out_height = screen_h;
            (void)gce_valid;
            (void)gce_delay_ms;
            (void)gce_dispose;
            return 1;

        skip_image_data:
            if (!gif_skip_sub_blocks(r))
                break;
            gce_valid = 0;
            gce_delay_ms = 0;
            gce_dispose = 0;
            gce_transparent = -1;
            continue;
        }
    }

    free(canvas);
    return 0;
}
