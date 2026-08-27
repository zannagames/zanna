//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal.m
// Purpose: Metal GPU backend for Zanna.Graphics3D.
//
// Key invariants:
//   - MSL shaders compile at context creation and all required pipeline variants must exist.
//   - Scene rendering uses reversed-Z while shadow maps retain the standard depth convention.
//   - Compact particles bind one 64-byte center/right/up/color record at vertex buffer index 6.
//
// Ownership/Lifetime:
//   - VGFXMetalContext owns all Metal resources and releases them when destroy_ctx drops the
//   object.
//   - Deferred command payloads remain Canvas3D-owned until the current frame has been encoded.
//
// Links: vgfx3d_backend.h, vgfx3d_backend_metal_shared.h, plans/3d/02-metal-backend.md
//
//===----------------------------------------------------------------------===//

#if defined(__APPLE__) && defined(ZANNA_ENABLE_GRAPHICS)

/* Include sys/types.h before Metal headers to define BSD types (u_int, u_char)
 * that are hidden by _POSIX_C_SOURCE but required by macOS system headers. */
#include <sys/types.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rt_postfx3d.h"
#include "rt_textureasset3d.h"
#include "vgfx.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_metal_shared.h"
#include "vgfx3d_backend_utils.h"
#include "vgfx3d_brdf_lut.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);

#define VGFX3D_STR_IMPL(x) #x
#define VGFX3D_STR(x) VGFX3D_STR_IMPL(x)

//=============================================================================
// Objective-C wrapper to hold Metal objects under ARC
//=============================================================================

@interface VGFXMetalContext : NSObject {
  @public
    float _view[16];
    float _projection[16];
    float _vp[16];
    float _prevVP[16];
    float _invVP[16];
    float _camPos[3];
    float _camForward[3];
    BOOL _camIsOrtho;
    BOOL _prevVPValid;
    /* MTL-07: Fog parameters (stored per-frame from begin_frame) */
    float _fogColor[3];
    float _fogNear, _fogFar;
    BOOL _fogEnabled;
    float _heightFog[4];       /* base, falloff, density (0 = off, folds blend), pad */
    float _heightFogSun[4];    /* sun tint rgb + amount (0 = off) */
    float _heightFogSunDir[4]; /* direction toward the sun + power */
    /* Image-based lighting (stored per-frame from begin_frame) */
    BOOL _iblEnabled;
    float _iblIntensity;
    float _iblSh[27];
    /* Light-snapshot revision last encoded into the active render encoder.
     * Encoder argument state persists across draws, so scene/light constant
     * uploads are skipped while consecutive draws share a revision. */
    uint32_t _encoderLightsRevision;
    /* Plan 05 TAA: sub-pixel projection jitter (clip-space units) applied in
     * begin_frame when the latched post-FX chain contains a TAA pass. The
     * resolve pass subtracts the current-minus-previous jitter delta from the
     * sampled velocity, so motion vectors rendered under jitter reproject
     * cleanly. */
    float _taaJitterClip[2];
    float _taaPrevJitterClip[2];
    uint32_t _taaFrameIndex;
    /* Explicit public ivar: backend chain ownership cookies bind to this stable address while
     * read-only property accesses may still copy the struct as a borrowed view. */
    vgfx3d_postfx_chain_t _gpuPostfxChain;
    BOOL _taaJitterActive;
    /* MTL-12: Shadow light view-projection matrices (stored from shadow_begin) */
    float _shadowLightVP[VGFX3D_MAX_SHADOW_LIGHTS][16];
    id<MTLTexture> _shadowDepthTexture[VGFX3D_MAX_SHADOW_LIGHTS];
    /* Shadow atlas for slots >= VGFX3D_CSM_SLOTS: 4x2 tiles at the per-slot
     * resolution, cleared once per frame on its first tile pass. */
    id<MTLTexture> _shadowAtlasTexture;
    uint64_t _shadowAtlasClearedSerial;
    int32_t _shadowPassSlot;
    int8_t _shadowPassFailed;
    int32_t _shadowCount;
    int8_t _shadowComplete[VGFX3D_MAX_SHADOW_LIGHTS];
    float _shadowBias;
    /* Plan 06: per-frame shadow filtering params from camera params. */
    float _shadowStrength;
    float _shadowSlopeBias;
    int32_t _shadowQuality;
    /* Plan 07: last-four revision cache for GPU cluster tables. Binning is
     * camera-dependent, so revision keys are dropped every begin_frame. Each
     * miss gets new backing storage retained by frameBuffers through GPU
     * completion; evicting a cache handle therefore cannot mutate encoded data. */
    id<MTLBuffer> _clusterBuffers[4];
    uint32_t _clusterBufferRevisions[4];
    int32_t _clusterBufferCursor;
    id<MTLBuffer> _clusterDummyBuffer;
    /* Plan 10: opaque-pass depth snapshot (blitted at the opaque->transparent
     * seam) + validity latch; camera clip planes for shader linearization. */
    int8_t _opaqueDepthValid;
    float _camNear, _camFar;
    /* Ambient last encoded into the per-scene constants; ambient is a per-draw
     * parameter, so it participates in the lights-revision dirty check. */
    float _encoderAmbient[3];
    /* Scene-depth probes (lens flares): NDC points registered during frame
     * building are blitted from the scene depth texture into a frame-ring buffer at
     * commit, and the completed handler publishes them into _depthProbeResults
     * (guarded by @synchronized(self)) for reads while building the next frame.
     * Never stalls the pipeline; results carry one frame of latency. */
    float _depthProbeRequests[VGFX3D_DEPTH_PROBE_MAX][2];
    int32_t _depthProbeRequestCount;
    float _depthProbeResults[VGFX3D_DEPTH_PROBE_MAX];
    int32_t _depthProbeResultCount;
    /* GAP-10: backend telemetry snapshot mirrored by get_backend_stats.
     * Zero-initialized with the object; texture_fallback_binds stays zero on
     * Metal (streaming fallbacks resolve inside ctx-free helpers). */
    vgfx3d_backend_stats_t _stats;
    uint8_t *_textureUploadScratchRGBA;
    size_t _textureUploadScratchRGBABytes;
    uint8_t *_textureUploadScratchBGRA;
    size_t _textureUploadScratchBGRABytes;
}
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateAdditive;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnly;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnlyAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnlyAdditive;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineState;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateAdditive;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateColorOnly;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateColorOnlyAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateColorOnlyAdditive;
/* Retained-unit-quad particle variants consume the compact four-vec4 instance record. */
@property(nonatomic, strong) id<MTLRenderPipelineState> particlePipelineStateAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> particlePipelineStateAdditive;
@property(nonatomic, strong) id<MTLRenderPipelineState> particlePipelineStateColorOnlyAlpha;
@property(nonatomic, strong) id<MTLRenderPipelineState> particlePipelineStateColorOnlyAdditive;
/* R20 compact-vertex-stream twins of the twelve mesh pipelines above plus the
 * shadow pipeline. Same shader functions; only the vertex descriptor differs
 * (48-byte packed layout). compactPipelinesReady gates both cache encoding and
 * pipeline selection so a build failure degrades to the full layout. */
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateAlphaCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateAdditiveCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnlyCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnlyAlphaCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipelineStateColorOnlyAdditiveCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateAlphaCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateAdditiveCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateColorOnlyCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> instancedPipelineStateColorOnlyAlphaCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState>
    instancedPipelineStateColorOnlyAdditiveCompact;
@property(nonatomic, strong) id<MTLRenderPipelineState> shadowPipelineCompact;
@property(nonatomic) BOOL compactPipelinesReady;
@property(nonatomic, strong) id<MTLDepthStencilState> depthState;
@property(nonatomic, strong) id<MTLDepthStencilState> depthStateNoWrite;
@property(nonatomic, strong) id<MTLDepthStencilState> depthStateDisabled;
@property(nonatomic, strong) CAMetalLayer *metalLayer;
@property(nonatomic, strong) id<MTLTexture> depthTexture;
@property(nonatomic, strong) id<MTLTexture> opaqueDepthTexture;
@property(nonatomic, strong) id<MTLTexture> opaqueDepthDummy;
@property(nonatomic, strong) NSMutableArray *depthProbeBuffers;
@property(nonatomic, strong) id<MTLLibrary> library;
@property(nonatomic, strong) id<MTLCommandBuffer> cmdBuf;
@property(nonatomic, strong) id<MTLRenderCommandEncoder> encoder;
@property(nonatomic, strong) id<CAMetalDrawable> drawable;
@property(nonatomic) int32_t width;
@property(nonatomic) int32_t height;
@property(nonatomic) float renderScale;
@property(nonatomic) float clearR, clearG, clearB;
@property(nonatomic) BOOL inFrame;
/* Per-frame Metal buffer references — prevents ARC from releasing vertex/index
 * buffers before the GPU finishes executing draw commands. */
@property(nonatomic, strong) NSMutableArray *frameBuffers;
/* Default 1x1 white texture (bound when no material texture is set) */
@property(nonatomic, strong) id<MTLTexture> defaultTexture;
@property(nonatomic, strong) id<MTLSamplerState> defaultSampler;
/* Render-to-texture state */
@property(nonatomic, strong) id<MTLTexture> rttColorTexture;
@property(nonatomic, strong) id<MTLTexture> rttMotionTexture;
@property(nonatomic, strong) id<MTLTexture> rttDepthTexture;
@property(nonatomic) BOOL rttActive;
@property(nonatomic) int32_t rttWidth, rttHeight;
@property(nonatomic, assign) vgfx3d_rendertarget_t *rttTarget; /* CPU-side RT for readback */
/* Generation-aware texture/cubemap caches keyed by runtime object identity. */
@property(nonatomic, strong) NSMutableDictionary *textureCache;
@property(nonatomic, strong) NSMutableDictionary *cubemapCache;
@property(nonatomic, strong) NSMutableDictionary *geometryCache;
@property(nonatomic, strong) NSMutableDictionary *morphCache;
@property(nonatomic, strong) NSMutableDictionary *extraInfluenceCache;
@property(nonatomic, strong) NSMutableDictionary *renderTargetCache;
@property(nonatomic, strong) NSMutableDictionary *samplerCache;
@property(nonatomic) uint64_t frameSerial;
@property(nonatomic) uint64_t textureUploadBytes;
@property(nonatomic) uint64_t textureUploadBudgetBytes;
@property(nonatomic, strong) id<MTLSamplerState> sharedSampler;
@property(nonatomic, strong) id<MTLSamplerState> cubeSampler;
@property(nonatomic, strong) id<MTLTexture> defaultCubemap;
@property(nonatomic, strong) id<MTLTexture> brdfLutTexture;
/* MTL-12: Shadow mapping state */
@property(nonatomic, strong) id<MTLRenderPipelineState> shadowPipeline;
@property(nonatomic, strong) id<MTLSamplerState> shadowSampler;
@property(nonatomic, strong) id<MTLDepthStencilState> shadowDepthState;
/* MTL-13: Instanced rendering — per-draw slices of a transient ring slot */
@property(nonatomic, strong) id<MTLBuffer> instanceBuf;
@property(nonatomic) NSUInteger instanceBufLength;
@property(nonatomic) NSUInteger instanceWriteOffset;
/* Reusable transient geometry buffers for non-cacheable draws. */
@property(nonatomic, strong) id<MTLBuffer> dynamicVertexBuf;
@property(nonatomic, strong) id<MTLBuffer> dynamicIndexBuf;
@property(nonatomic) NSUInteger dynamicVertexBufLength;
@property(nonatomic) NSUInteger dynamicIndexBufLength;
@property(nonatomic) NSUInteger dynamicVertexWriteOffset;
@property(nonatomic) NSUInteger dynamicIndexWriteOffset;
/* Frames-in-flight throttle + transient-buffer ring.
 * Every context command buffer waits on the semaphore at creation and signals it
 * on completion, capping unfinished command buffers at VGFX3D_METAL_MAX_INFLIGHT.
 * Each creation also rotates the transient ring slot that backs the dynamic
 * vertex/index/instance buffers, so the CPU never rewrites a slice the GPU may
 * still be reading for an earlier command buffer. */
@property(nonatomic, strong) dispatch_semaphore_t inflightSemaphore;
@property(nonatomic, strong) NSMutableArray *dynamicVertexPool;
@property(nonatomic, strong) NSMutableArray *dynamicIndexPool;
@property(nonatomic, strong) NSMutableArray *instancePool;
@property(nonatomic) NSUInteger transientRingCursor;
@property(nonatomic) NSUInteger transientRingSlot;
/* MTL-11: Post-processing state */
@property(nonatomic, strong) id<MTLTexture> postfxColorTexture;
@property(nonatomic, strong) id<MTLTexture> postfxScratchTexture;
/* Plan 61: cached 256x16 COLOR_LUT strip; (key, revision) mirror the snapshot's
 * (texel pointer, pixels generation) so a re-authored LUT re-uploads. */
@property(nonatomic, strong) id<MTLTexture> postfxLutTexture;
@property(nonatomic) uintptr_t postfxLutKey;
@property(nonatomic) uint64_t postfxLutRevision;
/* ADR 0247 / V1b: the ping-pong intermediates are RGBA16F so pre-tonemap
 * passes keep >1.0 energy (ACES gets its shoulder); the final image resolves
 * into this BGRA8 target because the present path blits format-matched. */
@property(nonatomic, strong) id<MTLTexture> postfxResolveTexture;
@property(nonatomic, strong) id<MTLRenderPipelineState> postfxPipeline;
@property(nonatomic, strong) id<MTLRenderPipelineState> postfxPipelineHdr;
@property(nonatomic, strong) id<MTLLibrary> postfxLibrary;
/* Plan 05: mip-chain bloom targets (half-res RGBA16F chain) + pass pipelines. */
@property(nonatomic, strong) NSMutableArray<id<MTLTexture>> *bloomMipTextures;
@property(nonatomic, strong) id<MTLRenderPipelineState> bloomDownPipeline;
@property(nonatomic, strong) id<MTLRenderPipelineState> bloomUpPipeline;
/* Plan 05: TAA resolve pipeline + ping-pong history (RGBA16F, persisted across frames). */
@property(nonatomic, strong) id<MTLRenderPipelineState> taaResolvePipeline;
/* Plan 10: screen-space reflections pass (ray-march against scene depth). */
@property(nonatomic, strong) id<MTLRenderPipelineState> ssrPipeline;
@property(nonatomic, strong) id<MTLTexture> taaHistoryA;
@property(nonatomic, strong) id<MTLTexture> taaHistoryB;
@property(nonatomic) int32_t taaHistoryParity;
@property(nonatomic) int8_t taaHistoryValid;
@property(nonatomic, strong) id<MTLTexture> overlayColorTexture;
@property(nonatomic, strong) id<MTLTexture> overlayMotionTexture;
@property(nonatomic, strong) id<MTLTexture> overlayDepthTexture;
/* Window-backed scene/postfx rendering stays offscreen until present time. */
@property(nonatomic, strong) id<MTLTexture> offscreenColor;
@property(nonatomic, strong) id<MTLTexture> offscreenMotion;
@property(nonatomic, strong) id<MTLTexture> displayTexture;
@property(nonatomic, strong) id<MTLRenderPipelineState> skyboxPipeline;
@property(nonatomic, strong) id<MTLRenderPipelineState> skyboxColorPipeline;
@property(nonatomic, strong) id<MTLDepthStencilState> skyboxDepthState;
@property(nonatomic, strong) id<MTLBuffer> skyboxVertexBuffer;
@property(nonatomic, assign) vgfx_window_t vgfxWin; /* for framebuffer readback */
@property(nonatomic, strong) NSMutableData *instanceScratch;
@property(nonatomic) NSUInteger instanceCapacity;
@property(nonatomic) vgfx3d_metal_target_kind_t currentTargetKind;
@property(nonatomic) int8_t gpuPostfxEnabled;
@property(nonatomic) int8_t gpuPostfxChainValid;
@property(nonatomic) vgfx3d_postfx_chain_t gpuPostfxChain;
@property(nonatomic) vgfx3d_metal_frame_history_t frameHistory;
@property(nonatomic) int8_t postfxEncodedThisFrame;
@property(nonatomic) int8_t postfxCompositedToDrawable;
@property(nonatomic) int8_t captureAfterPresent;
@end

@implementation VGFXMetalContext
@synthesize gpuPostfxChain = _gpuPostfxChain;

- (void)dealloc {
    free(_textureUploadScratchRGBA);
    free(_textureUploadScratchBGRA);
}

@end

@interface VGFXMetalTextureCacheEntry : NSObject
@property(nonatomic, strong) id<MTLTexture> texture;
@property(nonatomic, strong) VGFXMetalTextureCacheEntry *fallbackEntry;
@property(nonatomic) uint64_t generation;
@property(nonatomic) uint64_t pendingGeneration;
@property(nonatomic) uint64_t failedGeneration;
@property(nonatomic) int32_t width;
@property(nonatomic) int32_t height;
@property(nonatomic) int32_t uploadNextRow;
@property(nonatomic) int8_t uploadInProgress;
@property(nonatomic, assign) void *textureAsset;
@property(nonatomic) int32_t nativeFormat;
@property(nonatomic) int32_t nativeBlockWidth;
@property(nonatomic) int32_t nativeBlockHeight;
@property(nonatomic) int32_t nativeBlockBytes;
@property(nonatomic) int32_t nativeNextBlockRow;
@property(nonatomic) int64_t nativeNextMip;
@property(nonatomic) int64_t nativeMipStart;
@property(nonatomic) int64_t nativeMipCount;
@property(nonatomic) uint64_t lastUsedFrame;
@end

@implementation VGFXMetalTextureCacheEntry

@synthesize textureAsset = _textureAsset;

- (void)setTextureAsset:(void *)textureAsset {
    void *previous;
    if (_textureAsset == textureAsset)
        return;
    if (textureAsset)
        rt_obj_retain_maybe(textureAsset);
    previous = _textureAsset;
    _textureAsset = textureAsset;
    if (previous && rt_obj_release_check0(previous))
        rt_obj_free(previous);
}

- (void)dealloc {
    void *asset = _textureAsset;
    _textureAsset = NULL;
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
}

@end

@interface VGFXMetalCubemapCacheEntry : NSObject
@property(nonatomic, strong) id<MTLTexture> texture;
@property(nonatomic, strong) VGFXMetalCubemapCacheEntry *fallbackEntry;
@property(nonatomic) uint64_t generation;
@property(nonatomic) uint64_t pendingGeneration;
@property(nonatomic) uint64_t failedGeneration;
@property(nonatomic) int32_t faceSize;
@property(nonatomic) int32_t uploadFace;
@property(nonatomic) int32_t uploadNextRow;
@property(nonatomic) int8_t uploadInProgress;
@property(nonatomic) uint64_t lastUsedFrame;
@property(nonatomic) uint64_t appliedIblIdentity;
@property(nonatomic) uint64_t failedIblIdentity;
@property(nonatomic) uint64_t pendingIblIdentity;
@property(nonatomic) uint64_t pendingIblBytes;
@end

@implementation VGFXMetalCubemapCacheEntry
@end

@interface VGFXMetalGeometryCacheEntry : NSObject
@property(nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property(nonatomic, strong) id<MTLBuffer> indexBuffer;
@property(nonatomic) uint32_t revision;
@property(nonatomic) uint32_t vertexCount;
@property(nonatomic) uint32_t indexCount;
@property(nonatomic) uint64_t lastUsedFrame;
/* R20: vertex buffer holds the compact 48-byte encoding (vs the full 92-byte record). */
@property(nonatomic) BOOL compactStream;
@end

@implementation VGFXMetalGeometryCacheEntry
@end

@interface VGFXMetalMorphCacheEntry : NSObject
@property(nonatomic, strong) id<MTLBuffer> deltaBuffer;
@property(nonatomic, strong) id<MTLBuffer> normalBuffer;
@property(nonatomic, assign) const void *key;
@property(nonatomic) uint64_t revision;
@property(nonatomic) int32_t shapeCount;
@property(nonatomic) uint32_t vertexCount;
@property(nonatomic) int8_t hasNormalDeltas;
@property(nonatomic) uint64_t lastUsedFrame;
@end

@implementation VGFXMetalMorphCacheEntry
@end

/* Influences 5-8 side-stream buffers, keyed by the mesh geometry identity.
 * Import-time data: revalidated by geometry_revision, so re-uploads happen
 * only when the mesh's geometry actually changes. */
@interface VGFXMetalExtraInfluenceCacheEntry : NSObject
@property(nonatomic, strong) id<MTLBuffer> buffer;
@property(nonatomic, assign) const void *key;
@property(nonatomic) uint32_t revision;
@property(nonatomic) uint32_t vertexCount;
@property(nonatomic) uint64_t lastUsedFrame;
@end

@implementation VGFXMetalExtraInfluenceCacheEntry
@end

@interface VGFXMetalRenderTargetCacheEntry : NSObject
@property(nonatomic, strong) id<MTLTexture> colorTexture;
@property(nonatomic, strong) id<MTLTexture> motionTexture;
@property(nonatomic, strong) id<MTLTexture> depthTexture;
@property(nonatomic, strong) id<MTLCommandBuffer> pendingCommandBuffer;
@property(nonatomic, assign) vgfx3d_rendertarget_t *target;
@property(nonatomic) uint64_t cacheIdentity;
@property(nonatomic) int32_t width;
@property(nonatomic) int32_t height;
@property(nonatomic) MTLPixelFormat colorPixelFormat;
@property(nonatomic) uint64_t lastUsedFrame;
@end

@implementation VGFXMetalRenderTargetCacheEntry
@end

/* Target setup is included before context/pipeline selection, but both need
 * the same attachment-format predicate. Keep the declaration at the shared
 * translation-unit boundary so the textual include order remains explicit. */
static BOOL metal_target_uses_hdr_color(VGFXMetalContext *ctx);

//=============================================================================
// MSL Shader source (vertex + fragment in two halves for portable C string literal limits)
//=============================================================================

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"

/* The Metal backend is split into sequential textual chunks (one translation
 * unit): shader library, target/cache management, presentation, context
 * creation, texture uploads, and draw submission. Include order matters —
 * later chunks call earlier definitions directly. */
// clang-format off
#include "vgfx3d_backend_metal_shaders.inc"
#include "vgfx3d_backend_metal_targets.inc"
#include "vgfx3d_backend_metal_present.inc"
#include "vgfx3d_backend_metal_context.inc"
#include "vgfx3d_backend_metal_texture.inc"
#include "vgfx3d_backend_metal_draw.inc"
// clang-format on

/// @brief Copy the Metal context's telemetry snapshot (GAP-10, ADR 0233 era).
/// @details Metal always renders into its own drawable or offscreen target, so
///          the default framebuffer is reported writable unconditionally.
/// @param ctx_ptr Borrowed `VGFXMetalContext`, or NULL.
/// @param out_stats Caller-owned snapshot destination.
static void metal_get_backend_stats(void *ctx_ptr, vgfx3d_backend_stats_t *out_stats) {
    if (!out_stats)
        return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx_ptr)
        return;
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    *out_stats = ctx->_stats;
    out_stats->default_framebuffer_writable = 1;
}

const vgfx3d_backend_t vgfx3d_metal_backend = {
    .name = "metal",
    .gpu_skinning = 1,
    .gpu_skinning_extras = 1,
    .particle_instancing = 1,
    .clustered_lighting = 1,
    .shadow_csm = 1,
    /* Slots >= VGFX3D_CSM_SLOTS render into the internal 4x2 depth atlas
     * (texture index 17); the shader remaps their UVs by static tile rects. */
    .shadow_atlas_slots = 1,
    /* Scene passes render reversed-Z (Canvas3D negates the projection z row;
     * clears are 0 with Greater compares). Shadow maps stay standard. */
    .reversed_z = 1,
    .create_ctx = metal_create_ctx,
    .destroy_ctx = metal_destroy_ctx,
    .clear = metal_clear,
    .resize = metal_resize,
    .begin_frame = metal_begin_frame,
    .submit_draw = metal_submit_draw,
    .end_frame = metal_end_frame,
    .set_render_target = metal_set_render_target,
    .shadow_begin = metal_shadow_begin,
    .shadow_draw = metal_shadow_draw,
    .shadow_end = metal_shadow_end,
    .shadow_reuse = metal_shadow_reuse,
    .draw_skybox = metal_draw_skybox,
    .submit_draw_instanced = metal_submit_draw_instanced,
    .present = metal_present,
    .readback_rgba = metal_readback_rgba,
    .present_postfx = metal_present_postfx,
    .resolve_opaque_targets = metal_resolve_opaque_targets,
    .apply_postfx = metal_apply_postfx,
    .set_gpu_postfx_enabled = metal_set_gpu_postfx_enabled,
    .set_gpu_postfx_snapshot = metal_set_gpu_postfx_snapshot,
    .show_gpu_layer = metal_show_gpu_layer,
    .hide_gpu_layer = metal_hide_gpu_layer,
    .set_texture_upload_budget = metal_set_texture_upload_budget,
    .get_texture_upload_pending_bytes = metal_get_texture_upload_pending_bytes,
    .get_texture_upload_bytes = metal_get_texture_upload_bytes,
    .get_native_texture_caps = metal_get_native_texture_caps,
    .get_feature_caps = metal_get_feature_caps,
    .get_backend_stats = metal_get_backend_stats,
    .set_vsync = metal_set_vsync,
    .set_render_scale = metal_set_render_scale,
    .set_capture_after_present = metal_set_capture_after_present,
    .queue_depth_probe = metal_queue_depth_probe,
    .read_depth_probe = metal_read_depth_probe,
};

#endif /* __APPLE__ && ZANNA_ENABLE_GRAPHICS */
