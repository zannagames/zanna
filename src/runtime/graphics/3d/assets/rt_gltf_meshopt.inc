//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_meshopt.inc  (#included from rt_gltf.c; not a
// standalone translation unit)
// Purpose: EXT_meshopt_compression bufferView decoding — clean-room implementation of the
//   three bitstream codecs (ATTRIBUTES / TRIANGLES / INDICES) and three filters
//   (OCTAHEDRAL / QUATERNION / EXPONENTIAL) from the published extension specification
//   (Appendix A: Bitstream, Appendix B: Filters). No external codec code is used.
// Key invariants:
//   - Every read is bounds-checked against the compressed byte range; malformed data
//     fails the decode (recoverable load error), never traps.
//   - Decoded output is exactly byteStride * count bytes, matching the parent bufferView.
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_meshopt.inc
 * @brief Implements clean-room decoding for `EXT_meshopt_compression` buffer views.
 * @details This `rt_gltf.c` fragment decodes attribute, triangle, and index bitstreams;
 *          applies octahedral, quaternion, and exponential reconstruction filters; validates
 *          extension configuration and byte ranges; and owns the decoded shadow buffers used
 *          in place of compressed glTF buffer views.
 */

#define GLTF_MESHOPT_MODE_ATTRIBUTES 0
#define GLTF_MESHOPT_MODE_TRIANGLES 1
#define GLTF_MESHOPT_MODE_INDICES 2

#define GLTF_MESHOPT_FILTER_NONE 0
#define GLTF_MESHOPT_FILTER_OCTAHEDRAL 1
#define GLTF_MESHOPT_FILTER_QUATERNION 2
#define GLTF_MESHOPT_FILTER_EXPONENTIAL 3

/// @brief Decode the byte-level zigzag scheme used by the attribute codec.
/// @param v Encoded unsigned byte.
/// @return Decoded two's-complement delta represented as an unsigned byte.
static uint8_t gltf_meshopt_unzigzag8(uint8_t v) {
    return (uint8_t)((v & 1u) ? ~(v >> 1) : (v >> 1));
}

/// @brief Read one varint-7 (unsigned LEB128) value, capped at 32 bits / 5 bytes.
/// @param[in,out] cursor Address of the next encoded byte; advanced for consumed bytes.
/// @param[in] end One-past-the-end pointer for the encoded region.
/// @param[out] out Receives the decoded 32-bit value on success.
/// @return Non-zero on success; zero when the stream ends early or the value is overlong.
static int gltf_meshopt_read_leb128(const uint8_t **cursor, const uint8_t *end, uint32_t *out) {
    uint32_t value = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t byte;
        if (*cursor >= end)
            return 0;
        byte = *(*cursor)++;
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

/// @brief Write one decoded index to the output stream (16- or 32-bit little-endian).
/// @param[out] dst Beginning of the decoded index stream.
/// @param element Zero-based output index element.
/// @param stride Output element width, either two or four bytes.
/// @param value Decoded unsigned index value.
/// @return Non-zero on success; zero when @p value cannot be represented by a
///         two-byte output element.
static int gltf_meshopt_write_index(uint8_t *dst, size_t element, size_t stride, uint32_t value) {
    uint8_t *out = dst + element * stride;
    if (stride == 2u) {
        if (value > 0xFFFFu)
            return 0;
        out[0] = (uint8_t)(value & 0xFFu);
        out[1] = (uint8_t)((value >> 8) & 0xFFu);
        return 1;
    }
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
    return 1;
}

/// @brief Mode 0 (ATTRIBUTES): byte-deinterleaved delta blocks with 2-bit group headers.
/// @details Spec structure: header byte 0xa0; attribute blocks of
///          `min((8192 / byteStride) & ~15, 256)` elements, each holding `byteStride`
///          data blocks (one per element byte) of 16-element delta groups with 2-bit
///          headers (0 = all zero, 1 = 2-bit sentinel, 2 = 4-bit sentinel, 3 = raw
///          bytes); a tail block stores the baseline element verbatim, padded to
///          32 bytes. Deltas are zigzag-encoded byte differences with wrap-around.
/// @param[out] dst Output buffer providing at least `count * stride` bytes.
/// @param count Number of attribute elements to decode.
/// @param stride Byte width of each interleaved output element.
/// @param[in] src Compressed attribute bitstream.
/// @param src_len Number of readable bytes at @p src.
/// @return Non-zero when the complete stream is valid and decoded; zero on invalid
///         configuration, header, or truncated group data.
static int gltf_meshopt_decode_attributes(
    uint8_t *dst, size_t count, size_t stride, const uint8_t *src, size_t src_len) {
    size_t tail = stride < 32u ? 32u : stride;
    const uint8_t *cursor;
    const uint8_t *end;
    uint8_t baseline[256];
    size_t max_block;
    size_t decoded = 0;
    if (stride == 0 || stride > 256u)
        return 0;
    if (src_len < 1u + tail)
        return 0;
    if (src[0] != 0xA0u)
        return 0;
    memcpy(baseline, src + src_len - stride, stride);
    cursor = src + 1;
    end = src + src_len - tail;
    max_block = (8192u / stride) & ~(size_t)15u;
    if (max_block > 256u)
        max_block = 256u;
    if (max_block == 0)
        return 0;
    while (decoded < count) {
        size_t block = count - decoded < max_block ? count - decoded : max_block;
        size_t group_count = (block + 15u) / 16u;
        for (size_t k = 0; k < stride; k++) {
            /* group_count <= 16 because block <= 256 */
            uint8_t deltas[256];
            size_t header_bytes = (group_count + 3u) / 4u;
            const uint8_t *header = cursor;
            if ((size_t)(end - cursor) < header_bytes)
                return 0;
            cursor += header_bytes;
            for (size_t g = 0; g < group_count; g++) {
                int bits = (header[g >> 2] >> ((g & 3u) * 2u)) & 3;
                uint8_t *out = deltas + g * 16u;
                if (bits == 0) {
                    memset(out, 0, 16);
                } else if (bits == 1) {
                    /* 2-bit packed MSB-first: (d0<<6)|(d1<<4)|(d2<<2)|d3; 3 = sentinel */
                    const uint8_t *packed = cursor;
                    if ((size_t)(end - cursor) < 4u)
                        return 0;
                    cursor += 4;
                    for (int j = 0; j < 16; j++) {
                        int v = (packed[j >> 2] >> (6 - 2 * (j & 3))) & 3;
                        if (v == 3) {
                            if (cursor >= end)
                                return 0;
                            out[j] = *cursor++;
                        } else {
                            out[j] = (uint8_t)v;
                        }
                    }
                } else if (bits == 2) {
                    /* 4-bit packed MSB-first: (d0<<4)|d1; 15 = sentinel */
                    const uint8_t *packed = cursor;
                    if ((size_t)(end - cursor) < 8u)
                        return 0;
                    cursor += 8;
                    for (int j = 0; j < 16; j++) {
                        int v = (packed[j >> 1] >> (4 - 4 * (j & 1))) & 15;
                        if (v == 15) {
                            if (cursor >= end)
                                return 0;
                            out[j] = *cursor++;
                        } else {
                            out[j] = (uint8_t)v;
                        }
                    }
                } else {
                    if ((size_t)(end - cursor) < 16u)
                        return 0;
                    memcpy(out, cursor, 16);
                    cursor += 16;
                }
            }
            /* Apply deltas to the real elements; padding deltas in the final group are
             * decoded (consumed) above but ignored here per the spec. */
            {
                uint8_t prev = baseline[k];
                for (size_t e = 0; e < block; e++) {
                    prev = (uint8_t)(prev + gltf_meshopt_unzigzag8(deltas[e]));
                    dst[(decoded + e) * stride + k] = prev;
                }
                baseline[k] = prev;
            }
        }
        decoded += block;
    }
    return 1;
}

/// @brief Decode one explicitly-encoded index: zigzag varint delta applied to `last`.
/// @param[in,out] data Address of the next varint byte, advanced on success.
/// @param[in] data_end One-past-the-end pointer for the varint region.
/// @param[in,out] last Previous explicit index, replaced by the decoded index.
/// @return Non-zero on success; zero when the varint is truncated or overlong.
static int gltf_meshopt_decode_explicit_index(const uint8_t **data,
                                              const uint8_t *data_end,
                                              uint32_t *last) {
    uint32_t v;
    int32_t delta;
    if (!gltf_meshopt_read_leb128(data, data_end, &v))
        return 0;
    delta = (v & 1u) ? ~(int32_t)(v >> 1) : (int32_t)(v >> 1);
    *last = (uint32_t)((int64_t)*last + delta);
    return 1;
}

/// @brief Mode 1 (TRIANGLES): edge/vertex-FIFO triangle codec.
/// @details Spec structure: header byte 0xe1; one code byte per triangle; an extra-data
///          region for explicit indices (zigzag varint deltas vs `last`); a 16-byte
///          `codeaux` lookup table as the tail. Decoder state: `next` high-watermark,
///          `last` index, 16-entry edge FIFO and vertex FIFO (reads use pre-push state;
///          index 0 is the most recently pushed entry).
/// @param[out] dst Output index buffer providing at least `count * stride` bytes.
/// @param count Number of output indices; must be divisible by three.
/// @param stride Output index width, either two or four bytes.
/// @param[in] src Compressed triangle bitstream.
/// @param src_len Number of readable bytes at @p src.
/// @return Non-zero when all triangle codes and auxiliary data decode exactly;
///         zero on invalid input, truncation, or output-width overflow.
static int gltf_meshopt_decode_triangles(
    uint8_t *dst, size_t count, size_t stride, const uint8_t *src, size_t src_len) {
    size_t triangle_count = count / 3u;
    const uint8_t *code;
    const uint8_t *data;
    const uint8_t *data_end;
    const uint8_t *codeaux;
    uint32_t edgefifo[16][2];
    uint32_t vertexfifo[16];
    uint32_t efo = 0; /* edge fifo offset */
    uint32_t vfo = 0; /* vertex fifo offset */
    uint32_t next = 0;
    uint32_t last = 0;
    if (count % 3u != 0)
        return 0;
    if (src_len < 1u + triangle_count + 16u)
        return 0;
    if (src[0] != 0xE1u)
        return 0;
    memset(edgefifo, 0, sizeof(edgefifo));
    memset(vertexfifo, 0, sizeof(vertexfifo));
    code = src + 1;
    data = code + triangle_count;
    codeaux = src + src_len - 16u;
    data_end = codeaux;
    for (size_t t = 0; t < triangle_count; t++) {
        uint8_t cb = code[t];
        uint32_t a;
        uint32_t b;
        uint32_t c;
        int fe = cb >> 4;
        if (fe < 15) {
            int fec = cb & 15;
            a = edgefifo[(efo - 1u - (uint32_t)fe) & 15u][0];
            b = edgefifo[(efo - 1u - (uint32_t)fe) & 15u][1];
            if (fec == 0) {
                /* 0xX0: third vertex is `next` */
                c = next++;
                vertexfifo[vfo++ & 15u] = c;
            } else if (fec < 0xd) {
                /* 0xXY: third vertex from the vertex FIFO at index Y (no push) */
                c = vertexfifo[(vfo - 1u - (uint32_t)fec) & 15u];
            } else if (fec == 0xd || fec == 0xe) {
                /* 0xXd / 0xXe: third vertex adjacent to `last` */
                c = (fec == 0xd) ? last - 1u : last + 1u;
                last = c;
                vertexfifo[vfo++ & 15u] = c;
            } else {
                /* 0xXf: third vertex encoded explicitly */
                if (!gltf_meshopt_decode_explicit_index(&data, data_end, &last))
                    return 0;
                c = last;
                vertexfifo[vfo++ & 15u] = c;
            }
            edgefifo[efo & 15u][0] = c;
            edgefifo[efo & 15u][1] = b;
            efo++;
            edgefifo[efo & 15u][0] = a;
            edgefifo[efo & 15u][1] = c;
            efo++;
        } else {
            int fea = cb & 15;
            uint32_t aux;
            int z;
            int w;
            int a_explicit = 0;
            if (fea < 0xe) {
                /* 0xfY: three indices via codeaux table + vertex FIFO */
                aux = codeaux[fea];
            } else {
                /* 0xfe / 0xff: aux byte read from the data stream (not LEB128) */
                if (data >= data_end)
                    return 0;
                aux = *data++;
                if (aux == 0u)
                    next = 0; /* explicit high-watermark restart */
                a_explicit = (fea == 0xf);
            }
            z = (int)(aux >> 4);
            w = (int)(aux & 15u);
            if (fea < 0xe) {
                a = next++;
            } else if (!a_explicit) {
                a = next++;
            } else {
                if (!gltf_meshopt_decode_explicit_index(&data, data_end, &last))
                    return 0;
                a = last;
            }
            if (z == 0) {
                b = next++;
            } else if (z < 0xf || fea < 0xe) {
                b = vertexfifo[(vfo - 1u - (uint32_t)(z - 1)) & 15u];
            } else {
                if (!gltf_meshopt_decode_explicit_index(&data, data_end, &last))
                    return 0;
                b = last;
            }
            if (w == 0) {
                c = next++;
            } else if (w < 0xf || fea < 0xe) {
                c = vertexfifo[(vfo - 1u - (uint32_t)(w - 1)) & 15u];
            } else {
                if (!gltf_meshopt_decode_explicit_index(&data, data_end, &last))
                    return 0;
                c = last;
            }
            /* Reads above use the pre-push FIFO state; pushes follow in spec order. */
            edgefifo[efo & 15u][0] = b;
            edgefifo[efo & 15u][1] = a;
            efo++;
            edgefifo[efo & 15u][0] = c;
            edgefifo[efo & 15u][1] = b;
            efo++;
            edgefifo[efo & 15u][0] = a;
            edgefifo[efo & 15u][1] = c;
            efo++;
            vertexfifo[vfo++ & 15u] = a;
            if (z == 0 || (z == 0xf && fea >= 0xe))
                vertexfifo[vfo++ & 15u] = b;
            if (w == 0 || (w == 0xf && fea >= 0xe))
                vertexfifo[vfo++ & 15u] = c;
        }
        if (!gltf_meshopt_write_index(dst, t * 3u + 0u, stride, a) ||
            !gltf_meshopt_write_index(dst, t * 3u + 1u, stride, b) ||
            !gltf_meshopt_write_index(dst, t * 3u + 2u, stride, c))
            return 0;
    }
    /* Spec: the data region must be fully consumed by the triangle codes. */
    return data == data_end;
}

/// @brief Mode 2 (INDICES): dual-baseline zigzag varint delta stream.
/// @details Spec structure: header byte 0xd1; one varint-7 value per index whose LSB
///          selects one of two baselines and whose remaining bits carry a zigzag delta;
///          a 4-byte reserved tail.
/// @param[out] dst Output index buffer providing at least `count * stride` bytes.
/// @param count Number of indices to decode.
/// @param stride Output index width, either two or four bytes.
/// @param[in] src Compressed index bitstream.
/// @param src_len Number of readable bytes at @p src.
/// @return Non-zero when every index is decoded and representable; zero on an
///         invalid header, truncated varint, or output-width overflow.
static int gltf_meshopt_decode_indices(
    uint8_t *dst, size_t count, size_t stride, const uint8_t *src, size_t src_len) {
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t last[2] = {0u, 0u};
    if (src_len < 1u + 4u)
        return 0;
    if (src[0] != 0xD1u)
        return 0;
    data = src + 1;
    data_end = src + src_len - 4u;
    for (size_t i = 0; i < count; i++) {
        uint32_t v;
        uint32_t baseline;
        int32_t delta;
        if (!gltf_meshopt_read_leb128(&data, data_end, &v))
            return 0;
        baseline = v & 1u;
        delta = (v & 2u) ? ~(int32_t)(v >> 2) : (int32_t)(v >> 2);
        last[baseline] = (uint32_t)((int64_t)last[baseline] + delta);
        if (!gltf_meshopt_write_index(dst, i, stride, last[baseline]))
            return 0;
    }
    return 1;
}

/// @brief Read a signed 16-bit little-endian component.
/// @param[in] p Address of two readable little-endian bytes.
/// @return Reconstructed signed 16-bit value.
static int16_t gltf_meshopt_read_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/// @brief Write a signed 16-bit little-endian component.
/// @param[out] p Address of two writable bytes.
/// @param v Signed value to encode.
static void gltf_meshopt_write_i16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v & 0xFFu);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFu);
}

/// @brief Round-to-nearest quantization of a [-1, 1] float into an N-bit signed integer.
/// @param value Normalized floating-point component to quantize.
/// @param intn_max Positive maximum of the destination signed integer representation.
/// @return Quantized integer using symmetric half-away-from-zero rounding.
static long gltf_meshopt_round_norm(double value, double intn_max) {
    double scaled = value * intn_max;
    return (long)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

/// @brief Filter 1 (OCTAHEDRAL): rebuild unit vectors from K-bit octahedral components.
/// @details Components are 8-bit (stride 4) or 16-bit (stride 8); the third input
///          component encodes 1.0 as a K-bit signed-normalized integer, the fourth is
///          passed through verbatim. Decoding folds the negative hemisphere, then
///          renormalizes and quantizes to INT8/INT16 max per the spec.
/// @param[in,out] data Decoded attribute buffer transformed in place.
/// @param count Number of vector elements in @p data.
/// @param stride Element width: four bytes for 8-bit components or eight bytes
///               for 16-bit components.
static void gltf_meshopt_filter_octahedral(uint8_t *data, size_t count, size_t stride) {
    int wide = stride == 8u;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = data + i * stride;
        double x;
        double y;
        double one;
        double z;
        double t;
        double len;
        if (wide) {
            x = (double)gltf_meshopt_read_i16(e + 0);
            y = (double)gltf_meshopt_read_i16(e + 2);
            one = (double)gltf_meshopt_read_i16(e + 4);
        } else {
            x = (double)(int8_t)e[0];
            y = (double)(int8_t)e[1];
            one = (double)(int8_t)e[2];
        }
        if (one <= 0.0)
            continue; /* invalid 1.0 encoding: result unspecified; keep raw bytes */
        x /= one;
        y /= one;
        z = 1.0 - fabs(x) - fabs(y);
        t = z < 0.0 ? z : 0.0;
        x -= copysign(t, x);
        y -= copysign(t, y);
        len = sqrt(x * x + y * y + z * z);
        if (len <= 0.0)
            continue;
        x /= len;
        y /= len;
        z /= len;
        if (wide) {
            gltf_meshopt_write_i16(e + 0, (int16_t)gltf_meshopt_round_norm(x, 32767.0));
            gltf_meshopt_write_i16(e + 2, (int16_t)gltf_meshopt_round_norm(y, 32767.0));
            gltf_meshopt_write_i16(e + 4, (int16_t)gltf_meshopt_round_norm(z, 32767.0));
        } else {
            e[0] = (uint8_t)(int8_t)gltf_meshopt_round_norm(x, 127.0);
            e[1] = (uint8_t)(int8_t)gltf_meshopt_round_norm(y, 127.0);
            e[2] = (uint8_t)(int8_t)gltf_meshopt_round_norm(z, 127.0);
        }
        /* fourth component passes through unchanged */
    }
}

/// @brief Filter 2 (QUATERNION): rebuild unit quaternions from max-component encoding.
/// @details Three stored components are scaled by sqrt(2); the fourth encodes 1.0 in its
///          upper bits and the omitted (largest, always positive) component index in the
///          low 2 bits. Output components are written in cyclic order per the spec.
/// @param[in,out] data Decoded attribute buffer transformed in place.
/// @param count Number of quaternion elements in @p data.
/// @param stride Element width; only the required eight-byte representation is processed.
static void gltf_meshopt_filter_quaternion(uint8_t *data, size_t count, size_t stride) {
    if (stride != 8u)
        return;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = data + i * stride;
        int16_t in0 = gltf_meshopt_read_i16(e + 0);
        int16_t in1 = gltf_meshopt_read_i16(e + 2);
        int16_t in2 = gltf_meshopt_read_i16(e + 4);
        int16_t in3 = gltf_meshopt_read_i16(e + 6);
        double range = 1.0 / sqrt(2.0);
        double one = (double)(int16_t)(in3 | 3);
        int maxcomp = in3 & 3;
        double x;
        double y;
        double z;
        double w;
        int16_t out[4];
        if (one <= 0.0)
            continue;
        x = (double)in0 / one * range;
        y = (double)in1 / one * range;
        z = (double)in2 / one * range;
        w = 1.0 - x * x - y * y - z * z;
        w = w > 0.0 ? sqrt(w) : 0.0;
        out[(maxcomp + 1) & 3] = (int16_t)gltf_meshopt_round_norm(x, 32767.0);
        out[(maxcomp + 2) & 3] = (int16_t)gltf_meshopt_round_norm(y, 32767.0);
        out[(maxcomp + 3) & 3] = (int16_t)gltf_meshopt_round_norm(z, 32767.0);
        out[maxcomp & 3] = (int16_t)gltf_meshopt_round_norm(w, 32767.0);
        gltf_meshopt_write_i16(e + 0, out[0]);
        gltf_meshopt_write_i16(e + 2, out[1]);
        gltf_meshopt_write_i16(e + 4, out[2]);
        gltf_meshopt_write_i16(e + 6, out[3]);
    }
}

/// @brief Filter 3 (EXPONENTIAL): rebuild float32 values from exponent/mantissa words.
/// @details Each 32-bit little-endian word carries a signed 8-bit exponent in the top
///          byte and a signed 24-bit mantissa; the result is the mantissa times two
///          raised to that exponent, written back as a little-endian float32.
/// @param[in,out] data Decoded attribute bytes transformed in place.
/// @param count Number of elements in @p data.
/// @param stride Byte width of each element; the total byte count must be word-aligned.
static void gltf_meshopt_filter_exponential(uint8_t *data, size_t count, size_t stride) {
    size_t words = (count * stride) / 4u;
    for (size_t i = 0; i < words; i++) {
        uint8_t *p = data + i * 4u;
        uint32_t raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                       ((uint32_t)p[3] << 24);
        int32_t e = (int32_t)raw >> 24;
        int32_t m = ((int32_t)(raw << 8)) >> 8;
        float value;
        float scale;
        uint32_t scale_bits;
        uint32_t bits;
        if (e < -100)
            e = -100;
        if (e > 100)
            e = 100;
        /* 2^e built directly from IEEE-754 bits: e in [-100,100] keeps the biased
         * exponent in [27, 227], safely inside the normalized range (no libm). */
        scale_bits = (uint32_t)(127 + e) << 23;
        memcpy(&scale, &scale_bits, sizeof(scale));
        value = (float)m * scale;
        memcpy(&bits, &value, sizeof(bits));
        p[0] = (uint8_t)(bits & 0xFFu);
        p[1] = (uint8_t)((bits >> 8) & 0xFFu);
        p[2] = (uint8_t)((bits >> 16) & 0xFFu);
        p[3] = (uint8_t)((bits >> 24) & 0xFFu);
    }
}

/// @brief Map the JSON `mode` string to a codec id (-1 when unknown).
/// @param[in] mode Extension mode name, or `NULL`.
/// @return One of the `GLTF_MESHOPT_MODE_*` identifiers, or -1 for an unsupported name.
static int gltf_meshopt_mode_from_string(const char *mode) {
    if (!mode)
        return -1;
    if (strcmp(mode, "ATTRIBUTES") == 0)
        return GLTF_MESHOPT_MODE_ATTRIBUTES;
    if (strcmp(mode, "TRIANGLES") == 0)
        return GLTF_MESHOPT_MODE_TRIANGLES;
    if (strcmp(mode, "INDICES") == 0)
        return GLTF_MESHOPT_MODE_INDICES;
    return -1;
}

/// @brief Map the JSON `filter` string to a filter id (-1 when unknown).
/// @param[in] filter Extension filter name, or `NULL` to select `NONE`.
/// @return One of the `GLTF_MESHOPT_FILTER_*` identifiers, or -1 for an unsupported name.
static int gltf_meshopt_filter_from_string(const char *filter) {
    if (!filter || strcmp(filter, "NONE") == 0)
        return GLTF_MESHOPT_FILTER_NONE;
    if (strcmp(filter, "OCTAHEDRAL") == 0)
        return GLTF_MESHOPT_FILTER_OCTAHEDRAL;
    if (strcmp(filter, "QUATERNION") == 0)
        return GLTF_MESHOPT_FILTER_QUATERNION;
    if (strcmp(filter, "EXPONENTIAL") == 0)
        return GLTF_MESHOPT_FILTER_EXPONENTIAL;
    return -1;
}

/// @brief Validate one extension object per the JSON-schema constraints in the spec.
/// @param mode Parsed meshopt codec identifier.
/// @param filter Parsed meshopt post-decode filter identifier.
/// @param stride Declared decoded byte stride.
/// @param count Declared decoded element count.
/// @return Non-zero when the mode, filter, stride, and count combination is legal.
static int gltf_meshopt_view_config_valid(int mode, int filter, size_t stride, int64_t count) {
    if (count <= 0 || stride == 0)
        return 0;
    switch (mode) {
    case GLTF_MESHOPT_MODE_ATTRIBUTES:
        if (stride % 4u != 0 || stride > 256u)
            return 0;
        if (filter == GLTF_MESHOPT_FILTER_OCTAHEDRAL && stride != 4u && stride != 8u)
            return 0;
        if (filter == GLTF_MESHOPT_FILTER_QUATERNION && stride != 8u)
            return 0;
        return 1;
    case GLTF_MESHOPT_MODE_TRIANGLES:
        if (count % 3 != 0)
            return 0;
        return (stride == 2u || stride == 4u) && filter == GLTF_MESHOPT_FILTER_NONE;
    case GLTF_MESHOPT_MODE_INDICES:
        return (stride == 2u || stride == 4u) && filter == GLTF_MESHOPT_FILTER_NONE;
    default:
        return 0;
    }
}

/// @brief Free every decoded shadow buffer and reset the table.
/// @param[in,out] table Table whose owned view buffers and arrays are released;
///                      `NULL` is accepted.
static void gltf_meshopt_table_free(gltf_meshopt_table_t *table) {
    if (!table)
        return;
    if (table->view_data) {
        for (int64_t i = 0; i < table->view_count; i++)
            free(table->view_data[i]);
    }
    free(table->view_data);
    free(table->view_len);
    memset(table, 0, sizeof(*table));
}

/// @brief Detect whether the document lists EXT_meshopt_compression in its extension arrays.
/// @param[in] root Parsed glTF document root.
/// @return Non-zero when either `extensionsUsed` or `extensionsRequired` names
///         `EXT_meshopt_compression`; zero otherwise.
static int gltf_document_lists_meshopt(void *root) {
    static const char *arrays[2] = {"extensionsUsed", "extensionsRequired"};
    for (int a = 0; a < 2; a++) {
        void *list = jarr(root, arrays[a]);
        for (int64_t i = 0; i < jarr_len(list); i++) {
            rt_string name = (rt_string)rt_seq_get(list, i);
            const char *ext = rt_string_is_handle(name) ? rt_string_cstr(name) : NULL;
            if (ext && strcmp(ext, "EXT_meshopt_compression") == 0)
                return 1;
        }
    }
    return 0;
}

/// @brief Decode every EXT_meshopt_compression bufferView into the shadow table.
/// @details Walks `bufferViews`; for each view carrying the extension, validates the
///          config against the spec's JSON-schema constraints (including that the parent
///          view's byteLength equals byteStride * count), decodes the compressed bytes
///          into an owned buffer, and applies the post-decompression filter. Any
///          malformed view fails the load with a named CORRUPT diagnostic.
/// @param[in] root Parsed glTF document root.
/// @param[in] buffers Materialized source buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param[out] table Destination shadow-buffer table, reset before decoding and
///                   owning all successful decoded allocations.
/// @return Non-zero on success, including when no compressed views exist; zero
///         on allocation failure or malformed extension data.
static int gltf_meshopt_decode_views(void *root,
                                     gltf_buffer_t *buffers,
                                     int buf_count,
                                     gltf_meshopt_table_t *table) {
    void *views = jarr(root, "bufferViews");
    int64_t view_count = jarr_len(views);
    memset(table, 0, sizeof(*table));
    if (!views || view_count <= 0)
        return 1;
    for (int64_t vi = 0; vi < view_count; vi++) {
        void *view = rt_seq_get(views, vi);
        void *extensions = view ? jget(view, "extensions") : NULL;
        void *ext = extensions ? jget(extensions, "EXT_meshopt_compression") : NULL;
        int64_t src_buffer;
        size_t src_offset;
        size_t src_length;
        size_t stride;
        int64_t count;
        int64_t parent_length;
        int mode;
        int filter;
        size_t decoded_len;
        size_t src_end;
        uint8_t *decoded;
        int ok;
        if (!ext)
            continue;
        if (!table->view_data) {
            if ((size_t)view_count > SIZE_MAX / sizeof(uint8_t *))
                return 0;
            table->view_data = (uint8_t **)calloc((size_t)view_count, sizeof(uint8_t *));
            table->view_len = (size_t *)calloc((size_t)view_count, sizeof(size_t));
            if (!table->view_data || !table->view_len) {
                gltf_meshopt_table_free(table);
                return 0;
            }
            table->view_count = view_count;
        }
        src_buffer = jint(ext, "buffer", -1);
        count = jint(ext, "count", -1);
        mode = gltf_meshopt_mode_from_string(jstr(ext, "mode"));
        filter = gltf_meshopt_filter_from_string(jstr(ext, "filter"));
        if (!gltf_nonnegative_size(ext, "byteOffset", 0, &src_offset) ||
            !gltf_nonnegative_size(ext, "byteLength", 0, &src_length))
            goto corrupt;
        {
            int64_t stride_raw = jint(ext, "byteStride", -1);
            if (stride_raw <= 0 || stride_raw > 256)
                goto corrupt;
            stride = (size_t)stride_raw;
        }
        if (mode < 0 || filter < 0 ||
            !gltf_meshopt_view_config_valid(mode, filter, stride, count))
            goto corrupt;
        if (src_buffer < 0 || src_buffer >= buf_count || !buffers[(int)src_buffer].data)
            goto corrupt;
        if (!gltf_checked_add_size(src_offset, src_length, &src_end) ||
            src_end > buffers[(int)src_buffer].len || src_length == 0)
            goto corrupt;
        if (!gltf_checked_mul_size(stride, (size_t)count, &decoded_len))
            goto corrupt;
        parent_length = jint(view, "byteLength", -1);
        if (parent_length < 0 || (size_t)parent_length != decoded_len)
            goto corrupt;
        decoded = (uint8_t *)malloc(decoded_len);
        if (!decoded) {
            gltf_meshopt_table_free(table);
            return 0;
        }
        {
            const uint8_t *src = buffers[(int)src_buffer].data + src_offset;
            if (mode == GLTF_MESHOPT_MODE_ATTRIBUTES)
                ok = gltf_meshopt_decode_attributes(
                    decoded, (size_t)count, stride, src, src_length);
            else if (mode == GLTF_MESHOPT_MODE_TRIANGLES)
                ok = gltf_meshopt_decode_triangles(
                    decoded, (size_t)count, stride, src, src_length);
            else
                ok = gltf_meshopt_decode_indices(decoded, (size_t)count, stride, src, src_length);
        }
        if (!ok) {
            free(decoded);
            goto corrupt;
        }
        if (filter == GLTF_MESHOPT_FILTER_OCTAHEDRAL)
            gltf_meshopt_filter_octahedral(decoded, (size_t)count, stride);
        else if (filter == GLTF_MESHOPT_FILTER_QUATERNION)
            gltf_meshopt_filter_quaternion(decoded, (size_t)count, stride);
        else if (filter == GLTF_MESHOPT_FILTER_EXPONENTIAL)
            gltf_meshopt_filter_exponential(decoded, (size_t)count, stride);
        table->view_data[vi] = decoded;
        table->view_len[vi] = decoded_len;
        continue;
    corrupt:
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                            "GLTF.Load: bufferView %lld has invalid EXT_meshopt_compression data",
                            (long long)vi);
        gltf_meshopt_table_free(table);
        return 0;
    }
    return 1;
}
