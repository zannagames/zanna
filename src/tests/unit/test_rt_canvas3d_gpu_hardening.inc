//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_canvas3d_gpu_hardening.inc
// Purpose: Canvas3D retained-geometry, submission-atomicity, particle, and alpha-cache regressions.
//
// Key invariants:
//   - Deferred commands own immutable geometry/instance snapshots until frame cleanup.
//   - Failed publication leaves all prior commands byte-for-byte intact and reports diagnostics.
//   - Pixels alpha classification runs once per content generation.
//
// Ownership/Lifetime:
//   - Included by test_rt_canvas3d_gpu_paths.cpp after its shared fake-backend fixtures.
//   - Each test tears down Canvas3D frame-owned allocations through cleanup_fake_canvas.
//
// Links: test_rt_canvas3d_gpu_paths.cpp, rt_canvas3d_internal.h, ADR 0173
//
//===----------------------------------------------------------------------===//

/// @brief Verify backend identity plus immutable retained-geometry lifetime across frames.
/// @details Confirms that a queued command keeps its pre-mutation bytes, a source transform forks
/// exactly one new revision, backend keys carry the corresponding source revisions, and subsequent
/// unchanged frames hit the retained cache without frame vertex/index copies.
static void test_static_mesh_geometry_identity_forwarded(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);

    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);

    rt_mesh3d *mesh_view = (rt_mesh3d *)mesh;
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    float queued_original_x = draws[0].cmd.vertices[0].pos[0];
    EXPECT_TRUE(canvas.draw_count == 1, "Static mesh draw enqueues one draw");
    EXPECT_TRUE(draws[0].cmd.geometry_key == mesh,
                "Static mesh draw forwards a stable geometry identity for backend caches");
    EXPECT_TRUE(draws[0].cmd.geometry_revision == mesh_view->geometry_revision,
                "Static mesh draw forwards the current geometry revision");
    EXPECT_TRUE(draws[0].cmd.vertices != mesh_view->vertices,
                "Static heap mesh draw snapshots vertex data for deferred submission");
    EXPECT_TRUE(draws[0].cmd.indices != mesh_view->indices,
                "Static heap mesh draw snapshots index data for deferred submission");
    EXPECT_TRUE(draws[0].cmd.vertices[1].pos[0] == mesh_view->vertices[1].pos[0] &&
                    draws[0].cmd.indices[2] == mesh_view->indices[2],
                "Static retained geometry revision preserves submitted contents");
    EXPECT_TRUE(canvas.temp_buf_count == 0 && canvas.mesh_snapshot_bytes == 0u,
                "Static retained geometry queues without per-frame vertex/index copies");
    EXPECT_TRUE(canvas.mesh_snapshot_count == 1 &&
                    canvas.mesh_snapshots[0].retained_revision != nullptr,
                "Canvas frame owns a reference to the queued immutable geometry revision");
    EXPECT_TRUE(mesh_view->retained_geometry_build_count == 1,
                "First static draw builds exactly one retained geometry revision");

    void *source_edit = rt_mat4_translate(5.0, 0.0, 0.0);
    rt_mesh3d_transform(mesh, source_edit);
    EXPECT_TRUE(draws[0].cmd.vertices[0].pos[0] == queued_original_x,
                "Queued immutable geometry survives a later source-mesh mutation");
    EXPECT_TRUE(mesh_view->vertices[0].pos[0] == queued_original_x + 5.0f,
                "Source mutation updates the live mesh independently of the queued revision");
    EXPECT_TRUE(mesh_view->retained_geometry == nullptr,
                "Source mutation detaches the mesh-owned old revision reference");

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 2 && draws[1].cmd.vertices != draws[0].cmd.vertices,
                "A post-mutation draw forks to a distinct immutable geometry revision");
    EXPECT_TRUE(draws[1].cmd.vertices[0].pos[0] == queued_original_x + 5.0f,
                "Forked geometry revision captures the mutated source bytes");
    EXPECT_TRUE(mesh_view->retained_geometry_build_count == 2,
                "Geometry mutation causes one additional retained revision build");

    canvas3d_clear_temp_buffers(&canvas);
    canvas3d_clear_temp_objects(&canvas);
    reset_canvas_frame(&canvas, 2);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    EXPECT_TRUE(mesh_view->retained_geometry_build_count == 2 && canvas.temp_buf_count == 0,
                "A later unchanged frame reuses retained geometry without CPU copies");
    canvas3d_clear_temp_buffers(&canvas);
    canvas3d_clear_temp_objects(&canvas);
    reset_canvas_frame(&canvas, 3);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    EXPECT_TRUE(mesh_view->retained_geometry_build_count == 2 &&
                    mesh_view->retained_geometry_cache_hit_count >= 2,
                "Repeated unchanged frames remain retained-geometry cache hits");
    EXPECT_TRUE(canvas.temp_buf_count == 0 && canvas.mesh_snapshot_bytes == 0u,
                "Repeated static frames allocate no per-frame geometry copies");
    cleanup_fake_canvas(&canvas);
}

/// @brief Verify a fault-injected retained-geometry snapshot denial is observable and atomic.
/// @details A distinct second heap mesh forces a new retained binding; the one-shot fault is
/// consumed before allocation/publication, leaving exactly the first command intact while both
/// snapshot and additive submission diagnostics record the denial.
static void test_mesh_snapshot_failure_reports_status_and_preserves_prior_commands(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    void *first_mesh = make_test_mesh();
    void *failed_mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();

    rt_canvas3d_draw_mesh(&canvas, first_mesh, transform, material);
    EXPECT_TRUE(canvas.draw_count == 1, "Snapshot-failure fixture publishes its first command");
    const test_deferred_draw_t first_before = ((test_deferred_draw_t *)canvas.draw_cmds)[0];
    rt_canvas3d_reset_submission_diagnostics(&canvas);
    rt_canvas3d_test_fail_next_mesh_snapshot(&canvas);
    rt_canvas3d_draw_mesh(&canvas, failed_mesh, transform, material);

    EXPECT_TRUE(canvas.draw_count == 1,
                "Snapshot denial does not publish a partial second deferred command");
    EXPECT_TRUE(std::memcmp(&first_before,
                            &((test_deferred_draw_t *)canvas.draw_cmds)[0],
                            sizeof(first_before)) == 0,
                "Snapshot denial leaves the previously queued command byte-for-byte unchanged");
    EXPECT_TRUE(rt_canvas3d_get_last_submission_status(&canvas) ==
                    RT_CANVAS3D_SUBMISSION_SNAPSHOT_FAILURE,
                "Snapshot denial publishes the additive snapshot status");
    EXPECT_TRUE(rt_canvas3d_get_submission_failure_count(&canvas) == 1,
                "Snapshot denial increments the resettable diagnostic count once");
    EXPECT_TRUE(rt_canvas3d_get_mesh_snapshot_drop_count(&canvas) == 1,
                "Snapshot denial also preserves the established snapshot-drop telemetry");
    cleanup_fake_canvas(&canvas);
}

/// @brief Verify Canvas3D snapshots compact records and reuses one immutable unit quad.
/// @details Caller scratch is mutated after queueing, two deferred commands are compared for a
/// stable geometry identity, and fake backend dispatch proves the matrix payload remains null.
static void test_compact_particle_batches_snapshot_instances_and_retain_unit_geometry() {
    vgfx3d_backend_t backend = {};
    rt_canvas3d canvas;
    vgfx3d_particle_instance_t instances[2] = {};
    const void *retained_geometry_key;
    const vgfx3d_vertex_t *retained_vertices;
    const uint32_t *retained_indices;
    backend.name = "opengl";
    backend.gpu_skinning = 1;
    backend.particle_instancing = 1;
    backend.submit_draw_instanced = record_draw_instanced;
    backend.end_frame = noop_end_frame;
    init_fake_canvas(&canvas, &backend);
    reset_recorded_instancing();
    void *material = rt_material3d_new();

    instances[0].center[0] = -0.25f;
    instances[0].center[2] = 0.25f;
    instances[0].center[3] = 1.0f;
    instances[0].right[0] = 0.1f;
    instances[0].up[1] = 0.2f;
    instances[0].color[0] = 1.0f;
    instances[0].color[1] = 0.5f;
    instances[0].color[2] = 0.25f;
    instances[0].color[3] = 0.75f;
    instances[1] = instances[0];
    instances[1].center[0] = 0.25f;
    instances[1].center[2] = -0.25f;
    instances[1].color[0] = 0.25f;

    EXPECT_TRUE(rt_canvas3d_supports_particle_instancing(&canvas) == 1,
                "Canvas reports compact particle support only through the explicit backend flag");
    EXPECT_TRUE(rt_canvas3d_queue_particle_batch(&canvas, material, instances, 2) == 1,
                "A finite compact particle batch enters the deferred instanced queue");
    EXPECT_TRUE(canvas.draw_count == 1,
                "One compact particle batch produces one deferred hardware draw");
    EXPECT_TRUE(canvas.frame_arena_frame_bytes >= sizeof(instances),
                "Compact particle snapshots use retained frame-arena storage");
    EXPECT_TRUE(canvas.temp_buf_count == 0,
                "Compact particle snapshots avoid tracked malloc buffers");
    test_deferred_draw_t *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[0].instance_matrices == nullptr,
                "Compact particle commands do not fabricate full instance matrices");
    EXPECT_TRUE(draws[0].instance_count == 2,
                "Compact particle commands preserve the requested instance count");
    EXPECT_TRUE(draws[0].cmd.particle_instances != nullptr &&
                    draws[0].cmd.particle_instances != instances,
                "Canvas snapshots compact records into frame-owned storage");
    EXPECT_TRUE(draws[0].cmd.particle_instances &&
                    draws[0].cmd.particle_instances[0].center[0] == -0.25f,
                "The compact snapshot preserves sorted source order and values");
    EXPECT_TRUE(draws[0].cmd.vertex_count == 4 && draws[0].cmd.index_count == 6,
                "Compact particle batches reference exactly one unit quad");
    retained_geometry_key = draws[0].cmd.geometry_key;
    retained_vertices = draws[0].cmd.vertices;
    retained_indices = draws[0].cmd.indices;

    instances[0].center[0] = 0.9f;
    EXPECT_TRUE(draws[0].cmd.particle_instances &&
                    draws[0].cmd.particle_instances[0].center[0] == -0.25f,
                "Mutating emitter scratch cannot alter an already queued particle draw");
    EXPECT_TRUE(rt_canvas3d_queue_particle_batch(&canvas, material, instances, 2) == 1,
                "A repeated compact batch queues without rebuilding unit geometry");
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 2 && draws[1].cmd.geometry_key == retained_geometry_key &&
                    draws[1].cmd.vertices == retained_vertices &&
                    draws[1].cmd.indices == retained_indices,
                "Repeated hardware particle draws share one process-lifetime unit quad");
    EXPECT_TRUE(draws[1].cmd.particle_instances != draws[0].cmd.particle_instances,
                "Each deferred particle draw owns an independent compact snapshot");
    rt_canvas3d_end(&canvas);
    EXPECT_TRUE(last_instance_count == 2 && last_instance_matrices == nullptr,
                "Backend dispatch receives compact particles without a matrix payload");
    EXPECT_TRUE(last_instanced_cmd.particle_instances != nullptr,
                "Backend dispatch retains the compact-command marker");
    cleanup_fake_canvas(&canvas);
}

/// @brief Verify deferred alpha routing scans each Pixels content generation exactly once.
/// @details A moderately large texture is submitted repeatedly as opaque, then mutated to binary
/// cutout and fractional alpha. Each generation chooses OPAQUE/MASK/BLEND correctly, and repeated
/// draws reuse the cached classification until a supported write invalidates it.
static void test_pixels_alpha_classification_is_cached_by_content_generation(void) {
    rt_canvas3d canvas;
    init_fake_canvas(&canvas, &kOpenGLBackend);
    void *mesh = make_test_mesh();
    void *material = rt_material3d_new();
    void *transform = rt_mat4_identity();
    void *pixels = rt_pixels_new(128, 128);
    auto *pixels_view = rt_pixels_checked_impl_or_null(pixels);
    rt_pixels_fill_rgba(pixels, 0xFFFFFFFFLL);
    rt_material3d_set_texture(material, pixels);

    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    auto *draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(canvas.draw_count == 3,
                "Repeated opaque Pixels submissions queue every requested draw");
    EXPECT_TRUE(draws[0].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE &&
                    draws[1].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE &&
                    draws[2].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE,
                "All-opaque Pixels retain opaque deferred routing");
    EXPECT_TRUE(pixels_view && pixels_view->alpha_classification_scan_count == 1,
                "Repeated draws scan an unchanged Pixels generation only once");

    rt_pixels_set_rgba(pixels, 17, 23, 0xFFFFFF00LL);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[3].cmd.has_alpha_texture == 1 &&
                    draws[3].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK,
                "Zero/opaque binary alpha promotes automatic routing to MASK");
    EXPECT_TRUE(pixels_view->alpha_classification_scan_count == 2,
                "A supported Pixels write invalidates classification for one new scan");

    rt_pixels_set_rgba(pixels, 17, 23, 0xFFFFFF80LL);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    rt_canvas3d_draw_mesh(&canvas, mesh, transform, material);
    draws = (test_deferred_draw_t *)canvas.draw_cmds;
    EXPECT_TRUE(draws[4].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND &&
                    draws[5].cmd.alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND,
                "Fractional alpha promotes automatic routing to BLEND");
    EXPECT_TRUE(pixels_view->alpha_classification_scan_count == 3,
                "Repeated fractional-alpha draws reuse the generation cache");
    cleanup_fake_canvas(&canvas);
}
