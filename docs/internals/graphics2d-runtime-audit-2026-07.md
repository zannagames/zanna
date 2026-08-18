---
status: active
audience: contributors
last-verified: 2026-08-17
---

# Graphics 2D Runtime Correctness Audit (July 2026)

This audit covers the C runtime under `src/runtime/graphics/2d/`, including
input actions, cameras, pixels and image codecs, raster drawing, canvas
integration, sprites, tilemaps, scenes, Physics2D, and the consolidated
Graphics2D object families.

The review combined line-by-line arithmetic, allocation, object-layout,
ownership, serialization, and algorithmic-complexity analysis with focused
regression tests. The 215 findings below are fixed. The fixes preserve the
registered runtime C ABI and scripting surface, so no ADR was required.

Validation intentionally uses incremental target builds and the focused
binaries listed below. A full `ctest` run is excluded because other work may be
using the shared build tree concurrently.

## Regression suites

| Tag | Focused binaries |
|-----|------------------|
| `ACT` | `test_rt_action_mapping`, `test_rt_action_persist` |
| `CAM` | `test_rt_camera`, `test_rt_camera_enhance` |
| `PIX` | `test_rt_pixels` |
| `JPEG` | `test_jpeg_decode` |
| `DRAW` | `test_rt_pixels_draw` |
| `CAN` | `test_rt_canvas_contract`, `test_rt_canvas_state_contract`, `test_rt_color_utils` |
| `G2D` | `test_rt_graphics2d` |
| `SPR` | `test_rt_sprite_contract`, `test_rt_spritebatch_contract`, `test_rt_sprite_consolidated`, `test_rt_spriteanim` |
| `TILE` | `test_rt_tilemap_io`, `test_rt_tilemap_layers`, `test_rt_tilemap_anim`, `test_rt_tilemap_render_contract` |
| `SCN` | `test_rt_scene` |
| `PHY` | `test_rt_physics2d`, `test_rt_physics_joints`, `test_rt_runtime_additions` |

## Validation status

- An incremental build of all 26 focused targets completed without warnings.
- All 26 focused binaries passed; no full `ctest` invocation was made.
- Scoped `cppcheck` warning, performance, and portability analysis completed
  without diagnostics.
- `scripts/lint_platform_policy.sh` passed.
- Modified source headers, `clang-format`, and `git diff --check` are clean.
- The audit page passes the repository documentation link, frontmatter,
  filename, and code-fence rules.

## Action mapping and persistence

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-001 | Correctness | Empty action names created registry entries that could not be addressed consistently. Definition and loading now reject them. | `ACT` |
| G2D-002 | Bug | Embedded NUL bytes made distinct runtime action names alias through C-string storage. Names are now length-validated and NUL-free. | `ACT` |
| G2D-003 | Correctness | Malformed, overlong, surrogate, and otherwise non-scalar UTF-8 action names could be persisted as invalid JSON. Names now pass a strict UTF-8 decoder before admission. | `ACT` |
| G2D-004 | Bug | Action lookup recomputed `strlen` and compared it with an unchecked signed runtime length. Stored byte lengths now drive exact comparisons. | `ACT` |
| G2D-005 | Bug | Action removal had the same prefix-alias and signed-length problem as lookup. It now compares the complete stored/runtime byte spans. | `ACT` |
| G2D-006 | Resource | The global action list had no admission bound, allowing untrusted configuration to grow it without limit. Definitions now stop at `ACTION_MAX_ACTIONS`. | `ACT` |
| G2D-007 | Resource | Per-action binding lists were unbounded. Every public binder and loader now enforces `ACTION_MAX_BINDINGS_PER_ACTION`. | `ACT` |
| G2D-008 | Bug | Successful unbinds did not decrement the new binding count, eventually making reusable slots appear permanently full. All removal paths now maintain the count. | `ACT` |
| G2D-009 | Correctness | Keyboard bind and unbind APIs accepted negative and out-of-range key codes. Codes now pass one shared range validator. | `ACT` |
| G2D-010 | Correctness | Mouse button APIs accepted unsupported button numbers. Bind, unbind, and persistence paths now reject them. | `ACT` |
| G2D-011 | Correctness | Explicit controller bindings accepted nonexistent pad indices. Only supported indices or the documented any-pad sentinel are admitted. | `ACT` |
| G2D-012 | Correctness | Controller-button bindings accepted values outside the runtime button enum. They now use shared validation. | `ACT` |
| G2D-013 | Correctness | Controller-axis bindings accepted unsupported axis discriminators. They now reject them before allocation or load commit. | `ACT` |
| G2D-014 | Correctness | Chords validated only their length, so invalid key codes reached device queries. Every member is validated before a binding is allocated. | `ACT` |
| G2D-015 | Bug | A chord could contain the same key repeatedly, making its serialized and queried meaning ambiguous. Duplicate chord members are now rejected. | `ACT` |
| G2D-016 | Correctness | Chord unbind read and compared unvalidated sequence values while mutating the list. It now stages and validates the complete chord first. | `ACT` |
| G2D-017 | Correctness | Key-axis bindings accepted NaN and infinity, poisoning the cached axis value. Contributions must now be finite. | `ACT` |
| G2D-018 | Correctness | Mouse delta and wheel sensitivity accepted non-finite multipliers. All mouse-axis binders now require finite values. | `ACT` |
| G2D-019 | Correctness | Pad-axis and pad-button-axis scales accepted non-finite values. Public and JSON paths now share finite validation. | `ACT` |
| G2D-020 | Correctness | Non-finite values returned by a device backend propagated through action queries. Device samples are now neutralized before accumulation. | `ACT` |
| G2D-021 | Correctness | Summing many finite axis contributions could overflow to infinity. Raw accumulation now saturates at finite `DBL_MAX` magnitude. | `ACT` |
| G2D-022 | Correctness | Multiplying a finite device sample by a finite scale could overflow before the saturating add. Scaled contributions now detect product overflow first. | `ACT` |
| G2D-023 | Correctness | `Axis()` allowed NaN to pass through its comparisons, while `AxisRaw()` could expose infinity. Both APIs now preserve the finite-axis contract. | `ACT` |
| G2D-024 | Bug | Any-pad queries were hard-coded to pads 0 through 3 even when the platform exposed more. They now traverse `ZANNA_PAD_MAX`. | `ACT` |
| G2D-025 | Correctness | Any-pad axis lookup returned the first nonzero controller, making results depend on connection order. It now selects the strongest finite magnitude. | `ACT` |
| G2D-026 | Bug | Binding descriptions used fixed-size assembly and could truncate long valid names. The runtime string builder now receives checked, length-aware appends. | `ACT` |
| G2D-027 | Bug | `KeyBoundTo` and related name-returning queries treated stored names as unbounded C strings. They now use the exact stored name length. | `ACT` |
| G2D-028 | Bug | Persistence copied names through a legacy stack buffer, truncating otherwise valid long names. JSON save/load now operates on explicit spans. | `ACT` |
| G2D-029 | Correctness | JSON numbers destined for integer fields accepted fractional or out-of-range doubles. Conversion now requires an exact representable integer. | `ACT` |
| G2D-030 | Correctness | Duplicate schema fields could silently overwrite earlier values. Object parsers now require unique, schema-complete fields. | `ACT` |
| G2D-031 | Correctness | A button action could load axis-only bindings, and an axis action could load button-only bindings. Binding kind is now checked against action kind. | `ACT` |
| G2D-032 | Resource | Pending JSON arrays and temporary binding chains could grow beyond runtime limits before rejection. Parsing now applies action, binding, chord, and allocation bounds while staging. | `ACT` |
| G2D-033 | Bug | Save trusted corrupt private nodes and could emit invalid or partial configuration. It now validates names, kinds, counts, and binding payloads before publishing a string. | `ACT` |
| G2D-034 | Bug | Installing a preset repeatedly appended duplicate actions and bindings. Preset construction now replaces nothing unnecessarily and inserts only unique bindings. | `ACT` |
| G2D-035 | Bug | Allocation failure during preset installation left a partially modified registry. Presets now clone, build, and commit transactionally. | `ACT` |

## Camera and parallax

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-036 | Bug | Camera APIs accepted a same-class object whose allocation was smaller than `rt_camera_impl`. Checked casts now enforce the minimum payload size. | `CAM` |
| G2D-037 | Portability | Camera multiply/divide fell back to `long double`, which has only binary64 precision on Windows. Exact unsigned quotient/remainder arithmetic now produces identical saturation and rounding on all targets. | `CAM` |
| G2D-038 | Correctness | Taking the magnitude of `INT64_MIN` overflowed in zoom and parallax arithmetic. Unsigned-magnitude helpers now cover the complete signed range. | `CAM` |
| G2D-039 | Correctness | Position, center, and offset additions could wrap at signed limits. Camera coordinate arithmetic now saturates explicitly. | `CAM` |
| G2D-040 | Bug | Empty or overflowed visibility rectangles could be treated as visible. Visibility rejects nonpositive extents and computes exclusive edges with saturation. | `CAM` |
| G2D-041 | Correctness | World-bound clamping added viewport extents with unchecked overflow. Bounds now compare and clamp using safe differences and saturated centers. | `CAM` |
| G2D-042 | Correctness | Center accessors and `SetCenter` could overflow while adding or subtracting half-extents. They now use saturated integer operations. | `CAM` |
| G2D-043 | Correctness | `Move`, `Follow`, and dead-zone corrections could wrap large coordinates. Every mutation now uses the shared saturating path. | `CAM` |
| G2D-044 | Bug | Smooth follow rounded a nonzero subpixel correction to zero forever. It now makes one-pixel progress when a finite nonzero correction remains. | `CAM` |
| G2D-045 | Bug | A one-pixel dead zone admitted an extra coordinate because inclusive edges were built from the full size. Dead zones now have exact pixel coverage. | `CAM` |
| G2D-046 | Performance | Idempotent setters and clamped no-op movement marked the camera dirty, forcing needless downstream recomputation. Dirty state now changes only when observable state changes. | `CAM` |
| G2D-047 | Correctness | `SetBounds` failed to mark dirty when applying new bounds immediately moved the camera. It now accounts for both metadata and clamped-position changes. | `CAM` |
| G2D-048 | Correctness | Huge degree values were converted directly to radians, losing periodicity in libm argument reduction. Camera rotations are reduced modulo 360 before trigonometry. | `CAM` |
| G2D-049 | Bug | Forward and inverse camera transforms used separately rounded angles and center arithmetic, breaking rotated round trips. They now share one cached transform definition. | `CAM` |
| G2D-050 | Performance | Parallax transformed every tile while recomputing sine, cosine, centers, and inverse scale. One transform is now precomputed per layer draw. | `CAM` |
| G2D-051 | Bug | Direct tiled parallax used a quotient-plus-two estimate that could both overdraw and miss exact edge cases. Coverage now uses an exact ceiling span from the first tile origin. | `CAM` |
| G2D-052 | Correctness | Tile-count multiplication could overflow before comparison with the work budget. Spans are now checked by division before multiplication. | `CAM` |
| G2D-053 | Bug | Coordinate-based `<=` tile loops could overflow their induction variable and hang. Draw loops now iterate checked counts from zero. | `CAM` |
| G2D-054 | Performance | Zoomed/rotated parallax images were reallocated and transformed every frame. Each layer now caches its prepared tile. | `CAM` |
| G2D-055 | Correctness | The prepared parallax cache ignored source Pixels mutations. The source generation is now part of the cache key. | `CAM` |
| G2D-056 | Correctness | The prepared cache could also be reused after zoom or rotation changed. Reduced rotation and zoom are both key fields. | `CAM` |
| G2D-057 | Bug | Transformed parallax covered camera dimensions instead of the actual destination canvas, clipping or overworking mismatched targets. Coverage now uses validated canvas extents. | `CAM` |
| G2D-058 | Correctness | Inverse-transformed floating bounds could be cast outside `int64_t`, invoking undefined behavior. Floor/ceil conversions are now range checked. | `CAM` |
| G2D-059 | Resource | Removing layers or destroying a camera omitted prepared-cache ownership in some paths. Cache references are now released with the layer. | `CAM` |
| G2D-060 | Performance | Pathological zoom, rotation, or tiny tiles could request billions of parallax iterations. Exact span and product admission now rejects work beyond the fixed draw budget. | `CAM` |

## Pixels, transforms, and image codecs

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-061 | Bug | Pixels APIs trusted class ID alone and could dereference undersized same-class payloads. Central validation now enforces `sizeof(rt_pixels_impl)` and layout invariants. | `PIX`, `G2D` |
| G2D-062 | Bug | Width-times-height and row-byte products mixed signed arithmetic, permitting overflow before allocation checks. Products now use checked unsigned/`size_t` arithmetic. | `PIX` |
| G2D-063 | Portability | Counts that fit `int64_t` could still exceed a 32-bit allocator's `SIZE_MAX`. All allocation paths now enforce both limits. | `PIX`, `G2D` |
| G2D-064 | Performance | Setting a pixel to its existing value advanced the generation. Idempotent sets now leave generation and caches unchanged. | `PIX` |
| G2D-065 | Performance | Filling with the current color invalidated every consumer despite no content change. Fill now detects equality before publishing one generation change. | `PIX` |
| G2D-066 | Performance | Clearing an already transparent image had the same unnecessary invalidation. Clear now shares exact change detection. | `PIX` |
| G2D-067 | Correctness | Copy operations advanced generation even when clipping removed all work or all copied values matched. They now touch once only after an actual write. | `PIX` |
| G2D-068 | Correctness | Generation increment could wrap to zero, colliding with cache sentinels. Touch normalizes wrapped zero to one. | `PIX` |
| G2D-069 | Bug | Alpha classification could remain cached after a raw-changing operation. Every mutation now invalidates or updates the classification exactly once. | `PIX` |
| G2D-070 | Performance | Exact clones discarded a known alpha classification and forced a full rescan. Clone and identity transforms now copy the valid classification cache. | `PIX` |
| G2D-071 | Portability | Byte conversion depended on host integer byte order. RGBA bytes are now packed and unpacked by explicit shifts. | `PIX` |
| G2D-072 | Bug | `FromBytes` trusted a foreign object, signed length, and unchecked expected-byte product. It now validates object type and exact bounded payload length. | `PIX` |
| G2D-073 | Bug | Self-copy used forward row order even when source and destination overlapped. It now chooses a safe direction and uses overlap-safe row copies. | `PIX` |
| G2D-074 | Correctness | Clipping with extreme negative destinations negated or subtracted `INT64_MIN`. Copy clipping now uses unsigned magnitudes and saturated differences. | `PIX` |
| G2D-075 | Bug | Image paths with embedded NUL bytes could address a different file than the runtime string represented. Load and save reject non-lossless paths. | `PIX`, `JPEG` |
| G2D-076 | Bug | BMP header extents and pixel offsets were trusted before signed/unsigned conversion. The loader now validates dimensions, offsets, and total file coverage. | `PIX` |
| G2D-077 | Correctness | BMP padded row-stride arithmetic could overflow or accept short rows. Row and image byte sizes are now checked exactly. | `PIX` |
| G2D-078 | Bug | Truncated BMP reads and invalid seek ranges could leave partially decoded output. Decode now stages and publishes only after complete coverage. | `PIX` |
| G2D-079 | Correctness | PNG accepted invalid chunk-type bytes and a lowercase reserved bit. Chunk names now satisfy the PNG alphabetic and reserved-bit rules. | `PIX` |
| G2D-080 | Bug | PNG chunk CRCs were not enforced consistently. Every consumed chunk now verifies its CRC before use. | `PIX` |
| G2D-081 | Bug | The zlib Adler-32 trailer was ignored, allowing corrupted scanline data. The exact decompressed payload now has its checksum verified. | `PIX` |
| G2D-082 | Bug | Missing/duplicate `IEND`, data after `IEND`, and malformed critical-chunk order could be accepted. The decoder now runs an explicit chunk-state machine. | `PIX` |
| G2D-083 | Correctness | Unsupported IHDR compression, filter, and interlace values reached decode paths. Header methods are now validated before allocation. | `PIX` |
| G2D-084 | Correctness | `tRNS` was accepted for alpha-bearing types and with invalid lengths/ranges. Transparency metadata now follows each color-type contract exactly. | `PIX` |
| G2D-085 | Bug | Indexed PNG decode could proceed without a palette. `PLTE` is now required before indexed image data. | `PIX` |
| G2D-086 | Bug | Palette sample indices were used without proving they were within `PLTE`. Every decoded index is range checked. | `PIX` |
| G2D-087 | Correctness | Sub-byte grayscale transparency compared an expanded 8-bit value with the raw `tRNS` sample. It now compares in the encoded sample domain. | `PIX` |
| G2D-088 | Bug | Adam7 passes did not reject an invalid filter byte uniformly. Every nonempty pass now applies the same filter-range validation. | `PIX` |
| G2D-089 | Bug | Deflate accepted incomplete or trailing encoded data after producing the expected scanline count. The inflater now requires exact block, bitstream, and checksum consumption. | `PIX` |
| G2D-090 | Bug | JPEG table lookup assumed only low-numbered quantization and Huffman IDs. All four legal table IDs are now parsed and selected correctly. | `JPEG` |
| G2D-091 | Correctness | JPEG component roles were inferred from component order, swapping channels in reordered YCbCr streams. Roles are now mapped by component ID. | `JPEG` |
| G2D-092 | Correctness | Unsupported RGB and Adobe transform variants were decoded as YCbCr. The decoder now accepts only the color model it implements. | `JPEG` |
| G2D-093 | Bug | Invalid sampling factors and inconsistent MCU geometry could drive out-of-bounds coefficient access. Frame and scan sampling are now cross-validated. | `JPEG` |
| G2D-094 | Bug | Oversubscribed/incomplete Huffman tables, malformed entropy markers, and truncated scans were insufficiently rejected. Table construction and bit consumption are now exact. | `JPEG` |
| G2D-095 | Bug | A failed direct JPEG decode could leave stale width, height, or pixel outputs. Outputs are cleared first and published transactionally. | `JPEG` |
| G2D-096 | Correctness | Direct decode could succeed before trailing EXIF orientation metadata was validated. The complete marker stream is now consumed before publication. | `JPEG` |
| G2D-097 | Bug | EXIF IFD entry-count arithmetic could overflow and read beyond APP1. Counts, offsets, widths, and orientation values are now bounds checked. | `JPEG` |
| G2D-098 | Correctness | Rotating an empty image skipped angle validation and returned an inconsistent shape. Angle validation now precedes the exact empty-image result. | `PIX` |
| G2D-099 | Bug | Angles merely close to a cardinal rotation took the lossless cardinal shortcut. Only exact reduced cardinal values now use it. | `PIX` |
| G2D-100 | Correctness | Scale and resize sample mapping failed to preserve both source endpoints. Shared exact rational mapping now covers the complete destination interval. | `PIX` |
| G2D-101 | Correctness | Scale/resize accepted nonpositive target dimensions in some transform branches. All entry paths now reject them consistently. | `PIX` |
| G2D-102 | Correctness | Tint multiplication truncated channel products, systematically darkening results. It now rounds the normalized product. | `PIX` |
| G2D-103 | Correctness | Blur averaged straight RGB and alpha independently, creating colored halos around transparency. It now accumulates premultiplied RGB and unpremultiplies safely. | `PIX` |
| G2D-104 | Performance | Blur recomputed every radius-wide neighborhood per output pixel. Horizontal and vertical sliding windows reduce work to linear time. | `PIX` |
| G2D-105 | Performance | Radius-zero blur rebuilt an identical image but discarded exact metadata. It now returns an exact clone with reusable classification. | `PIX` |
| G2D-106 | Correctness | Bilinear resize mixed the runtime's byte order and straight-alpha channels. It now samples canonical RGBA in premultiplied form. | `PIX` |
| G2D-107 | Correctness | Area resize used truncated source boundaries for odd ratios, biasing coverage. Boundary mapping now uses exact rounded rational intervals. | `PIX` |
| G2D-108 | Correctness | A one-axis downscale forced area sampling on the other upscaled axis, losing interpolation. Hybrid resize selects the appropriate filter per axis. | `PIX` |
| G2D-109 | Performance | A chain of alpha-preserving transforms rescanned every output image. Valid opaque/translucent classification now propagates through preserving operations. | `PIX` |
| G2D-110 | Resource | Temporary transform results had scattered failure cleanup and could leak after a second-stage allocation failed. One release helper now owns all staged cleanup. | `PIX` |

## Raster drawing, canvas integration, and shared rendering

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-111 | Correctness | Shared raster add/subtract/interpolate helpers overflowed on `INT64_MIN` and full-range endpoint differences. They now use unsigned magnitudes and exact ratio scaling. | `DRAW`, `CAN`, `G2D` |
| G2D-112 | Bug | Line clipping formed overflowing differences and produced incorrect intersections at signed extremes. Clipping now uses exact wide unsigned arithmetic. | `DRAW`, `CAN` |
| G2D-113 | Correctness | Box/frame endpoints used unchecked `x + width` and `y + height`. Shapes now clip via safe extents before iteration. | `DRAW` |
| G2D-114 | Performance | A huge disc radius could iterate billions of invisible scanlines. Disc rasterization is now bounded to the validated destination clip. | `DRAW`, `CAN` |
| G2D-115 | Correctness | Filled circle and ellipse edges rounded coordinate extents rather than testing pixel centers. Coverage now follows consistent center-sampling semantics. | `DRAW` |
| G2D-116 | Performance | Primitive helpers touched Pixels once per plotted pixel and also touched idempotent redraws. Raw raster paths now publish one generation only after a change. | `DRAW` |
| G2D-117 | Bug | An asymmetric zero-size or corrupt framebuffer could still reach row arithmetic. Direct drawing now rejects every invalid layout before work. | `DRAW`, `CAN` |
| G2D-118 | Correctness | Ring and ellipse paths accepted invalid radii or overflowed doubled-radius terms. They now validate dimensions and use bounded geometric tests. | `DRAW` |
| G2D-119 | Resource | Flood fill pushed individual pixels into a potentially image-sized stack. A dynamically checked scanline-segment stack bounds allocations and reduces memory. | `DRAW`, `CAN` |
| G2D-120 | Bug | Flood fill compared only RGB in some paths and could cross alpha-only boundaries. Matching and writes now use complete RGBA values. | `DRAW` |
| G2D-121 | Correctness | Triangle collinearity used overflow-prone cross products. A portable wide-magnitude representation now determines degeneracy exactly. | `DRAW`, `CAN` |
| G2D-122 | Bug | Degenerate triangles selected an edge using overflowed squared distances. Exact wide comparisons now draw the true longest edge. | `DRAW`, `CAN` |
| G2D-123 | Bug | Bézier drawing plotted sparse samples without connecting them, leaving visible holes. Consecutive samples are now joined by clipped line segments. | `DRAW` |
| G2D-124 | Performance | Extreme Bézier and thick-line inputs could choose work proportional to offscreen coordinate magnitude. Sampling and raster work are now capped to useful visible precision. | `CAN` |
| G2D-125 | Bug | Text drawing and measurement trusted a negative runtime-string length. String extraction now validates handle, signed length, and `SIZE_MAX`. | `DRAW`, `CAN` |
| G2D-126 | Bug | Direct canvas framebuffer operations trusted width, height, stride, and data-pointer combinations. One layout validator now guards every raw path. | `CAN` |
| G2D-127 | Correctness | Canvas gradient and line interpolation multiplied full-range signed differences. Unsigned rational interpolation now avoids overflow and preserves endpoints. | `CAN` |
| G2D-128 | Correctness | Advanced RGBA interpolation truncated or overflowed channel-weight products. It now uses explicit wide multiplication and rounded division. | `CAN` |
| G2D-129 | Correctness | Polygon scanline sorting compared coordinates by subtraction, which could overflow and violate `qsort` ordering. The comparator now uses relational comparisons. | `CAN` |
| G2D-130 | Correctness | A clipped gradient with an extreme negative origin overflowed its relative-position calculation. Saturated span arithmetic now produces stable colors. | `CAN` |
| G2D-131 | Performance | Opaque alpha discs missed the clip-bounded raw fast path and fell into expensive blend work. They now reuse bounded disc rasterization. | `CAN` |
| G2D-132 | Performance | Canvas state resynchronization resent unchanged backend state. A cached snapshot now skips redundant state calls. | `CAN` |
| G2D-133 | Correctness | Frame timing subtraction could overflow for clocks straddling signed limits. Elapsed-time computation now saturates. | `CAN` |
| G2D-134 | Bug | A backwards/reset platform clock could leave frame pacing permanently stalled. Timing state now recognizes and recovers from clock reset. | `CAN` |
| G2D-135 | Correctness | HSL conversion rounded intermediate channels too early, losing fractional precision. It now delays rounding until final byte conversion. | `CAN` |
| G2D-136 | Performance | Transparent/no-op render blits advanced destination generation. The blitter now records actual changed pixels before touching. | `G2D` |
| G2D-137 | Performance | Scaled and rotated sampling revalidated the source object and bounds for every tap. Validated raw pixel implementations are now passed into inner samplers. | `G2D` |
| G2D-138 | Bug | Scaling a Pixels object into itself read already-overwritten source samples, making output scan-order dependent. Self-scaled blits now snapshot the source. | `G2D` |
| G2D-139 | Bug | Rotating a Pixels object into itself had the same alias corruption. Self-rotated blits also use a source snapshot. | `G2D` |
| G2D-140 | Correctness | Bilinear premultiplied-alpha sampling unpremultiplied with rounded output alpha, distorting low-alpha colors. It now divides by the exact interpolated alpha. | `G2D` |
| G2D-141 | Correctness | Huge scaled sample coordinates were cast to `int64_t` before clamping. Coordinate ranges are now clamped before floor conversion. | `G2D` |
| G2D-142 | Correctness | Repeat-mode rotated coordinates could exceed `int64_t` before modulo. Floating coordinates are now normalized with `fmod` first. | `G2D` |
| G2D-143 | Performance | Tint/alpha material application invoked validated setters per pixel and advanced generation repeatedly. It now performs a raw bulk pass and one conditional touch. | `G2D` |
| G2D-144 | Bug | A failed canvas post-processing clone was indistinguishable from “no processing needed,” so an unprocessed command could be drawn. Required processing failure now skips that command. | `G2D` |
| G2D-145 | Correctness | Rotated temporary-buffer dimensions and origins could subtract or negate `INT64_MIN`. All temporary extent/origin calculations now saturate. | `G2D` |

## Sprites, tilemaps, and scenes

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-146 | Bug | Sprite-family checked casts accepted undersized same-class allocations. Sprite, batch, sheet, atlas, and related accessors now enforce concrete payload sizes. | `SPR` |
| G2D-147 | Correctness | SpriteBatch queued source rectangles outside the source Pixels object. Regions are now clipped before queue admission. | `SPR` |
| G2D-148 | Portability | Full-range sprite scaling used floating fallback and differed on Windows. Exact divide-before-saturate integer arithmetic now handles all signed values. | `SPR` |
| G2D-149 | Correctness | Huge sprite rotations lost periodicity after conversion to double. Rotation is canonicalized before transform math. | `SPR` |
| G2D-150 | Correctness | Rotated destination recentering overflowed at extreme coordinates. Center offsets now use saturating add/subtract. | `SPR` |
| G2D-151 | Bug | Sprite transform caches ignored mutations to the current source frame. The Pixels generation is now part of cache validity. | `SPR` |
| G2D-152 | Performance | Nonzero multiples of 360 took the transformed slow path. Canonical identity rotations now use the untransformed path. | `SPR` |
| G2D-153 | Bug | Timestamp zero was treated as “not initialized,” so animation starting at time zero repeatedly reset. Sprite animation now tracks initialization separately. | `SPR` |
| G2D-154 | Bug | Animator clips had the same zero-timestamp sentinel collision. They now maintain an explicit timing baseline. | `SPR` |
| G2D-155 | Bug | Runtime clip names with embedded NUL bytes aliased prefixes. Names are now validated as complete byte spans. | `SPR` |
| G2D-156 | Bug | Sprite file paths accepted embedded NUL bytes. Runtime paths must now be exactly representable to the file adapter. | `SPR` |
| G2D-157 | Bug | A partial GIF magic prefix was treated as a supported GIF. Format detection now requires the complete signature. | `SPR` |
| G2D-158 | Resource | Sprite frame storage stopped at a legacy fixed capacity. Checked geometric growth now supports the runtime limit without overflow. | `SPR` |
| G2D-159 | Correctness | Per-frame delays were not kept aligned through growth and replacement. Frame IDs and delay arrays now resize and update transactionally. | `SPR` |
| G2D-160 | Bug | `Contains` ignored scaled origin when building sprite bounds. Hit testing now uses the same transformed origin as drawing. | `SPR` |
| G2D-161 | Bug | `Overlaps` used the same incorrect unscaled-origin bounds. Both sprites now contribute their scaled origins. | `SPR` |
| G2D-162 | Resource | SpriteSheet construction and replacement could retain an invalid Pixels handle or release the old object before the new ownership succeeded. Validation and retain-before-release ordering now preserve state. | `SPR` |
| G2D-163 | Bug | Empty and embedded-NUL SpriteSheet region names could alias hash entries. Exact nonempty runtime names are now required. | `SPR` |
| G2D-164 | Resource | SpriteSheet capacity growth could overflow count or bytes. Reserve now checks `INT64_MAX`, `SIZE_MAX`, and pointer/count invariants. | `SPR` |
| G2D-165 | Bug | TextureAtlas lookup hashed C-string prefixes, so embedded-NUL names could retrieve another region. Hashing and equality now include exact length. | `SPR` |
| G2D-166 | Correctness | The exact documented maximum atlas name length was rejected by an off-by-one terminator check. It is now accepted while longer names fail. | `SPR` |
| G2D-167 | Bug | Atlas slot insertion could fail after a region record was written, leaving a published region with no lookup entry. Slot binding now reports failure and count publication occurs only after it succeeds. | `SPR` |
| G2D-168 | Correctness | Grid atlas/sheet creation multiplied columns, rows, and cell dimensions without overflow checks. Geometry is now validated before any binding changes. | `SPR`, `TILE` |
| G2D-169 | Resource | SpriteBatch command growth trusted count/capacity and byte multiplication. Queue reserve now validates invariants and both integer size limits. | `SPR` |
| G2D-170 | Resource | Tilemap clone/finalize paths held inconsistent tileset reference counts, risking leaks or premature frees. Map- and layer-owned references now have one clear retain/release contract. | `TILE` |
| G2D-171 | Correctness | Tilemap full-range scaling multiplied before division and saturated too early. It now divides exactly before deciding saturation. | `TILE` |
| G2D-172 | Bug | An overflowing tileset grid partially replaced the existing binding. Grid geometry is now staged and the previous tileset remains intact on failure. | `TILE` |
| G2D-173 | Bug | An overlong layer name consumed a layer slot even though the stored name was truncated. Admission now validates the name before growing the layer list. | `TILE` |
| G2D-174 | Bug | Embedded-NUL layer names and property keys aliased stored prefixes. Tilemap string APIs now require exact bounded spans. | `TILE` |
| G2D-175 | Correctness | Negative pixel-to-tile conversion truncated toward zero, falsely mapping positions left/up of the map into tile zero. Conversion now performs mathematical floor division. | `TILE` |
| G2D-176 | Bug | A fast downward body could cross a one-way platform between samples. Collision now tests the swept previous-to-current bottom edge. | `TILE` |
| G2D-177 | Correctness | A body teleported into a one-way tile could be incorrectly ejected. The crossing test now rejects bodies that began inside/below the surface tolerance. | `TILE` |
| G2D-178 | Correctness | Negative animation delta moved state backward, while huge delta iterated frame by frame. Updates now ignore negative time and advance by bounded modular arithmetic. | `TILE` |
| G2D-179 | Correctness | Replacement animation frames could be zero or negative. Definitions and JSON normalization now preserve positive tile IDs. | `TILE` |
| G2D-180 | Bug | Collision queried a currently animated replacement tile instead of the authored base tile. Physics metadata now remains attached to the base tile. | `TILE` |

## Physics2D and consolidated object families

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-181 | Correctness | Tile collision-type setters accepted values outside the enum. Invalid values now leave state unchanged. | `TILE` |
| G2D-182 | Correctness | Tile `FillRect` and pixel conversions overflowed extreme coordinates. Clipping and conversion now use saturated bounds. | `TILE` |
| G2D-183 | Resource | Clearing an autotile rule did not reclaim its fixed-capacity slot. Rule removal now compacts/reuses capacity. | `TILE` |
| G2D-184 | Bug | CSV accepted overflowing integers, silently truncated long lines, and treated embedded NUL as end-of-input. The importer now validates exact lines and numbers. | `TILE` |
| G2D-185 | Correctness | CSV accepted invalid tile dimensions and file APIs accepted embedded-NUL paths. Both are now rejected before construction or I/O. | `TILE` |
| G2D-186 | Correctness | JSON tilemaps ignored the format version. Loading now requires the supported version explicitly. | `TILE` |
| G2D-187 | Resource | JSON accepted excess layers, mismatched tile counts, and truncated tileset pixel payloads. Exact bounded structural coverage is now required before commit. | `TILE` |
| G2D-188 | Bug | Fractional AABBs tested too few overlapped tiles due to truncated endpoints. Collision now checks every tile touched by the continuous bounds. | `TILE` |
| G2D-189 | Correctness | Non-finite body geometry reached tile collision arithmetic. Hit/collision APIs now reject it before coordinate conversion. | `TILE` |
| G2D-190 | Bug | Hit testing reported an empty success when result-object allocation failed. Allocation failure now propagates distinctly. | `TILE` |
| G2D-191 | Portability | Scene scale composition used `long double` and lost full-range exactness on Windows. Exact percentage multiplication now rounds and saturates identically everywhere. | `SCN` |
| G2D-192 | Correctness | Scene rotations accumulated arbitrary full turns until addition or trigonometry lost precision. Local/world rotations are now reduced modulo 360. | `SCN` |
| G2D-193 | Bug | Scene lookup compared NUL-terminated prefixes instead of complete runtime names. It now validates and compares exact string spans. | `SCN` |
| G2D-194 | Bug | Reparenting detached from the old parent before proving insertion into the new parent, losing hierarchy on allocation failure. It now acquires new ownership first. | `SCN` |
| G2D-195 | Bug | Re-adding an existing direct child duplicated it in the sequence. The operation is now idempotent. | `SCN` |
| G2D-196 | Correctness | Parent-chain validation and transform traversal used inconsistent off-by-one depth limits. Both now enforce the same bound, with deep hierarchy coverage exercising the iterative walks. | `SCN` |
| G2D-197 | Resource | Scene draw-entry growth could overflow capacity doubling or `SIZE_MAX`. Growth now validates count, bytes, and required progress. | `SCN` |
| G2D-198 | Bug | Physics world, body, joint, and projectile APIs trusted class ID without minimum object size. Every checked cast now enforces its concrete payload. | `PHY` |
| G2D-199 | Bug | A body could be added to two worlds, causing double integration and conflicting ownership. Cross-world add now traps and preserves both worlds. | `PHY` |
| G2D-200 | Correctness | Body sanitization repopulated circle width/height and left box radius stale. Shape-specific dimensions now remain canonical after every step. | `PHY` |
| G2D-201 | Correctness | Subnormal dynamic masses produced infinite inverse mass or behaved inconsistently as static. Positive subnormals normalize to `DBL_MIN`. | `PHY` |
| G2D-202 | Correctness | Force, impulse, acceleration, velocity, and position arithmetic could become NaN/infinite. Shared finite saturating operations now bound every integration stage. | `PHY` |
| G2D-203 | Resource | Pair/contact append incremented unchecked counts and could reserve after invalid contact data. Finite validation and checked count/capacity admission now precede mutation. | `PHY` |
| G2D-204 | Correctness | Circle distances and normals used overflow-prone squared sums and fixed tiny thresholds. `hypot` and scale-aware tolerances preserve tiny and huge geometry. | `PHY` |
| G2D-205 | Bug | Swept collision discarded valid sub-picometer motion and used unstable quadratic roots. Scale-normalized, cancellation-resistant roots now retain real impacts. | `PHY` |
| G2D-206 | Correctness | Collision response formed a scalar impulse that overflowed for huge masses. Stable inverse-mass shares apply representable relative-velocity deltas directly. | `PHY` |
| G2D-207 | Correctness | Friction, penetration correction, and joint corrections could overflow even when the final weighted change was representable. All use normalized shares and saturating products. | `PHY` |
| G2D-208 | Correctness | Distance/spring/pin/rope/motor joints used unstable distances, invalid `dt`, and overflow-prone force math. They now use `hypot`, finite time checks, stable shares, and bounded deltas. | `PHY` |
| G2D-209 | Correctness | Projectile drag formulas suffered cancellation for small `drag*t` and overflow for huge inputs. `expm1`/series forms and saturated closed-form terms now remain stable. | `PHY` |
| G2D-210 | Bug | Projectile ground time missed asymptotic, turning-point, and cancellation-sensitive roots. Stable quadratic/logarithmic analysis plus bracketed bisection now finds the earliest valid root. | `PHY` |
| G2D-211 | Bug | `Advance` only examined the sampled endpoint, so a projectile could cross ground and return above it within one step. It now latches the first analytic crossing. | `PHY` |
| G2D-212 | Bug | Path, renderer, render-target, texture, material, and 20 extended object families accepted undersized same-class handles. Each family now validates its concrete payload and backing Pixels layout. | `G2D` |
| G2D-213 | Resource | Path, renderer, and render-graph reserve logic trusted corrupt count/capacity/pointer invariants and unchecked byte growth. All reserve paths now validate invariants, `INT64_MAX`, and `SIZE_MAX`. | `G2D` |
| G2D-214 | Correctness | Transform2D converted huge rotations directly and lost integer identity transforms near signed limits. Rotations are reduced first, and identity-scale/full-turn transforms use exact saturated integer math. | `G2D` |
| G2D-215 | Performance | Collision-mask construction, gradient fill/sample, and palette application performed repeated object checks or sparse searches per pixel. Validated bulk loops, one conditional generation touch, exact empty-palette clones, and compact color candidates remove that overhead. | `G2D` |

## Public-contract corrections

The implementation review also exposed stale documentation. The public input,
Physics2D, Game2D, and Rendering2D references now describe finite saturation,
transactional persistence, controller wildcard behavior, circle dimensions,
projectile landing, self-blit snapshot semantics, transform periodicity,
gradient generation, palette cloning, and the live runtime-class inventory.
These are documentation corrections only; they do not add or change runtime
ABI symbols.
