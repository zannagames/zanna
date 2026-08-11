//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_textureasset3d.c
// Purpose: TextureAsset3D — loads KTX2 textures (RGBA8/BC3/BC7/ETC2/ASTC),
//   retains native compressed mip payloads for GPU upload, and software-decodes
//   RGBA8/BC3/BC7/ETC2 plus ASTC LDR void-extent blocks to a Pixels fallback
//   for backends without native support.
//
// Key invariants:
//   - Only 2D, single-layer, single-face KTX2 (no array/cube/3D) is accepted.
//   - Software fallbacks cap at TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES; files
//     cap at TEXTUREASSET3D_MAX_FILE_BYTES. All offset/length math is overflow-
//     and bounds-checked against the file size before any read.
//   - BC7 software decode covers modes 0-7. ETC2 fallback covers RGBA8/EAC
//     blocks in individual/differential color modes. ASTC fallback covers LDR
//     2D void-extent blocks; compressed decode failures keep native payloads
//     and expose an 8x8 magenta/black checker fallback.
//   - cache_identity is a process-unique nonzero id; native_revision bumps on
//     every resident-range change so the backend cache key invalidates.
//
// Ownership/Lifetime:
//   - TextureAsset3D is GC-managed; the finalizer frees the mip table, the
//     per-mip native payloads, and releases each decoded Pixels mip.
//
// Links: rt_textureasset3d.h, rt_pixels.h, rt_asset.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_textureasset3d.c
 * @brief Assembles the TextureAsset3D KTX2 loader and software codec implementation.
 * @details Defines container limits and supported Vulkan format identifiers, then includes the
 *          ownership/core, block-codec, BasisLZ, UASTC, and KTX2 parsing fragments into one
 *          graphics-enabled translation unit. Native compressed mip payloads are retained for
 *          capable backends while decoded Pixels fallbacks serve unsupported formats.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_textureasset3d.h"

#include "rt_alloc_size.h"
#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_compress.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_trap.h"
#include "rt_zstd.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXTUREASSET3D_KTX2_HEADER_SIZE 80u
#define TEXTUREASSET3D_KTX2_LEVEL_ENTRY_SIZE 24u
#define TEXTUREASSET3D_MAX_FILE_BYTES (256u * 1024u * 1024u)
#define TEXTUREASSET3D_MAX_RGBA8_FALLBACK_BYTES (256u * 1024u * 1024u)
#define TEXTUREASSET3D_MAX_MIP_COUNT 32u

#define textureasset3d_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))
#define textureasset3d_ftell(fp) rt_file_stdio_tell64((fp))

#define VK_FORMAT_R8G8B8A8_UNORM 37u
#define VK_FORMAT_R8G8B8A8_SRGB 43u
#define VK_FORMAT_BC1_RGB_UNORM_BLOCK 131u
#define VK_FORMAT_BC1_RGB_SRGB_BLOCK 132u
#define VK_FORMAT_BC1_RGBA_UNORM_BLOCK 133u
#define VK_FORMAT_BC1_RGBA_SRGB_BLOCK 134u
#define VK_FORMAT_BC3_UNORM_BLOCK 137u
#define VK_FORMAT_BC3_SRGB_BLOCK 138u
#define VK_FORMAT_BC4_UNORM_BLOCK 139u
#define VK_FORMAT_BC5_UNORM_BLOCK 141u
#define VK_FORMAT_BC6H_UFLOAT_BLOCK 143u
#define VK_FORMAT_BC6H_SFLOAT_BLOCK 144u
#define VK_FORMAT_BC7_UNORM_BLOCK 145u
#define VK_FORMAT_BC7_SRGB_BLOCK 146u
#define VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK 151u
#define VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK 152u
#define VK_FORMAT_ASTC_4X4_UNORM_BLOCK 157u
#define VK_FORMAT_ASTC_12X12_SRGB_BLOCK 184u

// clang-format off
#include "rt_textureasset3d_core.inc"
#include "rt_textureasset3d_codecs.inc"
#include "rt_textureasset3d_bc6h.inc"
#include "rt_textureasset3d_basislz.inc"
#include "rt_textureasset3d_uastc.inc"
#include "rt_textureasset3d_ktx2.inc"
// clang-format on
#endif
