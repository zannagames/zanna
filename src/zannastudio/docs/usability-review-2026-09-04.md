Zanna Studio scene-authoring review — 4 September 2026

Studio has substantial editing infrastructure, but several breaks between authoring, persistence, and execution currently prevent its scene editors from being a dependable way to build a game. The highest-value work is to make authored content survive import/save, make scene edits affect the running game, and make editor previews agree with runtime behavior. Adding more toolbar tools should follow those repairs.

This report contains **25 recommendations**. **P1** means a correctness problem or a major obstacle to creating a working game; **P2** means an important workflow, maintainability, or performance improvement. “Reproduced” means exercised against this checkout's runtime. “Source-confirmed” means traced through the implementation, without claiming a complete interactive reproduction. Performance recommendations identify work performed by the code; their proposed targets are acceptance criteria, not measured current timings.

The review covers the Zia scene editors, project templates, embedded Play, component and asset workflows, and supporting C graphics, animation, navigation, serialization, and GUI integration. The 2D SceneDocument implementation is C++ behind a C ABI; it was included as well. Existing user changes were present before the review and were left intact. No product source was changed.

1. **P1 · Bug · Preserve rigid animation clips through scene import and save.**

   A glTF with one rigid translation clip loads correctly as a SceneAsset, but its instantiated SceneGraph reports zero clips; saving and reopening the graph also produces zero clips. Studio imports through exactly this graph conversion. Animated doors, platforms, props, and imported cinematics can therefore become static after authoring. The runtime copies `model->animations` into the graph's clip carrier but omits `model->node_animations`; the graph writer also lacks the node-animation emission used by the asset writer. **Reproduced:** source clip count 1 → graph count 0 → reopened clip count 0.

   Carry both clip families with explicit types through instantiation, merge, cloning, undo, and graph serialization. Preserve target identities when merging assets. **Acceptance:** import two independently animated props, rename/reparent their roots, save/reopen, and verify both clips and their correct targets survive.

   Evidence: [Studio import](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:818), [runtime instantiation](/Users/stephen/git/zanna/src/runtime/graphics/3d/render/rt_model3d_api.inc:1977), [graph writer](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c:2669).

2. **P1 · Bug · Actually apply hot-reloaded scenes in the generated 3D game.**

   The generated `pollWatch()` notices a changed file, loads it into `probe`, and advances `watchStamp`, then discards the new graph. The World3D continues rendering its old scene. This makes the basic “move something, Save, see it in Play” loop fail for a newly created 3D project. This is source-confirmed; the generated code's local watcher does not transfer the loaded content to the running world.

   Atomically replace the active scene after successful validation, including root metadata, animation carriers, and any registered gameplay/physics state. Decide explicitly whether reload resets or preserves player progress. Publish “reloaded” only after installation succeeds. **Acceptance:** move the goal while the template runs; one save updates both its rendering and its gameplay location without restarting.

   Evidence: [generated watcher](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:451), [initial scene attachment](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:435), [world loop](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:530).

3. **P1 · Bug · Make generated 3D gameplay honor the scene hierarchy and world transforms.**

   The starter game's gameplay loop visits only `scene.Root`'s direct children and measures distance using each node's local `Position`. Grouping a pickup or goal under an empty node removes it from gameplay traversal. Prefab-contained gameplay nodes have the same problem. The editor's preserve-world parenting workflow consequently changes game behavior even though the object remains visually in place.

   Traverse the complete authored hierarchy, respect effective visibility, and evaluate interaction positions in world space. Resolve gameplay identities independently of traversal order. **Acceptance:** collecting a pickup and reaching a goal behave identically before and after grouping, nested reparenting, and prefab instantiation.

   Evidence: [direct-child/local-position loop](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:475), [editor preserve-world parenting](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_hierarchy_surface.zia:639).

4. **P1 · Usability/contract bug · Make starter component forms produce the behavior they advertise.**

   The template schema offers a Pickup component with editable `game.value`, but both generated games always add 1 to the score. The 3D game recognizes `game.kind = "pickup"`, while the component defines only `game.value`. It also supplies no creation recipe to establish the required identity. Applying “Pickup” to a new ordinary node does not make it collectible. These are source-confirmed mismatches between the authoring UI and its supplied consumer.

   Ship complete Pickup and Goal recipes for both dimensions, with required identity fields, previews, and defaults. Read the authored value in the generated game. Explain which component fields the selected runtime adapter consumes. **Acceptance:** create a pickup entirely through the inspector, set Value to 10, run, and receive 10 points.

   Evidence: [template component schema](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:142), [2D scoring](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:285), [3D identity and scoring](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:479).

5. **P1 · Improvement/refactor · Supply a complete scene-to-game adapter in the game templates.**

   The 2D template renders hardcoded colored boxes from object positions; it never builds/draws the authored tilemap or consumes sprite, tile-collision, object-collider, camera, or lighting authoring. Its window size is also hardcoded to map dimensions multiplied by 64. The 3D template changes player coordinates directly and does not turn collider metadata into collision-aware movement. A user can spend time painting and configuring a level whose important features have no effect in the supplied game.

   Provide small, readable Zia adapters using the existing runtime: build the 2D tilemap, instantiate sprites and collider conventions, and use actual runtime movement/collision. Share these conventions with editor previews. Keep a minimal example, but make the default game templates demonstrate a usable authoring loop. **Acceptance:** a painted wall appears and blocks movement; an authored trigger fires; changing the camera affects Play.

   Evidence: [2D loading/drawing](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:226), [3D movement](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:464), [existing BuildTilemap API](/Users/stephen/git/zanna/src/runtime/game/rt_scene_editor.h:902), [existing physics substrate](/Users/stephen/git/zanna/src/runtime/graphics/3d/physics/rt_physics3d.h:8).

6. **P1 · Bug · Prevent stuck input when embedded Play loses the pointer, focus, or queue capacity.**

   Mouse release events are sent only when the pointer is inside the pane, but the remembered button state changes even when the event was not sent. Press inside, release outside, then re-enter: the game never receives the release. Losing pane focus returns before releasing held keys/buttons, so the game can keep moving while the user edits elsewhere. A full C input ring returns failure, which the controller also ignores while advancing its local edge state.

   Track successfully delivered state, capture active mouse gestures through release, and send a release/reset transition on blur. Preserve or resynchronize critical edges after queue overflow; coalesce pointer motion separately. **Acceptance:** test press-drag-release outside, focus loss while holding movement, and queue saturation followed by release. The game must return to neutral input in every case.

   Evidence: [focus and mouse forwarding](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_play_controller.zia:148), [key edge bookkeeping](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_play_controller.zia:252), [bounded C input ring](/Users/stephen/git/zanna/src/lib/graphics/src/vgfx_embed_channel.c:594).

7. **P1 · Improvement · Forward a complete game input stream in embedded Play.**

   Play polls a hardcoded subset of keys. Several letters, digits, function keys, and right-side modifiers are absent; Unicode text is never forwarded, and key modifier payloads are always zero. The transport and producer already support a text event. Games with remappable controls, name entry, chat, or modifier-sensitive shortcuts cannot be tested faithfully in the pane. Absolute pointer forwarding also needs an explicit captured/relative mode for mouse-look games.

   Forward the platform's event stream through a shared input adapter, including text, modifiers, full key coverage, and negotiated relative mouse capture. Provide a visible release-capture shortcut and route Studio shortcuts according to focus. **Acceptance:** type Unicode into a game field, bind an omitted key such as B or 0, use right Shift, and exercise mouse-look without the pointer escaping the game.

   Evidence: [tracked key list](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_play_controller.zia:204), [zero modifier payload](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_play_controller.zia:260), [producer text/modifier handling](/Users/stephen/git/zanna/src/lib/graphics/src/vgfx.c:1537).

8. **P1 · Bug · Negotiate embedded resolution and report rejected frames.**

   Studio always creates a 1920×1080 channel. The C publisher rejects larger frames, while its presentation hook discards that result. A 2560×1440 game can attach successfully but leave Studio waiting forever for a first frame. This is particularly reachable in the 2D starter, where increasing map dimensions also increases the game window. The controller only updates image size on the first presented frame, leaving a later resolution change without an explicit layout update there.

   Negotiate output dimensions before launch, support a bounded resize handshake, and publish a clear resolution error or intentionally scaled frame when capacity is exceeded. Refresh presentation geometry on dimension changes. **Acceptance:** launch at 1440p, resize during Play, and verify frame presentation, aspect ratio, and pointer mapping remain correct.

   Evidence: [channel capacity and first-frame sizing](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_play_controller.zia:41), [frame rejection](/Users/stephen/git/zanna/src/lib/graphics/src/vgfx_embed_channel.c:538), [ignored publication result](/Users/stephen/git/zanna/src/lib/graphics/src/vgfx.c:1496), [template window dimensions](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:364).

9. **P1 · Bug · Preserve rectangular tile proportions in the 2D canvas.**

   The document/runtime support distinct tile width and height, but the editor maps both axes to one `displayCellSize`. `ScaleTile()` explicitly scales a rectangular frame to `cellSize × cellSize`. A 32×16 map is shown as square cells; world-space distances and sprite/collider presentation are consequently distorted relative to a pixel-faithful game view. This is source-confirmed, not just a cosmetic concern about grid styling.

   Use a uniform world-pixel zoom with separate displayed tile width and height throughout rendering, picking, rulers, guides, marquee selection, and snapping. **Acceptance:** a 32×16 tileset retains its 2:1 ratio at every zoom, and a known square in world pixels looks square in Studio and Play.

   Evidence: [axis conversion](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_canvas_camera.zia:126), [square tile scaling](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_tileset_2d.zia:660), [independent runtime tile dimensions](/Users/stephen/git/zanna/src/runtime/graphics/2d/rt_tilemap_io.c:118).

10. **P1 · Bug/optimization · Evict preview cache entries instead of letting valid art disappear.**

   The sprite cache permanently refuses new paths after its first 64 entries. The scaled tile cache returns null when its pixel budget fills, without evicting older offscreen tiles. Moving through a scene or browsing enough art can therefore make later valid assets fall back to markers or colored cells. The rendering result depends on what was visited earlier, even when the currently visible content could fit in memory.

   Use a byte-budgeted LRU or viewport working-set cache for decoded sprites and scaled frames, with bounded retry and actionable diagnostics for individually oversized assets. Preserve memory caps. **Acceptance:** pan among more than 64 sprite assets and enough tile variants to saturate the scale cache; returning to any viewport renders its correct art without reopening the scene.

   Evidence: [sprite refusal](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_tileset_2d.zia:592), [scale-cache saturation](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_tileset_2d.zia:686), [tile fallback rendering](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_renderer.zia:70).

11. **P1 · Improvement · Make every valid tileset frame visually selectable.**

   Both palette hit testing and rendering stop at the first 512 tiles. A 1024×1024 atlas of 32×32 tiles has 1024 frames; half cannot be chosen through the visual palette. Numeric workarounds do not make a practical level-art workflow. The cap is explicitly reported, so this is a product limitation rather than silent corruption.

   Add virtualized palette scrolling or paging, stable selected-frame reveal, jump-to-ID, atlas-region selection, and named saved stamps. Keep all runtime-addressable frames reachable without raising the retained widget/pixel budget. **Acceptance:** select tile 900 visually, paint it, inspect its behavior, and reopen the scene with the same selected frame revealed.

   Evidence: [palette limits/hit test](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_tileset_2d_base.zia:146), [rendering cap](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_tileset_2d_base.zia:388), [runtime behavior ID range](/Users/stephen/git/zanna/src/runtime/game/rt_scene_editor.cpp:4761).

12. **P2 · Improvement · Complete 2D sprite animation authoring and preview.**

   Studio exposes `anim.start`, `anim.count`, `anim.delayMs`, and `anim.loop`, but the object sprite renderer chooses `editor.frame` or a schema-selected static frame. The tile-animation preview clock does not make those object clips play. Users cannot readily verify that a character animation uses the right cells, duration, or loop boundary.

   Add reusable named clips with an atlas frame-strip picker, play/pause/scrub, and preview in both inspector and scene canvas. Validate `start + count` against the actual atlas and use the same clip-resolution logic as the supplied game adapter. **Acceptance:** create idle/walk clips visually and verify frame order, timing, and non-looping completion before running the game.

   Evidence: [clip convention and controls](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_sprite_clip_2d.zia:34), [static object-frame selection](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_tile_previews.zia:395), [tile-oriented preview clock](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_game_preview.zia).

13. **P2 · Refactor/usability · Virtualize the 2D hierarchy and parent chooser.**

   The 2D tree represents only the first 4096 objects, and the parent chooser uses the same bounded prefix. Objects beyond the limit remain in the file, but users lose normal hierarchy access and organization. Building the retained tree also repeatedly walks the represented objects by depth. The 3D editor already has a VirtualTree implementation suitable as an architectural precedent.

   Project the 2D hierarchy from stable IDs through a virtual model, and give the parent chooser a searchable model covering all eligible objects. A safety ceiling should not silently become an inaccessible tail. **Acceptance:** a 10,000-object map supports search, reveal, selection, rename, and reparent for its last object, with viewport-sized widget work.

   Evidence: [2D truncation and construction](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_inspector_state.zia:743), [parent prefix](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_inspector_controls.zia:230), [3D virtual hierarchy](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_inspector_hierarchy.zia).

14. **P1 · Optimization/refactor · Replace whole-scene edit snapshots with transactional deltas and checkpoints.**

   Every accepted 3D edit calls `SaveToText()` and stores the preceding complete serialized scene. 2D commits likewise retain complete canonical content. The budgets prevent unbounded retention, but near the 64 MB 3D editing limit the 256 MB history budget holds only about four snapshots. A small metadata change can require processing all embedded geometry/textures, and a long history jump repeatedly restores intermediate scenes.

   Introduce shared edit transactions with inverse operations, gesture coalescing, stable object identities, and periodic checkpoints. Serialize for save/recovery at revision boundaries without copying immutable resource payloads per edit. Preserve exact rollback semantics. **Acceptance:** 100 transforms on an asset-heavy level remain undoable, while simple edit latency and history memory scale primarily with changed data. Measure p95 before selecting a release target.

   Evidence: [3D commit and history](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:1079), [history jumping](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:326), [2D commit](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_2d_layers_history.zia:335), [history budgets](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_model.zia:93).

15. **P1 · Improvement/refactor · Add referenced assets and repeatable reimport.**

   Import merges model nodes/resources into the canonical scene; import scale and up-axis settings are applied to those roots during that transaction. Ordinary texture pixels serialize as base64 RGBA. A single 4096×4096 RGBA image requires about 89 MB of base64, already above Studio's 64 MB scene editing ceiling before geometry. Source-container textures and prefab references offer useful existing alternatives, but ordinary authoring still needs a durable source/reimport workflow.

   Give imported assets stable project identities, persisted scale/up-axis/material settings, dependency tracking, and non-destructive reimport that preserves placed instances and explicit overrides. Support external resource references while retaining deliberate self-contained exports. **Acceptance:** update a source model and a 4K texture, reimport once, and see all placed instances update without bloating every scene or losing overrides.

   Evidence: [merge/import settings](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:812), [RGBA serialization](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c:1033), [scene size refusal](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:1094).

16. **P2 · Improvement · Support explicit prefab descendant overrides.**

   The current instance contract persists root transform, name, visibility, and metadata; referenced descendants are not serialized as per-instance edits. That prevents common reusable-scene workflows such as recoloring one enemy's child mesh, adjusting a weapon socket, or changing a nested trigger without unpacking or creating another source prefab. Studio does communicate the current limitation.

   Add stable descendant identities, an override list, per-field Revert, reviewed Apply-to-source, and conflict handling when source descendants change or disappear. Keep inherited values visually distinct from local overrides. **Acceptance:** override one child material on one of two instances; changing the source transform updates both while retaining only the intended material exception.

   Evidence: [instance UI contract](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_selection_inspector.zia:635), [runtime prefab serialization](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c:1587), [descendant exclusion](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c:1881).

17. **P2 · Improvement · Add a useful, isolated 3D animation preview workspace.**

   The animation UI refuses skeletal clips and directs users to embedded Play, where their project must already supply a controller. Rigid preview advances by a fixed 1/60 second per editor pump, so playback speed depends on pump frequency. After clip retention is repaired, its live authored-node binding also needs an isolation regression: the runtime writes node transforms directly and ordinary commits serialize the graph. That latter concern is a source-level risk, not a reproduced reachable preview failure in this review.

   Preview both rig and node clips on a separate presentation instance; add a timeline, scrubbing, speed, looping, and root-motion visualization. Advance interactive playback using elapsed time, retaining explicit fixed stepping for tests. **Acceptance:** inspect a newly imported character without writing game code, and edit an unrelated object during preview without persisting any sampled pose.

   Evidence: [preview timing/refusal/binding](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_pump_routing.zia:899), [runtime transform mutation](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_nodeanim.c:1765), [ordinary commit](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:1089).

18. **P1 · Runtime limitation · Bake navigation for stacked floors and bridges.**

   The voxel baker stores only the highest walkable height in each X/Z cell. A bridge removes the navigable surface underneath it; multi-storey buildings cannot retain independent walkable floors through this bake path. **Reproduced:** two overlapping 10×10 floor slabs with top surfaces at Y=0 and Y=5; sampling the baked navmesh at (0,0,0) returns Y=5. Agent-height settings cannot restore a surface that rasterization discarded.

   Use multiple vertical spans per cell with clearance and step-height connectivity. Until that exists, detect overlapping walkable layers and explain the limitation before publishing a bake as usable. Add editor start/end path probes at selectable elevations. **Acceptance:** independently navigate both levels of a bridge and connect floors only through stairs, ramps, or authored links.

   Evidence: [highest-surface rasterizer](/Users/stephen/git/zanna/src/runtime/graphics/3d/nav/rt_navmesh3d_bake.inc:481), [Studio bake call](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:438).

19. **P2 · Optimization/usability · Move expensive asset and bake work out of the UI pump.**

   Thumbnail generation performs a complete image/model load synchronously; limiting work to one visible card per frame does not bound the cost of that load. Model-node and image-pixel limits are checked after loading. Navmesh and probe-grid baking are also single synchronous calls from button handlers. Lightmap baking checks an 8 ms budget between `BakeStep()` calls, which cannot interrupt one expensive step.

   Introduce cancellable jobs for decode/import and CPU baking, with immutable input snapshots, progress, and staged publication on the UI thread. Keep GUI/GPU resource work on its owning thread. Preflight dimensions/dependencies before allocating full assets where possible. **Acceptance:** scrolling a folder of substantial models or baking a level leaves selection, typing, and Cancel responsive; cancellation preserves the prior scene and completed artifacts.

   Evidence: [thumbnail pump](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_asset_browser.zia:701), [load-before-budget checks](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_asset_thumbnails.zia:107), [probe/nav bake calls](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:420), [lightmap pump budget](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:235).

20. **P1 · Bug/workflow · Track bake freshness against scene content and dependencies.**

   Navmesh baking uses the current in-memory scene and writes a sibling artifact as long as a document path exists; it does not require the authored edits to be saved. Later geometry edits do not invalidate the loaded nav overlay. Probe baking accepts the last completed baker without checking it against subsequent scene/light changes. Users can unknowingly ship navigation or lighting that describes a different level revision.

   Stamp artifacts with scene/settings/dependency digests; show Fresh, Stale, or Missing beside bake controls. Save the matching scene snapshot or explicitly bind the bake to an unsaved revision. Invalidate overlays and probe-baker reuse when inputs change, and check artifact freshness before packaging. **Acceptance:** moving a wall or light marks the appropriate bake stale; restarting Studio and packaging cannot silently treat it as current.

   Evidence: [nav path-only precondition](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:438), [probe baker reuse](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:405), [unconditional nav overlay drawing](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_renderer.zia:132), [ordinary edit publication](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_materials_terrain.zia:1104).

21. **P2 · Optimization · Finish GPU viewport presentation and remove avoidable frame allocations.**

   The authored 3D viewport renders offscreen and reads pixels back to the CPU before GUI presentation. Interactive reduced-resolution frames then call `Scale()`, and framed Game View allocates a new composite Pixels buffer. Reusing the readback buffer is valuable, but these branches still add full-image work/allocations precisely during navigation and game-preview use.

   Add a backend-owned texture presentation path with GPU scaling, matte, and overlays, preserving the software fallback. As an incremental step, retain scale/composite buffers and avoid rebuilding unchanged overlays. Measure render, readback, composition, upload, and allocations separately. **Acceptance:** fixed-size interactive frames allocate no new image buffers after warm-up; 1080p and HiDPI navigation meet an explicit frame-time budget on each supported backend.

   Evidence: [readback/scale/composite path](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_renderer.zia:152), [runtime readback API](/Users/stephen/git/zanna/src/runtime/graphics/3d/render/rt_rendertarget3d.c:555).

22. **P2 · Optimization/refactor · Return complete pick results from one spatial query.**

   Viewport picking separately queries the nearest node and nearest triangle hit for the same ray, including the project-preview scene. Locked geometry can cause up to 32 repeated queries while advancing the ray by an epsilon. The runtime already has a common precise traversal, but the split public results make Studio repeat work and manage ordering across multiple scene sources itself.

   Provide one query result containing node identity, distance, normal, and triangle, with filtering for pick locks and transient preview ownership. Merge canonical/preview candidates by distance once; reuse ordered hits for overlap cycling. **Acceptance:** picking a dense imported model uses one traversal per participating spatial structure, and locked/overlapping objects preserve correct front-to-back selection without arbitrary depth failure.

   Evidence: [repeated editor queries](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_viewport_selection.zia:158), [32-attempt locked walk](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_viewport_selection.zia:188), [shared runtime traversal](/Users/stephen/git/zanna/src/runtime/graphics/3d/scene/rt_scene3d_query.c:498).

23. **P2 · Improvement · Connect terrain authoring to the runtime's larger terrain feature set.**

   Studio's sculptable terrain is a canonical mesh heightfield with 9–65 samples on each axis. That is useful for small terrain patches, but the editor does not expose the runtime Terrain3D splat layers and chunk/LOD workflow through this tool. Increasing mesh spacing gives a larger area at the cost of sculpting detail; users cannot build a textured outdoor level with the same workflow.

   Add a persistent terrain asset with chunked heights, paintable material weights, texture scale, and runtime LOD settings; support tiled sculpting and incremental mesh/collision/nav invalidation. Preserve the simple mesh-terrain option. **Acceptance:** author a multi-material outdoor level with local fine detail, save/reopen, and verify matching geometry, material blending, collision, and LOD behavior in Play.

   Evidence: [Studio terrain limits](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_terrain_3d.zia:54), [canonical mesh replacement](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_terrain_authoring.zia:119), [runtime splat layers](/Users/stephen/git/zanna/src/runtime/graphics/3d/world/rt_terrain3d.h:125), [runtime LOD controls](/Users/stephen/git/zanna/src/runtime/graphics/3d/world/rt_terrain3d.h:239).

24. **P2 · Usability/refactor · Make navigation settings and bake degradation observable.**

   Studio exposes requested `nav.cellSize`, agent radius/height, and slope, then reports a successful sidecar bake. The runtime can coarsen the voxel grid to its dimension/memory limits. A large level can therefore receive navigation at a different effective resolution than the designer requested, with no corresponding effective-resolution explanation in Studio's success message. Narrow doors and small walkable features are exactly where that distinction matters.

   Return structured bake diagnostics: requested/effective cell size, dimensions, discarded surfaces, memory estimate, and disconnected region counts. Show an estimated cost before baking and highlight coarse/problematic areas afterward. Add agent-size and path-query overlays, plus visual off-mesh-link editing backed by the existing runtime API. **Acceptance:** an oversized fine-resolution bake reports its degradation explicitly, and the designer can diagnose why a particular doorway or jump is not navigable.

   Evidence: [requested settings/success text](/Users/stephen/git/zanna/src/zannastudio/src/ui/scene_editor_3d_bake_environment.zia:445), [runtime grid coarsening](/Users/stephen/git/zanna/src/runtime/graphics/3d/nav/rt_navmesh3d_bake.inc:704), [existing off-mesh-link surface](/Users/stephen/git/zanna/src/il/runtime/defs/graphics3d/extras.def:401).

25. **P1 · Validation improvement · Gate releases on complete scene-authored game workflows.**

   The generated smoke games prove that their original hardcoded route can reach a pickup or goal. They do not exercise saving during Play, grouping gameplay objects, painting runtime-visible tiles, authoring collision, or changing component values. The existing animation-import probe tests skeletal-clip enumeration and refusal; it does not cover rigid-clip persistence. These gaps explain how broad test success can coexist with the failures above. Some important scene tests, including DnD and 65K capacity, are also labeled slow and excluded by the default build script.

   Add two maintained acceptance projects: a 2D tile/collider/sprite game and a 3D nested-prefab/animated-prop/multi-level-navigation game. Exercise creation through real UI controls, save/reopen, Play, edit/save/reload, undo, and packaging. Assert observable game behavior and pixels, not only flags or canonical strings. Run a small mandatory journey per platform, with larger stress and display matrices separately. **Acceptance:** a tester can build and package both games using the scene editors without manually repairing generated runtime glue.

   Evidence: [2D template smoke](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:328), [3D template smoke](/Users/stephen/git/zanna/src/zannastudio/src/services/project_templates_games.zia:495), [animation probe coverage](/Users/stephen/git/zanna/src/zannastudio/src/probes/scene_dnd_probe.zia:419), [slow scene tests](/Users/stephen/git/zanna/src/tests/CMakeLists.txt:1501).

Suggested execution order: first repair **1–10, 18, and 20**, together with the targeted acceptance tests in **25**. Then complete the everyday content workflows in **11–17**. Follow measured bottlenecks for **19, 21, and 22**, and deepen outdoor/navigation authoring in **23–24**. Keep all implementation within the existing zero-dependency and cross-platform policies. Changes to the public runtime ABI, layering, or applicable scene-format contracts need the repository's spec/ADR review; this report does not change those contracts.

Verification recorded during this review:

- Ran `ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_INSTALL=1 ./scripts/build_zanna_mac.sh`. The incremental Debug build, including native Studio, succeeded. The default suite reported 2024 tests: 2022 passed, one skipped, and one failed. The failure was `test_vaud_core_fixes`, assertion `music.buffer_frames[0] == VAUD_MUSIC_BUFFER_FRAMES` at `src/lib/audio/tests/test_vaud_core_fixes.c:822`. An isolated rerun passed. This suggests a timing-sensitive failure but does not establish its cause; the initial full run was not clean. The script stopped at that failure, so later pipeline stages were not claimed as passed. Log: `/tmp/zanna-studio-review-build.log`.
- Ran `ctest --test-dir build -L scene --output-on-failure -j 4`: **52/52 scene tests passed**, including 14 slow tests (307.52 seconds elapsed). Log: `/tmp/zanna-studio-review-scene-tests.log`. These existing tests do not negate the reproduced failures; recommendation 25 identifies the missing workflow coverage.
- Executed a small runtime reproduction using a generated glTF with two translation keys and a two-floor navigation fixture. Output was `sourceNodeClips=1`, `instantiatedGraphClips=0`, `savedReloadedNodeClips=0`, and `lowerFloorRequestedY=0 sampledY=5`. Fixtures/script/log are in `/tmp/zanna-studio-review/`. A Studio-import probe also showed zero graph clips for that rigid-animation asset; it could not activate the hypothesized live-preview contamination path, which is therefore not presented as a reproduced bug.
- No Windows/Linux execution, sustained interactive game-authoring session, or performance benchmark was performed. Source-confirmed issues and proposed improvements are distinguished above from the two reproduced runtime failures.
