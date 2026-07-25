//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_mesh.inc
// Purpose: OpenGL mesh buffers/attributes and skin, morph, and compact-instance uploads.
// Key invariants:
//   - Vertex attribute layouts match the full or compact shared C record selected by each command.
//   - Particle slots 7 through 10 advance once per 64-byte instance; prior-matrix slots are off.
// Ownership/Lifetime:
//   - Static caches and streaming VBOs belong to gl_context_t and are released with the context.
//   - Uploaded source arrays are borrowed only for the duration of BufferSubData.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_opengl_frame.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements OpenGL geometry caches, vertex layouts, instancing, skinning, and morph data.
/// @details Static meshes use revision-keyed VBO/IBO entries while changing geometry and instance
///          payloads use orphaned streaming buffers. The helpers also enforce attribute-slot
///          contracts and prepare UBO/TBO resources for skeletal and morph deformation.

/// @brief Release every mesh-cache slot's VBO + IBO.
/// @param ctx Borrowed OpenGL context whose fixed mesh-cache slots are cleared.
static void gl_destroy_mesh_cache(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < GL_MESH_CACHE_CAPACITY; i++) {
        if (ctx->mesh_cache[i].vbo)
            gl.DeleteBuffers(1, &ctx->mesh_cache[i].vbo);
        if (ctx->mesh_cache[i].ibo)
            gl.DeleteBuffers(1, &ctx->mesh_cache[i].ibo);
        memset(&ctx->mesh_cache[i], 0, sizeof(ctx->mesh_cache[i]));
    }
}

/// @brief Evict stale static mesh-cache entries using the same frame-age policy as texture caches.
///
/// Static geometry can otherwise stay resident forever after a level or scene transition because
/// the fixed-size cache only evicts on new cache misses. Pruning once per frame keeps long-running
/// sessions bounded even when no new static meshes are submitted after old ones disappear.
/// @param ctx Borrowed OpenGL context whose stale mesh entries are evicted.
static void gl_mesh_cache_prune(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < GL_MESH_CACHE_CAPACITY; i++) {
        gl_mesh_cache_entry_t *entry = &ctx->mesh_cache[i];
        if (!entry->key)
            continue;
        if (!vgfx3d_opengl_should_prune_cache_entry(
                ctx->frame_serial, entry->last_used_frame, 240)) {
            continue;
        }
        if (entry->vbo)
            gl.DeleteBuffers(1, &entry->vbo);
        if (entry->ibo)
            gl.DeleteBuffers(1, &entry->ibo);
        memset(entry, 0, sizeof(*entry));
    }
}

/// @brief Mesh-cache lookup; uploads to a static VBO/IBO on miss.
///
/// Static meshes (with `geometry_key`) are cached so subsequent
/// frames don't re-upload. On miss, finds an empty slot or evicts
/// the LRU entry. Same key + revision = cache hit. Returns 0 if the
/// command isn't cacheable (no key or zero revision).
/// @param ctx Borrowed OpenGL context owning the fixed mesh cache.
/// @param cmd Borrowed draw command containing geometry data and revision identity.
/// @param[out] out_vbo Required destination for the resolved vertex-buffer name.
/// @param[out] out_ibo Required destination for the resolved index-buffer name.
/// @param[out] out_compact Optional destination indicating the packed vertex layout.
/// @return 1 for a valid cached or newly uploaded entry, otherwise 0.
static int gl_lookup_cached_mesh_buffers(gl_context_t *ctx,
                                         const vgfx3d_draw_cmd_t *cmd,
                                         GLuint *out_vbo,
                                         GLuint *out_ibo,
                                         int *out_compact) {
    gl_mesh_cache_entry_t *slot = NULL;
    gl_mesh_cache_entry_t *oldest = NULL;
    int8_t wants_compact;

    if (out_compact)
        *out_compact = 0;
    if (!ctx || !cmd || !out_vbo || !out_ibo || !cmd->geometry_key || cmd->geometry_revision == 0 ||
        !cmd->vertices || !cmd->indices)
        return 0;
    wants_compact = cmd->compact_vertex_stream ? 1 : 0;

    for (int32_t i = 0; i < GL_MESH_CACHE_CAPACITY; i++) {
        gl_mesh_cache_entry_t *entry = &ctx->mesh_cache[i];
        if (entry->key == cmd->geometry_key) {
            slot = entry;
            break;
        }
        if (!entry->key && !slot)
            slot = entry;
        if (!oldest || entry->last_used_frame < oldest->last_used_frame)
            oldest = entry;
    }

    if (!slot)
        slot = oldest;
    if (!slot)
        return 0;

    if (slot->key != cmd->geometry_key || slot->revision != cmd->geometry_revision ||
        slot->vertex_count != cmd->vertex_count || slot->index_count != cmd->index_count ||
        slot->compact != wants_compact || !slot->vbo || !slot->ibo) {
        size_t vbytes;
        size_t ibytes;
        ctx->stats.mesh_cache_misses++;

        if ((size_t)cmd->vertex_count > (size_t)PTRDIFF_MAX / sizeof(vgfx3d_vertex_t) ||
            (size_t)cmd->index_count > (size_t)PTRDIFF_MAX / sizeof(uint32_t)) {
            return 0;
        }
        if (slot->vbo)
            gl.DeleteBuffers(1, &slot->vbo);
        if (slot->ibo)
            gl.DeleteBuffers(1, &slot->ibo);
        memset(slot, 0, sizeof(*slot));

        gl.GenBuffers(1, &slot->vbo);
        gl.GenBuffers(1, &slot->ibo);
        if (!slot->vbo || !slot->ibo) {
            if (slot->vbo)
                gl.DeleteBuffers(1, &slot->vbo);
            if (slot->ibo)
                gl.DeleteBuffers(1, &slot->ibo);
            memset(slot, 0, sizeof(*slot));
            return 0;
        }
        vbytes = (size_t)cmd->vertex_count * sizeof(vgfx3d_vertex_t);
        ibytes = (size_t)cmd->index_count * sizeof(uint32_t);
        gl.BindBuffer(GL_ARRAY_BUFFER, slot->vbo);
        if (wants_compact) {
            /* R20: encode into the packed 48-byte layout; configure_mesh_attributes'
             * compact branch decodes it via fixed-function attribute conversion. */
            size_t compact_bytes = (size_t)cmd->vertex_count * VGFX3D_COMPACT_VERTEX_STRIDE;
            uint8_t *packed = (uint8_t *)malloc(compact_bytes);
            if (!packed) {
                gl.DeleteBuffers(1, &slot->vbo);
                gl.DeleteBuffers(1, &slot->ibo);
                memset(slot, 0, sizeof(*slot));
                return 0;
            }
            vgfx3d_encode_compact_vertices(cmd->vertices, cmd->vertex_count, packed);
            gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)compact_bytes, packed, GL_STATIC_DRAW);
            free(packed);
        } else {
            gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbytes, cmd->vertices, GL_STATIC_DRAW);
        }
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot->ibo);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibytes, cmd->indices, GL_STATIC_DRAW);
        if (!gl_upload_ok()) {
            if (slot->vbo)
                gl.DeleteBuffers(1, &slot->vbo);
            if (slot->ibo)
                gl.DeleteBuffers(1, &slot->ibo);
            memset(slot, 0, sizeof(*slot));
            return 0;
        }

        slot->key = cmd->geometry_key;
        slot->revision = cmd->geometry_revision;
        slot->vertex_count = cmd->vertex_count;
        slot->index_count = cmd->index_count;
        slot->compact = wants_compact;
    } else {
        ctx->stats.mesh_cache_hits++;
    }

    slot->last_used_frame = ctx->frame_serial;
    *out_vbo = slot->vbo;
    *out_ibo = slot->ibo;
    if (out_compact)
        *out_compact = slot->compact ? 1 : 0;
    return 1;
}

/// @brief Hand back VBO/IBO for the mesh — cached path or stream-upload path.
///
/// First tries `gl_lookup_cached_mesh_buffers` (static mesh fast path).
/// On miss, ensures the dynamic streaming buffers are large enough,
/// orphans them (avoids stalls), and uploads via `BufferSubData`.
/// @param ctx Borrowed OpenGL context owning cached and streaming geometry buffers.
/// @param cmd Borrowed draw command containing source vertices and indices.
/// @param[out] out_vbo Required destination for the selected vertex-buffer name.
/// @param[out] out_ibo Required destination for the selected index-buffer name.
/// @param[out] out_compact Optional destination indicating the packed vertex layout.
/// @return 1 when both buffers contain the requested geometry, otherwise 0.
static int prepare_mesh_buffers(gl_context_t *ctx,
                                const vgfx3d_draw_cmd_t *cmd,
                                GLuint *out_vbo,
                                GLuint *out_ibo,
                                int *out_compact) {
    size_t vbytes;
    size_t ibytes;

    if (out_compact)
        *out_compact = 0;
    if (!ctx || !cmd || !out_vbo || !out_ibo || !cmd->vertices || !cmd->indices ||
        (size_t)cmd->vertex_count > (size_t)PTRDIFF_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)cmd->index_count > (size_t)PTRDIFF_MAX / sizeof(uint32_t)) {
        return 0;
    }
    *out_vbo = 0;
    *out_ibo = 0;
    vbytes = (size_t)cmd->vertex_count * sizeof(vgfx3d_vertex_t);
    ibytes = (size_t)cmd->index_count * sizeof(uint32_t);
    if (gl_lookup_cached_mesh_buffers(ctx, cmd, out_vbo, out_ibo, out_compact))
        return 1;
    /* Streaming path always uploads the full layout. */
    ctx->stats.mesh_stream_uploads++;

    if (ensure_buffer_capacity(GL_ARRAY_BUFFER,
                               ctx->mesh_vbo,
                               &ctx->mesh_vbo_capacity,
                               vbytes,
                               4u * 1024u * 1024u,
                               GL_STREAM_DRAW) != 0) {
        return 0;
    }
    if (!orphan_stream_buffer(
            GL_ARRAY_BUFFER, ctx->mesh_vbo, ctx->mesh_vbo_capacity, GL_STREAM_DRAW)) {
        return 0;
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->mesh_vbo);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vbytes, cmd->vertices);
    if (!gl_upload_ok())
        return 0;

    if (ensure_buffer_capacity(GL_ELEMENT_ARRAY_BUFFER,
                               ctx->mesh_ibo,
                               &ctx->mesh_ibo_capacity,
                               ibytes,
                               1u * 1024u * 1024u,
                               GL_STREAM_DRAW) != 0) {
        return 0;
    }
    if (!orphan_stream_buffer(
            GL_ELEMENT_ARRAY_BUFFER, ctx->mesh_ibo, ctx->mesh_ibo_capacity, GL_STREAM_DRAW)) {
        return 0;
    }
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->mesh_ibo);
    gl.BufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)ibytes, cmd->indices);
    if (!gl_upload_ok())
        return 0;
    *out_vbo = ctx->mesh_vbo;
    *out_ibo = ctx->mesh_ibo;
    return 1;
}

/// @brief Disable instance attribute arrays and supply identity-matrix constants.
///
/// When the draw is non-instanced, attribute slots 7..14 (which would
/// hold the 4×4 instance matrix + previous instance matrix) need
/// per-vertex constant data so the vertex shader behaves as if the
/// instance matrix was identity. Calling `glVertexAttrib4f` provides
/// constants when the attribute array is disabled.
static void set_identity_instance_constants(void) {
    gl.DisableVertexAttribArray(7);
    gl.DisableVertexAttribArray(8);
    gl.DisableVertexAttribArray(9);
    gl.DisableVertexAttribArray(10);
    gl.DisableVertexAttribArray(11);
    gl.DisableVertexAttribArray(12);
    gl.DisableVertexAttribArray(13);
    gl.DisableVertexAttribArray(14);
    gl.VertexAttribDivisor(7, 0);
    gl.VertexAttribDivisor(8, 0);
    gl.VertexAttribDivisor(9, 0);
    gl.VertexAttribDivisor(10, 0);
    gl.VertexAttribDivisor(11, 0);
    gl.VertexAttribDivisor(12, 0);
    gl.VertexAttribDivisor(13, 0);
    gl.VertexAttribDivisor(14, 0);
    gl.VertexAttrib4f(7, 1.0f, 0.0f, 0.0f, 0.0f);
    gl.VertexAttrib4f(8, 0.0f, 1.0f, 0.0f, 0.0f);
    gl.VertexAttrib4f(9, 0.0f, 0.0f, 1.0f, 0.0f);
    gl.VertexAttrib4f(10, 0.0f, 0.0f, 0.0f, 1.0f);
    gl.VertexAttrib4f(11, 1.0f, 0.0f, 0.0f, 0.0f);
    gl.VertexAttrib4f(12, 0.0f, 1.0f, 0.0f, 0.0f);
    gl.VertexAttrib4f(13, 0.0f, 0.0f, 1.0f, 0.0f);
    gl.VertexAttrib4f(14, 0.0f, 0.0f, 0.0f, 1.0f);
}

/// @brief Bind the mesh VBO/IBO and wire up the 7 per-vertex attributes.
///
/// Attribute slots: 0=pos, 1=normal, 2=uv0, 15=uv1, 3=color (4f), 4=tangent,
/// 5=bone indices (4×u8 as int), 6=bone weights (4f). Layout matches
/// the `vgfx3d_vertex_t` struct's offsets. When @p compact is nonzero the VBO
/// holds the packed 48-byte stream (R20) and fixed-function attribute
/// conversion (GL_SHORT/GL_HALF_FLOAT/GL_UNSIGNED_BYTE normalized) decodes it
/// to the same float shader inputs — no shader change.
/// Also resets instance attributes to identity defaults.
/// @param ctx Borrowed OpenGL context containing the primary VAO.
/// @param mesh_vbo Vertex-buffer object holding full or compact vertices.
/// @param mesh_ibo Index-buffer object for the draw.
/// @param compact Nonzero when @p mesh_vbo uses `VGFX3D_COMPACT_VERTEX_STRIDE`.
static void configure_mesh_attributes(gl_context_t *ctx,
                                      GLuint mesh_vbo,
                                      GLuint mesh_ibo,
                                      int compact) {
    gl.BindVertexArray(ctx->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh_ibo);
    if (compact) {
        GLsizei stride = (GLsizei)VGFX3D_COMPACT_VERTEX_STRIDE;
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)(uintptr_t)0u);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 4, GL_SHORT, GL_TRUE, stride, (void *)(uintptr_t)12u);
        gl.EnableVertexAttribArray(1);
        gl.VertexAttribPointer(2, 2, GL_HALF_FLOAT, GL_FALSE, stride, (void *)(uintptr_t)20u);
        gl.EnableVertexAttribArray(2);
        gl.VertexAttribPointer(15, 2, GL_HALF_FLOAT, GL_FALSE, stride, (void *)(uintptr_t)24u);
        gl.EnableVertexAttribArray(15);
        gl.VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *)(uintptr_t)28u);
        gl.EnableVertexAttribArray(3);
        gl.VertexAttribPointer(4, 4, GL_SHORT, GL_TRUE, stride, (void *)(uintptr_t)32u);
        gl.EnableVertexAttribArray(4);
        gl.VertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, stride, (void *)(uintptr_t)40u);
        gl.EnableVertexAttribArray(5);
        gl.VertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *)(uintptr_t)44u);
        gl.EnableVertexAttribArray(6);
        set_identity_instance_constants();
        return;
    }
    GLsizei stride = (GLsizei)sizeof(vgfx3d_vertex_t);
    gl.VertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, pos));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, normal));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, uv));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(
        15, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, uv1));
    gl.EnableVertexAttribArray(15);
    gl.VertexAttribPointer(
        3, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, color));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(
        4, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, tangent));
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribIPointer(
        5, 4, GL_UNSIGNED_BYTE, stride, (void *)offsetof(vgfx3d_vertex_t, bone_indices));
    gl.EnableVertexAttribArray(5);
    gl.VertexAttribPointer(
        6, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_vertex_t, bone_weights));
    gl.EnableVertexAttribArray(6);
    set_identity_instance_constants();
}

/// @brief Upload per-instance matrices and bind them to attribute slots 7..14.
///
/// 4×4 matrices are unpacked into four vec4 attribute slots each
/// (HW limit: max attribute size is vec4). `glVertexAttribDivisor(slot, 1)`
/// makes them advance once per instance instead of per vertex. Previous-
/// instance matrices (slots 11..14) are uploaded if `prev_instance_matrices`
/// is non-NULL; otherwise the slots get identity constants for "no
/// motion this frame" semantics.
/// @param ctx Borrowed OpenGL context owning current and previous instance buffers.
/// @param instance_matrices Borrowed array of current row-major 4x4 model matrices.
/// @param prev_instance_matrices Borrowed prior matrix array, or NULL.
/// @param instance_count Number of matrices in each supplied array.
/// @return 1 when current and optional prior attributes are uploaded and configured.
static int configure_instance_attributes(gl_context_t *ctx,
                                         const float *instance_matrices,
                                         const float *prev_instance_matrices,
                                         int32_t instance_count) {
    size_t bytes;
    if (!ctx || !instance_matrices || instance_count <= 0 ||
        (size_t)instance_count > (size_t)PTRDIFF_MAX / (16u * sizeof(float))) {
        return 0;
    }
    bytes = (size_t)instance_count * 16u * sizeof(float);
    if (ensure_buffer_capacity(GL_ARRAY_BUFFER,
                               ctx->instance_vbo,
                               &ctx->instance_vbo_capacity,
                               bytes,
                               64u * 1024u,
                               GL_STREAM_DRAW) != 0) {
        return 0;
    }
    if (!orphan_stream_buffer(
            GL_ARRAY_BUFFER, ctx->instance_vbo, ctx->instance_vbo_capacity, GL_STREAM_DRAW)) {
        return 0;
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->instance_vbo);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, instance_matrices);
    if (!gl_upload_ok())
        return 0;

    gl.EnableVertexAttribArray(7);
    gl.EnableVertexAttribArray(8);
    gl.EnableVertexAttribArray(9);
    gl.EnableVertexAttribArray(10);
    gl.VertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)0);
    gl.VertexAttribPointer(
        8, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(4 * sizeof(float)));
    gl.VertexAttribPointer(
        9, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(8 * sizeof(float)));
    gl.VertexAttribPointer(
        10, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(12 * sizeof(float)));
    gl.VertexAttribDivisor(7, 1);
    gl.VertexAttribDivisor(8, 1);
    gl.VertexAttribDivisor(9, 1);
    gl.VertexAttribDivisor(10, 1);

    if (prev_instance_matrices) {
        if (ensure_buffer_capacity(GL_ARRAY_BUFFER,
                                   ctx->prev_instance_vbo,
                                   &ctx->prev_instance_vbo_capacity,
                                   bytes,
                                   64u * 1024u,
                                   GL_STREAM_DRAW) != 0) {
            return 0;
        }
        if (!orphan_stream_buffer(GL_ARRAY_BUFFER,
                                  ctx->prev_instance_vbo,
                                  ctx->prev_instance_vbo_capacity,
                                  GL_STREAM_DRAW)) {
            return 0;
        }
        gl.BindBuffer(GL_ARRAY_BUFFER, ctx->prev_instance_vbo);
        gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, prev_instance_matrices);
        if (!gl_upload_ok())
            return 0;
        gl.EnableVertexAttribArray(11);
        gl.EnableVertexAttribArray(12);
        gl.EnableVertexAttribArray(13);
        gl.EnableVertexAttribArray(14);
        gl.VertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)0);
        gl.VertexAttribPointer(
            12, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(4 * sizeof(float)));
        gl.VertexAttribPointer(
            13, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(8 * sizeof(float)));
        gl.VertexAttribPointer(
            14, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void *)(12 * sizeof(float)));
        gl.VertexAttribDivisor(11, 1);
        gl.VertexAttribDivisor(12, 1);
        gl.VertexAttribDivisor(13, 1);
        gl.VertexAttribDivisor(14, 1);
    } else {
        gl.DisableVertexAttribArray(11);
        gl.DisableVertexAttribArray(12);
        gl.DisableVertexAttribArray(13);
        gl.DisableVertexAttribArray(14);
        gl.VertexAttribDivisor(11, 0);
        gl.VertexAttribDivisor(12, 0);
        gl.VertexAttribDivisor(13, 0);
        gl.VertexAttribDivisor(14, 0);
        gl.VertexAttrib4f(11, 1.0f, 0.0f, 0.0f, 0.0f);
        gl.VertexAttrib4f(12, 0.0f, 1.0f, 0.0f, 0.0f);
        gl.VertexAttrib4f(13, 0.0f, 0.0f, 1.0f, 0.0f);
        gl.VertexAttrib4f(14, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    return 1;
}

/// @brief Upload compact retained-quad particle records into instance attribute slots 7..10.
/// @details Each record is four aligned vec4 lanes: center, right half-axis, up half-axis, and
///   color. The ordinary matrix and previous-matrix attributes are disabled, so the vertex shader
///   advances exactly 64 bytes per particle and never materializes model/normal matrices.
/// @param ctx Borrowed OpenGL context owning the instance stream buffer.
/// @param instances Borrowed array of compact particle records.
/// @param instance_count Number of records to upload.
/// @return Non-zero after a successful orphan/upload/bind sequence.
static int configure_particle_instance_attributes(gl_context_t *ctx,
                                                  const vgfx3d_particle_instance_t *instances,
                                                  int32_t instance_count) {
    size_t bytes;
    GLsizei stride = (GLsizei)sizeof(vgfx3d_particle_instance_t);
    if (!ctx || !instances || instance_count <= 0 ||
        (size_t)instance_count > (size_t)PTRDIFF_MAX / sizeof(*instances))
        return 0;
    bytes = (size_t)instance_count * sizeof(*instances);
    if (ensure_buffer_capacity(GL_ARRAY_BUFFER,
                               ctx->instance_vbo,
                               &ctx->instance_vbo_capacity,
                               bytes,
                               64u * 1024u,
                               GL_STREAM_DRAW) != 0 ||
        !orphan_stream_buffer(
            GL_ARRAY_BUFFER, ctx->instance_vbo, ctx->instance_vbo_capacity, GL_STREAM_DRAW))
        return 0;
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->instance_vbo);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, instances);
    if (!gl_upload_ok())
        return 0;
    for (GLuint slot = 7; slot <= 10; slot++) {
        gl.EnableVertexAttribArray(slot);
        gl.VertexAttribDivisor(slot, 1);
    }
    gl.VertexAttribPointer(
        7, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_particle_instance_t, center));
    gl.VertexAttribPointer(
        8, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_particle_instance_t, right));
    gl.VertexAttribPointer(
        9, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_particle_instance_t, up));
    gl.VertexAttribPointer(
        10, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(vgfx3d_particle_instance_t, color));
    for (GLuint slot = 11; slot <= 14; slot++) {
        gl.DisableVertexAttribArray(slot);
        gl.VertexAttribDivisor(slot, 0);
    }
    return 1;
}

/// @brief Write a 4×4 identity matrix into @p dst (row-major).
/// @param[out] dst Caller-owned storage for 16 float components.
static void store_identity4x4(float *dst) {
    memset(dst, 0, sizeof(float) * 16u);
    dst[0] = 1.0f;
    dst[5] = 1.0f;
    dst[10] = 1.0f;
    dst[15] = 1.0f;
}

/// @brief Upload up to 256 bone matrices into a UBO, transposing each.
///
/// GLSL's `mat4` UBO layout is column-major; our matrices are row-
/// major. We transpose during upload so the shader sees the right
/// layout without per-multiply transposes.
/// @param ctx Borrowed OpenGL context providing error accounting.
/// @param ubo Uniform-buffer object receiving the fixed-size palette.
/// @param bone_palette Borrowed array of row-major 4x4 matrices.
/// @param bone_count Number of source matrices, clamped to the shader maximum.
static void upload_bone_palette(gl_context_t *ctx,
                                GLuint ubo,
                                const float *bone_palette,
                                int32_t bone_count) {
    float upload[VGFX3D_OPENGL_BONE_PALETTE_FLOATS];

    if (!ctx || !bone_palette || bone_count <= 0 || !ubo)
        return;

    for (int32_t i = 0; i < VGFX3D_OPENGL_MAX_BONES; i++)
        store_identity4x4(&upload[(size_t)i * 16u]);
    if (bone_count > VGFX3D_OPENGL_MAX_BONES)
        bone_count = VGFX3D_OPENGL_MAX_BONES;
    for (int32_t i = 0; i < bone_count; i++) {
        float safe_matrix[16];
        vgfx3d_copy_mat4_finite_or_identity(safe_matrix, &bone_palette[(size_t)i * 16u]);
        transpose4x4(safe_matrix, &upload[(size_t)i * 16u]);
    }

    if (!orphan_stream_buffer(
            GL_UNIFORM_BUFFER, ubo, VGFX3D_OPENGL_BONE_PALETTE_BYTES, GL_DYNAMIC_DRAW)) {
        return;
    }
    gl.BindBuffer(GL_UNIFORM_BUFFER, ubo);
    gl.BufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(upload), upload);
    gl_upload_ok();
}

/// @brief Upload current + previous bone palettes and bind to UBO slots 0/1.
///
/// If no `prev_bone_palette` is supplied (e.g., first frame of an
/// animation), the current palette is used for both — motion blur
/// then sees zero motion for skinned vertices.
/// @param ctx Borrowed OpenGL context owning current and previous bone UBOs.
/// @param cmd Borrowed draw command containing current and optional prior palettes.
static void upload_active_bone_palettes(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    if (!ctx || !cmd || !cmd->bone_palette || cmd->bone_count <= 0)
        return;
    upload_bone_palette(ctx, ctx->bone_ubo, cmd->bone_palette, cmd->bone_count);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, ctx->bone_ubo);
    if (cmd->prev_bone_palette) {
        upload_bone_palette(ctx, ctx->prev_bone_ubo, cmd->prev_bone_palette, cmd->bone_count);
        gl.BindBufferBase(GL_UNIFORM_BUFFER, 1, ctx->prev_bone_ubo);
    } else {
        upload_bone_palette(ctx, ctx->prev_bone_ubo, cmd->bone_palette, cmd->bone_count);
        gl.BindBufferBase(GL_UNIFORM_BUFFER, 1, ctx->prev_bone_ubo);
    }
}

/// @brief Allocate (if needed) a TBO + buffer pair and upload morph data.
///
/// Texture-buffer objects (TBO) let the vertex shader sample large
/// morph-target arrays via the texture-fetch path — no UBO size limits.
/// Format is `R32F` so each delta is one float; XYZ triples consume
/// three texels.
/// @param[in,out] buffer Caller-owned buffer name, generated when zero.
/// @param[in,out] tbo Caller-owned texture-buffer name, generated when zero.
/// @param payload Borrowed float payload to upload.
/// @param bytes Number of bytes in @p payload.
/// @return 1 when allocation, upload, and texture-buffer binding succeed.
static int gl_upload_morph_payload_texture(GLuint *buffer,
                                           GLuint *tbo,
                                           const float *payload,
                                           size_t bytes) {
    if (!buffer || !tbo || !payload || bytes == 0 || bytes > (size_t)PTRDIFF_MAX)
        return 0;
    if (*buffer == 0)
        gl.GenBuffers(1, buffer);
    if (*tbo == 0)
        gl.GenTextures(1, tbo);
    if (*buffer == 0 || *tbo == 0)
        return 0;
    gl.BindBuffer(GL_TEXTURE_BUFFER, *buffer);
    gl.BufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)bytes, payload, GL_STATIC_DRAW);
    gl.BindTexture(GL_TEXTURE_BUFFER, *tbo);
    gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32F, *buffer);
    return gl_upload_ok();
}

/// @brief Validate a morph payload against the context's texture-buffer limit.
///
/// OpenGL exposes `GL_MAX_TEXTURE_BUFFER_SIZE` in texels, not bytes. Morph
/// payloads are uploaded as `R32F`, so each texel is one float. This check keeps
/// both cached and transient morph uploads inside the hardware TBO address range
/// before `glTexBuffer` binds the buffer to the shader.
/// @param ctx Borrowed OpenGL context containing the maximum texture-buffer texel count.
/// @param bytes Proposed `R32F` payload byte count.
/// @return Non-zero when @p bytes can be represented by this context's TBO limit.
static int gl_morph_payload_fits_texture_buffer(const gl_context_t *ctx, size_t bytes) {
    if (!ctx)
        return 0;
    return vgfx3d_opengl_texture_buffer_accepts_r32f_payload(bytes, ctx->max_texture_buffer_size);
}

/// @brief Streaming variant of `gl_upload_morph_payload_texture` (orphan-then-update).
///
/// Used for non-cached (per-frame-changing) morph payloads. Geometric
/// resize of the underlying buffer; orphans existing storage so the
/// driver doesn't stall.
/// @param ctx Borrowed OpenGL context providing capacity growth and error state.
/// @param buffer Existing streaming buffer object name.
/// @param tbo Existing texture-buffer object name.
/// @param[in,out] capacity_bytes Caller-owned current capacity, updated after growth.
/// @param payload Borrowed float payload to upload.
/// @param bytes Number of bytes in @p payload.
/// @return 1 when storage, upload, and texture-buffer binding succeed.
static int gl_upload_transient_morph_payload(gl_context_t *ctx,
                                             GLuint buffer,
                                             GLuint tbo,
                                             size_t *capacity_bytes,
                                             const float *payload,
                                             size_t bytes) {
    if (!ctx || !capacity_bytes || !payload || bytes == 0 || bytes > (size_t)PTRDIFF_MAX)
        return 0;
    if (ensure_buffer_capacity(
            GL_TEXTURE_BUFFER, buffer, capacity_bytes, bytes, 64u * 1024u, GL_STREAM_DRAW) != 0) {
        return 0;
    }
    if (!orphan_stream_buffer(GL_TEXTURE_BUFFER, buffer, *capacity_bytes, GL_STREAM_DRAW))
        return 0;
    gl.BindBuffer(GL_TEXTURE_BUFFER, buffer);
    gl.BufferSubData(GL_TEXTURE_BUFFER, 0, (GLsizeiptr)bytes, payload);
    gl.BindTexture(GL_TEXTURE_BUFFER, tbo);
    gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32F, buffer);
    return gl_upload_ok();
}

/// @brief Compute the byte size of a morph delta texture-buffer payload
///   (morph_count × vertex_count × 3 floats) with overflow checking; writes @p out_bytes
///   and returns 1, or zeroes it and returns 0 on overflow or zero counts.
/// @param vertex_count Number of vertices represented by each morph shape.
/// @param morph_count Number of morph shapes in the payload.
/// @param[out] out_bytes Required destination for the checked byte count.
/// @return 1 when a nonzero representable size is written, otherwise 0.
static int gl_compute_morph_payload_bytes(uint32_t vertex_count,
                                          int32_t morph_count,
                                          size_t *out_bytes) {
    size_t elements;
    size_t bytes;

    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || vertex_count == 0 || morph_count <= 0)
        return 0;
    if ((size_t)morph_count > SIZE_MAX / (size_t)vertex_count)
        return 0;
    elements = (size_t)morph_count * (size_t)vertex_count;
    if (elements > SIZE_MAX / 3u)
        return 0;
    elements *= 3u;
    if (elements > SIZE_MAX / sizeof(float))
        return 0;
    bytes = elements * sizeof(float);
    if (bytes > (size_t)PTRDIFF_MAX)
        return 0;
    *out_bytes = bytes;
    return 1;
}

/// @brief Find or allocate a morph-cache slot for the given draw command.
///
/// Cache hit → bump LRU and return. Miss with room → claim a fresh slot.
/// Miss without room → evict the LRU entry. Returns NULL when the
/// command isn't cacheable (no key or zero revision).
/// @param ctx Borrowed OpenGL context owning the growable morph cache.
/// @param cmd Borrowed draw command containing morph identity and payloads.
/// @param morph_count Sanitized number of morph shapes to cache.
/// @return Matching or newly uploaded cache entry, or NULL on validation/allocation failure.
static gl_morph_cache_entry_t *gl_get_cached_morph_entry(gl_context_t *ctx,
                                                         const vgfx3d_draw_cmd_t *cmd,
                                                         int32_t morph_count) {
    gl_morph_cache_entry_t *slot = NULL;
    gl_morph_cache_entry_t *oldest = NULL;
    size_t bytes;

    if (!ctx || !cmd || morph_count <= 0 || !cmd->morph_key || cmd->morph_revision == 0)
        return NULL;

    for (int32_t i = 0; i < ctx->morph_cache_count; i++) {
        gl_morph_cache_entry_t *entry = &ctx->morph_cache[i];
        if (vgfx3d_opengl_should_reuse_morph_cache(entry->key,
                                                   entry->revision,
                                                   entry->shape_count,
                                                   entry->vertex_count,
                                                   entry->has_normal_deltas,
                                                   cmd)) {
            entry->last_used_frame = ctx->frame_serial;
            return entry;
        }
        if (entry->key == cmd->morph_key)
            slot = entry;
        if (!oldest || entry->last_used_frame < oldest->last_used_frame)
            oldest = entry;
    }

    if (!slot && ctx->morph_cache_count < ctx->morph_cache_capacity) {
        slot = &ctx->morph_cache[ctx->morph_cache_count++];
        memset(slot, 0, sizeof(*slot));
    }
    if (!slot && ctx->morph_cache_count == ctx->morph_cache_capacity) {
        if (ctx->morph_cache_count >= INT_MAX)
            return NULL;
        int32_t new_cap =
            vgfx3d_opengl_next_capacity(ctx->morph_cache_capacity, ctx->morph_cache_count + 1, 8);
        if (new_cap <= ctx->morph_cache_count ||
            (size_t)new_cap > SIZE_MAX / sizeof(gl_morph_cache_entry_t))
            return NULL;
        gl_morph_cache_entry_t *nv = (gl_morph_cache_entry_t *)realloc(
            ctx->morph_cache, (size_t)new_cap * sizeof(gl_morph_cache_entry_t));
        if (!nv)
            return NULL;
        memset(nv + ctx->morph_cache_capacity,
               0,
               (size_t)(new_cap - ctx->morph_cache_capacity) * sizeof(gl_morph_cache_entry_t));
        ctx->morph_cache = nv;
        ctx->morph_cache_capacity = new_cap;
        slot = &ctx->morph_cache[ctx->morph_cache_count++];
    }
    if (!slot)
        slot = oldest;
    if (!slot)
        return NULL;

    morph_cache_release_entry(slot);
    if (!gl_compute_morph_payload_bytes(cmd->vertex_count, morph_count, &bytes))
        return NULL;
    if (!gl_morph_payload_fits_texture_buffer(ctx, bytes))
        return NULL;
    if (!gl_upload_morph_payload_texture(
            &slot->morph_buffer, &slot->morph_tbo, cmd->morph_deltas, bytes)) {
        morph_cache_release_entry(slot);
        return NULL;
    }
    if (cmd->morph_normal_deltas &&
        !gl_upload_morph_payload_texture(
            &slot->morph_normal_buffer, &slot->morph_normal_tbo, cmd->morph_normal_deltas, bytes)) {
        morph_cache_release_entry(slot);
        return NULL;
    }
    slot->key = cmd->morph_key;
    slot->revision = cmd->morph_revision;
    slot->shape_count = morph_count;
    slot->vertex_count = cmd->vertex_count;
    slot->has_normal_deltas = cmd->morph_normal_deltas ? 1 : 0;
    slot->morph_bytes = bytes;
    slot->morph_normal_bytes = cmd->morph_normal_deltas ? bytes : 0;
    slot->last_used_frame = ctx->frame_serial;
    return slot;
}

/// @brief Push skinning + morph state into the program.
///
/// Bone palette → UBO. Morph weights → uniform float array. Morph
/// deltas → TBOs bound to dedicated vertex texture units. Cached entries take
/// the fast path; transient morph data goes through the streaming
/// path. Uniform values fall back to safe zeros when arrays aren't
/// available so the shader's `if (morphShapeCount > 0)` short-circuits.
/// @param ctx Borrowed OpenGL context owning deformation buffers.
/// @param cmd Borrowed draw command containing bone and morph payloads.
/// @param uHasSkinning Uniform location for the current-palette enable flag.
/// @param uInstanceBoneStride Uniform location for per-instance palette stride.
/// @param uMorphShapeCount Uniform location for the active morph count.
/// @param uVertexCount Uniform location for vertices per morph shape.
/// @param uMorphWeights Uniform location for current morph weights.
/// @param uPrevMorphWeights Uniform location for prior-frame morph weights.
/// @param uMorphDeltas Sampler uniform location for position deltas.
/// @param uHasMorphNormalDeltas Uniform location for the normal-delta enable flag.
/// @param uMorphNormalDeltas Sampler uniform location for normal deltas.
static void bind_morph_payload(gl_context_t *ctx,
                               const vgfx3d_draw_cmd_t *cmd,
                               GLint uHasSkinning,
                               GLint uInstanceBoneStride,
                               GLint uMorphShapeCount,
                               GLint uVertexCount,
                               GLint uMorphWeights,
                               GLint uPrevMorphWeights,
                               GLint uMorphDeltas,
                               GLint uHasMorphNormalDeltas,
                               GLint uMorphNormalDeltas) {
    gl_morph_cache_entry_t *cached_entry = NULL;
    GLuint morph_tbo = 0;
    GLuint morph_normal_tbo = 0;
    int use_skinning = (cmd->bone_palette && cmd->bone_count > 0) ? 1 : 0;
    int morph_count =
        (cmd->morph_deltas && cmd->morph_weights && cmd->morph_shape_count > 0)
            ? vgfx3d_opengl_clamp_morph_shape_count(cmd->vertex_count, cmd->morph_shape_count)
            : 0;

    gl.Uniform1i(uHasSkinning, use_skinning);
    /* R18 per-instance palette stride; 0 keeps the shared-palette indexing and
     * the location is -1 (silent no-op) for programs without the uniform. */
    gl.Uniform1i(uInstanceBoneStride,
                 (use_skinning && cmd->instance_bone_stride > 0) ? cmd->instance_bone_stride : 0);

    if (use_skinning)
        upload_active_bone_palettes(ctx, cmd);

    gl.ActiveTexture(GL_TEXTURE0 + GL_TU_MORPH_DELTAS);
    if (morph_count > 0) {
        size_t bytes;
        if (!gl_compute_morph_payload_bytes(cmd->vertex_count, morph_count, &bytes)) {
            morph_count = 0;
        } else if (!gl_morph_payload_fits_texture_buffer(ctx, bytes)) {
            morph_count = 0;
        } else {
            cached_entry = gl_get_cached_morph_entry(ctx, cmd, morph_count);
            if (cached_entry) {
                morph_tbo = cached_entry->morph_tbo;
                morph_normal_tbo = cached_entry->morph_normal_tbo;
            } else if (gl_upload_transient_morph_payload(ctx,
                                                         ctx->morph_buffer,
                                                         ctx->morph_tbo,
                                                         &ctx->morph_capacity_bytes,
                                                         cmd->morph_deltas,
                                                         bytes)) {
                morph_tbo = ctx->morph_tbo;
                if (cmd->morph_normal_deltas &&
                    gl_upload_transient_morph_payload(ctx,
                                                      ctx->morph_normal_buffer,
                                                      ctx->morph_normal_tbo,
                                                      &ctx->morph_normal_capacity_bytes,
                                                      cmd->morph_normal_deltas,
                                                      bytes)) {
                    morph_normal_tbo = ctx->morph_normal_tbo;
                }
            }
        }
        if (morph_tbo == 0)
            morph_count = 0;
    }
    gl.Uniform1i(uMorphShapeCount, morph_count);
    gl.Uniform1i(uVertexCount, morph_count > 0 ? (GLint)cmd->vertex_count : 0);
    if (morph_count > 0 && uMorphWeights >= 0)
        gl.Uniform1fv(uMorphWeights, morph_count, cmd->morph_weights);
    if (morph_count > 0 && uPrevMorphWeights >= 0)
        gl.Uniform1fv(uPrevMorphWeights,
                      morph_count,
                      cmd->prev_morph_weights ? cmd->prev_morph_weights : cmd->morph_weights);
    if (morph_count > 0) {
        gl.BindTexture(GL_TEXTURE_BUFFER, morph_tbo);
    } else {
        gl.BindTexture(GL_TEXTURE_BUFFER, 0);
    }
    gl.Uniform1i(uMorphDeltas, GL_TU_MORPH_DELTAS);
    gl.ActiveTexture(GL_TEXTURE0 + GL_TU_MORPH_NORMAL_DELTAS);
    if (uHasMorphNormalDeltas >= 0) {
        gl.Uniform1i(uHasMorphNormalDeltas,
                     (morph_count > 0 && cmd->morph_normal_deltas && morph_normal_tbo != 0) ? 1
                                                                                            : 0);
    }
    if (morph_count > 0 && cmd->morph_normal_deltas && uMorphNormalDeltas >= 0) {
        gl.BindTexture(GL_TEXTURE_BUFFER, morph_normal_tbo);
    } else {
        gl.BindTexture(GL_TEXTURE_BUFFER, 0);
    }
    if (uMorphNormalDeltas >= 0)
        gl.Uniform1i(uMorphNormalDeltas, GL_TU_MORPH_NORMAL_DELTAS);
    gl.ActiveTexture(GL_TEXTURE0);
}
