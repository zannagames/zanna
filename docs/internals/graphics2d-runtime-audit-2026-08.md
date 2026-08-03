---
status: active
audience: contributors
last-verified: 2026-08-03
---

# Graphics 2D Runtime Follow-up Audit (August 2026)

This follow-up reviews the C runtime under `src/runtime/graphics/2d/` after the
July 2026 correctness audit. It focuses on gaps that remained after
`G2D-001` through `G2D-215`: complete private-object invariants, inline payload
coverage, full-domain integer arithmetic, alpha-preserving composition, and
avoidable work in frequently called rendering paths.

The 104 findings below are fixed. No public runtime function, registered class,
IL rule, or C ABI entry point changed; the new cookies and invariant fields are
private implementation state. Consequently this follow-up does not require an
ADR.

## Regression suites

| Tag | Focused binaries |
|-----|------------------|
| `PIX` | `test_rt_pixels` |
| `DRAW` | `test_rt_pixels_draw` |
| `SPR` | `test_rt_sprite_contract` |
| `BAT` | `test_rt_spritebatch_contract` |
| `TILE` | `test_rt_tilemap_render_contract` |
| `G2D` | `test_rt_graphics2d` |
| `PHY` | `test_rt_physics2d`, `test_rt_physics_joints`, `test_rt_runtime_additions` |

## Pixels and raster arithmetic

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-216 | Bug | A same-class Pixels object could advertise dimensions whose trailing pixel payload was not present. Central validation now proves the complete managed allocation covers every pixel. | `PIX` |
| G2D-217 | Bug | A Pixels payload could redirect `data` to unrelated memory. Validation now requires the exact canonical pointer immediately after the implementation header. | `PIX` |
| G2D-218 | Correctness | Zero-area Pixels payloads could retain a non-null data pointer and violate the allocator contract. The canonical empty layout now requires `data == NULL`. | `PIX` |
| G2D-219 | Correctness | A zero cache identity collided with the reserved invalid/sentinel identity. Initialized Pixels handles now require a nonzero identity. | `PIX` |
| G2D-220 | Correctness | Alpha-cache flags and classifications could contain noncanonical values. The validator now admits only Boolean validity and the defined classification range. | `PIX` |
| G2D-221 | Bug | A cache marked valid could describe an older Pixels generation. Valid cache state now requires an exact generation match. | `PIX` |
| G2D-222 | Bug | Strict and soft Pixels casts enforced different layout contracts, so callers behaved differently for the same corrupt handle. Both paths now share the complete invariant set. | `PIX`, `G2D` |
| G2D-223 | Bug | `Sprite.New` accepted a class-correct Pixels header without proving its backing payload. Construction now uses the complete soft Pixels validator. | `SPR` |
| G2D-224 | Bug | `Sprite.AddFrame` had the same shallow Pixels admission and could retain malformed frame storage. It now validates the complete layout before retaining. | `SPR` |
| G2D-225 | Bug | Tilemap base and per-layer tilesets were checked only superficially. Runtime Tilemap validation now proves every retained tileset is a complete Pixels object. | `TILE` |
| G2D-226 | Bug | Tilemap save/load entry points could traverse malformed retained tilesets. The I/O validator now applies the same complete Pixels contract before serialization. | `TILE` |
| G2D-227 | Performance | Nearest-neighbor `Scale` discarded a known alpha classification even though it cannot introduce a new alpha value. The result now inherits a valid source classification. | `PIX` |
| G2D-228 | Correctness | Separable blur quantized each horizontal window to straight-alpha RGBA before the vertical pass. Column state now carries full premultiplied sums and sample counts until the final two-dimensional result is produced. | `PIX` |
| G2D-229 | Bug | The intermediate blur quantization double-rounded low alpha and could turn an intended alpha of one into two while losing color precision. Only the final two-dimensional footprint is now quantized. | `PIX` |
| G2D-230 | Performance | Blurring a known opaque image forced a later full alpha scan even though averaging opaque samples remains opaque. Blur now publishes that classification directly. | `PIX` |
| G2D-231 | Correctness | Triangle filling saturated a full-range vertical edge length to `INT64_MAX`, changing scanline intercepts. Edge heights now use exact unsigned differences through `UINT64_MAX`. | `DRAW` |
| G2D-232 | Correctness | Triangle intercepts relied on floating conversion for truncation toward zero. Exact quotient/remainder interpolation now preserves the raster rule for every signed coordinate. | `DRAW` |
| G2D-233 | Portability | Triangle results depended on whether host `long double` had 64 or 80 bits of precision. Integer ratio evaluation now produces identical coverage on Windows, macOS, and Linux. | `DRAW` |
| G2D-234 | Portability | Quadratic Bezier samples also depended on host `long double` precision at large coordinates. Exact Bernstein weights now evaluate the conceptual wide products portably. | `DRAW` |
| G2D-235 | Correctness | Simply rounding both intermediate de Casteljau lerps changes the final rounded curve point. The new evaluator combines quotient/remainder terms first and rounds only the exact final coordinate. | `DRAW` |

## Sprite and batch state

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-236 | Bug | A full-size, same-class but uninitialized Sprite payload passed the old class/size cast. Initialized Sprites now carry and validate a private state cookie. | `SPR` |
| G2D-237 | Bug | Sprite frame counts, capacities, and parallel frame/delay arrays were not revalidated at API boundaries. One state checker now enforces their complete relationship and allocation-size limits. | `SPR` |
| G2D-238 | Correctness | Corrupt Sprite scale, visibility, flip, timing, and transform-cache flags could reach drawing and animation paths. All mutable scalar invariants are now normalized or rejected. | `SPR` |
| G2D-239 | Correctness | Sprite construction depended on allocator-zeroed bytes for fields not assigned individually. The private payload is now explicitly zero-initialized before defaults are installed. | `SPR` |
| G2D-240 | Bug | Assigning the already-current frame reset the animation clock, so idempotent property writes could indefinitely postpone advancement. The clock resets only when the effective frame changes. | `SPR` |
| G2D-241 | Resource | Rotating around a distant origin allocated transparent padding proportional to that distance, allowing tiny sprites to request enormous buffers. Rotation now transforms the real frame only. | `SPR` |
| G2D-242 | Correctness | The padded-origin path rejected otherwise valid off-image pivots when doubled padding dimensions overflowed. Origin coordinates are now transformed analytically without dimension growth. | `SPR` |
| G2D-243 | Correctness | Cardinal rotations replaced the requested origin with the output center. Exact 90/180/270-degree pixel mappings now transform the configured origin. | `SPR` |
| G2D-244 | Correctness | Arbitrary rotations likewise lost non-centered origin placement. The origin now follows the same center-relative rotation and final rounding as the image transform. | `SPR` |
| G2D-245 | Bug | A cached “centered output” override could silently discard the analytically computed origin. That obsolete cache state and draw-time override have been removed. | `SPR` |
| G2D-246 | Resource | Sprite finalization could iterate a negative or corrupt capacity relationship. Cleanup now requires the cookie and bounds the released frame count conservatively. | `SPR` |
| G2D-247 | Bug | A full-size uninitialized SpriteBatch payload could pass the class/size check. Batches now carry a private state cookie. | `BAT` |
| G2D-248 | Bug | Batch count/capacity/item-pointer relationships were trusted outside reserve. Every public entry point now validates the complete queue structure. | `BAT` |
| G2D-249 | Correctness | Batch active/sort flags, alpha, and submission order could hold invalid values. The shared checker now enforces their ranges before use. | `BAT` |
| G2D-250 | Bug | Clipping a negative source origin advanced the destination by unscaled source pixels even for a scaled draw. The skipped source prefix is now converted to destination space. | `BAT` |
| G2D-251 | Correctness | Independently rounding `skipped * scale` could move the clipped far edge by a pixel at fractional percentages. Prefix width is now `scaled(full) - scaled(remaining)`. | `BAT` |
| G2D-252 | Correctness | Destination overflow checks used the unscaled skip and could reject safe placements or admit an overflowing scaled placement. They now use the exact destination prefix. | `BAT` |
| G2D-253 | Correctness | Submission order could increment through `INT64_MAX`, destroying the deterministic equal-depth tie break. Admission now stops before the counter can wrap. | `BAT` |
| G2D-254 | Performance | A batch-wide alpha of zero still sorted commands and created scale, rotation, and color temporaries that could not affect output. `End` now releases the queue immediately. | `BAT` |
| G2D-255 | Resource | Batch finalization trusted negative or inconsistent count/capacity fields while releasing sources. Cleanup now requires initialized state and clamps the release traversal. | `BAT` |

## Tilemap layout, animation, and composition

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-256 | Bug | A full-size uninitialized Tilemap could be mistaken for constructed state. Tilemaps now carry a private initialization cookie. | `TILE` |
| G2D-257 | Bug | Tilemap casts proved only the fixed header, not the dynamic inline tile grid. Validation now recomputes the exact grid byte size and checks full allocation coverage. | `TILE` |
| G2D-258 | Bug | The base `tiles` pointer could be redirected away from inline storage. It must now equal the byte immediately after the implementation header. | `TILE` |
| G2D-259 | Correctness | Layer zero could stop aliasing the base grid or claim ownership of inline memory. Both alias and nonownership invariants are now enforced. | `TILE` |
| G2D-260 | Resource | Additional layers could expose null storage or claim nonownership, confusing access and finalization. Every nonbase live layer must have owned tile storage. | `TILE` |
| G2D-261 | Correctness | Layer visibility/ownership flags and imported projection/parallax scalars were not validated. Flags must be Boolean and all layout scalars finite. | `TILE` |
| G2D-262 | Bug | Tile animation records could carry null parallel arrays or an out-of-range current frame. Runtime validation now proves all shallow animation state before resolution or update. | `TILE` |
| G2D-263 | Correctness | A negative timer or a timer outside the current positive frame duration violated animation phase invariants. Invalid state is now rejected at the boundary. | `TILE` |
| G2D-264 | Bug | Tilemap I/O validated only the active animation frame and could serialize invalid later entries. It now checks every frame tile and duration before saving. | `TILE` |
| G2D-265 | Correctness | Imported animation frame tile IDs could be nonpositive in retained state. Deep I/O validation now preserves the positive-tile contract. | `TILE` |
| G2D-266 | Performance | Resolving empty or negative tile IDs scanned every registered animation. The fast resolver now returns immediately for all non-drawable IDs. | `TILE` |
| G2D-267 | Bug | Native-size tile drawing used opaque region blits, so transparent upper tiles erased lower layers. It now uses source-over region compositing. | `TILE` |
| G2D-268 | Bug | Scaled tile drawing used the same opaque overwrite behavior. Scaled temporaries now use source-over compositing as well. | `TILE` |
| G2D-269 | Correctness | The inner animation traversal assumed every encountered duration remained positive. It now fails closed if invalid retained state reaches that loop. | `TILE` |
| G2D-270 | Resource | Tilemap finalization trusted corrupt layer and animation counts. Cleanup now requires initialized state and clamps both fixed-array traversals to their hard limits. | `TILE` |

## Viewport and extended 2D objects

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-271 | Bug | Full-size uninitialized Viewport2D/ScreenScaler payloads passed type checks. A private cookie and dimension/flag/scale validation now distinguish constructed state. | `G2D` |
| G2D-272 | Correctness | Viewport construction depended on zero-filled scale, offsets, and integer-scaling fields before recalculation. Defaults are now explicitly initialized. | `G2D` |
| G2D-273 | Portability | Viewport scale and letterbox recalculation used `long double` despite bounded integer operands. Exact bounded integer operations now avoid Windows precision differences. | `G2D` |
| G2D-274 | Portability | World-to-screen multiplication lost low bits on platforms where `long double` equals `double`. Portable conceptual-128-bit multiply/divide now rounds exactly. | `G2D` |
| G2D-275 | Correctness | Adding the letterbox offset after coordinate scaling could exceed the signed range. The final addition now saturates explicitly. | `G2D` |
| G2D-276 | Correctness | Screen-to-world formed `screen - offset` in a signed type, which can overflow at `INT64_MIN`. The exact conceptual difference is now formed as an unsigned magnitude plus sign. | `G2D` |
| G2D-277 | Portability | The inverse viewport ratio also depended on host floating precision. It now shares the exact portable quotient/remainder implementation. | `G2D` |
| G2D-278 | Bug | TileChunkCache2D accepted full-size uninitialized payloads as valid caches. It now validates a private cookie, normalized dimensions, and a nonnegative counter. | `G2D` |
| G2D-279 | Bug | TilemapRenderer2D accepted uninitialized cache and counter state. It now validates its cookie, draw count, and optional cache binding. | `G2D` |
| G2D-280 | Bug | AnimationClip2D getters trusted uninitialized frame metadata. Clips now require a cookie, nonnegative start, positive count/delay, and Boolean loop state. | `G2D` |
| G2D-281 | Bug | AnimatedSprite2D could use forged retained handles and impossible playback phase. It now validates its cookie, sprite/clip classes, frame, elapsed remainder, and playing flag. | `G2D` |
| G2D-282 | Bug | TextLayout2D accepted uninitialized font, scale, wrap, and alignment state. Its complete settings are now validated before mutation or measurement. | `G2D` |
| G2D-283 | Bug | RenderPass2D accepted forged source, target, shader, and enabled fields. Every setter and execution now uses one semantic pass validator. | `G2D` |
| G2D-284 | Bug | RenderGraph2D accepted inconsistent count/capacity/pointer state outside reserve. All graph APIs now validate the complete backing-array relationship. | `G2D` |
| G2D-285 | Bug | CollisionMask2D accepted dimensions without a valid backing grid. A private cookie, checked cell count, and non-null storage are now required. | `G2D` |
| G2D-286 | Resource | Extended-object finalizers could release pointer fields from an uninitialized same-class payload. Retaining families now require their private cookie before cleanup. | `G2D` |
| G2D-287 | Correctness | TileChunkCache dirty notifications could wrap negative at `INT64_MAX`. `MarkDirty` now saturates. | `G2D` |
| G2D-288 | Bug | A failed TilemapRenderer draw left the previous successful `DrawCount`, misreporting stale work. The count now resets before validating each draw request. | `G2D` |
| G2D-289 | Correctness | AnimatedSprite elapsed-time addition saturated before division, losing the mathematical frame phase of large deltas. Delta quotient and remainder are now combined without overflow. | `G2D` |
| G2D-290 | Bug | Looping clips could select the wrong frame after an overflowing update. Modular carry arithmetic now preserves the exact effective frame. | `G2D` |
| G2D-291 | Correctness | Non-looping completion compared against an already-saturated elapsed total. It now compares frame steps without forming that total and stops exactly at the last frame. | `G2D` |
| G2D-292 | Bug | Text measurement converted a signed runtime length to `size_t` before validating it. Pointer and length are now checked before conversion. | `G2D` |
| G2D-293 | Bug | Every built-in-font segment depended on allocating a temporary runtime string, and allocation failure silently changed its measured width to zero. Decoder-compatible unit counting now has no fallible allocation. | `G2D` |
| G2D-294 | Performance | Built-in text measurement allocated and released a runtime string for every word and space. The monospace path is now allocation-free. | `G2D` |
| G2D-295 | Bug | Leading, trailing, and whitespace-only text spans disappeared from measured width. Tokenization now preserves every authored horizontal whitespace run. | `G2D` |
| G2D-296 | Bug | Pending spaces were discarded when the following word wrapped, so measured content width no longer represented the input. Whitespace is now laid out as explicit tokens. | `G2D` |
| G2D-297 | Correctness | An overlong word was represented as artificial `WrapWidth`-sized chunks rather than the widths of real glyphs. It is now measured unit by unit only when splitting is necessary. | `G2D` |
| G2D-298 | Bug | Overlong multibyte text could conceptually split inside a UTF-8 input unit. Split points now follow the same 1-to-4-byte consumption rules as the renderers. | `G2D` |
| G2D-299 | Performance | A shaderless RenderPass cloned the complete source before copying to a distinct target. It now clears and draws the borrowed source directly. | `G2D` |
| G2D-300 | Performance | A shaderless pass whose source and target were identical cloned, cleared, and restored unchanged pixels. It is now an exact no-op, including generation state. | `G2D` |
| G2D-301 | Bug | RenderGraph accepted a class-correct but semantically invalid pass. `AddPass` now admits only a fully validated RenderPass2D. | `G2D` |
| G2D-302 | Resource | RenderGraph finalization trusted negative or inconsistent count/capacity state while releasing entries. Cleanup now requires the cookie and bounds the traversal. | `G2D` |
| G2D-303 | Performance | CollisionMask overlap called public handle validation twice for every intersecting cell. Both masks are now validated once and their proven row spans scanned directly. | `G2D` |

## Physics-adjacent 2D runtime

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-304 | Bug | A full-size uninitialized Physics2D.World payload passed class/size validation. Worlds now carry a private initialization cookie. | `PHY` |
| G2D-305 | Bug | A full-size uninitialized Physics2D.Body payload passed validation. Bodies now carry a private initialization cookie. | `PHY` |
| G2D-306 | Bug | A full-size uninitialized Physics2D.Joint payload passed validation. Joints now carry a private initialization cookie. | `PHY` |
| G2D-307 | Bug | A full-size uninitialized Projectile2D payload passed validation. Projectiles now carry a private initialization cookie. | `PHY` |
| G2D-308 | Bug | World counts, capacities, and byte-size limits were trusted after the initial allocation. The world checker now validates every parallel collection relationship. | `PHY` |
| G2D-309 | Bug | Force snapshot pointers could be only partially populated for a nonzero shared capacity. The three arrays must now be all present or all absent. | `PHY` |
| G2D-310 | Correctness | Body validation admitted non-finite or noncanonical circle/AABB geometry. Shape flags, dimensions, radius, motion, and forces now satisfy the sanitized body contract. | `PHY` |
| G2D-311 | Correctness | Body mass/inverse-mass and material coefficients could disagree. Static/dynamic mass pairs and restitution/friction ranges are now checked exactly. | `PHY` |
| G2D-312 | Bug | A body could expose an owner without a valid membership index, or an index without an owner. The owner/index relationship is now invariant. | `PHY` |
| G2D-313 | Bug | Joint validation admitted missing, identical, or malformed body endpoints. Both distinct endpoints must now be complete Body handles. | `PHY` |
| G2D-314 | Correctness | Joint anchors, length, stiffness, damping, type, and active state could be non-finite or outside their domains. The joint checker now enforces every solver precondition. | `PHY` |
| G2D-315 | Correctness | Projectile state could retain NaN, infinity, negative drag/time, or a non-Boolean landed flag. All analytic inputs and phase state are now validated. | `PHY` |
| G2D-316 | Correctness | Projectile ground state accepted arbitrary non-finite values. Only a finite threshold or positive infinity as the disabled sentinel is now valid. | `PHY` |
| G2D-317 | Resource | Growing the three force-snapshot arrays with sequential `realloc` calls could install one or two new arrays before a later allocation failed. Growth now allocates, copies, and commits all three transactionally. | `PHY` |
| G2D-318 | Performance | Transactional force growth does not need zero-filled spare capacity because every live snapshot slot is overwritten. Replacement arrays now use `malloc` rather than paying to clear unused tails. | `PHY` |

## Final implementation review

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-319 | Resource | Preserving unquantized blur precision initially retained a five-`int64_t` accumulator for every image pixel, multiplying temporary memory by ten over RGBA storage. Horizontal windows are now recomputed only when a row enters or leaves the vertical footprint, retaining exact results with `O(width)` auxiliary memory and `O(width * height)` work. | `PIX` |

## Incidental repository correction

The canonical sanitizer lane exposed one issue outside the 2D runtime: the Zia
workspace-command integration probe carried a wait counter from its reference
phase into its rename phase, and its 12-second budget was too short for either
instrumented build. The counter is now phase-local and each CPU-heavy workspace
phase has a 60-second budget within the sanitizer lane's 600-second test timeout.
The corrected probe passes normally and under both ASan and UBSan.

## Validation

- The warnings-as-errors macOS build completed, and all 1,987 non-slow tests
  passed in 532.83 seconds; the audio-unavailable contract was the one expected
  skip on an audio-enabled build.
- The focused final 2D slice passed 11 of 11 binaries, including a deterministic
  naive blur oracle across degenerate, rectangular, oversized-radius,
  low-alpha, and random RGBA inputs.
- Runtime surface audit passed all 8 focused tests and matched 7,723 registered
  functions, 530 classes, and 9,067 header declarations. All cross-platform
  host smoke slices passed. The privileged `/usr/local` install was deliberately
  skipped in the noninteractive verification rerun.
- The canonical ASan and UBSan lanes each selected 1,969 sanitizer-compatible
  tests. Their sole failure was the incidental probe-budget issue above; after
  correction, that exact test passed under ASan in 82.02 seconds and UBSan in
  62.68 seconds, as well as normally in 3.80 seconds. Neither lane reported a
  sanitizer memory or undefined-behavior diagnostic.
- `cppcheck` warning/performance/portability analysis, platform-policy lint,
  documentation checks, ClangFormat verification, and whitespace/error-marker
  checks all passed. The file-header audit found zero missing headers; its 43
  pre-existing prototype inventory entries are outside the touched files.

## Public-contract corrections

The Pixels/Sprite, Shapes2D, Tilemaps2D, Game2D, Rendering2D, and Scene
references now describe final-only blur quantization, origin-preserving sprite
rotation, idempotent frame selection, scaled SpriteBatch clipping, alpha tile
composition, exact viewport transforms, large animation deltas,
whitespace-preserving text measurement, and allocation-free shaderless passes.
