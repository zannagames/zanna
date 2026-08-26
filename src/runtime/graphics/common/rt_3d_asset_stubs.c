//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_3d_asset_stubs.c
/// @brief Graphics-disabled 3D model, texture, animation, terrain, and asset
/// loader stubs.
///
/// @details This split source isolates asset-family unavailable-backend entry
/// points from Canvas3D and scene code while retaining the exact public runtime
/// symbols exposed by the original monolithic stub file.
///
// File: src/runtime/graphics/common/rt_3d_asset_stubs.c
// Purpose: Graphics-disabled 3D asset, material, animation, and render-resource entry points.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/// @brief Set the env map of the material3d.
/// @param o Material3D handle; ignored because graphics support is unavailable.
/// @param cm CubeMap3D handle; ignored because graphics support is unavailable.
void rt_material3d_set_env_map(void *o, void *cm) {
    (void)o;
    (void)cm;
}

/// @brief Set the reflectivity of the material3d.
/// @param o Material3D handle; ignored because graphics support is unavailable.
/// @param r Requested reflectivity; ignored because graphics support is unavailable.
void rt_material3d_set_reflectivity(void *o, double r) {
    (void)o;
    (void)r;
}

/// @brief Stub for `Material3D.Reflectivity` — would normally return the
///        material's reflectivity coefficient (0..1).
///
/// Silent stub: returns `0.0` rather than trapping so that asset-loading
/// code paths that probe material properties before binding to the GPU
/// don't fail headlessly.
///
/// @param o Material3D handle (ignored).
///
/// @return `0.0` — non-reflective default.
double rt_material3d_get_reflectivity(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Mesh3D.New` — would normally allocate an empty mesh
///        ready to receive vertices and triangles.
///
/// Trapping stub: there's no useful empty mesh to return — callers will
/// immediately try to populate it with `AddVertex`/`AddTriangle`, all of
/// which would also be no-ops, producing silent rendering bugs.
///
/// @return Never returns normally.
void *rt_mesh3d_new(void) {
    rt_graphics_unavailable_("Mesh3D.New: graphics support not compiled in");
    return NULL;
}

/**
 * @brief Graphics-disabled stub for `Mesh3D.Simplify`.
 * @param o Mesh3D receiver (ignored).
 * @param target_triangles Requested triangle budget (ignored).
 * @return Never returns normally; graphics-unavailable handling yields `NULL`.
 */
void *rt_mesh3d_simplify(void *o, int64_t target_triangles) {
    (void)o;
    (void)target_triangles;
    RT_GRAPHICS_TRAP_RET("Mesh3D.Simplify: graphics support not compiled in", NULL);
}

/// @brief Stub for `Mesh3D.Clear` — would normally reset vertex/index
///        counts to zero without freeing backing arrays (mesh reuse).
///
/// Silent no-op stub.
///
/// @param o Mesh3D handle (ignored).
void rt_mesh3d_clear(void *o) {
    (void)o;
}

/// @brief Stub for `Mesh3D.NewBox` — would normally allocate a box mesh
///        with the given full extents along each axis.
///
/// @param sx Full extent along X (ignored).
/// @param sy Full extent along Y (ignored).
/// @param sz Full extent along Z (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_new_box(double sx, double sy, double sz) {
    (void)sx;
    (void)sy;
    (void)sz;
    rt_graphics_unavailable_("Mesh3D.NewBox: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.NewSphere` — would normally allocate a UV
///        sphere with the given radius and subdivision count.
///
/// @param r Sphere radius (ignored).
/// @param s Subdivision count along each axis (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_new_sphere(double r, int64_t s) {
    (void)r;
    (void)s;
    rt_graphics_unavailable_("Mesh3D.NewSphere: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.NewPlane` — would normally allocate an
///        XZ-plane quad with full extents `(sx, sz)` centered at the origin.
///
/// @param sx Full extent along X (ignored).
/// @param sz Full extent along Z (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_new_plane(double sx, double sz) {
    (void)sx;
    (void)sz;
    rt_graphics_unavailable_("Mesh3D.NewPlane: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.NewCylinder` — would normally allocate an
///        upright Y-axis cylinder with the given radius, height, and
///        side-segment count.
///
/// @param r Cylinder radius (ignored).
/// @param h Cylinder height (ignored).
/// @param s Side segment count around the circumference (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_new_cylinder(double r, double h, int64_t s) {
    (void)r;
    (void)h;
    (void)s;
    rt_graphics_unavailable_("Mesh3D.NewCylinder: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.FromOBJ` — would normally load a Wavefront OBJ
///        file at `p` and return a populated mesh.
///
/// @param p Filesystem path to the OBJ file (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_from_obj(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("Mesh3D.FromOBJ: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.FromSTL` — would normally load an STL file
///        (binary or ASCII auto-detected) at `p` and return a populated mesh.
///
/// @param p Filesystem path to the STL file (ignored).
///
/// @return Never returns normally.
void *rt_mesh3d_from_stl(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("Mesh3D.FromSTL: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Mesh3D.VertexCount` — would normally return the
///        number of vertices currently stored in the mesh.
///
/// Silent stub returning `0`.
///
/// @param o Mesh3D handle (ignored).
///
/// @return `0`.
int64_t rt_mesh3d_get_vertex_count(void *o) {
    (void)o;
    return 0;
}

/// @brief Graphics-disabled fallback for `Mesh3D.VertexPosition`.
/// @param o Mesh3D handle (ignored).
/// @param index Zero-based vertex index (ignored).
/// @return NULL because no mesh geometry exists without Graphics3D.
void *rt_mesh3d_get_vertex_position(void *o, int64_t index) {
    (void)o;
    (void)index;
    return NULL;
}

/// @brief Stub for `Mesh3D.TriangleCount` — would normally return the
///        number of triangles currently stored in the mesh (== `IndexCount / 3`).
///
/// Silent stub returning `0`.
///
/// @param o Mesh3D handle (ignored).
///
/// @return `0`.
int64_t rt_mesh3d_get_triangle_count(void *o) {
    (void)o;
    return 0;
}

/**
 * @brief Graphics-disabled fallback for `Mesh3D.SimplifyRequestedTriangles`.
 * @param o Mesh3D receiver (ignored).
 * @return Zero because no simplification result can exist without Graphics3D.
 */
int64_t rt_mesh3d_get_simplify_requested_triangles(void *o) {
    (void)o;
    return 0;
}

/**
 * @brief Graphics-disabled fallback for `Mesh3D.SimplifyAchievedTriangles`.
 * @param o Mesh3D receiver (ignored).
 * @return Zero because no simplification result can exist without Graphics3D.
 */
int64_t rt_mesh3d_get_simplify_achieved_triangles(void *o) {
    (void)o;
    return 0;
}

/**
 * @brief Graphics-disabled fallback for `Mesh3D.SimplifyStatus`.
 * @param o Mesh3D receiver (ignored).
 * @return Zero (`NOT_RUN`).
 */
int64_t rt_mesh3d_get_simplify_status(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Mesh3D.Resident` — resident payload state.
///
/// Silent stub returning `false`.
/// @param o Mesh3D handle (ignored).
/// @return `0` because graphics-disabled meshes have no resident payload.
int8_t rt_mesh3d_get_resident(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Mesh3D.Resident` setter.
///
/// Silent no-op stub.
/// @param o Mesh3D handle (ignored).
/// @param resident Requested resident state (ignored).
void rt_mesh3d_set_resident(void *o, int8_t resident) {
    (void)o;
    (void)resident;
}

/// @brief Stub for `Mesh3D.CompactStreams` setter.
///
/// Silent no-op stub.
/// @param o Mesh3D handle (ignored).
/// @param enabled Requested compact-stream state (ignored).
void rt_mesh3d_set_compact_streams(void *o, int8_t enabled) {
    (void)o;
    (void)enabled;
}

/// @brief Stub for `Mesh3D.CompactStreams` getter.
///
/// Silent stub returning `0`.
/// @param o Mesh3D handle (ignored).
/// @return `0` because no mesh streams exist.
int8_t rt_mesh3d_get_compact_streams(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Mesh3D.ResidentBytes`.
///
/// Silent stub returning `0`.
/// @param o Mesh3D handle (ignored).
/// @return `0` because no resident mesh storage exists.
int64_t rt_mesh3d_get_resident_bytes(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Mesh3D.RetainedBytes`.
///
/// Disabled Graphics3D builds retain no mesh payload, so the neutral value is `0`.
/// @param o Mesh3D handle (ignored).
/// @return `0` because no retained mesh storage exists.
int64_t rt_mesh3d_get_retained_bytes(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Mesh3D.AddVertex` — append a vertex with position
///        `(x, y, z)`, normal `(nx, ny, nz)`, and UV `(u, v)` to the mesh.
///        Returns the new vertex index implicitly via insertion order.
///
/// Silent no-op stub. The real implementation grows the underlying vertex
/// buffer; you almost always pair this with `AddTriangle` to reference
/// the new vertex.
///
/// @param o  Mesh3D handle (ignored).
/// @param x  Position x (ignored).
/// @param y  Position y (ignored).
/// @param z  Position z (ignored).
/// @param nx Normal x; should be normalized (ignored).
/// @param ny Normal y (ignored).
/// @param nz Normal z (ignored).
/// @param u  Texture U coordinate, typically 0..1 (ignored).
/// @param v  Texture V coordinate, typically 0..1 (ignored).
void rt_mesh3d_add_vertex(
    void *o, double x, double y, double z, double nx, double ny, double nz, double u, double v) {
    (void)o;
    (void)x;
    (void)y;
    (void)z;
    (void)nx;
    (void)ny;
    (void)nz;
    (void)u;
    (void)v;
}

/// @brief Stub for `Mesh3D.AddTriangle` — append a triangle to the
///        mesh's index buffer using three previously-added vertex indices.
///
/// Silent no-op stub. Winding order matters for backface culling: with
/// the default cull mode, triangles wound counter-clockwise (when viewed
/// from outside) are visible.
///
/// @param o  Mesh3D handle (ignored).
/// @param v0 First vertex index, 0..VertexCount-1 (ignored).
/// @param v1 Second vertex index (ignored).
/// @param v2 Third vertex index (ignored).
void rt_mesh3d_add_triangle(void *o, int64_t v0, int64_t v1, int64_t v2) {
    (void)o;
    (void)v0;
    (void)v1;
    (void)v2;
}

/// @brief Recalc the normals of the mesh3d.
/// @param o Mesh3D handle; ignored because graphics support is unavailable.
void rt_mesh3d_recalc_normals(void *o) {
    (void)o;
}

/// @brief Stub for `Mesh3D.Clone` — would normally allocate a fresh
///        mesh holding deep copies of the source's vertex and index
///        buffers (no aliasing back to the source). Use when you want
///        to mutate a mesh per-instance without affecting the original.
///
/// Silent stub returning NULL.
///
/// @param o Source Mesh3D handle (ignored).
///
/// @return `NULL`.
void *rt_mesh3d_clone(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Mesh3D.Transform` — apply a Mat4 transformation to
///        every vertex in the mesh in place. Positions are transformed
///        directly; normals are transformed by the inverse-transpose of
///        the upper 3x3 to remain perpendicular under non-uniform scale.
///
/// Silent no-op stub.
///
/// @param o Mesh3D handle (ignored).
/// @param m Mat4 transformation handle (ignored).
void rt_mesh3d_transform(void *o, void *m) {
    (void)o;
    (void)m;
}

/// @brief Stub for `Mesh3D.BendArc` — would wrap the mesh's X extent around a
///        vertical circular arc (ADR 0294).
///
/// Silent no-op stub.
///
/// @param o Mesh3D handle (ignored).
/// @param radius Bend radius (ignored).
/// @param arc_degrees Arc span in degrees (ignored).
void rt_mesh3d_bend_arc(void *o, double radius, double arc_degrees) {
    (void)o;
    (void)radius;
    (void)arc_degrees;
}

/// @brief Stub for `Material3D.New` — would normally create a default
///        Blinn-Phong material (white diffuse, no textures, shininess 32).
///        Use `NewColor`, `NewTextured`, or `NewPBR` for shorthand
///        constructors.
///
/// Trapping stub.
///
/// @return Never returns normally.
void *rt_material3d_new(void) {
    rt_graphics_unavailable_("Material3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.NewColor` — would normally create a
///        Blinn-Phong material with the given solid base color and no
///        texture maps.
///
/// @param r Base color red, 0..1 (ignored).
/// @param g Base color green, 0..1 (ignored).
/// @param b Base color blue, 0..1 (ignored).
///
/// @return Never returns normally.
void *rt_material3d_new_color(double r, double g, double b) {
    (void)r;
    (void)g;
    (void)b;
    rt_graphics_unavailable_("Material3D.NewColor: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.NewTextured` — would normally create a
///        Blinn-Phong material with the given diffuse texture (slot 0)
///        and white tint.
///
/// @param p Pixels handle for the diffuse texture (ignored).
///
/// @return Never returns normally.
void *rt_material3d_new_textured(void *p) {
    (void)p;
    rt_graphics_unavailable_("Material3D.NewTextured: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.NewPBR` — would normally create a PBR
///        material with metallic-roughness workflow. `(r, g, b)` are
///        traditionally interpreted as `(metallic, roughness, ao)` in the
///        runtime; pair with `SetAlbedoMap` for the base color.
///
/// @param r Metallic, 0..1 (ignored).
/// @param g Roughness, 0..1 (ignored).
/// @param b Ambient occlusion, 0..1 (ignored).
///
/// @return Never returns normally.
void *rt_material3d_new_pbr(double r, double g, double b) {
    (void)r;
    (void)g;
    (void)b;
    rt_graphics_unavailable_("Material3D.NewPBR: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.Clone` — would normally return a deep
///        copy of the material with its own scalar parameters and
///        independent texture references (incremented refcounts on the
///        bound textures).
///
/// Trapping stub.
///
/// @param o Source Material3D handle (ignored).
///
/// @return Never returns normally.
void *rt_material3d_clone(void *o) {
    (void)o;
    rt_graphics_unavailable_("Material3D.Clone: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.MakeInstance` — would normally produce a
///        per-instance override material that shares the base material's
///        textures but allows independent scalar parameters.
///
/// Trapping stub: there is no base material to clone in the headless build.
///
/// @param o Source Material3D handle (ignored).
///
/// @return Never returns normally.
void *rt_material3d_make_instance(void *o) {
    (void)o;
    rt_graphics_unavailable_("Material3D.MakeInstance: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Material3D.SetColor` — would normally set the
///        material's diffuse / base color.
///
/// Silent no-op stub. Components are linear-space 0..1 floats.
///
/// @param o Material3D handle (ignored).
/// @param r Red, 0..1 (ignored).
/// @param g Green, 0..1 (ignored).
/// @param b Blue, 0..1 (ignored).
void rt_material3d_set_color(void *o, double r, double g, double b) {
    (void)o;
    (void)r;
    (void)g;
    (void)b;
}

/// @brief Return no material color object in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_color(void *o) {
    (void)o;
    return NULL;
}

/// @brief Graphics-disabled material map inspection stubs.
/// @details No decoded graphics resources exist in this build, so every read-only map-pixel
///          property returns NULL without trapping.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_texture_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded normal-map Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_normal_map_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded specular-map Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_specular_map_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded emissive-map Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_emissive_map_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded metallic-roughness-map Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_metallic_roughness_map_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded ambient-occlusion-map Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_ao_map_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no decoded lightmap Pixels in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always NULL.
void *rt_material3d_get_lightmap_pixels(void *o) {
    (void)o;
    return NULL;
}

/// @brief Stub for `Material3D.SetTexture` — would normally bind a Pixels
///        surface as the diffuse texture (slot 0).
///
/// Silent no-op stub. Equivalent to `SetAlbedoMap` for non-PBR materials.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the texture, or NULL to clear (ignored).
void rt_material3d_set_texture(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.SetAlbedoMap` — PBR-workflow alias for
///        `SetTexture`. Binds the base-color texture (slot 0).
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the albedo map, or NULL (ignored).
void rt_material3d_set_albedo_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.SetShininess` — Blinn-Phong specular
///        exponent. Higher values produce a tighter specular highlight.
///
/// Silent no-op stub. Typical range 1..200.
///
/// @param o Material3D handle (ignored).
/// @param s Specular exponent (ignored).
void rt_material3d_set_shininess(void *o, double s) {
    (void)o;
    (void)s;
}

/// @brief Stub for `Material3D.SetAlpha` — sets the per-material alpha
///        multiplier applied during blending.
///
/// Silent no-op stub. Multiplied with per-vertex/texture alpha.
///
/// @param o Material3D handle (ignored).
/// @param a Alpha multiplier, 0..1 (ignored).
void rt_material3d_set_alpha(void *o, double a) {
    (void)o;
    (void)a;
}

/// @brief Stub for `Material3D.Alpha` — get the per-material alpha.
///
/// Silent stub returning `1.0` (fully opaque) so blend-mode probes don't
/// see a misleading transparency value.
///
/// @param o Material3D handle (ignored).
///
/// @return `1.0`.
double rt_material3d_get_alpha(void *o) {
    (void)o;
    return 1.0;
}

/// @brief Stub for `Material3D.SetMetallic` — PBR metallic coefficient.
///        0 = dielectric (insulator), 1 = pure metal.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param v Metallic value, 0..1 (ignored).
void rt_material3d_set_metallic(void *o, double v) {
    (void)o;
    (void)v;
}

/// @brief Stub for `Material3D.Metallic` — get the PBR metallic value.
///
/// Silent stub returning `0.0` (non-metal).
///
/// @param o Material3D handle (ignored).
///
/// @return `0.0`.
double rt_material3d_get_metallic(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Material3D.SetRoughness` — PBR microfacet roughness.
///        0 = mirror-smooth, 1 = fully rough/diffuse.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param v Roughness, 0..1 (ignored).
void rt_material3d_set_roughness(void *o, double v) {
    (void)o;
    (void)v;
}

/// @brief Stub for `Material3D.Roughness` — get the PBR roughness value.
///
/// Silent stub returning `0.5` (the PBR shader default).
///
/// @param o Material3D handle (ignored).
///
/// @return `0.5`.
double rt_material3d_get_roughness(void *o) {
    (void)o;
    return 0.5;
}

/// @brief Stub for `Material3D.SetAO` — PBR ambient-occlusion multiplier.
///        Modulates the ambient/indirect lighting term.
///
/// Silent no-op stub. Pair with `SetAOMap` to vary AO across the surface.
///
/// @param o Material3D handle (ignored).
/// @param v AO value, 0..1 (ignored).
void rt_material3d_set_ao(void *o, double v) {
    (void)o;
    (void)v;
}

/// @brief Stub for `Material3D.AmbientOcclusion` — get the PBR ambient-occlusion value.
///
/// Silent stub returning `1.0` (no occlusion).
///
/// @param o Material3D handle (ignored).
///
/// @return `1.0`.
double rt_material3d_get_ao(void *o) {
    (void)o;
    return 1.0;
}

/// @brief Stub for `Material3D.SetEmissiveIntensity` — multiplier on the
///        emissive map / color contribution.
///
/// Silent no-op stub. Values > 1 are valid for HDR / bloom workflows.
///
/// @param o Material3D handle (ignored).
/// @param v Emissive intensity (ignored).
void rt_material3d_set_emissive_intensity(void *o, double v) {
    (void)o;
    (void)v;
}

/// @brief Stub for `Material3D.EmissiveIntensity` — get the emissive
///        multiplier.
///
/// Silent stub returning `1.0`.
///
/// @param o Material3D handle (ignored).
///
/// @return `1.0`.
double rt_material3d_get_emissive_intensity(void *o) {
    (void)o;
    return 1.0;
}

/// @brief Stub for `Material3D.SetUnlit` — when enabled, the material
///        skips lighting entirely (treat the diffuse color/texture as
///        already-shaded final pixel color). Used for HUD elements and
///        flat-shaded sprites in 3D space.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param u Non-zero to enable unlit shading (ignored).
void rt_material3d_set_unlit(void *o, int8_t u) {
    (void)o;
    (void)u;
}

/// @brief Return the neutral disabled value for `Material3D.Unlit`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_unlit(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetShadingModel` — selects the per-material
///        fragment shading path: 0=BlinnPhong (default), 1=Toon, 4=Fresnel,
///        5=Emissive.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param m Shading model index (ignored).
void rt_material3d_set_shading_model(void *o, int64_t m) {
    (void)o;
    (void)m;
}

/// @brief Return the default shading-model identifier in a graphics-disabled build.
/// @param o Material3D handle (ignored).
/// @return Always `0`, the default Blinn-Phong model.
int64_t rt_material3d_get_shading_model(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetCustomParam` — write to one of the 12
///        per-material float parameter slots used by the active shading
///        model (e.g. Toon band count, Fresnel power/bias).
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param i Parameter slot index, 0..11 (ignored).
/// @param v Parameter value (ignored).
void rt_material3d_set_custom_param(void *o, int64_t i, double v) {
    (void)o;
    (void)i;
    (void)v;
}

/// @brief Stub for `Material3D.SetNormalMap` — bind a tangent-space normal
///        map (slot 1).
///
/// Silent no-op stub. Real implementation does TBN perturbation with
/// Gram-Schmidt orthonormalization.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the normal map, or NULL (ignored).
void rt_material3d_set_normal_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.HasTexture` — whether the base-color/albedo slot is bound.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_texture(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.HasNormalMap` — whether the normal-map slot is bound.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_normal_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetMetallicRoughnessMap` — bind a packed
///        PBR map where R = metallic, G = roughness (slots 4/5 in the
///        unified PBR shader).
///
/// Silent no-op stub. Matches the glTF 2.0 metallic-roughness workflow.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the packed map, or NULL (ignored).
void rt_material3d_set_metallic_roughness_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.HasMetallicRoughnessMap`.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_metallic_roughness_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetAOMap` — bind an ambient-occlusion
///        map. Modulates the ambient lighting term per-fragment.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the AO map, or NULL (ignored).
void rt_material3d_set_ao_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Silent stub for `Material3D.SetLightmap` — no-op.
/// @param obj Material3D handle (ignored).
/// @param pixels Pixels lightmap handle (ignored).
void rt_material3d_set_lightmap(void *obj, void *pixels) {
    (void)obj;
    (void)pixels;
}

/// @brief Silent stub for `Material3D.get_HasLightmap` — no-op; returns 0.
/// @param obj Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_lightmap(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Stub for `Material3D.HasAmbientOcclusionMap`.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_ao_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetSpecularMap` — bind a per-texel
///        specular intensity map (slot 2).
///
/// Silent no-op stub. Used by the Blinn-Phong shading path; PBR workflows
/// derive specular response from the metallic-roughness pair instead.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the specular map, or NULL (ignored).
void rt_material3d_set_specular_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.HasSpecularMap`.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_specular_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetEmissiveMap` — bind a per-texel
///        emissive color map (slot 3). Sampled and added on top of the lit
///        result.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param p Pixels handle for the emissive map, or NULL (ignored).
void rt_material3d_set_emissive_map(void *o, void *p) {
    (void)o;
    (void)p;
}

/// @brief Stub for `Material3D.HasEmissiveMap`.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_emissive_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.HasEnvMap`.
///
/// Silent stub returning `0`.
/// @param o Material3D handle (ignored).
/// @return Always `0`.
int8_t rt_material3d_get_has_env_map(void *o) {
    (void)o;
    return 0;
}

/// @brief Set the emissive color of the material3d.
/// @param o Material3D handle (ignored).
/// @param r Requested red component (ignored).
/// @param g Requested green component (ignored).
/// @param b Requested blue component (ignored).
void rt_material3d_set_emissive_color(void *o, double r, double g, double b) {
    (void)o;
    (void)r;
    (void)g;
    (void)b;
}

/// @brief Stub for `Material3D.SetNormalScale` — strength multiplier on
///        the bound normal map's perturbation. `1.0` is full effect; `0.0`
///        flattens the normal map back to the geometric normal; values
///        > 1 over-exaggerate.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param v Normal map intensity multiplier (ignored).
void rt_material3d_set_normal_scale(void *o, double v) {
    (void)o;
    (void)v;
}

/// @brief Stub for `Material3D.NormalScale` — get the normal map
///        intensity multiplier.
///
/// Silent stub returning `1.0` (full effect — the renderer's default).
///
/// @param o Material3D handle (ignored).
///
/// @return `1.0`.
double rt_material3d_get_normal_scale(void *o) {
    (void)o;
    return 1.0;
}

/// @brief Stub for `Material3D.SetAlphaMode` — choose how the material
///        composites: 0=Opaque (alpha ignored), 1=Mask (alpha-test
///        cutout, no blending), 2=Blend (full alpha blending). Mask is
///        cheaper than Blend but produces hard edges.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param m Alpha mode 0..2 (ignored).
void rt_material3d_set_alpha_mode(void *o, int64_t m) {
    (void)o;
    (void)m;
}

/// @brief Stub for `Material3D.AlphaMode` — get the material's
///        transparency mode: 0=Opaque, 1=Mask (alpha-tested), 2=Blend
///        (alpha-blended).
///
/// Silent stub returning Opaque (the renderer-friendly default).
///
/// @param o Material3D handle (ignored).
///
/// @return `RT_MATERIAL3D_ALPHA_MODE_OPAQUE`.
int64_t rt_material3d_get_alpha_mode(void *o) {
    (void)o;
    return RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
}

/// @brief Stub for `Material3D.ShadowMode` — choose whether the material casts shadows.
///
/// Silent no-op stub. The full renderer accepts 0=Auto, 1=None, 2=Cast.
///
/// @param o Material3D handle (ignored).
/// @param m Shadow mode 0..2 (ignored).
void rt_material3d_set_shadow_mode(void *o, int64_t m) {
    (void)o;
    (void)m;
}

/// @brief Stub for `Material3D.ShadowMode` getter.
///
/// Silent stub returning Auto, matching the full renderer default.
///
/// @param o Material3D handle (ignored).
///
/// @return `RT_MATERIAL3D_SHADOW_MODE_AUTO`.
int64_t rt_material3d_get_shadow_mode(void *o) {
    (void)o;
    return RT_MATERIAL3D_SHADOW_MODE_AUTO;
}

/// @brief Stub for `Material3D.SetDoubleSided` — when enabled, both faces
///        of each triangle are rendered (no backface culling for this
///        material).
///
/// Silent no-op stub. Used for foliage and thin geometry.
///
/// @param o Material3D handle (ignored).
/// @param e Non-zero to enable double-sided rendering (ignored).
void rt_material3d_set_double_sided(void *o, int8_t e) {
    (void)o;
    (void)e;
}

/// @brief Stub for `Material3D.DoubleSided` — get the double-sided flag.
///
/// Silent stub returning `0` (single-sided / culled).
///
/// @param o Material3D handle (ignored).
///
/// @return `0`.
int8_t rt_material3d_get_double_sided(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Material3D.SetDepthBias` — would normally configure constant and
///        slope-scaled depth offsets for coplanar/overlay geometry.
///
/// Silent no-op stub.
///
/// @param o Material3D handle (ignored).
/// @param c Constant depth offset (ignored).
/// @param s Slope-scaled depth offset (ignored).
void rt_material3d_set_depth_bias(void *o, double c, double s) {
    (void)o;
    (void)c;
    (void)s;
}

/// @brief Stub for `Mesh3D.CalcTangents` — would normally compute and
///        store per-vertex tangents from positions, normals, and UVs (used
///        by normal-map shading).
///
/// Silent no-op stub.
///
/// @param o Mesh3D handle (ignored).
void rt_mesh3d_calc_tangents(void *o) {
    (void)o;
}

/// @brief Stub for `Light3D.NewDirectional` — would normally create a
///        directional (sun-like) light with the given direction and RGB
///        color.
///
/// Trapping stub: lights are referenced by Canvas3D draws, so a NULL
/// return would crash later when bound.
///
/// @param d Vec3 direction handle (must be normalized) (ignored).
/// @param r Light color red, 0..1 (ignored).
/// @param g Light color green, 0..1 (ignored).
/// @param b Light color blue, 0..1 (ignored).
///
/// @return Never returns normally.
void *rt_light3d_new_directional(void *d, double r, double g, double b) {
    (void)d;
    (void)r;
    (void)g;
    (void)b;
    rt_graphics_unavailable_("Light3D.NewDirectional: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Light3D.NewPoint` — would normally create a point
///        light with the given position, RGB color, and falloff radius.
///
/// @param p Vec3 position handle (ignored).
/// @param r Light color red, 0..1 (ignored).
/// @param g Light color green, 0..1 (ignored).
/// @param b Light color blue, 0..1 (ignored).
/// @param a Attenuation/falloff radius in world units (ignored).
///
/// @return Never returns normally.
void *rt_light3d_new_point(void *p, double r, double g, double b, double a) {
    (void)p;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    rt_graphics_unavailable_("Light3D.NewPoint: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Light3D.NewAmbient` — would normally create a global
///        ambient light contribution with the given RGB color.
///
/// In the real shader the ambient term applies uniformly regardless of
/// surface normal.
///
/// @param r Ambient red, 0..1 (ignored).
/// @param g Ambient green, 0..1 (ignored).
/// @param b Ambient blue, 0..1 (ignored).
///
/// @return Never returns normally.
void *rt_light3d_new_ambient(double r, double g, double b) {
    (void)r;
    (void)g;
    (void)b;
    rt_graphics_unavailable_("Light3D.NewAmbient: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Light3D.NewSpot` — would normally create a cone-
///        attenuated spotlight at the given position pointing in the given
///        direction with smoothstep falloff between inner and outer angles.
///
/// @param p Vec3 position handle (ignored).
/// @param d Vec3 direction handle (must be normalized) (ignored).
/// @param r Light color red, 0..1 (ignored).
/// @param g Light color green, 0..1 (ignored).
/// @param b Light color blue, 0..1 (ignored).
/// @param a Attenuation/falloff radius in world units (ignored).
/// @param i Inner cone half-angle in radians (full intensity inside) (ignored).
/// @param o Outer cone half-angle in radians (zero intensity beyond) (ignored).
///
/// @return Never returns normally.
void *rt_light3d_new_spot(
    void *p, void *d, double r, double g, double b, double a, double i, double o) {
    (void)p;
    (void)d;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    (void)i;
    (void)o;
    rt_graphics_unavailable_("Light3D.NewSpot: graphics support not compiled in");
    return NULL;
}

/// @brief Trap because rectangular area lights require unavailable Graphics3D support.
/// @param position Vec3 light position (ignored).
/// @param direction Vec3 emission direction (ignored).
/// @param width Rectangle width (ignored).
/// @param height Rectangle height (ignored).
/// @param r Red color component (ignored).
/// @param g Green color component (ignored).
/// @param b Blue color component (ignored).
/// @param attenuation Distance attenuation (ignored).
/// @param range Light range (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_light3d_new_area_rectangle(void *position,
                                    void *direction,
                                    double width,
                                    double height,
                                    double r,
                                    double g,
                                    double b,
                                    double attenuation,
                                    double range) {
    (void)position;
    (void)direction;
    (void)width;
    (void)height;
    (void)r;
    (void)g;
    (void)b;
    (void)attenuation;
    (void)range;
    rt_graphics_unavailable_("Light3D.AreaRectangle: graphics support not compiled in");
    return NULL;
}

/// @brief Trap because spherical area lights require unavailable Graphics3D support.
/// @param position Vec3 light position (ignored).
/// @param radius Sphere radius (ignored).
/// @param r Red color component (ignored).
/// @param g Green color component (ignored).
/// @param b Blue color component (ignored).
/// @param range Light range (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_light3d_new_area_sphere(
    void *position, double radius, double r, double g, double b, double range) {
    (void)position;
    (void)radius;
    (void)r;
    (void)g;
    (void)b;
    (void)range;
    rt_graphics_unavailable_("Light3D.AreaSphere: graphics support not compiled in");
    return NULL;
}

/// @brief Trap because volume lights require unavailable Graphics3D support.
/// @param position Vec3 volume center (ignored).
/// @param radius Volume radius (ignored).
/// @param r Red color component (ignored).
/// @param g Green color component (ignored).
/// @param b Blue color component (ignored).
/// @param range Light range (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_light3d_new_volume(
    void *position, double radius, double r, double g, double b, double range) {
    (void)position;
    (void)radius;
    (void)r;
    (void)g;
    (void)b;
    (void)range;
    rt_graphics_unavailable_("Light3D.Volume: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Light3D.SetIntensity` — multiplier applied to the
///        light's RGB contribution (HDR-friendly, may exceed 1.0).
///
/// Silent no-op stub.
///
/// @param o Light3D handle (ignored).
/// @param i Intensity multiplier (ignored).
void rt_light3d_set_intensity(void *o, double i) {
    (void)o;
    (void)i;
}

/// @brief Stub for `Light3D.SetAttenuation` — set a local light's distance falloff.
///
/// Silent no-op stub.
///
/// @param o Light3D handle (ignored).
/// @param a Attenuation factor (ignored).
void rt_light3d_set_attenuation(void *o, double a) {
    (void)o;
    (void)a;
}

/// @brief Stub for `Light3D.get_Attenuation` — read a light's distance falloff.
///
/// Silent no-op stub.
///
/// @param o Light3D handle (ignored).
/// @return Always 0.0.
double rt_light3d_get_attenuation(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Stub for `Light3D.SetColor` — overwrite the light's RGB color.
///
/// Silent no-op stub. Components are linear-space 0..1 floats.
///
/// @param o Light3D handle (ignored).
/// @param r Red, 0..1 (ignored).
/// @param g Green, 0..1 (ignored).
/// @param b Blue, 0..1 (ignored).
void rt_light3d_set_color(void *o, double r, double g, double b) {
    (void)o;
    (void)r;
    (void)g;
    (void)b;
}

/// @brief Return the neutral light-type identifier in a graphics-disabled build.
/// @param o Light3D handle (ignored).
/// @return Always `0`.
int64_t rt_light3d_get_type(void *o) {
    (void)o;
    return 0;
}

/// @brief Return no Light3D color object in a graphics-disabled build.
/// @param o Light3D handle (ignored).
/// @return Always NULL.
void *rt_light3d_get_color(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return the neutral Light3D intensity.
/// @param o Light3D handle (ignored).
/// @return Always `0.0`.
double rt_light3d_get_intensity(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Ignore a Light3D enabled-state update.
/// @param o Light3D handle (ignored).
/// @param enabled Requested enabled state (ignored).
void rt_light3d_set_enabled(void *o, int8_t enabled) {
    (void)o;
    (void)enabled;
}

/// @brief Return the disabled Light3D fallback state.
/// @param o Light3D handle (ignored).
/// @return Always `0`.
int8_t rt_light3d_get_enabled(void *o) {
    (void)o;
    return 0;
}

/// @brief Ignore a Light3D shadow-casting update.
/// @param o Light3D handle (ignored).
/// @param enabled Requested shadow state (ignored).
void rt_light3d_set_casts_shadows(void *o, int8_t enabled) {
    (void)o;
    (void)enabled;
}

/// @brief Return the disabled shadow-casting fallback state.
/// @param o Light3D handle (ignored).
/// @return Always `0`.
int8_t rt_light3d_get_casts_shadows(void *o) {
    (void)o;
    return 0;
}

/// @brief Return no Light3D direction object.
/// @param o Light3D handle (ignored).
/// @return Always NULL.
void *rt_light3d_get_direction(void *o) {
    (void)o;
    return NULL;
}

/// @brief Return no Light3D position object.
/// @param o Light3D handle (ignored).
/// @return Always NULL.
void *rt_light3d_get_position(void *o) {
    (void)o;
    return NULL;
}

/// @brief Ignore a Light3D position update.
/// @param o Light3D handle (ignored).
/// @param position Vec3 position (ignored).
void rt_light3d_set_position(void *o, void *position) {
    (void)o;
    (void)position;
}

/// @brief Ignore a Light3D direction update.
/// @param o Light3D handle (ignored).
/// @param direction Vec3 direction (ignored).
void rt_light3d_set_direction(void *o, void *direction) {
    (void)o;
    (void)direction;
}

/// @brief Return the neutral rectangular-light width.
/// @param o Light3D handle (ignored).
/// @return Always `0.0`.
double rt_light3d_get_width(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Ignore a rectangular-light width update.
/// @param o Light3D handle (ignored).
/// @param width Requested width (ignored).
void rt_light3d_set_width(void *o, double width) {
    (void)o;
    (void)width;
}

/// @brief Return the neutral rectangular-light height.
/// @param o Light3D handle (ignored).
/// @return Always `0.0`.
double rt_light3d_get_height(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Ignore a rectangular-light height update.
/// @param o Light3D handle (ignored).
/// @param height Requested height (ignored).
void rt_light3d_set_height(void *o, double height) {
    (void)o;
    (void)height;
}

/// @brief Return the neutral area/volume-light radius.
/// @param o Light3D handle (ignored).
/// @return Always `0.0`.
double rt_light3d_get_radius(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Ignore an area/volume-light radius update.
/// @param o Light3D handle (ignored).
/// @param radius Requested radius (ignored).
void rt_light3d_set_radius(void *o, double radius) {
    (void)o;
    (void)radius;
}

/// @brief Return the default light decay-type identifier.
/// @param o Light3D handle (ignored).
/// @return Always `0`.
int64_t rt_light3d_get_decay_type(void *o) {
    (void)o;
    return 0;
}

/// @brief Ignore a light decay-type update.
/// @param o Light3D handle (ignored).
/// @param decay_type Requested decay identifier (ignored).
void rt_light3d_set_decay_type(void *o, int64_t decay_type) {
    (void)o;
    (void)decay_type;
}

/// @brief Return the neutral light range.
/// @param o Light3D handle (ignored).
/// @return Always `0.0`.
double rt_light3d_get_range(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Ignore a light range update.
/// @param o Light3D handle (ignored).
/// @param range Requested range (ignored).
void rt_light3d_set_range(void *o, double range) {
    (void)o;
    (void)range;
}

/// @brief Stub for `NodeAnimation3D.New`; graphics support is unavailable.
/// @param name Animation name (ignored).
/// @param duration Animation duration (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_node_animation3d_new(rt_string name, double duration) {
    (void)name;
    (void)duration;
    rt_graphics_unavailable_("NodeAnimation3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `NodeAnimation3D.Name`.
/// @param animation NodeAnimation3D handle (ignored).
/// @return Empty runtime string.
rt_string rt_node_animation3d_get_name(void *animation) {
    (void)animation;
    return rt_const_cstr("");
}

/// @brief Stub for `NodeAnimation3D.Duration`.
/// @param animation NodeAnimation3D handle (ignored).
/// @return Always `0.0`.
double rt_node_animation3d_get_duration(void *animation) {
    (void)animation;
    return 0.0;
}

/// @brief Stub for `NodeAnimation3D.ChannelCount`.
/// @param animation NodeAnimation3D handle (ignored).
/// @return Always `0`.
int64_t rt_node_animation3d_get_channel_count(void *animation) {
    (void)animation;
    return 0;
}

/// @brief Stub for `NodeAnimator3D.New`; graphics support is unavailable.
/// @param clip Initial NodeAnimation3D clip (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_node_animator3d_new(void *clip) {
    (void)clip;
    rt_graphics_unavailable_("NodeAnimator3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `NodeAnimator3D.ClipCount`.
/// @param animator NodeAnimator3D handle (ignored).
/// @return Always `0`.
int64_t rt_node_animator3d_get_clip_count(void *animator) {
    (void)animator;
    return 0;
}

/// @brief Stub for `NodeAnimator3D.GetClip`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param index Clip index (ignored).
/// @return Always NULL.
void *rt_node_animator3d_get_clip(void *animator, int64_t index) {
    (void)animator;
    (void)index;
    return NULL;
}

/// @brief Stub for `NodeAnimator3D.GetClipName`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param index Clip index (ignored).
/// @return Empty runtime string.
rt_string rt_node_animator3d_get_clip_name(void *animator, int64_t index) {
    (void)animator;
    (void)index;
    return rt_const_cstr("");
}

/// @brief Stub for `NodeAnimator3D.CurrentClip`.
/// @param animator NodeAnimator3D handle (ignored).
/// @return Empty runtime string.
rt_string rt_node_animator3d_get_current_clip(void *animator) {
    (void)animator;
    return rt_const_cstr("");
}

/// @brief Stub for `NodeAnimator3D.Playing`.
/// @param animator NodeAnimator3D handle (ignored).
/// @return Always `0`.
int8_t rt_node_animator3d_get_playing(void *animator) {
    (void)animator;
    return 0;
}

/// @brief Stub for `NodeAnimator3D.Speed`.
/// @param animator NodeAnimator3D handle (ignored).
/// @return Always `0.0`.
double rt_node_animator3d_get_speed(void *animator) {
    (void)animator;
    return 0.0;
}

/// @brief Stub for `NodeAnimator3D.Time`.
/// @param animator NodeAnimator3D handle (ignored).
/// @return Always `0.0`.
double rt_node_animator3d_get_time(void *animator) {
    (void)animator;
    return 0.0;
}

/// @brief Stub for `NodeAnimator3D.Play`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param name Clip name (ignored).
/// @return Always `0`.
int8_t rt_node_animator3d_play(void *animator, rt_string name) {
    (void)animator;
    (void)name;
    return 0;
}

/// @brief Stub for `NodeAnimator3D.Stop`.
/// @param animator NodeAnimator3D handle (ignored).
void rt_node_animator3d_stop(void *animator) {
    (void)animator;
}

/// @brief Stub for `NodeAnimator3D.SetSpeed`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param speed Requested playback speed (ignored).
void rt_node_animator3d_set_speed(void *animator, double speed) {
    (void)animator;
    (void)speed;
}

/// @brief Stub for `NodeAnimator3D.SetTime`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param time Requested playback time (ignored).
void rt_node_animator3d_set_time(void *animator, double time) {
    (void)animator;
    (void)time;
}

/// @brief Stub for `NodeAnimator3D.Update`.
/// @param animator NodeAnimator3D handle (ignored).
/// @param dt Elapsed time (ignored).
void rt_node_animator3d_update(void *animator, double dt) {
    (void)animator;
    (void)dt;
}

/* Skeleton3D / Animation3D / AnimPlayer3D stubs */

/// @brief Stub for `Skeleton3D.New` — would normally allocate an empty
///        skeleton ready to receive bones via `AddBone`.
///
/// Trapping stub: callers expect a usable handle for bone-add calls.
///
/// @return Never returns normally.
void *rt_skeleton3d_new(void) {
    rt_graphics_unavailable_("Skeleton3D.New: graphics support not compiled in");
    return NULL;
}

void *rt_skeleton3d_clone_mutable(void *skeleton) {
    (void)skeleton;
    return NULL;
}

/// @brief Stub for `Skeleton3D.AddBone` — append a bone with name `n`,
///        parent index `p` (`-1` for root), and bind-pose transform
///        matrix `m`. Returns the assigned bone index, or `-1` on failure.
///
/// Silent stub returning `-1`.
///
/// @param s Skeleton3D handle (ignored).
/// @param n Bone name (ignored).
/// @param p Parent bone index, or `-1` for root (ignored).
/// @param m Mat4 bind-pose transform handle (ignored).
///
/// @return `-1`.
int64_t rt_skeleton3d_add_bone(void *s, rt_string n, int64_t p, void *m) {
    (void)s;
    (void)n;
    (void)p;
    (void)m;
    return -1;
}

/// @brief Stub for `Skeleton3D.ComputeInverseBind` — precompute and
///        cache the per-bone inverse-bind-pose matrices used by skinning.
///        Must be called once after all bones are added; before this the
///        skeleton cannot be used for rendering.
///
/// Silent no-op stub.
///
/// @param s Skeleton3D handle (ignored).
void rt_skeleton3d_compute_inverse_bind(void *s) {
    (void)s;
}

/// @brief Stub for `Skeleton3D.BoneCount` — number of bones currently in
///        the skeleton.
///
/// Silent stub returning `0`.
///
/// @param s Skeleton3D handle (ignored).
///
/// @return `0`.
int64_t rt_skeleton3d_get_bone_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `Skeleton3D.FindBone` — get the index of the bone
///        with the given name, or `-1` if not found. O(n) search.
///
/// Silent stub returning `-1`.
///
/// @param s Skeleton3D handle (ignored).
/// @param n Bone name to search for (ignored).
///
/// @return `-1`.
int64_t rt_skeleton3d_find_bone(void *s, rt_string n) {
    (void)s;
    (void)n;
    return -1;
}

/// @brief Stub for `Skeleton3D.FindBoneOption` — named bone lookup as Option.
/// @details Graphics-disabled builds have no skeleton data, so this returns
///          `None` instead of the legacy `-1` sentinel.
/// @param s Skeleton3D handle (ignored).
/// @param n Bone name to search for (ignored).
/// @return `None`.
void *rt_skeleton3d_find_bone_option(void *s, rt_string n) {
    (void)s;
    (void)n;
    return rt_option_none();
}

/// @brief Stub for `Skeleton3D.BoneName(i)` — get the name of the `i`th
///        bone (as set during `AddBone`).
///
/// Silent stub returning NULL.
///
/// @param s Skeleton3D handle (ignored).
/// @param i Bone index (ignored).
///
/// @return `NULL`.
rt_string rt_skeleton3d_get_bone_name(void *s, int64_t i) {
    (void)s;
    (void)i;
    return NULL;
}

/// @brief Stub for `Skeleton3D.BoneBindPose(i)` — get the `i`th bone's
///        bind-pose transform as a Mat4 handle.
///
/// Silent stub returning NULL.
///
/// @param s Skeleton3D handle (ignored).
/// @param i Bone index (ignored).
///
/// @return `NULL`.
void *rt_skeleton3d_get_bone_bind_pose(void *s, int64_t i) {
    (void)s;
    (void)i;
    return NULL;
}

/// @brief Stub for `Animation3D.New` — would normally allocate an empty
///        animation track with the given name and duration. Keyframes are
///        added afterward via `AddKeyframe`.
///
/// Trapping stub: callers expect a usable handle for the keyframe pipeline.
///
/// @param n Animation name (ignored).
/// @param d Duration in seconds (ignored).
///
/// @return Never returns normally.
void *rt_animation3d_new(rt_string n, double d) {
    (void)n;
    (void)d;
    rt_graphics_unavailable_("Animation3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Animation3D.AddKeyframe` — would normally append a
///        TRS keyframe to the animation track for the given bone.
///
/// Silent no-op stub. Each keyframe captures position, rotation, and scale
/// at a particular time; the player interpolates between adjacent frames.
///
/// @param a Animation3D handle (ignored).
/// @param b Bone index this keyframe applies to (ignored).
/// @param t Time stamp in seconds (ignored).
/// @param p Vec3 position handle (ignored).
/// @param r Quaternion rotation handle (ignored).
/// @param s Vec3 scale handle (ignored).
void rt_animation3d_add_keyframe(void *a, int64_t b, double t, void *p, void *r, void *s) {
    (void)a;
    (void)b;
    (void)t;
    (void)p;
    (void)r;
    (void)s;
}

/// @brief Stub for `Animation3D.SetLooping` — when enabled, the animation
///        wraps around to the start after reaching its duration.
///
/// Silent no-op stub.
///
/// @param a Animation3D handle (ignored).
/// @param l Non-zero to enable looping (ignored).
void rt_animation3d_set_looping(void *a, int8_t l) {
    (void)a;
    (void)l;
}

/// @brief Stub for `Animation3D.Looping` — get the looping flag.
///
/// Silent stub returning `0` (one-shot).
///
/// @param a Animation3D handle (ignored).
///
/// @return `0`.
int8_t rt_animation3d_get_looping(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub for `Animation3D.Duration` — get the animation length in
///        seconds (time of the last keyframe across all bones).
///
/// Silent stub returning `0.0`.
///
/// @param a Animation3D handle (ignored).
///
/// @return `0.0`.
double rt_animation3d_get_duration(void *a) {
    (void)a;
    return 0.0;
}

/// @brief Stub for `Animation3D.Name` — get the animation's name (e.g.
///        "Idle", "Run", set during glTF/FBX import).
///
/// Silent stub returning NULL.
///
/// @param a Animation3D handle (ignored).
///
/// @return `NULL`.
rt_string rt_animation3d_get_name(void *a) {
    (void)a;
    return NULL;
}

/// @brief Stub for `Animation3D.Retarget`.
///
/// Silent stub returning NULL.
/// @param a Animation3D handle (ignored).
/// @param src Source Skeleton3D handle (ignored).
/// @param dst Destination Skeleton3D handle (ignored).
/// @return Always NULL.
void *rt_animation3d_retarget(void *a, void *src, void *dst) {
    (void)a;
    (void)src;
    (void)dst;
    return NULL;
}

/// @brief Stub for `AnimPlayer3D.New` — would normally create a player
///        bound to the given Skeleton3D. The player owns a per-bone pose
///        buffer that the player updates each tick.
///
/// Trapping stub: a NULL player would crash on the first `Play` call.
///
/// @param s Skeleton3D handle (ignored).
///
/// @return Never returns normally.
void *rt_anim_player3d_new(void *s) {
    (void)s;
    rt_graphics_unavailable_("AnimPlayer3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `AnimPlayer3D.Play` — start playing the given animation
///        from the beginning, replacing any currently-active animation.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param a Animation3D handle (ignored).
void rt_anim_player3d_play(void *p, void *a) {
    (void)p;
    (void)a;
}

/// @brief Stub for `AnimPlayer3D.Crossfade` — start blending into a new
///        animation over `d` seconds. The previous animation is faded out
///        as the new one is faded in.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param a Target Animation3D handle (ignored).
/// @param d Crossfade duration in seconds (ignored).
void rt_anim_player3d_crossfade(void *p, void *a, double d) {
    (void)p;
    (void)a;
    (void)d;
}

/// @brief Stub for `AnimPlayer3D.Stop` — halt playback and freeze the
///        skeleton at the current pose.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
void rt_anim_player3d_stop(void *p) {
    (void)p;
}

/// @brief Stub for `AnimPlayer3D.Update` — advance the animation clock by
///        `d` seconds and recompute the pose. Should be called once per
///        frame.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param d Delta time in seconds (ignored).
void rt_anim_player3d_update(void *p, double d) {
    (void)p;
    (void)d;
}

/// @brief Stub for `AnimPlayer3D.SetSpeed` — multiplier on the per-tick
///        delta-time. 1.0 = normal, 0.5 = half-speed, 2.0 = double-speed.
///        Negative values play the animation in reverse.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param s Speed multiplier (ignored).
void rt_anim_player3d_set_speed(void *p, double s) {
    (void)p;
    (void)s;
}

/// @brief Stub for `AnimPlayer3D.Speed` — get the current playback speed
///        multiplier.
///
/// Silent stub returning `1.0` (normal speed).
///
/// @param p AnimPlayer3D handle (ignored).
///
/// @return `1.0`.
double rt_anim_player3d_get_speed(void *p) {
    (void)p;
    return 1.0;
}

/// @brief Stub for `AnimPlayer3D.IsPlaying` — true while an animation is
///        active and the clock is advancing.
///
/// Silent stub returning `0` (idle).
///
/// @param p AnimPlayer3D handle (ignored).
///
/// @return `0`.
int8_t rt_anim_player3d_is_playing(void *p) {
    (void)p;
    return 0;
}

/// @brief Stub for `AnimPlayer3D.Time` — current playback time in seconds
///        within the active animation.
///
/// Silent stub returning `0.0`.
///
/// @param p AnimPlayer3D handle (ignored).
///
/// @return `0.0`.
double rt_anim_player3d_get_time(void *p) {
    (void)p;
    return 0.0;
}

/// @brief Stub for `AnimPlayer3D.SetTime` — seek to the given time within
///        the active animation; pose is recomputed from keyframes.
///
/// Silent no-op stub.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param t Time in seconds, 0..duration (ignored).
void rt_anim_player3d_set_time(void *p, double t) {
    (void)p;
    (void)t;
}

/// @brief Stub for `AnimPlayer3D.BoneMatrix` — get the world-space
///        transform matrix for bone `i` at the current pose. Used by the
///        renderer to compute final per-vertex skinning.
///
/// Silent stub returning NULL.
///
/// @param p AnimPlayer3D handle (ignored).
/// @param i Bone index, 0..BoneCount-1 (ignored).
///
/// @return `NULL`.
void *rt_anim_player3d_get_bone_matrix(void *p, int64_t i) {
    (void)p;
    (void)i;
    return NULL;
}

/// @brief Stub for `Mesh3D.SetSkeleton` — bind a Skeleton3D so the mesh
///        can be rendered with `DrawMeshSkinned`. Per-vertex bone weights
///        must also be set via `SetBoneWeights`.
///
/// Silent no-op stub.
///
/// @param m Mesh3D handle (ignored).
/// @param s Skeleton3D handle, or NULL to detach (ignored).
void rt_mesh3d_set_skeleton(void *m, void *s) {
    (void)m;
    (void)s;
}

/// @brief Stub for `Mesh3D.SetBoneWeights` — set the four bone-weight
///        pairs influencing vertex `v`. Bone indices are into the bound
///        Skeleton3D; weights should sum to ~1.0.
///
/// Silent no-op stub. Each vertex can be influenced by up to 4 bones —
/// this matches the GPU-skinning implementation across all backends.
///
/// @param m  Mesh3D handle (ignored).
/// @param v  Vertex index, 0..VertexCount-1 (ignored).
/// @param b0 First bone index (ignored).
/// @param w0 First bone weight (ignored).
/// @param b1 Second bone index (ignored).
/// @param w1 Second bone weight (ignored).
/// @param b2 Third bone index (ignored).
/// @param w2 Third bone weight (ignored).
/// @param b3 Fourth bone index (ignored).
/// @param w3 Fourth bone weight (ignored).
void rt_mesh3d_set_bone_weights(void *m,
                                int64_t v,
                                int64_t b0,
                                double w0,
                                int64_t b1,
                                double w1,
                                int64_t b2,
                                double w2,
                                int64_t b3,
                                double w3) {
    (void)m;
    (void)v;
    (void)b0;
    (void)w0;
    (void)b1;
    (void)w1;
    (void)b2;
    (void)w2;
    (void)b3;
    (void)w3;
}

/* FBX Loader stubs */

/// @brief Stub for `FBX.Load` — would normally parse an Autodesk FBX
///        file (binary or ASCII auto-detected) and return a populated FBX
///        document handle.
///
/// Trapping stub: a NULL document handle would crash on every subsequent
/// query.
///
/// @param p Filesystem path to the .fbx file (ignored).
///
/// @return Never returns normally.
void *rt_fbx_load(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("FBX.Load: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for the recoverable FBX loader used by `Model3D.Load`.
/// @details In a graphics-disabled build, FBX support is not merely unavailable as
/// an asset-specific failure; the whole graphics subsystem is missing. The stub
/// therefore preserves the graphics-disabled contract by trapping instead of
/// returning NULL.
/// @param p Filesystem path to the .fbx file (ignored).
/// @return Never returns normally.
void *rt_fbx_load_recoverable(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("FBX.Load: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `FBX.MeshCount` — number of meshes in the loaded FBX
///        document.
///
/// Silent stub returning `0`.
///
/// @param f FBX document handle (ignored).
///
/// @return `0`.
int64_t rt_fbx_mesh_count(void *f) {
    (void)f;
    return 0;
}

/// @brief Stub for `FBX.Mesh(i)` — get the `i`th mesh as a Mesh3D.
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
/// @param i Mesh index (ignored).
///
/// @return `NULL`.
void *rt_fbx_get_mesh(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub for `FBX.Skeleton` — get the document's primary skeleton
///        (FBX typically embeds a single bind-pose skeleton shared by all
///        skinned meshes).
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
///
/// @return `NULL`.
void *rt_fbx_get_skeleton(void *f) {
    (void)f;
    return NULL;
}

/// @brief Stub for `FBX.AnimationCount` — number of animation tracks
///        (clips) in the document.
///
/// Silent stub returning `0`.
///
/// @param f FBX document handle (ignored).
///
/// @return `0`.
int64_t rt_fbx_animation_count(void *f) {
    (void)f;
    return 0;
}

/// @brief Stub for `FBX.Animation(i)` — get the `i`th animation as an
///        Animation3D.
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
/// @param i Animation index (ignored).
///
/// @return `NULL`.
void *rt_fbx_get_animation(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub for `FBX.AnimationName(i)` — get the name of the `i`th
///        animation (typically authored in the DCC tool).
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
/// @param i Animation index (ignored).
///
/// @return `NULL`.
rt_string rt_fbx_get_animation_name(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub object/morph animation count for graphics-disabled builds.
/// @param f FBX document handle (ignored).
/// @return Always `0`.
int64_t rt_fbx_node_animation_count(void *f) {
    (void)f;
    return 0;
}

/// @brief Stub object/morph animation lookup for graphics-disabled builds.
/// @param f FBX document handle (ignored).
/// @param i Animation index (ignored).
/// @return Always NULL.
void *rt_fbx_get_node_animation(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub object/morph animation name lookup for graphics-disabled builds.
/// @param f FBX document handle (ignored).
/// @param i Animation index (ignored).
/// @return Empty runtime string.
rt_string rt_fbx_get_node_animation_name(void *f, int64_t i) {
    (void)f;
    (void)i;
    return rt_const_cstr("");
}

/// @brief Stub imported-camera count for graphics-disabled builds.
/// @param f FBX document handle (ignored).
/// @return Always `0`.
int64_t rt_fbx_camera_count(void *f) {
    (void)f;
    return 0;
}

/// @brief Stub imported-camera lookup for graphics-disabled builds.
/// @param f FBX document handle (ignored).
/// @param i Camera index (ignored).
/// @return Always NULL.
void *rt_fbx_get_camera(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub for `FBX.MaterialCount` — number of materials defined in
///        the document.
///
/// Silent stub returning `0`.
///
/// @param f FBX document handle (ignored).
///
/// @return `0`.
int64_t rt_fbx_material_count(void *f) {
    (void)f;
    return 0;
}

/// @brief Stub for `FBX.Material(i)` — get the `i`th material as a
///        Material3D (texture paths and connection traces extracted from
///        the FBX node graph).
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
/// @param i Material index (ignored).
///
/// @return `NULL`.
void *rt_fbx_get_material(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/// @brief Stub for `FBX.MorphTarget(i)` — get the `i`th morph target
///        as a MorphTarget3D. FBX BlendShape / Shape nodes are extracted
///        with sparse position/normal deltas during import.
///
/// Silent stub returning NULL.
///
/// @param f FBX document handle (ignored).
/// @param i Morph target index (ignored).
///
/// @return `NULL`.
void *rt_fbx_get_morph_target(void *f, int64_t i) {
    (void)f;
    (void)i;
    return NULL;
}

/* GLTF Loader stubs */

/// @brief Stub for `glTF.Load` — would normally parse a `.gltf` (JSON +
///        external buffers) or `.glb` (single binary container) file and
///        return a populated glTF document handle.
///
/// Trapping stub: a NULL document handle would crash on every subsequent
/// query (`MeshCount`, `Material(i)`, etc.).
///
/// @param p Filesystem path to the .gltf or .glb file (ignored).
///
/// @return Never returns normally.
void *rt_gltf_load(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("GLTF.Load: graphics support not compiled in");
    return NULL;
}

/// @brief Trap because asset-URI glTF loading requires unavailable Graphics3D support.
/// @param p Asset path or URI (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_gltf_load_asset(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("GLTF.LoadAsset: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `glTF.MeshCount` — number of meshes in the loaded
///        document.
///
/// Silent stub returning `0`.
///
/// @param g glTF document handle (ignored).
///
/// @return `0`.
int64_t rt_gltf_mesh_count(void *g) {
    (void)g;
    return 0;
}

/// @brief Stub for `glTF.Mesh(i)` — get the `i`th mesh as a Mesh3D.
///
/// Silent stub returning NULL.
///
/// @param g glTF document handle (ignored).
/// @param i Mesh index, 0..MeshCount-1 (ignored).
///
/// @return `NULL`.
void *rt_gltf_get_mesh(void *g, int64_t i) {
    (void)g;
    (void)i;
    return NULL;
}

/// @brief Stub for `glTF.MaterialCount` — number of materials in the
///        loaded document.
///
/// Silent stub returning `0`.
///
/// @param g glTF document handle (ignored).
///
/// @return `0`.
int64_t rt_gltf_material_count(void *g) {
    (void)g;
    return 0;
}

/// @brief Stub for `glTF.Material(i)` — get the `i`th material as a
///        Material3D (PBR metallic-roughness).
///
/// Silent stub returning NULL.
///
/// @param g glTF document handle (ignored).
/// @param i Material index, 0..MaterialCount-1 (ignored).
///
/// @return `NULL`.
void *rt_gltf_get_material(void *g, int64_t i) {
    (void)g;
    (void)i;
    return NULL;
}

/// @brief Stub for `glTF.NodeCount` — total number of scene nodes in the
///        document (recursive count from the scene root).
///
/// Silent stub returning `0`.
///
/// @param g glTF document handle (ignored).
///
/// @return `0`.
int64_t rt_gltf_node_count(void *g) {
    (void)g;
    return 0;
}

/// @brief Stub for `glTF.CameraCount` — number of imported active-scene cameras.
///
/// Silent stub returning `0`.
///
/// @param g glTF document handle (ignored).
///
/// @return `0`.
int64_t rt_gltf_camera_count(void *g) {
    (void)g;
    return 0;
}

/// @brief Stub for `glTF.Camera(i)` — get the `i`th imported Camera3D.
///
/// Silent stub returning NULL.
///
/// @param g glTF document handle (ignored).
/// @param i Camera index, 0..CameraCount-1 (ignored).
///
/// @return `NULL`.
void *rt_gltf_get_camera(void *g, int64_t i) {
    (void)g;
    (void)i;
    return NULL;
}

/// @brief Return the neutral glTF scene count.
/// @param g glTF document handle (ignored).
/// @return Always `0`.
int64_t rt_gltf_scene_count(void *g) {
    (void)g;
    return 0;
}

/// @brief Return no glTF scene name in a graphics-disabled build.
/// @param g glTF document handle (ignored).
/// @param i Scene index (ignored).
/// @return Empty runtime string.
rt_string rt_gltf_get_scene_name(void *g, int64_t i) {
    (void)g;
    (void)i;
    return rt_const_cstr("");
}

/// @brief Return no indexed glTF scene root.
/// @param g glTF document handle (ignored).
/// @param i Scene index (ignored).
/// @return Always NULL.
void *rt_gltf_get_scene_root_at(void *g, int64_t i) {
    (void)g;
    (void)i;
    return NULL;
}

/// @brief Return the neutral camera count for an indexed glTF scene.
/// @param g glTF document handle (ignored).
/// @param scene_index Scene index (ignored).
/// @return Always `0`.
int64_t rt_gltf_scene_camera_count(void *g, int64_t scene_index) {
    (void)g;
    (void)scene_index;
    return 0;
}

/// @brief Return no indexed camera for a glTF scene.
/// @param g glTF document handle (ignored).
/// @param scene_index Scene index (ignored).
/// @param i Camera index (ignored).
/// @return Always NULL.
void *rt_gltf_get_scene_camera(void *g, int64_t scene_index, int64_t i) {
    (void)g;
    (void)scene_index;
    (void)i;
    return NULL;
}

/// @brief Stub for `glTF.SceneRoot` — get the document's default-scene
///        root SceneNode3D.
///
/// Silent stub returning NULL.
///
/// @param g glTF document handle (ignored).
///
/// @return `NULL`.
void *rt_gltf_get_scene_root(void *g) {
    (void)g;
    return NULL;
}

/* Model3D stubs */

/// @brief Stub for `Model3D.Load` — would normally route by file
///        extension (.vscn / .fbx / .gltf / .glb) and build an internal
///        resource collection (meshes, materials, skeletons, animations).
///
/// Trapping stub. The real `Model3D` is the unified asset container
/// callers go through to share resources across instances.
///
/// @param p Filesystem path to the asset file (ignored).
///
/// @return Never returns normally.
void *rt_model3d_load(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("Model3D.Load: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadResult` in graphics-disabled builds.
/// @details Result-returning APIs do not need a secondary side channel, so disabled builds report
///          unavailable graphics support as `Err(String)`.
/// @param p Filesystem path to the asset file (ignored).
/// @return `Err("SceneAsset.Load: graphics support not compiled in")`.
void *rt_model3d_load_result(rt_string p) {
    (void)p;
    return rt_result_err_str(rt_const_cstr("SceneAsset.Load: graphics support not compiled in"));
}

/// @brief Trap because SceneAsset URI loading requires unavailable Graphics3D support.
/// @param p Asset path or URI (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_model3d_load_asset(rt_string p) {
    (void)p;
    rt_graphics_unavailable_("Model3D.LoadAsset: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadAssetResult` in graphics-disabled builds.
/// @param p Asset path or URI (ignored).
/// @return `Err("SceneAsset.LoadAsset: graphics support not compiled in")`.
void *rt_model3d_load_asset_result(rt_string p) {
    (void)p;
    return rt_result_err_str(
        rt_const_cstr("SceneAsset.LoadAsset: graphics support not compiled in"));
}

/// @brief Stub for `SceneAsset.Save` in graphics-disabled builds.
/// @param model SceneAsset handle (ignored).
/// @param path Destination path (ignored).
/// @return Never returns normally; graphics-unavailable handling yields `0`.
int64_t rt_model3d_save(void *model, rt_string path) {
    (void)model;
    (void)path;
    RT_GRAPHICS_TRAP_RET("SceneAsset.Save: graphics support not compiled in", 0);
}

/// @brief Stub for `Model3D.MeshCount` — number of meshes in the loaded
///        model's resource collection.
///
/// Silent stub returning `0`.
///
/// @param m Model3D handle (ignored).
///
/// @return `0`.
int64_t rt_model3d_get_mesh_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.MaterialCount` — number of materials.
///
/// Silent stub returning `0`.
///
/// @param m Model3D handle (ignored).
///
/// @return `0`.
int64_t rt_model3d_get_material_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.SkeletonCount` — number of skeletons (one
///        per skinned mesh in the model).
///
/// Silent stub returning `0`.
///
/// @param m Model3D handle (ignored).
///
/// @return `0`.
int64_t rt_model3d_get_skeleton_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.AnimationCount` — number of animations
///        embedded in the model.
///
/// Silent stub returning `0`.
///
/// @param m Model3D handle (ignored).
///
/// @return `0`.
int64_t rt_model3d_get_animation_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.NodeAnimationCount`.
/// @param m Model3D handle (ignored).
/// @return Always `0`.
int64_t rt_model3d_get_node_animation_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `SceneAsset.MorphTargetCount`.
/// @param m SceneAsset handle (ignored).
/// @return Always `0`.
int64_t rt_model3d_get_morph_target_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `SceneAsset.MorphShapeCount`.
/// @param m SceneAsset handle (ignored).
/// @return Always `0`.
int64_t rt_model3d_get_morph_shape_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.NodeCount` — total node count across the
///        model's scene tree.
///
/// Silent stub returning `0`.
///
/// @param m Model3D handle (ignored).
///
/// @return `0`.
int64_t rt_model3d_get_node_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Model3D.Mesh(i)` — get the `i`th mesh from the
///        model's resource collection.
///
/// Silent stub returning NULL.
///
/// @param m Model3D handle (ignored).
/// @param i Mesh index, 0..MeshCount-1 (ignored).
///
/// @return `NULL`.
void *rt_model3d_get_mesh(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `Model3D.Material(i)` — get the `i`th material.
///
/// Silent stub returning NULL.
///
/// @param m Model3D handle (ignored).
/// @param i Material index, 0..MaterialCount-1 (ignored).
///
/// @return `NULL`.
void *rt_model3d_get_material(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `Model3D.Skeleton(i)` — get the `i`th skeleton.
///
/// Silent stub returning NULL.
///
/// @param m Model3D handle (ignored).
/// @param i Skeleton index, 0..SkeletonCount-1 (ignored).
///
/// @return `NULL`.
void *rt_model3d_get_skeleton(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `Model3D.Animation(i)` — get the `i`th animation.
///
/// Silent stub returning NULL.
///
/// @param m Model3D handle (ignored).
/// @param i Animation index, 0..AnimationCount-1 (ignored).
///
/// @return `NULL`.
void *rt_model3d_get_animation(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `Model3D.GetNodeAnimation`.
/// @param m Model3D handle (ignored).
/// @param i Node-animation index (ignored).
/// @return Always NULL.
void *rt_model3d_get_node_animation(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `SceneAsset.GetMorphTarget(meshIndex)`.
/// @param m SceneAsset handle (ignored).
/// @param i Mesh index (ignored).
/// @return Always NULL.
void *rt_model3d_get_morph_target(void *m, int64_t i) {
    (void)m;
    (void)i;
    return NULL;
}

/// @brief Stub for `Model3D.GetNodeAnimationName`.
/// @param m Model3D handle (ignored).
/// @param i Node-animation index (ignored).
/// @return Empty runtime string.
rt_string rt_model3d_get_node_animation_name(void *m, int64_t i) {
    (void)m;
    (void)i;
    return rt_const_cstr("");
}

/// @brief Stub for `Model3D.LoadAnimation`.
/// @param path Model path (ignored).
/// @param index Animation index (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_model3d_load_animation(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    rt_graphics_unavailable_("Model3D.LoadAnimation: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadAnimationResult` in graphics-disabled builds.
/// @param path Model path (ignored).
/// @param index Animation index (ignored).
/// @return `Err("SceneAsset.LoadAnimation: graphics support not compiled in")`.
void *rt_model3d_load_animation_result(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    return rt_result_err_str(
        rt_const_cstr("SceneAsset.LoadAnimation: graphics support not compiled in"));
}

/// @brief Stub for `Model3D.LoadAnimationAsset`.
/// @param path Asset path or URI (ignored).
/// @param index Animation index (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_model3d_load_animation_asset(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    rt_graphics_unavailable_("Model3D.LoadAnimationAsset: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadAnimationAssetResult` in graphics-disabled builds.
/// @param path Asset path or URI (ignored).
/// @param index Animation index (ignored).
/// @return `Err("SceneAsset.LoadAnimationAsset: graphics support not compiled in")`.
void *rt_model3d_load_animation_asset_result(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    return rt_result_err_str(
        rt_const_cstr("SceneAsset.LoadAnimationAsset: graphics support not compiled in"));
}

/// @brief Stub for `Model3D.LoadNodeAnimation`.
/// @param path Model path (ignored).
/// @param index Node-animation index (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_model3d_load_node_animation(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    rt_graphics_unavailable_("Model3D.LoadNodeAnimation: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadNodeAnimationResult` in graphics-disabled builds.
/// @param path Model path (ignored).
/// @param index Node animation index (ignored).
/// @return `Err("SceneAsset.LoadNodeAnimation: graphics support not compiled in")`.
void *rt_model3d_load_node_animation_result(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    return rt_result_err_str(
        rt_const_cstr("SceneAsset.LoadNodeAnimation: graphics support not compiled in"));
}

/// @brief Stub for `Model3D.LoadNodeAnimationAsset`.
/// @param path Asset path or URI (ignored).
/// @param index Node-animation index (ignored).
/// @return Never returns normally; graphics-unavailable handling yields NULL.
void *rt_model3d_load_node_animation_asset(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    rt_graphics_unavailable_("Model3D.LoadNodeAnimationAsset: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneAsset.LoadNodeAnimationAssetResult` in graphics-disabled builds.
/// @param path Asset path or URI (ignored).
/// @param index Node animation index (ignored).
/// @return `Err("SceneAsset.LoadNodeAnimationAsset: graphics support not compiled in")`.
void *rt_model3d_load_node_animation_asset_result(rt_string path, int64_t index) {
    (void)path;
    (void)index;
    return rt_result_err_str(
        rt_const_cstr("SceneAsset.LoadNodeAnimationAsset: graphics support not compiled in"));
}

/// @brief Stub for `Model3D.FindNode(name)` — recursive name lookup
///        within the model's node hierarchy.
///
/// Silent stub returning NULL.
///
/// @param m    Model3D handle (ignored).
/// @param name Node name to search for (ignored).
///
/// @return `NULL`.
void *rt_model3d_find_node(void *m, rt_string name) {
    (void)m;
    (void)name;
    return NULL;
}

/// @brief Stub for `SceneAsset.FindNodeOption` — template node lookup as Option.
/// @details Graphics-disabled builds have no imported model hierarchy, so this
///          returns `None`.
/// @param m SceneAsset/Model3D handle (ignored).
/// @param name Node name to search for (ignored).
/// @return `None`.
void *rt_model3d_find_node_option(void *m, rt_string name) {
    (void)m;
    (void)name;
    return rt_option_none();
}

/// @brief Stub for `SceneAsset.FlattenStatic` — would merge a template subtree's
///        static meshes into one Mesh3D per material (ADR 0294).
/// @details Graphics-disabled builds have no imported model hierarchy, so this
///          returns NULL like the other model-query stubs.
/// @param m SceneAsset/Model3D handle (ignored).
/// @param root_name Subtree root name (ignored).
/// @param transform Placement Mat4 (ignored).
/// @return NULL.
void *rt_model3d_flatten_static(void *m, rt_string root_name, void *transform) {
    (void)m;
    (void)root_name;
    (void)transform;
    return NULL;
}

/// @brief Stub for `Model3D.Instantiate` — would normally clone the
///        model's node hierarchy into a fresh SceneNode3D tree, sharing
///        underlying mesh/material/skeleton resources with other instances.
///
/// Silent stub returning NULL. The real implementation enables a single
/// imported model to be drawn many times without duplicating geometry.
///
/// @param m Model3D handle (ignored).
///
/// @return `NULL`.
void *rt_model3d_instantiate(void *m) {
    (void)m;
    return NULL;
}

/// @brief Stub for `Model3D.InstantiateScene` — would normally create a
///        fresh `Scene3D` and attach cloned top-level nodes below the
///        scene root.
///
/// Silent stub returning NULL.
///
/// @param m Model3D handle (ignored).
///
/// @return `NULL`.
void *rt_model3d_instantiate_scene(void *m) {
    (void)m;
    return NULL;
}

/* MorphTarget3D stubs */

/// @brief Stub for `MorphTarget3D.New` — would normally allocate a
///        morph-target container for a mesh with `vc` vertices. The
///        container holds zero shapes; add shapes via `AddShape`.
///
/// Trapping stub: callers expect a usable handle for shape-add and weight
/// queries.
///
/// @param vc Vertex count of the bound mesh (ignored).
///
/// @return Never returns normally.
void *rt_morphtarget3d_new(int64_t vc) {
    (void)vc;
    rt_graphics_unavailable_("MorphTarget3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `MorphTarget3D.AddShape` — append a named shape (a
///        per-vertex delta set) to the container. Returns the assigned
///        shape index used by `SetDelta` / `SetWeight`, or `-1` on failure.
///
/// Silent stub returning `-1`.
///
/// @param m MorphTarget3D handle (ignored).
/// @param n Shape name (e.g. "smile", "blink_left") (ignored).
///
/// @return `-1`.
int64_t rt_morphtarget3d_add_shape(void *m, rt_string n) {
    (void)m;
    (void)n;
    return -1;
}

/// @brief Stub for `MorphTarget3D.SetDelta` — set the per-vertex
///        position delta for shape `s`, vertex `v`. Deltas are accumulated
///        weighted by the shape's current weight during morphing.
///
/// Silent no-op stub.
///
/// @param m  MorphTarget3D handle (ignored).
/// @param s  Shape index (ignored).
/// @param v  Vertex index, 0..VertexCount-1 (ignored).
/// @param dx Position delta x (ignored).
/// @param dy Position delta y (ignored).
/// @param dz Position delta z (ignored).
void rt_morphtarget3d_set_delta(void *m, int64_t s, int64_t v, double dx, double dy, double dz) {
    (void)m;
    (void)s;
    (void)v;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Stub for `MorphTarget3D.SetNormalDelta` — set the per-vertex
///        normal delta for shape `s`, vertex `v`. Required for correct
///        lighting on heavily-morphed surfaces (faces).
///
/// Silent no-op stub.
///
/// @param m  MorphTarget3D handle (ignored).
/// @param s  Shape index (ignored).
/// @param v  Vertex index (ignored).
/// @param dx Normal delta x (ignored).
/// @param dy Normal delta y (ignored).
/// @param dz Normal delta z (ignored).
void rt_morphtarget3d_set_normal_delta(
    void *m, int64_t s, int64_t v, double dx, double dy, double dz) {
    (void)m;
    (void)s;
    (void)v;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Stub for `MorphTarget3D.SetWeight` — set the blend weight of
///        shape `s`. Weights are typically 0..1; multiple shapes can be
///        active simultaneously and their deltas sum.
///
/// Silent no-op stub.
///
/// @param m MorphTarget3D handle (ignored).
/// @param s Shape index (ignored).
/// @param w Blend weight (ignored).
void rt_morphtarget3d_set_weight(void *m, int64_t s, double w) {
    (void)m;
    (void)s;
    (void)w;
}

/// @brief Stub for `MorphTarget3D.Weight` — get the current blend weight
///        of shape `s`.
///
/// Silent stub returning `0.0`.
///
/// @param m MorphTarget3D handle (ignored).
/// @param s Shape index (ignored).
///
/// @return `0.0`.
double rt_morphtarget3d_get_weight(void *m, int64_t s) {
    (void)m;
    (void)s;
    return 0.0;
}

/// @brief Stub for `MorphTarget3D.SetWeightByName` — set blend weight of
///        the shape with the given name. Convenience wrapper around
///        `SetWeight` for callers that don't track indices.
///
/// Silent no-op stub.
///
/// @param m MorphTarget3D handle (ignored).
/// @param n Shape name (ignored).
/// @param w Blend weight (ignored).
void rt_morphtarget3d_set_weight_by_name(void *m, rt_string n, double w) {
    (void)m;
    (void)n;
    (void)w;
}

/// @brief Stub for `MorphTarget3D.ShapeCount` — number of shapes
///        currently in the container.
///
/// Silent stub returning `0`.
///
/// @param m MorphTarget3D handle (ignored).
///
/// @return `0`.
int64_t rt_morphtarget3d_get_shape_count(void *m) {
    (void)m;
    return 0;
}

/// @brief Stub for `Mesh3D.SetMorphTargets` — bind a MorphTarget3D
///        container to this mesh so subsequent `Canvas3D.DrawMeshMorphed`
///        calls apply weighted shape deltas during vertex transformation.
///
/// Silent no-op stub.
///
/// @param m  Mesh3D handle (ignored).
/// @param mt MorphTarget3D handle, or NULL to clear (ignored).
void rt_mesh3d_set_morph_targets(void *m, void *mt) {
    (void)m;
    (void)mt;
}

/* Particles3D stubs */

/// @brief Stub for `Particles3D.New` — would normally allocate a
///        particle system with `n` slots in the per-particle pool.
///
/// Trapping stub: callers will configure / start the system and depend
/// on a usable handle.
///
/// @param n Maximum live particle count (ignored).
///
/// @return Never returns normally.
void *rt_particles3d_new(int64_t n) {
    (void)n;
    rt_graphics_unavailable_("Particles3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Particles3D.SetPosition` — set the emitter origin
///        in world space. Particles spawn around this position (offset by
///        the emitter shape).
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
/// @param x World x (ignored).
/// @param y World y (ignored).
/// @param z World z (ignored).
void rt_particles3d_set_position(void *o, double x, double y, double z) {
    (void)o;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `Particles3D.SetDirection` — set the dominant
///        emission direction `(dx, dy, dz)` and initial speed `s`.
///        Particles inherit this velocity at spawn time.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param dx Direction x (ignored).
/// @param dy Direction y (ignored).
/// @param dz Direction z (ignored).
/// @param s  Initial speed in world units / second (ignored).
void rt_particles3d_set_direction(void *o, double dx, double dy, double dz, double s) {
    (void)o;
    (void)dx;
    (void)dy;
    (void)dz;
    (void)s;
}

/// @brief Stub for `Particles3D.SetSpeed` — randomized initial speed
///        range. Each particle is spawned with a speed uniformly sampled
///        from `[mn, mx]` along its emission direction.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param mn Minimum speed in world units / second (ignored).
/// @param mx Maximum speed in world units / second (ignored).
void rt_particles3d_set_speed(void *o, double mn, double mx) {
    (void)o;
    (void)mn;
    (void)mx;
}

/// @brief Stub for `Particles3D.SetLifetime` — randomized particle
///        lifetime range. Each particle dies when its age exceeds a
///        value uniformly sampled from `[mn, mx]` at spawn time.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param mn Minimum lifetime in seconds (ignored).
/// @param mx Maximum lifetime in seconds (ignored).
void rt_particles3d_set_lifetime(void *o, double mn, double mx) {
    (void)o;
    (void)mn;
    (void)mx;
}

/// @brief Stub for `Particles3D.SetSize` — particle size animation
///        envelope: linearly interpolate from start size `s` (at spawn)
///        to end size `e` (at end of lifetime).
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
/// @param s Start size in world units (ignored).
/// @param e End size in world units (ignored).
void rt_particles3d_set_size(void *o, double s, double e) {
    (void)o;
    (void)s;
    (void)e;
}

/// @brief Stub for `Particles3D.SetGravity` — per-system gravity vector
///        applied each tick to integrate particle velocity. Useful for
///        smoke/embers/sparks. Independent from the Physics3D world's
///        global gravity.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param gx Gravity x in world units / second² (ignored).
/// @param gy Gravity y (ignored).
/// @param gz Gravity z (ignored).
void rt_particles3d_set_gravity(void *o, double gx, double gy, double gz) {
    (void)o;
    (void)gx;
    (void)gy;
    (void)gz;
}

/// @brief Stub for `Particles3D.SetColor` — particle color animation
///        envelope: linearly interpolate from start color `sc` to end
///        color `ec` over each particle's lifetime.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param sc Start color, packed 0xAARRGGBB (ignored).
/// @param ec End color, packed 0xAARRGGBB (ignored).
void rt_particles3d_set_color(void *o, int64_t sc, int64_t ec) {
    (void)o;
    (void)sc;
    (void)ec;
}

/// @brief Stub for `Particles3D.SetAlpha` — particle alpha animation
///        envelope: linearly interpolate from start alpha `sa` to end
///        alpha `ea`. Common pattern: `(1.0, 0.0)` for fade-out.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param sa Start alpha, 0..1 (ignored).
/// @param ea End alpha, 0..1 (ignored).
void rt_particles3d_set_alpha(void *o, double sa, double ea) {
    (void)o;
    (void)sa;
    (void)ea;
}

/// @brief Stub for `Particles3D.SetRate` — emission rate in particles
///        per second when the system is in continuous mode.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
/// @param r Particles per second (ignored).
void rt_particles3d_set_rate(void *o, double r) {
    (void)o;
    (void)r;
}

/// @brief Stub for `Particles3D.SetAdditive` — when enabled, particles
///        composite using additive blending (good for fire/glow effects)
///        instead of source-over alpha.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
/// @param a Non-zero to enable additive blending (ignored).
void rt_particles3d_set_additive(void *o, int8_t a) {
    (void)o;
    (void)a;
}

/// @brief Stub for `Particles3D.SetTexture` — bind a Pixels surface as
///        the per-particle billboard texture.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
/// @param t Pixels handle, or NULL for untextured (ignored).
void rt_particles3d_set_texture(void *o, void *t) {
    (void)o;
    (void)t;
}

/// @brief Stub for `Particles3D.SetEmitterShape` — selects how new
///        particle positions are sampled: 0=Point, 1=Box, 2=Sphere,
///        3=Cone (axis-aligned).
///
/// Silent no-op stub. Combine with `SetEmitterSize` to control the volume.
///
/// @param o Particles3D handle (ignored).
/// @param s Emitter shape index (ignored).
void rt_particles3d_set_emitter_shape(void *o, int64_t s) {
    (void)o;
    (void)s;
}

/// @brief Stub for `Particles3D.SetEmitterSize` — full extents of the
///        emitter volume along each axis. Interpretation depends on the
///        active emitter shape.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param sx Extent along X (ignored).
/// @param sy Extent along Y (ignored).
/// @param sz Extent along Z (ignored).
void rt_particles3d_set_emitter_size(void *o, double sx, double sy, double sz) {
    (void)o;
    (void)sx;
    (void)sy;
    (void)sz;
}

/// @brief Stub for `Particles3D.Start` — begin continuous emission at
///        the configured rate.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
void rt_particles3d_start(void *o) {
    (void)o;
}

/// @brief Stub for `Particles3D.Stop` — halt continuous emission.
///        Existing particles continue to live out their lifetimes.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
void rt_particles3d_stop(void *o) {
    (void)o;
}

/// @brief Stub for `Particles3D.Burst` — emit `n` particles immediately
///        in a single tick (one-shot, independent of continuous emission).
///
/// Silent no-op stub. Used for one-off effects like explosions / hits.
///
/// @param o Particles3D handle (ignored).
/// @param n Particle count to emit (ignored).
void rt_particles3d_burst(void *o, int64_t n) {
    (void)o;
    (void)n;
}

/// @brief Stub for `Particles3D.Clear` — destroy all live particles
///        immediately, ignoring remaining lifetime.
///
/// Silent no-op stub.
///
/// @param o Particles3D handle (ignored).
void rt_particles3d_clear(void *o) {
    (void)o;
}

/// @brief Stub for `Particles3D.Update` — advance every live particle by
///        `dt` seconds: integrate motion, age out expired particles, and
///        spawn new ones from the continuous rate.
///
/// Silent no-op stub.
///
/// @param o  Particles3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_particles3d_update(void *o, double dt) {
    (void)o;
    (void)dt;
}

/// @brief Stub for `Particles3D.Draw` — render all live particles as
///        camera-facing billboards using the bound texture and the
///        currently-selected blend mode.
///
/// Silent no-op stub.
///
/// @param o   Particles3D handle (ignored).
/// @param c   Canvas3D handle (ignored).
/// @param cam Camera3D handle (ignored).
void rt_particles3d_draw(void *o, void *c, void *cam) {
    (void)o;
    (void)c;
    (void)cam;
}

/// @brief Stub for `Particles3D.Count` — number of currently-live
///        particles.
///
/// Silent stub returning `0`.
///
/// @param o Particles3D handle (ignored).
///
/// @return `0`.
int64_t rt_particles3d_get_count(void *o) {
    (void)o;
    return 0;
}

/// @brief Stub for `Particles3D.IsEmitting` — true while continuous
///        emission is active (i.e. since the last `Start` and before
///        the next `Stop`).
///
/// Silent stub returning `0`.
///
/// @param o Particles3D handle (ignored).
///
/// @return `0`.
int8_t rt_particles3d_get_emitting(void *o) {
    (void)o;
    return 0;
}

/// @brief Graphics-disabled silent no-op stub for `Particles3D.set_RenderFinalFrame`.
/// @details No particle object can be constructed in this configuration, so the compatibility
///          option is accepted as a silent no-op to preserve the registry ABI.
/// @param o Particles3D handle (ignored).
/// @param enabled Requested terminal-frame state (ignored).
void rt_particles3d_set_render_final_frame(void *o, int8_t enabled) {
    (void)o;
    (void)enabled;
}

/// @brief Graphics-disabled silent fallback for `Particles3D.get_RenderFinalFrame`.
/// @param o Particles3D handle (ignored).
/// @return 0 because no particle simulation exists.
int8_t rt_particles3d_get_render_final_frame(void *o) {
    (void)o;
    return 0;
}

/// @brief Graphics-disabled silent fallback for cumulative Particles3D dropped-time telemetry.
/// @param o Particles3D handle (ignored).
/// @return 0 because no particle simulation runs.
double rt_particles3d_get_dropped_time(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Graphics-disabled silent fallback for latest Particles3D dropped-time telemetry.
/// @param o Particles3D handle (ignored).
/// @return 0 because no particle simulation runs.
double rt_particles3d_get_last_dropped_time(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Graphics-disabled silent fallback for Particles3D fixed-step residual telemetry.
/// @param o Particles3D handle (ignored).
/// @return 0 because no particle simulation runs.
double rt_particles3d_get_residual_time(void *o) {
    (void)o;
    return 0.0;
}

/// @brief Graphics-disabled silent no-op stub for resetting Particles3D dropped-time telemetry.
/// @param o Particles3D handle (ignored).
void rt_particles3d_reset_dropped_time(void *o) {
    (void)o;
}

/* Stub-parity audit additions: entry points registered in the 3D runtime
 * defs whose implementations compile only in graphics-enabled builds. */

/// @brief Silent fallback stub for `BlendTree3D.get_BlendMode` (graphics-disabled build).
///
/// @param o BlendTree3D handle (ignored).
///
/// @return `0`, the fallback blend-mode value.
int64_t rt_blend_tree3d_get_blend_mode(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BlendTree3D.get_BlendMode: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `BlendTree3D.set_BlendMode` (graphics-disabled build).
///
/// @param o  BlendTree3D handle (ignored before trapping).
/// @param a1 Blend-mode value (ignored before trapping).
void rt_blend_tree3d_set_blend_mode(void *o, int64_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("BlendTree3D.set_BlendMode: graphics support not compiled in");
}

/// @brief Trapping stub for `Decal3D.SetDepthBias` (graphics-disabled build).
///
/// @param o  Decal3D handle (ignored before trapping).
/// @param a1 Depth-bias value (ignored before trapping).
void rt_decal3d_set_depth_bias(void *o, double a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Decal3D.SetDepthBias: graphics support not compiled in");
}

/// @brief Trapping stub for `Material3D.ClearAlbedoRenderTarget` (graphics-disabled build).
///
/// @param o Material3D handle (ignored before trapping).
void rt_material3d_clear_albedo_render_target(void *o) {
    (void)o;
    RT_GRAPHICS_TRAP_VOID("Material3D.ClearAlbedoRenderTarget: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Material3D.get_SsrEnabled` (graphics-disabled build).
///
/// @param o Material3D handle (ignored).
///
/// @return `0`, indicating that screen-space reflections are disabled.
int8_t rt_material3d_get_ssr_enabled(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_SsrEnabled: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Material3D.SetAlbedoRenderTarget` (graphics-disabled build).
///
/// @param o  Material3D handle (ignored before trapping).
/// @param a1 Render-target handle (ignored before trapping).
void rt_material3d_set_albedo_render_target(void *o, void *a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Material3D.SetAlbedoRenderTarget: graphics support not compiled in");
}

/// @brief Trapping stub for `Material3D.SetEmissiveRenderTarget` (graphics-disabled build).
///
/// @param o  Material3D handle (ignored before trapping).
/// @param a1 Render-target handle (ignored before trapping).
void rt_material3d_set_emissive_render_target(void *o, void *a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Material3D.SetEmissiveRenderTarget: graphics support not compiled in");
}

/// @brief Trapping stub for `Material3D.set_SsrEnabled` (graphics-disabled build).
///
/// @param o  Material3D handle (ignored before trapping).
/// @param a1 Non-zero to enable screen-space reflections (ignored before trapping).
void rt_material3d_set_ssr_enabled(void *o, int8_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Material3D.set_SsrEnabled: graphics support not compiled in");
}

/// @brief Silent fallback stub for `SceneAsset.ApplyVariant` (graphics-disabled build).
///
/// @param o  SceneAsset handle (ignored).
/// @param a1 Destination scene or model handle (ignored).
/// @param a2 Variant index to apply (ignored).
///
/// @return `0`, indicating that no variant was applied.
int64_t rt_model3d_apply_variant(void *o, void *a1, int64_t a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneAsset.ApplyVariant: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `SceneAsset.GenerateLODs` (graphics-disabled build).
///
/// @param o  SceneAsset handle (ignored).
/// @param a1 Number of levels of detail to generate (ignored).
/// @param a2 Simplification ratio between levels (ignored).
///
/// @return `0`, indicating that no levels of detail were generated.
int64_t rt_model3d_generate_lods(void *o, int64_t a1, double a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneAsset.GenerateLODs: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `SceneAsset.get_VariantCount` (graphics-disabled build).
///
/// @param o SceneAsset handle (ignored).
///
/// @return `0`, indicating that no material variants are available.
int64_t rt_model3d_get_variant_count(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneAsset.get_VariantCount: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `SceneAsset.GetVariantName` (graphics-disabled build).
///
/// @param o  SceneAsset handle (ignored).
/// @param a1 Variant index (ignored).
///
/// @return An empty runtime string.
rt_string rt_model3d_get_variant_name(void *o, int64_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneAsset.GetVariantName: graphics support not compiled in",
                                  rt_string_from_bytes("", 0));
}

/// @brief Trapping stub for `SceneAsset.LoadResultWithOptions` (graphics-disabled build).
///
/// @param a0 Asset path to load (ignored before trapping).
/// @param a1 Non-zero to request the optional loading behavior (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_model3d_load_result_with_options(rt_string a0, int8_t a1) {
    (void)a0;
    (void)a1;
    RT_GRAPHICS_TRAP_RET("SceneAsset.LoadResultWithOptions: graphics support not compiled in",
                         NULL);
}

/// @brief Trapping stub for `SceneAsset.LoadWithOptions` (graphics-disabled build).
///
/// @param a0 Asset path to load (ignored before trapping).
/// @param a1 Non-zero to request the optional loading behavior (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_model3d_load_with_options(rt_string a0, int8_t a1) {
    (void)a0;
    (void)a1;
    RT_GRAPHICS_TRAP_RET("SceneAsset.LoadWithOptions: graphics support not compiled in", NULL);
}

/// @brief Trapping stub for `SceneAsset.LoadWithOptionsEx` (graphics-disabled build).
///
/// @param a0 Asset path to load (ignored before trapping).
/// @param a1 Serialized or named loading options (ignored before trapping).
///
/// @return This stub traps before returning; `NULL` is supplied as the macro fallback.
void *rt_model3d_load_with_options_ex(rt_string a0, rt_string a1) {
    (void)a0;
    (void)a1;
    RT_GRAPHICS_TRAP_RET("SceneAsset.LoadWithOptionsEx: graphics support not compiled in", NULL);
}

/// @brief Trapping stub for `Particles3D.SetStretch` (graphics-disabled build).
///
/// @param o  Particles3D handle (ignored before trapping).
/// @param a1 Stretch factor (ignored before trapping).
void rt_particles3d_set_stretch(void *o, double a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Particles3D.SetStretch: graphics support not compiled in");
}

/// @brief Trapping stub for `Particles3D.SetTrail` (graphics-disabled build).
///
/// @param o  Particles3D handle (ignored before trapping).
/// @param a1 Trail duration or length setting (ignored before trapping).
/// @param a2 Trail segment count (ignored before trapping).
void rt_particles3d_set_trail(void *o, double a1, int64_t a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_TRAP_VOID("Particles3D.SetTrail: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Skeleton3D.get_AliasCount` (graphics-disabled build).
///
/// @param o Skeleton3D handle (ignored).
///
/// @return `0`, indicating that no bone aliases are available.
int64_t rt_skeleton3d_get_alias_count(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Skeleton3D.get_AliasCount: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Skeleton3D.SetBoneAlias` (graphics-disabled build).
///
/// @param o  Skeleton3D handle (ignored before trapping).
/// @param a1 Alias name (ignored before trapping).
/// @param a2 Target bone name (ignored before trapping).
void rt_skeleton3d_set_bone_alias(void *o, rt_string a1, rt_string a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_TRAP_VOID("Skeleton3D.SetBoneAlias: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Sprite3D.get_Additive` (graphics-disabled build).
///
/// @param o Sprite3D handle (ignored).
///
/// @return `0`, indicating that additive blending is disabled.
int8_t rt_sprite3d_get_additive(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Sprite3D.get_Additive: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Sprite3D.set_Additive` (graphics-disabled build).
///
/// @param o  Sprite3D handle (ignored before trapping).
/// @param a1 Non-zero to enable additive blending (ignored before trapping).
void rt_sprite3d_set_additive(void *o, int8_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Sprite3D.set_Additive: graphics support not compiled in");
}

/// @brief Trapping stub for `Sprite3D.SetColor` (graphics-disabled build).
///
/// @param o  Sprite3D handle (ignored before trapping).
/// @param a1 Packed tint color (ignored before trapping).
void rt_sprite3d_set_color(void *o, int64_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("Sprite3D.SetColor: graphics support not compiled in");
}
