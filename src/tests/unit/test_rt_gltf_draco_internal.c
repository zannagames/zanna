//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_gltf_draco_internal.c
// Purpose: White-box arithmetic and malformed-input regression tests for the
//   clean-room Draco decoder used by KHR_draco_mesh_compression.
//
// Key invariants:
//   - Serialized counts, products, shifts, and signed predictor arithmetic fail
//     closed before narrowing, overflow, allocation, or pointer traversal.
//   - Valid boundary encodings preserve their exact signed and unsigned values.
//
// Links: src/runtime/graphics/3d/assets/rt_gltf_draco.inc
//
//===----------------------------------------------------------------------===//

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/graphics/3d/assets/rt_gltf_draco.inc"

static int g_failures = 0;

static void expect(int condition, const char *message) {
    printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition)
        g_failures++;
}

static size_t append_varint(uint8_t *bytes, size_t position, uint64_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value != 0u)
            byte |= 0x80u;
        bytes[position++] = byte;
    } while (value != 0u);
    return position;
}

static void test_checked_arithmetic(void) {
    size_t size_result = 0;
    uint32_t scalar_count = 0;
    int64_t signed_result = 0;
    uint64_t unsigned_result = 0;

    expect(draco_size_mul(7u, 9u, &size_result) && size_result == 63u,
           "checked size product accepts a representable product");
    expect(!draco_size_mul(SIZE_MAX, 2u, &size_result), "checked size product rejects overflow");
    expect(draco_scalar_count(3u, 4u, &scalar_count) && scalar_count == 12u,
           "tuple scalar count preserves a valid product");
    expect(!draco_scalar_count(UINT32_MAX, 4u, &scalar_count),
           "tuple scalar count rejects uint32 overflow");
    expect(draco_i64_add(INT64_MAX - 1, 1, &signed_result) && signed_result == INT64_MAX,
           "checked signed addition accepts the upper boundary");
    expect(!draco_i64_add(INT64_MAX, 1, &signed_result),
           "checked signed addition rejects overflow");
    expect(!draco_i64_sub(INT64_MIN, 1, &signed_result),
           "checked signed subtraction rejects underflow");
    expect(!draco_i64_mul(INT64_MIN, -1, &signed_result),
           "checked signed multiplication rejects INT64_MIN negation");
    expect(draco_i64_mul(-7, 9, &signed_result) && signed_result == -63,
           "checked signed multiplication preserves valid signs");
    expect(!draco_u64_mul(UINT64_MAX, 2u, &unsigned_result),
           "checked unsigned multiplication rejects overflow");
    expect(!draco_i64_abs(INT64_MIN, &signed_result), "checked absolute value rejects INT64_MIN");
}

static void test_varints_and_zigzag(void) {
    static const uint8_t max_u64[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    static const uint8_t overflow_u64[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
    static const uint8_t overflow_u32[] = {0x80, 0x80, 0x80, 0x80, 0x10};
    draco_buf buffer = {max_u64, sizeof(max_u64), 0u, 0};
    uint32_t narrowed = 0;

    expect(draco_read_varint(&buffer) == UINT64_MAX && !buffer.failed,
           "ten-byte varint preserves UINT64_MAX");
    buffer.data = overflow_u64;
    buffer.size = sizeof(overflow_u64);
    buffer.pos = 0;
    buffer.failed = 0;
    (void)draco_read_varint(&buffer);
    expect(buffer.failed, "ten-byte varint rejects excess payload bits");
    buffer.data = overflow_u32;
    buffer.size = sizeof(overflow_u32);
    buffer.pos = 0;
    buffer.failed = 0;
    expect(!draco_read_varint_u32(&buffer, &narrowed), "32-bit varint reader rejects narrowing");
    expect(draco_read_varint(NULL) == 0u, "null varint reader fails without dereference");

    expect(draco_zigzag_decode_u32(0u) == 0, "zigzag zero decodes exactly");
    expect(draco_zigzag_decode_u32(1u) == -1, "zigzag one decodes to negative one");
    expect(draco_zigzag_decode_u32(2u) == 1, "zigzag two decodes to positive one");
    expect(draco_zigzag_decode_u32(UINT32_MAX) == INT32_MIN,
           "zigzag UINT32_MAX decodes to INT32_MIN");
}

static void test_prediction_transforms(void) {
    draco_attribute attribute;
    int64_t prediction[4] = {1, 0, 0, 0};
    int32_t correction[4] = {INT32_MAX, INT32_MIN, 0, 0};
    int32_t output[4] = {0, 0, 0, 0};
    int32_t max_q = 0;
    int32_t max_value = 0;
    int32_t center = 0;

    memset(&attribute, 0, sizeof(attribute));
    attribute.wrap_min = 0;
    attribute.wrap_max = 1;
    draco_wrap_compute_original(&attribute, prediction, correction, output, 2);
    expect(output[0] == 0 && output[1] == 0,
           "wrap transform applies a full modulus to hostile corrections");

    expect(draco_normal_quant_params(255, &max_q, &max_value, &center) && max_q == 255 &&
               max_value == 254 && center == 127,
           "normal quantization derives bounded canonical parameters");
    expect(!draco_normal_quant_params(1, &max_q, &max_value, &center),
           "normal quantization rejects a degenerate modulus");
    expect(!draco_normal_quant_params((int32_t)(1u << 30), &max_q, &max_value, &center),
           "normal quantization rejects a signed-shift overflow range");

    attribute.normal_max_q = 255;
    prediction[0] = INT64_MAX;
    prediction[1] = INT64_MIN;
    draco_normal_oct_compute_original(&attribute, prediction, correction, output);
    expect(output[0] >= 0 && output[0] <= 254 && output[1] >= 0 && output[1] <= 254,
           "normal prediction canonicalizes extreme inputs without signed overflow");
}

static void test_attribute_conversion(void) {
    draco_attribute attribute;
    int32_t values[2] = {255, 0};

    memset(&attribute, 0, sizeof(attribute));
    attribute.components = 1;
    attribute.num_values = 1;
    attribute.quantization_bits = 8;
    attribute.quant_range = 2.0f;
    attribute.quant_min[0] = 1.0f;
    attribute.values = values;
    expect(draco_dequantize_attribute(&attribute) && attribute.out_floats &&
               fabsf(attribute.out_floats[0] - 3.0f) < 1e-6f,
           "quantized scalar conversion preserves a valid endpoint");
    free(attribute.out_floats);
    attribute.out_floats = NULL;

    values[0] = INT32_MIN;
    expect(!draco_dequantize_attribute(&attribute),
           "quantized scalar conversion rejects negative hostile values");
    values[0] = 0;
    attribute.quantization_bits = 31;
    expect(!draco_dequantize_attribute(&attribute),
           "quantized scalar conversion rejects undefined shift widths");

    memset(&attribute, 0, sizeof(attribute));
    attribute.components = 3;
    attribute.num_values = 1;
    attribute.quantization_bits = 8;
    attribute.normal_max_q = 255;
    attribute.values = values;
    values[0] = 127;
    values[1] = 127;
    expect(draco_decode_normals_attribute(&attribute) && attribute.out_floats &&
               isfinite(attribute.out_floats[0]) && isfinite(attribute.out_floats[1]) &&
               isfinite(attribute.out_floats[2]),
           "octahedral normal conversion produces finite output");
    free(attribute.out_floats);
    attribute.out_floats = NULL;
    values[0] = 255;
    expect(!draco_decode_normals_attribute(&attribute),
           "octahedral normal conversion rejects out-of-range coordinates");

    memset(&attribute, 0, sizeof(attribute));
    attribute.decoder_type = DRACO_SEQ_INTEGER;
    attribute.components = 4;
    attribute.num_values = UINT32_MAX;
    attribute.values = values;
    expect(!draco_transform_attribute_to_original(&attribute),
           "integer output conversion rejects overflowing tuple products");
}

static void test_generic_and_transform_metadata(void) {
    uint8_t boolean_bytes[] = {2u};
    uint8_t transform_bytes[9] = {0};
    draco_buf buffer = {boolean_bytes, sizeof(boolean_bytes), 0u, 0};
    draco_attribute attribute;
    uint32_t nan_bits = 0x7FC00000u;

    expect(!draco_decode_generic_attribute(NULL, NULL),
           "generic attribute decoder rejects null arguments");

    memset(&attribute, 0, sizeof(attribute));
    attribute.att_type = 4;
    attribute.data_type = 11;
    attribute.components = 1;
    attribute.num_values = 1;
    expect(!draco_decode_generic_attribute(&buffer, &attribute),
           "generic BOOL decoder rejects values outside zero and one");
    free(attribute.out_ints);

    memcpy(transform_bytes, &nan_bits, sizeof(nan_bits));
    transform_bytes[4] = 0;
    transform_bytes[5] = 0;
    transform_bytes[6] = 0x80;
    transform_bytes[7] = 0x3F;
    transform_bytes[8] = 8;
    buffer.data = transform_bytes;
    buffer.size = sizeof(transform_bytes);
    buffer.pos = 0;
    buffer.failed = 0;
    memset(&attribute, 0, sizeof(attribute));
    attribute.decoder_type = DRACO_SEQ_QUANTIZATION;
    attribute.components = 1;
    expect(!draco_read_transform_data(&buffer, &attribute),
           "quantization metadata rejects non-finite origins");
}

static void test_edgebreaker_and_entry_guards(void) {
    uint8_t bytes[64] = {0};
    size_t position = 0;
    draco_buf buffer;
    draco_eb edgebreaker;
    draco_mesh mesh;
    int unsupported = 0;
    const uint64_t excessive_faces = (uint64_t)INT32_MAX / 3u + 1u;
    static const uint8_t point_cloud[] = {'D', 'R', 'A', 'C', 'O', 2, 2, 0, 0, 0, 0};

    bytes[position++] = DRACO_EB_STANDARD;
    position = append_varint(bytes, position, 1u);
    position = append_varint(bytes, position, excessive_faces);
    bytes[position++] = 0;
    position = append_varint(bytes, position, 0u);
    position = append_varint(bytes, position, 0u);
    buffer.data = bytes;
    buffer.size = position;
    buffer.pos = 0;
    buffer.failed = 0;
    memset(&edgebreaker, 0, sizeof(edgebreaker));
    expect(!draco_eb_parse_and_alloc(&buffer, &edgebreaker),
           "edgebreaker rejects corner counts that cannot fit int32 identifiers");
    draco_eb_free(&edgebreaker);

    expect(!draco_decode_mesh(point_cloud, sizeof(point_cloud), NULL, &unsupported),
           "mesh entry point rejects a null destination");
    expect(!draco_decode_mesh(point_cloud, sizeof(point_cloud), &mesh, NULL),
           "mesh entry point accepts an optional unsupported-status pointer");
    expect(draco_find_attribute(NULL, 0) == NULL, "attribute lookup rejects a null mesh");
    memset(&mesh, 0, sizeof(mesh));
    mesh.num_attributes = UINT32_MAX;
    expect(draco_find_attribute(&mesh, 7) == NULL,
           "attribute lookup bounds a corrupt descriptor count");
}

int main(void) {
    printf("test_rt_gltf_draco_internal:\n");
    test_checked_arithmetic();
    test_varints_and_zigzag();
    test_prediction_transforms();
    test_attribute_conversion();
    test_generic_and_transform_metadata();
    test_edgebreaker_and_entry_guards();
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
