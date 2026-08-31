//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl.c
// Purpose: OpenGL 3.3 Core GPU backend for Zanna.Graphics3D (Linux).
//   Implements the vgfx3d_backend_t vtable on a GLX-bound OpenGL context,
//   with GLSL shaders compiled at runtime, FBO-based scene targets for
//   GPU postfx, and per-mesh VAO / VBO / IBO caching keyed by mesh
//   identity + revision.
//
// Key invariants:
//   - GL functions resolved at runtime via dlsym; no static GL linkage.
//   - GLSL 330 core shaders; shader compile failures fall back to software.
//   - Row-major matrices match Zanna convention; HLSL/MSL transposes are
//     handled by sister backends, not this one.
//   - Per-mesh GPU cache aged out via vgfx3d_opengl_should_prune_cache_entry.
//
// Ownership/Lifetime:
//   - GL textures, buffers, programs, FBOs, and per-mesh caches are owned
//     by the backend context and released in destroy_ctx.
//
// Links: vgfx3d_backend.h, vgfx3d_backend_opengl_shared.h, plans/3d/04-opengl-backend.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the dynamically loaded Linux OpenGL 3.3 Core backend for Graphics3D.
/// @details This translation unit defines the OpenGL/GLX dispatch surface, native binding helpers,
///          shared state utilities, shader compilation, capability probes, and the backend vtable.
///          Specialized frame, material, mesh, target, texture, and context operations are
///          assembled from the adjacent implementation fragments.

#if defined(__linux__) && defined(ZANNA_ENABLE_GRAPHICS)

#include "rt_alloc_size.h"
#include "rt_platform.h"
#include "rt_textureasset3d.h"
#include "vgfx.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_opengl_shared.h"
#include "vgfx3d_backend_utils.h"
#include "vgfx3d_brdf_lut.h"
#include "vgfx3d_egl_wayland.h"

#if !defined(ZANNA_GRAPHICS_WAYLAND)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#else
typedef void Display;
typedef unsigned long Window;
typedef struct vgfx_xvisual_info XVisualInfo;
#endif

#include <dlfcn.h>
#include <math.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef void GLvoid;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef unsigned int GLbitfield;
typedef void *GLsync;

#define VGFX3D_STR_IMPL(x) #x
#define VGFX3D_STR(x) VGFX3D_STR_IMPL(x)

/* ADR 0246: every shadow slot lives in one depth texture array on a single unit,
 * so all VGFX3D_MAX_SHADOW_LIGHTS slots (cascades and general/point-light atlas
 * slots alike) are samplable without extra bind points. Collapsing the former
 * per-slot units 4-7 freed three of GL 3.3's guaranteed 16 fragment units: the
 * BRDF LUT took one as a dedicated unit — it previously aliased splat layer 3,
 * which silently downgraded terrain draws to an analytic environment-BRDF — and
 * units 6-7 are spare. */
#define GL_TU_SHADOW_ARRAY 4
#define GL_TU_BRDF_LUT 5
#define GL_TU_SPLAT_CONTROL 8
#define GL_TU_SPLAT_LAYER0 9
#define GL_TU_ENV_MAP 13
#define GL_TU_METALLIC_ROUGHNESS 14
#define GL_TU_AO 15
#define GL_TU_MORPH_DELTAS 16
#define GL_TU_MORPH_NORMAL_DELTAS 17

/* Depth of the GPU-timestamp query ring. Three frames keeps a result available
 * without ever blocking on a query the GPU has not retired. */
#define VGFX3D_GL_TIMER_FRAMES 3

#define GL_TRUE 1
#define GL_FALSE 0
#define GL_NONE 0
#define GL_NO_ERROR 0
#define GL_INVALID_VALUE 0x0501
#define GL_OUT_OF_MEMORY 0x0505
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_BUFFER 0x0C02
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_DOUBLEBUFFER 0x0C32
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_GEQUAL 0x0206
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_R32F 0x822E
#define GL_RG 0x8227
#define GL_RG32F 0x8230
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_EXTENSIONS 0x1F03
#define GL_VERSION 0x1F02
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_TEXTURE_BUFFER_SIZE 0x8C2B
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_RED_RGTC1 0x8DBB
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR 0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR 0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR 0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR 0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#define GL_COMPRESSED_RGBA_ASTC_10x5_KHR 0x93B8
#define GL_COMPRESSED_RGBA_ASTC_10x6_KHR 0x93B9
#define GL_COMPRESSED_RGBA_ASTC_10x8_KHR 0x93BA
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR 0x93BB
#define GL_COMPRESSED_RGBA_ASTC_12x10_KHR 0x93BC
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR 0x93BD
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
/* Depth texture arrays back the unified shadow atlas (ADR 0246). GL 3.0 core. */
#define GL_TEXTURE_2D_ARRAY 0x8C1A
/* ARB_timer_query (core since GL 3.3) — GPU frame timing telemetry. */
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_TIMESTAMP 0x8E28
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_ONE 1
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_BUFFER 0x0C01
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR_ATTACHMENT15 0x8CEF
#define GL_COLOR_ATTACHMENT1 0x8CE1
#define GL_COLOR_ATTACHMENT2 0x8CE2
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
/* ARB_clip_control / GL 4.5 */
#define GL_LOWER_LEFT 0x8CA1
#define GL_ZERO_TO_ONE 0x935F

typedef void (*PFNGLCLIPCONTROLPROC)(GLenum, GLenum);
typedef void (*PFNGLCLEARPROC)(GLbitfield);
typedef void (*PFNGLCLEARCOLORPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLCLEARDEPTHPROC)(double);
typedef void (*PFNGLENABLEPROC)(GLenum);
typedef void (*PFNGLDISABLEPROC)(GLenum);
typedef void (*PFNGLDEPTHFUNCPROC)(GLenum);
typedef void (*PFNGLCULLFACEPROC)(GLenum);
typedef void (*PFNGLFRONTFACEPROC)(GLenum);
typedef void (*PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFNGLPOLYGONMODEPROC)(GLenum, GLenum);
typedef void (*PFNGLPOLYGONOFFSETPROC)(GLfloat, GLfloat);
typedef void (*PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const void *);
typedef void (*PFNGLDRAWARRAYSPROC)(GLenum, GLint, GLsizei);
typedef void (*PFNGLDRAWELEMENTSINSTANCEDPROC)(GLenum, GLsizei, GLenum, const void *, GLsizei);
typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum);
typedef void (*PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*PFNGLCOMPILESHADERPROC)(GLuint);
typedef void (*PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)(void);
typedef void (*PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (*PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (*PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*PFNGLPROGRAMPARAMETERIPROC)(GLuint, GLenum, GLint);
typedef void (*PFNGLGETPROGRAMBINARYPROC)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
typedef void (*PFNGLPROGRAMBINARYPROC)(GLuint, GLenum, const void *, GLsizei);
typedef void (*PFNGLUSEPROGRAMPROC)(GLuint);
typedef void (*PFNGLDELETESHADERPROC)(GLuint);
typedef void (*PFNGLDELETEPROGRAMPROC)(GLuint);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar *);
typedef GLuint (*PFNGLGETUNIFORMBLOCKINDEXPROC)(GLuint, const GLchar *);
typedef void (*PFNGLUNIFORMBLOCKBINDINGPROC)(GLuint, GLuint, GLuint);
typedef void (*PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (*PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (*PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void (*PFNGLUNIFORM1FVPROC)(GLint, GLsizei, const GLfloat *);
typedef void (*PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void (*PFNGLUNIFORM3FPROC)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLUNIFORM3FVPROC)(GLint, GLsizei, const GLfloat *);
typedef void (*PFNGLUNIFORM4FPROC)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLUNIFORM4IPROC)(GLint, GLint, GLint, GLint, GLint);
typedef void (*PFNGLUNIFORM4FVPROC)(GLint, GLsizei, const GLfloat *);
typedef void (*PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLBINDBUFFERBASEPROC)(GLenum, GLuint, GLuint);
typedef void (*PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFNGLBUFFERSUBDATAPROC)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(
    GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*PFNGLVERTEXATTRIBIPOINTERPROC)(GLuint, GLint, GLenum, GLsizei, const void *);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (*PFNGLDISABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (*PFNGLVERTEXATTRIBDIVISORPROC)(GLuint, GLuint);
typedef void (*PFNGLVERTEXATTRIB4FPROC)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBLENDFUNCPROC)(GLenum, GLenum);
typedef void (*PFNGLDEPTHMASKPROC)(GLboolean);
typedef void (*PFNGLCOLORMASKPROC)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (*PFNGLPIXELSTOREIPROC)(GLenum, GLint);
typedef void (*PFNGLTEXIMAGE2DPROC)(
    GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFNGLTEXSUBIMAGE2DPROC)(
    GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*PFNGLCOMPRESSEDTEXIMAGE2DPROC)(
    GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)(
    GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
typedef void (*PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (*PFNGLTEXPARAMETERFPROC)(GLenum, GLenum, GLfloat);
typedef void (*PFNGLTEXBUFFERPROC)(GLenum, GLenum, GLuint);
typedef void (*PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef void (*PFNGLGENSAMPLERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETESAMPLERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLSAMPLERPARAMETERIPROC)(GLuint, GLenum, GLint);
typedef void (*PFNGLSAMPLERPARAMETERFPROC)(GLuint, GLenum, GLfloat);
typedef void (*PFNGLBINDSAMPLERPROC)(GLuint, GLuint);
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(
    GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (*PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef void (*PFNGLREADPIXELSPROC)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void *(*PFNGLMAPBUFFERRANGEPROC)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (*PFNGLUNMAPBUFFERPROC)(GLenum);
typedef GLsync (*PFNGLFENCESYNCPROC)(GLenum, GLbitfield);
typedef GLenum (*PFNGLCLIENTWAITSYNCPROC)(GLsync, GLbitfield, uint64_t);
typedef void (*PFNGLDELETESYNCPROC)(GLsync);
typedef void (*PFNGLTEXIMAGE3DPROC)(
    GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFNGLFRAMEBUFFERTEXTURELAYERPROC)(GLenum, GLenum, GLuint, GLint, GLint);
typedef void (*PFNGLGENQUERIESPROC)(GLsizei, GLuint *);
typedef void (*PFNGLDELETEQUERIESPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLQUERYCOUNTERPROC)(GLuint, GLenum);
typedef void (*PFNGLGETQUERYOBJECTIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETQUERYOBJECTUI64VPROC)(GLuint, GLenum, uint64_t *);
typedef void (*PFNGLDRAWBUFFERPROC)(GLenum);
typedef void (*PFNGLDRAWBUFFERSPROC)(GLsizei, const GLenum *);
typedef void (*PFNGLREADBUFFERPROC)(GLenum);
typedef GLenum (*PFNGLGETERRORPROC)(void);
typedef const unsigned char *(*PFNGLGETSTRINGPROC)(GLenum);
typedef const unsigned char *(*PFNGLGETSTRINGIPROC)(GLenum, GLuint);
typedef void (*PFNGLGETINTEGERVPROC)(GLenum, GLint *);
typedef void (*PFNGLGETFLOATVPROC)(GLenum, GLfloat *);

static struct {
    void *lib;
    PFNGLGETERRORPROC GetError;
    PFNGLGETSTRINGPROC GetString;
    PFNGLGETSTRINGIPROC GetStringi;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETFLOATVPROC GetFloatv;
    PFNGLCLEARPROC Clear;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCLEARDEPTHPROC ClearDepth;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLDEPTHFUNCPROC DepthFunc;
    PFNGLCULLFACEPROC CullFace;
    PFNGLFRONTFACEPROC FrontFace;
    PFNGLVIEWPORTPROC Viewport;
    PFNGLPOLYGONMODEPROC PolygonMode;
    PFNGLPOLYGONOFFSETPROC PolygonOffset;
    PFNGLDRAWELEMENTSPROC DrawElements;
    PFNGLDRAWARRAYSPROC DrawArrays;
    PFNGLDRAWELEMENTSINSTANCEDPROC DrawElementsInstanced;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    /* Optional (GL >= 4.5 / ARB_clip_control); NULL when unavailable. */
    PFNGLCLIPCONTROLPROC ClipControl;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLPROGRAMPARAMETERIPROC ProgramParameteri;
    PFNGLGETPROGRAMBINARYPROC GetProgramBinary;
    PFNGLPROGRAMBINARYPROC ProgramBinary;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex;
    PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM1FPROC Uniform1f;
    PFNGLUNIFORM1FVPROC Uniform1fv;
    PFNGLUNIFORM2FPROC Uniform2f;
    PFNGLUNIFORM3FPROC Uniform3f;
    PFNGLUNIFORM3FVPROC Uniform3fv;
    PFNGLUNIFORM4FPROC Uniform4f;
    PFNGLUNIFORM4IPROC Uniform4i;
    PFNGLUNIFORM4FVPROC Uniform4fv;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBINDBUFFERBASEPROC BindBufferBase;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLBUFFERSUBDATAPROC BufferSubData;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLVERTEXATTRIBIPOINTERPROC VertexAttribIPointer;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor;
    PFNGLVERTEXATTRIB4FPROC VertexAttrib4f;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLBLENDFUNCPROC BlendFunc;
    PFNGLDEPTHMASKPROC DepthMask;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLPIXELSTOREIPROC PixelStorei;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
    PFNGLCOMPRESSEDTEXIMAGE2DPROC CompressedTexImage2D;
    PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC CompressedTexSubImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLTEXPARAMETERFPROC TexParameterf;
    PFNGLTEXBUFFERPROC TexBuffer;
    PFNGLGENERATEMIPMAPPROC GenerateMipmap;
    PFNGLGENSAMPLERSPROC GenSamplers;
    PFNGLDELETESAMPLERSPROC DeleteSamplers;
    PFNGLSAMPLERPARAMETERIPROC SamplerParameteri;
    PFNGLSAMPLERPARAMETERFPROC SamplerParameterf;
    PFNGLBINDSAMPLERPROC BindSampler;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLREADPIXELSPROC ReadPixels;
    PFNGLMAPBUFFERRANGEPROC MapBufferRange;
    PFNGLUNMAPBUFFERPROC UnmapBuffer;
    PFNGLFENCESYNCPROC FenceSync;
    PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
    PFNGLDELETESYNCPROC DeleteSync;
    /* GL 3.0 core — depth texture array storage and per-layer FBO attachment. */
    PFNGLTEXIMAGE3DPROC TexImage3D;
    PFNGLFRAMEBUFFERTEXTURELAYERPROC FramebufferTextureLayer;
    /* Optional (ARB_timer_query): NULL disables GPU frame timing telemetry. */
    PFNGLGENQUERIESPROC GenQueries;
    PFNGLDELETEQUERIESPROC DeleteQueries;
    PFNGLQUERYCOUNTERPROC QueryCounter;
    PFNGLGETQUERYOBJECTIVPROC GetQueryObjectiv;
    PFNGLGETQUERYOBJECTUI64VPROC GetQueryObjectui64v;
    PFNGLDRAWBUFFERPROC DrawBuffer;
    PFNGLDRAWBUFFERSPROC DrawBuffers;
    PFNGLREADBUFFERPROC ReadBuffer;
} gl;

static int gl_debug_enabled(void);

/* Debug GL error checking — enabled in debug builds only */
#ifndef NDEBUG
/// @brief Debug-only `glGetError()` wrapper that prints to stderr on failure.
///
/// Wrapped by the `GL_CHECK()` macro at every API call site in debug
/// builds; compiled out (`((void)0)`) in release. Skips silently if
/// the GL function-pointer table hasn't been loaded yet.
/// @param file Source filename reported for each drained GL error.
/// @param line Source line reported for each drained GL error.
static __attribute__((unused)) void gl_check_error(const char *file, int line) {
    if (!gl.GetError)
        return;
    for (;;) {
        GLenum err = gl.GetError();
        if (err == GL_NO_ERROR)
            break;
        fprintf(stderr, "GL error 0x%04X at %s:%d\n", (unsigned)err, file, line);
    }
}

#define GL_CHECK() gl_check_error(__FILE__, __LINE__)
#else
#define GL_CHECK() ((void)0)
#endif

/// @brief Drain the GL error queue and report whether it was clean.
/// @details This helper is intentionally available in release builds, unlike `GL_CHECK`, because
///          resource creation and readback paths must not cache or expose objects after a driver
///          error. Draining the queue also prevents an old error from poisoning a later operation.
/// @param label Optional operation label included in diagnostics when debug reporting is enabled.
/// @return Nonzero when the queue contained no errors, otherwise zero.
static int gl_drain_errors(const char *label) {
    int ok = 1;
    if (!gl.GetError)
        return 1;
    for (;;) {
        GLenum err = gl.GetError();
        if (err == GL_NO_ERROR)
            break;
        ok = 0;
#ifndef NDEBUG
        fprintf(stderr, "GL %s error 0x%04X\n", label ? label : "operation", (unsigned)err);
#else
        if (gl_debug_enabled())
            fprintf(stderr, "GL %s error 0x%04X\n", label ? label : "operation", (unsigned)err);
        (void)label;
#endif
    }
    return ok;
}

/// @brief Return whether the most recent upload/resource operation produced no GL errors.
/// @return Nonzero when draining the GL error queue finds no upload errors.
static int gl_upload_ok(void) {
    return gl_drain_errors("upload");
}

/// @brief Return whether runtime OpenGL diagnostics are enabled through the environment.
/// @details The `ZANNA_OPENGL_DEBUG` value is parsed once and cached for subsequent hot-path calls.
/// @return Nonzero when the variable is set to a nonempty value other than `"0"`.
static int gl_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("ZANNA_OPENGL_DEBUG");
        cached = (value && value[0] && strcmp(value, "0") != 0) ? 1 : 0;
    }
    return cached;
}

/// @brief Parse the Linux OpenGL presentation override.
/// @details The default is `auto`: the framebuffer writability probe decides, and a resolve that
///          fails at runtime demotes the context to the offscreen path (see
///          `gl_demote_to_offscreen_present`). Previously the default discarded the probe and
///          always took the offscreen route, which charged every X11/GLX frame a full-screen
///          `glReadPixels` plus a CPU blit even on stacks that present directly without issue.
///          `direct` forces native GLX/default-framebuffer presentation and skips the probe;
///          `offscreen` forces the compatibility resolve. Unknown values fall back to `auto`.
/// @return 0 for offscreen, 1 for auto/probe, 2 for direct.
static int gl_present_override_mode(void) {
    const char *value = getenv("ZANNA_OPENGL_PRESENT");
    if (!value || value[0] == '\0')
        return 1;
    if (strcmp(value, "auto") == 0 || strcmp(value, "probe") == 0)
        return 1;
    if (strcmp(value, "direct") == 0 || strcmp(value, "glx") == 0)
        return 2;
    if (strcmp(value, "offscreen") == 0 || strcmp(value, "vgfx") == 0 || strcmp(value, "0") == 0)
        return 0;
    return 0;
}

typedef void *GLXContext;
typedef unsigned long GLXDrawable;
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef void (*__GLXextFuncPtr)(void);
typedef GLXContext (*PFNGLXCREATENEWCONTEXTPROC)(Display *, GLXFBConfig, int, GLXContext, int);
typedef GLXFBConfig *(*PFNGLXCHOOSEFBCONFIGPROC)(Display *, int, const int *, int *);
typedef void (*PFNGLXSWAPBUFFERSPROC)(Display *, GLXDrawable);
typedef int (*PFNGLXMAKECURRENTPROC)(Display *, GLXDrawable, GLXContext);
typedef void (*PFNGLXDESTROYCONTEXTPROC)(Display *, GLXContext);
typedef __GLXextFuncPtr (*PFNGLXGETPROCADDRESSPROC)(const unsigned char *);
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC)(
    Display *, GLXFBConfig, GLXContext, int, const int *);
typedef XVisualInfo *(*PFNGLXGETVISUALFROMFBCONFIGPROC)(Display *, GLXFBConfig);

typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *, GLXDrawable, int);

static struct {
    PFNGLXCHOOSEFBCONFIGPROC ChooseFBConfig;
    PFNGLXCREATENEWCONTEXTPROC CreateNewContext;
    PFNGLXCREATECONTEXTATTRIBSARBPROC CreateContextAttribsARB;
    PFNGLXGETVISUALFROMFBCONFIGPROC GetVisualFromFBConfig;
    PFNGLXSWAPBUFFERSPROC SwapBuffers;
    PFNGLXMAKECURRENTPROC MakeCurrent;
    PFNGLXDESTROYCONTEXTPROC DestroyContext;
    PFNGLXGETPROCADDRESSPROC GetProcAddress;
    /* Optional (EXT_swap_control); NULL when the driver lacks it. */
    PFNGLXSWAPINTERVALEXTPROC SwapIntervalEXT;
} glx;

static int gl_loaded = 0;
static int gl_wayland_binding = 0;
static int gl_load_lock = 0;

/// @brief Acquire the process-global OpenGL dispatch table lock.
///
/// The OpenGL backend keeps a single libGL/GLX function table shared by every
/// Canvas3D context in the process. Backend creation can happen from more than
/// one thread, so the dispatch resolver must be serialized without adding a new
/// library dependency to the runtime. The platform atomic wrapper keeps this
/// code aligned with the portable runtime atomic policy.
static void gl_dispatch_lock(void) {
    int spins = 0;
    while (rt_atomic_test_and_set(&gl_load_lock, __ATOMIC_ACQUIRE)) {
        if (++spins >= 1024) {
            sched_yield();
            spins = 0;
        }
    }
}

/// @brief Release the process-global OpenGL dispatch table lock.
static void gl_dispatch_unlock(void) {
    rt_atomic_clear(&gl_load_lock, __ATOMIC_RELEASE);
}

/// @brief Reset the global libGL/GLX dispatch state after a failed dynamic-load attempt.
///
/// `load_gl()` resolves dozens of entry points in sequence. If any late lookup fails, leaving a
/// partially-populated table would make later backend initialization attempts unsafe. This helper
/// closes the library handle and clears every resolved pointer so the next attempt starts cleanly.
static void gl_unload_partial_dispatch(void) {
    if (gl.lib)
        dlclose(gl.lib);
    memset(&gl, 0, sizeof(gl));
    memset(&glx, 0, sizeof(glx));
    gl_loaded = 0;
    gl_wayland_binding = 0;
}

/// @brief Resolve a context-dispatched OpenGL entry point without binding it to GLX or EGL.
/// @details A process may create headless EGL and windowed GLX contexts in either order. Exported
/// libGL/GLVND symbols are provider-neutral dispatch thunks, so they are preferred; GLX and EGL
/// lookup are compatibility fallbacks. The cached table is therefore independent of which native
/// binding happened to initialize first.
static void *gl_resolve_context_proc(const char *name) {
    void *result;
    if (!name || !gl.lib)
        return NULL;
    result = dlsym(gl.lib, name);
#if !defined(ZANNA_GRAPHICS_WAYLAND)
    if (!result && glx.GetProcAddress)
        result = (void *)glx.GetProcAddress((const unsigned char *)name);
#endif
    if (!result && vgfx3d_egl_available())
        result = vgfx3d_egl_wayland_get_proc(name);
    return result;
}

#define GLX_RGBA_BIT 0x0001
#define GLX_RENDER_TYPE 0x8011
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_WINDOW_BIT 0x0001
#define GLX_DOUBLEBUFFER 5
#define GLX_DEPTH_SIZE 12
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_RGBA_TYPE 0x8014
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef struct {
    const void *pixels;
    void *texture_asset;
    uint64_t generation;
    uint64_t pending_generation;
    uint64_t failed_generation;
    int64_t failed_mip_start;
    int64_t failed_mip_count;
    GLuint tex;
    GLuint fallback_tex;
    int32_t width;
    int32_t height;
    int32_t upload_next_row;
    int32_t native_format;
    int32_t native_block_width;
    int32_t native_block_height;
    int32_t native_block_bytes;
    int32_t native_next_block_row;
    int64_t native_next_mip;
    int64_t native_mip_start;
    int64_t native_mip_count;
    int8_t upload_in_progress;
    uint64_t last_used_frame;
} gl_texture_cache_entry_t;

#define GL_TEXTURE_CACHE_HINT_CAPACITY 512
_Static_assert((GL_TEXTURE_CACHE_HINT_CAPACITY & (GL_TEXTURE_CACHE_HINT_CAPACITY - 1)) == 0,
               "texture-cache hint capacity must be a power of two");

typedef struct {
    const void *cubemap;
    uint64_t generation;
    uint64_t pending_generation;
    uint64_t failed_generation;
    GLuint tex;
    GLuint fallback_tex;
    int32_t face_size;
    int32_t upload_face;
    int32_t upload_next_row;
    int8_t upload_in_progress;
    uint64_t last_used_frame;
    uint64_t applied_ibl_identity; /* prefiltered-mips overlay applied for this IBL payload */
    uint64_t failed_ibl_identity;  /* invalid/failed overlay suppressed until identity changes */
    uint64_t pending_ibl_identity; /* validated overlay waiting for the frame upload budget */
    uint64_t pending_ibl_bytes;
} gl_cubemap_cache_entry_t;

typedef struct {
    const void *key;
    uint64_t revision;
    int32_t shape_count;
    uint32_t vertex_count;
    int8_t has_normal_deltas;
    GLuint morph_buffer;
    GLuint morph_tbo;
    GLuint morph_normal_buffer;
    GLuint morph_normal_tbo;
    size_t morph_bytes;
    size_t morph_normal_bytes;
    uint64_t last_used_frame;
} gl_morph_cache_entry_t;

#define GL_MESH_CACHE_CAPACITY 256

typedef struct {
    const void *key;
    uint32_t revision;
    uint32_t vertex_count;
    uint32_t index_count;
    GLuint vbo;
    GLuint ibo;
    uint64_t last_used_frame;
    int8_t compact; /* R20: VBO holds the packed 48-byte vertex encoding */
} gl_mesh_cache_entry_t;

typedef struct {
    Display *display;
    Window window;
    vgfx_window_t vgfx_win;
    GLXContext glxCtx;
    vgfx3d_egl_wayland_t *egl_binding;

    GLuint program;
    GLuint unlit_program;
    GLuint shadow_program;
    GLuint postfx_program;
    GLuint skybox_program;
    GLuint bloom_down_program;
    GLuint bloom_up_program;
    GLuint taa_program;
    GLuint vao;
    GLuint fullscreen_vao;
    GLuint fullscreen_vbo;
    GLuint skybox_vao;
    GLuint skybox_vbo;
    GLuint default_white_tex;
    GLuint brdf_lut_tex;
    GLuint default_white_cubemap;

    GLuint mesh_vbo;
    GLuint mesh_ibo;
    size_t mesh_vbo_capacity;
    size_t mesh_ibo_capacity;

    GLuint instance_vbo;
    size_t instance_vbo_capacity;
    GLuint prev_instance_vbo;
    size_t prev_instance_vbo_capacity;

    GLuint bone_ubo;
    GLuint prev_bone_ubo;
    /* Plan 07: clustered forward+ froxel table (u16 data packed as std140 uvec4). */
    GLuint cluster_offsets_ubo;
    GLuint cluster_indices_ubo;
    /* Plan 10: opaque-pass depth snapshot (blitted at the opaque->transparent
     * seam) for soft particles; valid resets every begin_frame. */
    GLuint opaque_depth_tex;
    GLuint opaque_depth_fbo;
    int32_t opaque_depth_w, opaque_depth_h;
    int8_t opaque_depth_valid;
    float cam_znear, cam_zfar;

    GLuint morph_buffer;
    GLuint morph_tbo;
    size_t morph_capacity_bytes;

    GLuint morph_normal_buffer;
    GLuint morph_normal_tbo;
    size_t morph_normal_capacity_bytes;

    GLuint scene_fbo;
    GLuint scene_color_tex;
    GLuint scene_motion_tex;
    GLuint scene_postfx_tex;
    GLuint scene_depth_tex;
    int32_t scene_width;
    int32_t scene_height;

    GLuint postfx_readback_fbo;
    GLuint postfx_readback_tex;
    int32_t postfx_readback_width;
    int32_t postfx_readback_height;
    int8_t postfx_readback_hdr;
    GLuint postfx_scratch_fbo;
    GLuint postfx_scratch_tex;
    int32_t postfx_scratch_width;
    int32_t postfx_scratch_height;
    int8_t postfx_scratch_hdr;
    GLuint present_fbo;
    GLuint present_tex;
    int32_t present_width;
    int32_t present_height;
    uint8_t *readback_scratch_rgba;
    size_t readback_scratch_bytes;

    /* Plan 05: half-res RGBA16F bloom mip chain (built lazily per scene size). */
    GLuint bloom_mip_tex[6];
    GLuint bloom_mip_fbo[6];
    int32_t bloom_mip_w[6];
    int32_t bloom_mip_h[6];
    int32_t bloom_mip_count;
    int32_t bloom_base_width;
    int32_t bloom_base_height;
    int8_t bloom_hdr;
    /* Transient: mip-0 result bound as uBloomTex while a chain pass composites bloom. */
    GLuint postfx_current_bloom_tex;
    /* Plan 61: cached 256x16 COLOR_LUT strip; (key, revision) mirror the
     * snapshot's (texel pointer, pixels generation) so re-authored LUTs
     * re-upload. */
    GLuint postfx_lut_tex;
    uintptr_t postfx_lut_key;
    uint64_t postfx_lut_revision;
    /* Plan 05: TAA ping-pong history (RGBA16F, persisted across frames) + jitter state. */
    GLuint taa_history_tex[2];
    GLuint taa_history_fbo[2];
    int32_t taa_history_width;
    int32_t taa_history_height;
    int32_t taa_history_parity;
    int8_t taa_history_valid;
    int8_t taa_history_hdr;
    int8_t scene_hdr_active;
    float taa_jitter_clip[2];
    float taa_prev_jitter_clip[2];
    uint32_t taa_frame_index;

    GLuint rtt_fbo;
    GLuint rtt_color_tex;
    GLuint rtt_depth_rbo;
    int32_t rtt_width;
    int32_t rtt_height;
    int32_t rtt_color_format;
    int8_t rtt_active;
    vgfx3d_rendertarget_t *rtt_target;

    GLuint shadow_fbo[VGFX3D_MAX_SHADOW_LIGHTS];
    /* ADR 0246: one GL_TEXTURE_2D_ARRAY depth texture, layer per shadow slot.
     * Array layers share one size, so every slot renders at the same resolution
     * — Canvas3D already drives them all from a single shadow_resolution, and a
     * request at a different size reallocates the array and invalidates the
     * slots, which the shadow_complete[] reset below records. */
    GLuint shadow_array_tex;
    int32_t shadow_array_width;
    int32_t shadow_array_height;
    int32_t shadow_width[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_height[VGFX3D_MAX_SHADOW_LIGHTS];
    int32_t shadow_pass_slot;
    int8_t shadow_pass_failed;
    int32_t shadow_count;
    int8_t shadow_complete[VGFX3D_MAX_SHADOW_LIGHTS];
    float shadow_bias;
    float shadow_vp[VGFX3D_MAX_SHADOW_LIGHTS][16];
    /* Plan 06: per-frame shadow filtering params from camera params. */
    float shadow_strength;
    float shadow_slope_bias;
    int32_t shadow_quality;
    GLuint material_samplers[3][3][12][VGFX3D_OPENGL_ANISOTROPY_LEVEL_COUNT];
    int8_t anisotropy_supported;
    GLfloat max_texture_anisotropy;

    gl_texture_cache_entry_t *texture_cache;
    int32_t texture_cache_count;
    int32_t texture_cache_capacity;
    int32_t texture_cache_hints[GL_TEXTURE_CACHE_HINT_CAPACITY]; /* entry index + 1 */
    gl_cubemap_cache_entry_t *cubemap_cache;
    int32_t cubemap_cache_count;
    int32_t cubemap_cache_capacity;
    gl_morph_cache_entry_t *morph_cache;
    int32_t morph_cache_count;
    int32_t morph_cache_capacity;
    uint64_t *cache_age_scratch;
    int32_t cache_age_scratch_capacity;
    gl_mesh_cache_entry_t mesh_cache[GL_MESH_CACHE_CAPACITY];
    uint64_t frame_serial;
    uint64_t texture_upload_bytes;
    uint64_t texture_upload_budget_bytes;
    uint8_t *texture_upload_scratch_rgba;
    size_t texture_upload_scratch_bytes;
    int32_t gl_major_version;
    int32_t gl_minor_version;
    int32_t max_texture_size;
    int32_t max_vertex_attribs;
    int32_t max_combined_texture_units;
    int32_t max_vertex_texture_units;
    int32_t max_texture_buffer_size;
    int8_t zero_to_one_clip;
    int8_t supports_hdr_color_target;
    int8_t supports_depth_float_target;
    int8_t supports_bc7;
    int8_t supports_astc;
    int8_t supports_etc2;

    int32_t width;
    int32_t height;
    float render_scale;
    float view[16];
    float projection[16];
    float vp[16];
    float inv_vp[16];
    float draw_prev_vp[16];
    float scene_vp[16];
    float scene_prev_vp[16];
    float scene_inv_vp[16];
    int8_t scene_history_valid;
    float cam_pos[3];
    float cam_forward[3];
    int8_t cam_is_ortho;
    float scene_cam_pos[3];
    int8_t fog_enabled;
    float fog_near;
    float fog_far;
    float fog_color[3];
    float height_fog[4];         /* base, falloff, density*blend (0 = off), pad */
    float height_fog_sun[4];     /* sun tint rgb + amount (0 = off) */
    float height_fog_sun_dir[4]; /* direction toward the sun + power */
    int8_t ibl_enabled;
    float ibl_intensity;
    float ibl_sh[27];
    uint32_t uploaded_lights_revision; /* last light snapshot sent to the main program */
    /* Plan 07: cluster tables are camera-dependent, so unlike the light snapshot
     * this key is dropped every begin_frame (same revision != same froxels). */
    uint32_t uploaded_cluster_revision;
    float clearR, clearG, clearB;
    int8_t current_pass_is_overlay;
    int8_t gpu_postfx_enabled;
    int8_t gpu_postfx_chain_valid;
    int8_t frame_active;
    int8_t rtt_color_dirty;
    int8_t scene_postfx_pending;
    int8_t scene_composited_to_backbuffer;
    /// Nonzero when the caller asked every presented frame to stay readable.
    int8_t capture_after_present;
    /// Nonzero while `present_fbo` holds the copy taken at the last present.
    int8_t present_capture_valid;
    int8_t default_doublebuffered;
    int8_t default_framebuffer_writable;
    GLenum default_draw_buffer;
    int32_t present_path;
    vgfx3d_backend_stats_t stats;
    vgfx3d_opengl_target_kind_t active_target_kind;
    vgfx3d_postfx_chain_t gpu_postfx_chain;
    GLint unlit_uniform_locations[2048];
    size_t main_uniform_location_bytes;

    GLint uModelMatrix, uPrevModelMatrix, uViewProjection, uPrevViewProjection, uNormalMatrix;
    GLint uCameraPos, uCameraForward, uAmbientColor, uDiffuseColor, uSpecularColor, uEmissiveColor,
        uAlpha;
    GLint uPbrScalars0, uPbrScalars1;
    GLint uUnlit, uShadingModel, uLightCount, uHasTexture, uHasNormalMap, uHasSpecularMap,
        uHasEmissiveMap;
    GLint uHasEnvMap, uReflectivity, uEnvMaxLod, uWorkflow, uAlphaMode, uHasMetallicRoughnessMap,
        uHasAOMap, uCameraIsOrtho;
    GLint uCustomParams;
    GLint uHasSplat, uFogEnabled, uFogNear, uFogFar, uFogColor, uHeightFog;
    GLint uHeightFogSun, uHeightFogSunDir;
    GLint uIblEnabled, uIblIntensity, uEnvLodBase, uShIrradiance;
    GLint uTextureUvSets0, uTextureUvSets1;
    GLint uTextureUvTransform0, uTextureUvTransform1;
    GLint uShadowCount, uShadowBias;
    GLint uShadowSlopeBias, uShadowStrength, uShadowSampleCount;
    GLint uClusterGlobalCount, uClusterParams;
    GLint uOpaqueDepthTex;
    GLint uUseInstancing, uParticleMode, uHasSkinning, uMorphShapeCount, uVertexCount;
    GLint uInstanceBoneStride;
    GLint uHasPrevModelMatrix, uHasPrevInstanceMatrices, uHasPrevSkinning, uHasPrevMorphWeights;
    GLint uMorphWeights, uPrevMorphWeights, uMorphDeltas, uMorphNormalDeltas, uHasMorphNormalDeltas;
    GLint uDiffuseTex, uNormalTex, uSpecularTex, uEmissiveTex, uShadowArray, uEnvMap, uBrdfLut;
    GLint uMetallicRoughnessTex, uAOTex;
    GLint uSplatTex, uSplatLayer0, uSplatLayer1, uSplatLayer2, uSplatLayer3, uSplatScales;
    GLint uLightType[VGFX3D_MAX_LIGHTS], uLightShadowIndex[VGFX3D_MAX_LIGHTS],
        uLightShadowProjectionType[VGFX3D_MAX_LIGHTS], uLightDir[VGFX3D_MAX_LIGHTS],
        uLightPos[VGFX3D_MAX_LIGHTS], uLightColor[VGFX3D_MAX_LIGHTS],
        uLightIntensity[VGFX3D_MAX_LIGHTS];
    GLint uLightShadowCascadeCount[VGFX3D_MAX_LIGHTS], uLightShadowCascadeSplits[VGFX3D_MAX_LIGHTS];
    GLint uShadowVP[VGFX3D_MAX_SHADOW_LIGHTS];
    GLint uLightAtten[VGFX3D_MAX_LIGHTS], uLightInnerCos[VGFX3D_MAX_LIGHTS],
        uLightOuterCos[VGFX3D_MAX_LIGHTS];

    GLint shadow_uModelMatrix, shadow_uViewProjection;
    GLint shadow_uHasSkinning, shadow_uMorphShapeCount, shadow_uVertexCount;
    GLint shadow_uMorphWeights, shadow_uMorphDeltas;
    GLint shadow_uDiffuseTex, shadow_uHasTexture, shadow_uAlphaMode, shadow_uAlphaCutoff,
        shadow_uAlpha, shadow_uDiffuseColor;
    GLint shadow_uTextureUvSets0, shadow_uTextureUvSets1;
    GLint shadow_uTextureUvTransform0, shadow_uTextureUvTransform1;

    GLint skybox_uInverseProjection;
    GLint skybox_uInverseViewRotation;
    GLint skybox_uDirection;
    GLint skybox_uIsOrtho;
    GLint skybox_uSkybox;

    GLint postfx_uSceneTex, postfx_uSceneDepthTex, postfx_uSceneMotionTex, postfx_uInvResolution;
    GLint postfx_uBloomEnabled, postfx_uBloomThreshold, postfx_uBloomIntensity, postfx_uBloomPasses;
    GLint postfx_uTonemapMode, postfx_uTonemapExposure, postfx_uFxaaEnabled;
    GLint postfx_uColorGradeEnabled, postfx_uCgBrightness, postfx_uCgContrast, postfx_uCgSaturation;
    GLint postfx_uVignetteEnabled, postfx_uVignetteRadius, postfx_uVignetteSoftness;
    GLint postfx_uSsaoEnabled, postfx_uSsaoRadius, postfx_uSsaoIntensity, postfx_uSsaoSamples;
    GLint postfx_uDofEnabled, postfx_uDofFocusDistance, postfx_uDofAperture, postfx_uDofMaxBlur;
    GLint postfx_uMotionBlurEnabled, postfx_uMotionBlurIntensity, postfx_uMotionBlurSamples;
    GLint postfx_uCameraPos, postfx_uInvViewProjection, postfx_uPrevViewProjection;
    GLint postfx_uSceneHdr, postfx_uTonemapExplicit, postfx_uBloomTex, postfx_uBloomTexEnabled;
    /* Plan 61: COLOR_LUT pass uniforms. */
    GLint postfx_uLutEnabled, postfx_uLutBlend, postfx_uLutTex;
    /* Plan 61 / ADR 0271: sharpen pass uniforms. */
    GLint postfx_uSharpenEnabled, postfx_uSharpenAmount;

    GLint bloom_down_uSrcTex, bloom_down_uSrcInvSize, bloom_down_uThreshold, bloom_down_uFirstPass;
    GLint bloom_up_uSrcTex, bloom_up_uSrcInvSize;
    GLint taa_uCurrTex, taa_uHistTex, taa_uMotionTex, taa_uDepthTex;
    GLint taa_uInvResolution, taa_uJitterDelta, taa_uBlend, taa_uHistoryValid;
    GLint taa_uInvViewProjection, taa_uPrevViewProjection;
    /* Plan 10: SSR pass (program + scene-sized RGBA8 output target). */
    GLuint ssr_program;
    GLuint ssr_tex;
    GLuint ssr_fbo;
    int32_t ssr_width, ssr_height;
    int8_t ssr_hdr;
    GLint ssr_uSceneTex, ssr_uDepthTex, ssr_uMotionTex;
    GLint ssr_uInvResolution, ssr_uParams0, ssr_uCamPos;
    GLint ssr_uInvViewProjection, ssr_uViewProjection;
    /* Scene-depth probes (lens flares): requests queued during the frame are read
     * into a PBO at end_frame; the previous frame's PBO contents are harvested first,
     * so reads carry one frame of latency and never force a pipeline sync. */
    float depth_probe_requests[VGFX3D_DEPTH_PROBE_MAX][2];
    int32_t depth_probe_request_count;
    GLuint depth_probe_pbo;
    /* GAP-9 GPU frame timing. A ring of GL_TIMESTAMP query pairs so a result is
     * harvested only once the GPU has retired it — reading the current frame's
     * pair back immediately would stall the pipeline it is meant to measure.
     * `timer_slot_active` is the slot holding an issued-but-unterminated start,
     * or -1; a frame that never presents simply has its start overwritten. */
    GLuint timer_query[VGFX3D_GL_TIMER_FRAMES][2];
    int8_t timer_query_pending[VGFX3D_GL_TIMER_FRAMES];
    int32_t timer_query_head;
    int32_t timer_slot_active;
    int8_t timer_queries_ready;
    uint64_t frame_gpu_time_us;

    GLsync depth_probe_fence;
    int32_t depth_probe_pending_count;
    uint32_t depth_probe_pending_polls;
    float depth_probe_results[VGFX3D_DEPTH_PROBE_MAX];
    int32_t depth_probe_result_count;
} gl_context_t;

/// @brief Make the context's EGL/Wayland or GLX binding current on the calling thread.
/// @param ctx Borrowed OpenGL backend context containing the native binding.
/// @return Nonzero when the binding became current, otherwise zero.
static int gl_make_current(gl_context_t *ctx) {
    if (!ctx)
        return 0;
    if (ctx->egl_binding)
        return vgfx3d_egl_wayland_make_current(ctx->egl_binding);
    return ctx->display && ctx->window && ctx->glxCtx &&
           glx.MakeCurrent(ctx->display, ctx->window, ctx->glxCtx);
}

/// @brief Destroy the context's EGL/Wayland or GLX native binding.
/// @param ctx Borrowed OpenGL backend context whose native handles are released and cleared.
static void gl_destroy_native_binding(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->egl_binding) {
        vgfx3d_egl_wayland_destroy(ctx->egl_binding);
        ctx->egl_binding = NULL;
    } else if (ctx->glxCtx && ctx->display) {
        (void)glx.MakeCurrent(ctx->display, 0, NULL);
        glx.DestroyContext(ctx->display, ctx->glxCtx);
        ctx->glxCtx = NULL;
    }
}

/// @brief Present the current native surface through EGL/Wayland or GLX.
/// @param ctx Borrowed OpenGL backend context containing the active native binding.
static void gl_swap_native_buffers(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->egl_binding)
        (void)vgfx3d_egl_wayland_swap(ctx->egl_binding);
    else
        glx.SwapBuffers(ctx->display, ctx->window);
}

/// @brief Return whether the next OpenGL window scene uses a reduced render extent.
/// @details RTT selection remains independent and higher priority; the stored scale can therefore
///          rebuild window resources correctly even while an explicit target is temporarily
///          bound.
/// @param ctx Borrowed OpenGL backend context.
/// @return Non-zero for a finite scale in `[0.25, 0.999)`.
static int gl_render_scale_active(const gl_context_t *ctx) {
    return ctx && isfinite(ctx->render_scale) && ctx->render_scale >= 0.25f &&
           ctx->render_scale < 0.999f;
}

/// @brief Return whether OpenGL window rendering needs an offscreen scene and final composite.
/// @details Post-processing and reduced render scale share one route; actual effect execution
///          continues to depend only on `gpu_postfx_enabled` and the latched chain.
/// @param ctx Borrowed OpenGL backend context.
/// @return Non-zero when either feature requires the offscreen scene route.
static int8_t gl_window_scene_route_enabled(const gl_context_t *ctx) {
    return (ctx && (ctx->gpu_postfx_enabled || gl_render_scale_active(ctx))) ? 1 : 0;
}

static void query_main_uniforms(gl_context_t *ctx);
static int gl_query_unlit_uniforms(gl_context_t *ctx);
static void gl_save_main_uniform_locations(gl_context_t *ctx, GLint *storage);
static void gl_load_main_uniform_locations(gl_context_t *ctx, const GLint *storage);
static int gl_begin_material_program(gl_context_t *ctx,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     GLint *saved_locations,
                                     GLuint *saved_program);
static void gl_end_material_program(gl_context_t *ctx,
                                    int specialized,
                                    const GLint *saved_locations,
                                    GLuint saved_program);
static void query_shadow_uniforms(gl_context_t *ctx);
static void query_skybox_uniforms(gl_context_t *ctx);
static void query_postfx_uniforms(gl_context_t *ctx);
static void upload_main_uniforms(gl_context_t *ctx,
                                 const vgfx3d_draw_cmd_t *cmd,
                                 const vgfx3d_light_params_t *lights,
                                 int32_t light_count,
                                 const float *ambient,
                                 int8_t instanced);
static void bind_material_textures(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd);
static void bind_shadow_anim(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd);
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
                               GLint uMorphNormalDeltas);
static void texture_cache_prune(gl_context_t *ctx);
static void cubemap_cache_prune(gl_context_t *ctx);
static void morph_cache_prune(gl_context_t *ctx);
static void morph_cache_release_entry(gl_morph_cache_entry_t *entry);
static void texture_cache_destroy(gl_context_t *ctx);
static GLuint gl_get_cached_texture(gl_context_t *ctx, const void *pixels_ptr);
static GLuint gl_get_cached_cubemap(gl_context_t *ctx, const rt_cubemap3d *cubemap);
static float gl_cubemap_max_lod(const rt_cubemap3d *cubemap);
static GLuint gl_get_material_texture(gl_context_t *ctx,
                                      void *asset,
                                      const void *pixels,
                                      uint64_t asset_cache_key,
                                      int64_t mip_start,
                                      int64_t mip_count);
static void gl_destroy_mesh_cache(gl_context_t *ctx);
static void gl_mesh_cache_prune(gl_context_t *ctx);
static void set_identity_instance_constants(void);
static void configure_mesh_attributes(gl_context_t *ctx,
                                      GLuint mesh_vbo,
                                      GLuint mesh_ibo,
                                      int compact);
static int configure_instance_attributes(gl_context_t *ctx,
                                         const float *instance_matrices,
                                         const float *prev_instance_matrices,
                                         int32_t instance_count);
static int configure_particle_instance_attributes(gl_context_t *ctx,
                                                  const vgfx3d_particle_instance_t *instances,
                                                  int32_t instance_count);
static int prepare_mesh_buffers(gl_context_t *ctx,
                                const vgfx3d_draw_cmd_t *cmd,
                                GLuint *out_vbo,
                                GLuint *out_ibo,
                                int *out_compact);
static int draw_scene_texture(gl_context_t *ctx, const vgfx3d_postfx_chain_t *chain);
static void gl_draw_skybox_impl(gl_context_t *ctx, const rt_cubemap3d *cubemap);
static void destroy_scene_targets(gl_context_t *ctx);
static void destroy_postfx_readback_target(gl_context_t *ctx);
static void destroy_postfx_scratch_target(gl_context_t *ctx);
static void destroy_present_target(gl_context_t *ctx);
static int ensure_present_target(gl_context_t *ctx, int32_t w, int32_t h);
static int ensure_scene_targets(gl_context_t *ctx, int32_t w, int32_t h);
static void destroy_rtt_targets(gl_context_t *ctx);
static int ensure_rtt_targets(gl_context_t *ctx, vgfx3d_rendertarget_t *rt);
static int gl_sync_render_target_color(void *ctx_ptr, vgfx3d_rendertarget_t *rt);
static void destroy_shadow_targets(gl_context_t *ctx);
static int ensure_shadow_targets(gl_context_t *ctx, int32_t slot, int32_t w, int32_t h);
static void gl_recompute_shadow_count(gl_context_t *ctx);
static int32_t gl_sanitize_shadow_slot(gl_context_t *ctx, int32_t shadow_index);
static void bind_main_framebuffer(gl_context_t *ctx);
static void gl_configure_draw_output(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd);
static void gl_apply_depth_bias(const vgfx3d_draw_cmd_t *cmd, int reversed_z);
static void bind_texture_unit(GLint uniform_loc, int unit, GLenum target, GLuint texture);
static void bind_texture_unit_with_sampler(gl_context_t *ctx,
                                           GLint uniform_loc,
                                           int unit,
                                           GLenum target,
                                           GLuint texture,
                                           const vgfx3d_draw_cmd_t *cmd,
                                           int32_t slot);
static int gl_draw_texture_to_target(gl_context_t *ctx,
                                     GLuint source_color_tex,
                                     GLuint framebuffer,
                                     GLenum draw_buffer,
                                     int32_t source_width,
                                     int32_t source_height,
                                     int32_t target_width,
                                     int32_t target_height,
                                     const vgfx3d_postfx_snapshot_t *snapshot);
static int ensure_postfx_readback_target(gl_context_t *ctx, int32_t w, int32_t h);
static int gl_apply_postfx_chain(gl_context_t *ctx,
                                 GLuint source_tex,
                                 int32_t width,
                                 int32_t height,
                                 const vgfx3d_postfx_chain_t *chain,
                                 GLuint final_framebuffer,
                                 GLenum final_draw_buffer,
                                 int32_t final_width,
                                 int32_t final_height,
                                 int force_offscreen_final,
                                 GLuint *out_result_framebuffer,
                                 GLenum *out_result_read_buffer);
static int gl_apply_postfx_chain_in_scene(gl_context_t *ctx,
                                          int32_t width,
                                          int32_t height,
                                          const vgfx3d_postfx_chain_t *chain);
static void query_bloom_taa_uniforms(gl_context_t *ctx);
static void destroy_bloom_targets(gl_context_t *ctx);
static void destroy_taa_targets(gl_context_t *ctx);
static void destroy_ssr_target(gl_context_t *ctx);
static int ensure_bloom_targets(gl_context_t *ctx, int32_t w, int32_t h);
static int ensure_taa_targets(gl_context_t *ctx, int32_t w, int32_t h);
static int ensure_ssr_target(gl_context_t *ctx, int32_t w, int32_t h);
static GLuint gl_encode_bloom_chain(
    gl_context_t *ctx, GLuint source_tex, int32_t width, int32_t height, float threshold);
static GLuint gl_encode_taa_pass(gl_context_t *ctx,
                                 GLuint source_tex,
                                 int32_t width,
                                 int32_t height,
                                 const vgfx3d_postfx_snapshot_t *snapshot);

/// @brief Snapshot of framebuffer, viewport, and read/draw buffer state.
///
/// OpenGL state is global to the context. Backend helper passes such as
/// readback, post-FX composition, and FBO capability probes temporarily bind
/// different framebuffers and buffers. This compact state record lets those
/// helpers restore the caller's binding instead of approximating it through
/// `bind_main_framebuffer`, which can be wrong for nested operations.
typedef struct {
    GLint draw_framebuffer;
    GLint read_framebuffer;
    GLint draw_buffer;
    GLint read_buffer;
    GLint viewport[4];
    GLint pack_alignment;
    GLint unpack_alignment;
    GLint scissor_test;
    GLint active_texture;
    GLint texture_binding_2d;
    GLint renderbuffer;
} gl_framebuffer_state_t;

/// @brief Capture the current framebuffer/read/draw/viewport state.
/// @param[out] state Caller-owned snapshot receiving framebuffer, viewport, and pixel alignment.
static void gl_capture_framebuffer_state(gl_framebuffer_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_framebuffer);
    gl.GetIntegerv(GL_DRAW_BUFFER, &state->draw_buffer);
    gl.GetIntegerv(GL_READ_BUFFER, &state->read_buffer);
    gl.GetIntegerv(GL_VIEWPORT, state->viewport);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &state->pack_alignment);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
    gl.GetIntegerv(GL_SCISSOR_TEST, &state->scissor_test);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture_binding_2d);
    gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &state->renderbuffer);
}

/// @brief Restore framebuffer/read/draw/viewport state captured by
/// `gl_capture_framebuffer_state`.
/// @param state Borrowed state snapshot previously populated by gl_capture_framebuffer_state().
/// @brief Report whether a draw/read buffer token is legal for the given framebuffer.
/// @details The default framebuffer names its buffers (`GL_BACK`, `GL_FRONT`, …) while an FBO
///   names attachments (`GL_COLOR_ATTACHMENT0`+). Feeding one an enum from the other's namespace
///   is `GL_INVALID_OPERATION`, and a captured snapshot can legitimately pair an FBO binding with
///   a default-framebuffer token when the two were queried across a binding change. Restoring
///   such a pair blindly poisoned the error queue for every later operation.
/// @param framebuffer Framebuffer name the buffer token would be applied to.
/// @param buffer Draw or read buffer token from a captured snapshot.
/// @return Nonzero when the token is valid for that framebuffer.
static int gl_buffer_token_valid_for(GLuint framebuffer, GLenum buffer) {
    if (buffer == GL_NONE)
        return 1;
    if (framebuffer == 0)
        return buffer != GL_NONE &&
               (buffer < GL_COLOR_ATTACHMENT0 || buffer > GL_COLOR_ATTACHMENT15);
    return buffer >= GL_COLOR_ATTACHMENT0 && buffer <= GL_COLOR_ATTACHMENT15;
}

static void gl_restore_framebuffer_state(const gl_framebuffer_state_t *state) {
    if (!state)
        return;
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)state->draw_framebuffer);
    if (gl_buffer_token_valid_for((GLuint)state->draw_framebuffer, (GLenum)state->draw_buffer))
        gl.DrawBuffer((GLenum)state->draw_buffer);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)state->read_framebuffer);
    if (gl_buffer_token_valid_for((GLuint)state->read_framebuffer, (GLenum)state->read_buffer))
        gl.ReadBuffer((GLenum)state->read_buffer);
    gl.Viewport(state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
    gl.PixelStorei(GL_PACK_ALIGNMENT, state->pack_alignment);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, state->unpack_alignment);
    if (state->scissor_test)
        gl.Enable(GL_SCISSOR_TEST);
    else
        gl.Disable(GL_SCISSOR_TEST);
    gl.ActiveTexture((GLenum)state->active_texture);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)state->texture_binding_2d);
    gl.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)state->renderbuffer);
}

/// @brief Restore the default draw state expected after helper full-screen passes.
///
/// Post-FX and readback helpers disable depth/cull/blend and bind their own
/// program/VAO. Calling this before returning to normal scene or overlay drawing
/// keeps later submissions from inheriting helper-pass state.
/// @param ctx Borrowed OpenGL context whose primary program and VAO are rebound.
static void gl_restore_main_draw_state(gl_context_t *ctx) {
    if (!ctx)
        return;
    gl.Disable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Enable(GL_DEPTH_TEST);
    /* Reversed-Z initial state; scene draws pin GL_GREATER per draw. */
    gl.DepthFunc(GL_GEQUAL);
    gl.DepthMask(GL_TRUE);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.Enable(GL_CULL_FACE);
    gl.CullFace(GL_BACK);
    gl.FrontFace(GL_CCW);
    gl.Disable(GL_POLYGON_OFFSET_FILL);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.UseProgram(ctx->program);
    gl.BindVertexArray(ctx->vao);
}

/// @brief Return the texture name to bind when a material texture is not ready.
///
/// Budgeted streaming can leave a newly requested texture incomplete for a few
/// frames. Binding a known 1x1 white texture preserves material color and avoids
/// shader feature toggles flickering off while the real image upload catches up.
/// @param ctx Borrowed OpenGL context containing the default texture name.
/// @return The context's white 2D texture name, or zero for a NULL context.
static GLuint gl_fallback_white_texture(const gl_context_t *ctx) {
    return ctx ? ctx->default_white_tex : 0;
}

/// @brief Return the cubemap texture name to bind while an environment map is pending.
///
/// The cubemap cache uploads face rows over multiple frames. Binding a valid
/// white cubemap while that upload is incomplete keeps skybox and image-based
/// material paths from sampling an uninitialized texture object.
/// @param ctx Borrowed OpenGL context containing the default cubemap name.
/// @return The context's white cubemap texture name, or zero for a NULL context.
static GLuint gl_fallback_white_cubemap(const gl_context_t *ctx) {
    return ctx ? ctx->default_white_cubemap : 0;
}

/// @brief Row-major to column-major (or vice versa) 4×4 transpose.
///
/// Used because GLSL/glUniformMatrix4fv default to column-major while
/// our matrices are stored row-major. Out-of-place — `src` and `dst`
/// must not alias.
/// @param src Borrowed 4x4 source matrix.
/// @param[out] dst Caller-owned 4x4 destination receiving the transpose.
static void transpose4x4(const float *src, float *dst) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            dst[r * 4 + c] = src[c * 4 + r];
}

/// @brief Row-major 4×4 multiply: `out = a * b`.
///
/// Naive triple loop — same shape as `mat4f_mul_d3d`. We keep the
/// per-backend copy so the optimizer can inline it locally.
/// @param a Borrowed left-hand row-major 4x4 matrix.
/// @param b Borrowed right-hand row-major 4x4 matrix.
/// @param[out] out Caller-owned product matrix that does not alias either input.
static void mat4f_mul_gl(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

/// @brief Invert a 4×4 matrix using the cofactor expansion.
///
/// Returns 0 on success, -1 if `m` is singular (|det| < 1e-12). Used
/// to compute `inv_vp` for post-FX shaders that need to reconstruct
/// world-space positions from depth + screen UVs.
/// @param m Borrowed row-major 4x4 matrix to invert.
/// @param[out] out Caller-owned 4x4 inverse destination.
/// @return 0 on success, or -1 when @p m is singular.
static int mat4f_inverse_gl(const float *m, float *out) {
    return vgfx3d_opengl_inverse_matrix4(m, out) ? 0 : -1;
}

/// @brief Resolve every OpenGL + GLX function pointer the backend needs.
///
/// Three loading tiers via macros:
///   - `LOAD(x)`: dlsym on libGL itself for core functions.
///   - `LOADX(x)`: dlsym for GLX (X11 binding) functions.
///   - `LOADP(x)`: glXGetProcAddress for everything past GL 1.x — the
///     standard mechanism for fetching extension entry points.
/// Returns 0 on success, -1 if any required function couldn't be
/// resolved (the backend caller falls back to software in that case).
/// Idempotent — `gl_loaded` flag short-circuits subsequent calls after confirming any newly
/// requested EGL route is available. Linux-auto builds load GLX presentation symbols alongside
/// the shared GL dispatch so windowed and headless contexts can coexist in either creation order.
/// @param wayland_binding Nonzero to resolve the EGL presentation route instead of GLX. This
///   includes both native Wayland surfaces and headless EGL pbuffers.
/// @return 0 when every required symbol is available, otherwise -1.
static int load_gl(int wayland_binding) {
    const char *missing_symbol = NULL;
    if (gl_loaded)
        return wayland_binding && !vgfx3d_egl_available() ? -1 : 0;
    gl_dispatch_lock();
    if (gl_loaded) {
        int result = wayland_binding && !vgfx3d_egl_available() ? -1 : 0;
        gl_dispatch_unlock();
        return result;
    }

    gl.lib = dlopen("libGL.so.1", RTLD_LAZY);
    if (!gl.lib)
        gl.lib = dlopen("libGL.so", RTLD_LAZY);
    if (!gl.lib) {
        const char *err = dlerror();
        const char *debug = getenv("ZANNA_OPENGL_DEBUG");
        if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0)
            fprintf(stderr, "[OpenGL] failed to load libGL: %s\n", err ? err : "unknown error");
        gl_dispatch_unlock();
        return -1;
    }
    if (wayland_binding && !vgfx3d_egl_available()) {
        gl_unload_partial_dispatch();
        gl_dispatch_unlock();
        return -1;
    }
    gl_wayland_binding = wayland_binding ? 1 : 0;

#define LOAD(name)                                                                                 \
    gl.name = RT_FN_PTR_CAST((__typeof__(gl.name))dlsym(gl.lib, "gl" #name));                      \
    if (!gl.name) {                                                                                \
        missing_symbol = "gl" #name;                                                               \
        goto fail;                                                                                 \
    }
#define LOADX(name)                                                                                \
    glx.name = RT_FN_PTR_CAST((__typeof__(glx.name))dlsym(gl.lib, "glX" #name));                   \
    if (!glx.name) {                                                                               \
        missing_symbol = "glX" #name;                                                              \
        goto fail;                                                                                 \
    }
#define LOADP(name)                                                                                \
    gl.name = RT_FN_PTR_CAST((__typeof__(gl.name))gl_resolve_context_proc("gl" #name));            \
    if (!gl.name) {                                                                                \
        missing_symbol = "gl" #name;                                                               \
        goto fail;                                                                                 \
    }

/* Same provider-neutral resolution as LOADP, but a missing symbol is not fatal: the caller must
 * null-check before use and degrade the corresponding feature. */
#define LOADP_OPTIONAL(name)                                                                       \
    gl.name = RT_FN_PTR_CAST((__typeof__(gl.name))gl_resolve_context_proc("gl" #name));

    LOAD(GetError);
    LOAD(GetString);
    LOAD(GetIntegerv);
    LOAD(GetFloatv);
    LOAD(Clear);
    LOAD(ClearColor);
    LOAD(ClearDepth);
    LOAD(Enable);
    LOAD(Disable);
    LOAD(DepthFunc);
    LOAD(CullFace);
    LOAD(FrontFace);
    LOAD(Viewport);
    LOAD(BlendFunc);
    LOAD(DepthMask);

#if !defined(ZANNA_GRAPHICS_WAYLAND)
    {
        LOADX(ChooseFBConfig);
        LOADX(CreateNewContext);
        LOADX(GetVisualFromFBConfig);
        LOADX(SwapBuffers);
        LOADX(MakeCurrent);
        LOADX(DestroyContext);
        LOADX(GetProcAddress);
        glx.CreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glx.GetProcAddress(
            (const unsigned char *)"glXCreateContextAttribsARB");
        glx.SwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glx.GetProcAddress(
            (const unsigned char *)"glXSwapIntervalEXT");
    }
#endif

    LOADP(PolygonMode);
    LOADP(PolygonOffset);
    LOADP(DrawElements);
    LOADP(DrawArrays);
    LOADP(DrawElementsInstanced);
    LOADP(CreateShader);
    LOADP(ShaderSource);
    LOADP(CompileShader);
    /* Optional: ARB_clip_control (GL 4.5). NULL is fine — the backend keeps
     * the fixed [-1,1] depth mapping when the entry point is absent. */
    LOADP_OPTIONAL(ClipControl);
    LOADP(TexImage3D);
    LOADP(FramebufferTextureLayer);
    /* Optional: ARB_timer_query (core in GL 3.3). All five must resolve before
     * GPU frame timing is enabled; any NULL leaves FrameGpuTimeUs reading 0. */
    LOADP_OPTIONAL(GenQueries);
    LOADP_OPTIONAL(DeleteQueries);
    LOADP_OPTIONAL(QueryCounter);
    LOADP_OPTIONAL(GetQueryObjectiv);
    LOADP_OPTIONAL(GetQueryObjectui64v);
    LOADP(GetShaderiv);
    LOADP(GetShaderInfoLog);
    LOADP(CreateProgram);
    LOADP(AttachShader);
    LOADP(LinkProgram);
    LOADP(GetProgramiv);
    LOADP(GetProgramInfoLog);
    /* Optional GL 4.1 / ARB_get_program_binary startup cache. GL 3.3 drivers
     * without it keep the ordinary source compile/link route. */
    LOADP_OPTIONAL(ProgramParameteri);
    LOADP_OPTIONAL(GetProgramBinary);
    LOADP_OPTIONAL(ProgramBinary);
    LOADP(UseProgram);
    LOADP(DeleteShader);
    LOADP(DeleteProgram);
    LOADP(GetUniformLocation);
    LOADP(GetUniformBlockIndex);
    LOADP(UniformBlockBinding);
    LOADP(UniformMatrix4fv);
    LOADP(Uniform1i);
    LOADP(Uniform1f);
    LOADP(Uniform1fv);
    LOADP(Uniform2f);
    LOADP(Uniform3f);
    LOADP(Uniform3fv);
    LOADP(Uniform4f);
    LOADP(Uniform4i);
    LOADP(Uniform4fv);
    LOADP(GenVertexArrays);
    LOADP(BindVertexArray);
    LOADP(GenBuffers);
    LOADP(BindBuffer);
    LOADP(BindBufferBase);
    LOADP(BufferData);
    LOADP(BufferSubData);
    LOADP(VertexAttribPointer);
    LOADP(VertexAttribIPointer);
    LOADP(EnableVertexAttribArray);
    LOADP(DisableVertexAttribArray);
    LOADP(VertexAttribDivisor);
    LOADP(VertexAttrib4f);
    LOADP(DeleteBuffers);
    LOADP(DeleteVertexArrays);
    LOADP(GenTextures);
    LOADP(DeleteTextures);
    LOADP(ActiveTexture);
    LOADP(BindTexture);
    LOADP(PixelStorei);
    LOADP(TexImage2D);
    LOADP(TexSubImage2D);
#if defined(ZANNA_GRAPHICS_WAYLAND)
    gl.CompressedTexImage2D = RT_FN_PTR_CAST(
        (PFNGLCOMPRESSEDTEXIMAGE2DPROC)vgfx3d_egl_wayland_get_proc("glCompressedTexImage2D"));
    gl.CompressedTexSubImage2D = RT_FN_PTR_CAST(
        (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)vgfx3d_egl_wayland_get_proc("glCompressedTexSubImage2D"));
#elif defined(ZANNA_GRAPHICS_LINUX_AUTO)
    if (gl_wayland_binding) {
        gl.CompressedTexImage2D =
            (PFNGLCOMPRESSEDTEXIMAGE2DPROC)vgfx3d_egl_wayland_get_proc("glCompressedTexImage2D");
        gl.CompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)vgfx3d_egl_wayland_get_proc(
            "glCompressedTexSubImage2D");
    } else {
        gl.CompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)glx.GetProcAddress(
            (const unsigned char *)"glCompressedTexImage2D");
        gl.CompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)glx.GetProcAddress(
            (const unsigned char *)"glCompressedTexSubImage2D");
    }
#else
    gl.CompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)glx.GetProcAddress(
        (const unsigned char *)"glCompressedTexImage2D");
    gl.CompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)glx.GetProcAddress(
        (const unsigned char *)"glCompressedTexSubImage2D");
#endif
    LOADP(TexParameteri);
    LOADP(TexParameterf);
    LOADP(TexBuffer);
    LOADP(GenerateMipmap);
    LOADP(GenSamplers);
    LOADP(DeleteSamplers);
    LOADP(SamplerParameteri);
    LOADP(SamplerParameterf);
    LOADP(BindSampler);
    LOADP(GenFramebuffers);
    LOADP(DeleteFramebuffers);
    LOADP(BindFramebuffer);
    LOADP(CheckFramebufferStatus);
    LOADP(FramebufferTexture2D);
    LOADP(BlitFramebuffer);
    LOADP(GenRenderbuffers);
    LOADP(DeleteRenderbuffers);
    LOADP(BindRenderbuffer);
    LOADP(RenderbufferStorage);
    LOADP(FramebufferRenderbuffer);
    LOADP(ReadPixels);
    LOADP(MapBufferRange);
    LOADP(UnmapBuffer);
    LOADP(FenceSync);
    LOADP(ClientWaitSync);
    LOADP(DeleteSync);
    LOADP(DrawBuffer);
    LOADP(DrawBuffers);
    LOADP(ReadBuffer);
    LOADP(ColorMask);
#if defined(ZANNA_GRAPHICS_WAYLAND)
    gl.GetStringi =
        RT_FN_PTR_CAST((PFNGLGETSTRINGIPROC)vgfx3d_egl_wayland_get_proc("glGetStringi"));
#elif defined(ZANNA_GRAPHICS_LINUX_AUTO)
    if (gl_wayland_binding)
        gl.GetStringi =
            RT_FN_PTR_CAST((PFNGLGETSTRINGIPROC)vgfx3d_egl_wayland_get_proc("glGetStringi"));
    else
        gl.GetStringi =
            (PFNGLGETSTRINGIPROC)glx.GetProcAddress((const unsigned char *)"glGetStringi");
#else
    gl.GetStringi = (PFNGLGETSTRINGIPROC)glx.GetProcAddress((const unsigned char *)"glGetStringi");
#endif

#undef LOAD
#undef LOADX
#undef LOADP

    gl_loaded = 1;
    gl_dispatch_unlock();
    return 0;

fail:
    {
        const char *debug = getenv("ZANNA_OPENGL_DEBUG");
        if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0) {
            const char *err = dlerror();
            fprintf(stderr,
                    "[OpenGL] missing required symbol %s%s%s\n",
                    missing_symbol ? missing_symbol : "<unknown>",
                    err ? ": " : "",
                    err ? err : "");
        }
    }
    gl_unload_partial_dispatch();
    gl_dispatch_unlock();
    return -1;
}

#include "vgfx3d_backend_opengl_shaders.inc"

/// @brief Compile a GLSL shader from one-or-more source strings.
///
/// `src_count` allows splicing multiple strings (e.g., a #version
/// preamble + the body) into a single compile. The depth-convention
/// prologue is spliced immediately after the leading #version part
/// (GLSL requires #version first). On compile failure dumps the GLSL
/// info log to stderr and deletes the shader.
/// Returns the shader handle, or 0 on failure.
/// @param type OpenGL shader stage enum, such as `GL_VERTEX_SHADER`.
/// @param src Borrowed array of source-string pointers.
/// @param src_count Number of entries in @p src.
/// @param zero_to_one_clip Nonzero to splice the zero-to-one depth convention prologue.
/// @return Compiled shader object name, or zero on validation or compilation failure.
static GLuint compile_shader_parts(GLenum type,
                                   const char *const *src,
                                   GLsizei src_count,
                                   int zero_to_one_clip) {
    const char *spliced[24];
    GLint lengths[24];
    const char *prologue = zero_to_one_clip ? "#define ZANNA_DEPTH_ZERO_TO_ONE 1\n"
                                              "#define ZANNA_DEPTH_TO_NDC(d) (d)\n"
                                            : "#define ZANNA_DEPTH_TO_NDC(d) ((d) * 2.0 - 1.0)\n";
    GLsizei required_count;

    if (!src || src_count <= 0)
        return 0;
    for (GLsizei i = 0; i < src_count; i++) {
        if (!src[i])
            return 0;
    }
    if (src_count > (GLsizei)(sizeof(spliced) / sizeof(spliced[0])) - 2)
        return 0;
    GLuint shader = gl.CreateShader(type);
    if (!shader)
        return 0;
    required_count = src_count + (strncmp(src[0], "#version", 8) == 0 ? 2 : 1);
    if (required_count <= (GLsizei)(sizeof(spliced) / sizeof(spliced[0]))) {
        GLsizei out = 0;
        if (strncmp(src[0], "#version", 8) == 0) {
            const char *newline = strchr(src[0], '\n');
            if (!newline) {
                gl.DeleteShader(shader);
                return 0;
            }
            spliced[out++] = src[0];
            lengths[out - 1] = (GLint)(newline - src[0] + 1);
            spliced[out++] = prologue;
            lengths[out - 1] = -1;
            spliced[out++] = newline + 1;
            lengths[out - 1] = -1;
            for (GLsizei i = 1; i < src_count; i++) {
                spliced[out++] = src[i];
                lengths[out - 1] = -1;
            }
        } else {
            spliced[out++] = prologue;
            lengths[out - 1] = -1;
            for (GLsizei i = 0; i < src_count; i++) {
                spliced[out++] = src[i];
                lengths[out - 1] = -1;
            }
        }
        gl.ShaderSource(shader, out, spliced, lengths);
    } else {
        gl.DeleteShader(shader);
        return 0;
    }
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl.GetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        log[sizeof(log) - 1u] = '\0';
        {
            const char *debug = getenv("ZANNA_OPENGL_DEBUG");
            if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0)
                fprintf(stderr, "[OpenGL] shader compile failed: %s\n", log);
        }
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

/// @brief Single-source convenience wrapper around `compile_shader_parts`.
/// @param type OpenGL shader stage enum.
/// @param src Borrowed null-terminated GLSL source string.
/// @param zero_to_one_clip Nonzero to enable the zero-to-one depth convention.
/// @return Compiled shader object name, or zero on failure.
static GLuint compile_shader(GLenum type, const char *src, int zero_to_one_clip) {
    const char *parts[] = {src};
    return compile_shader_parts(type, parts, 1, zero_to_one_clip);
}

/// @brief Link a vertex + fragment shader pair into a program object.
///
/// Logs the linker info log to stderr on failure and deletes the
/// program. Doesn't `glDetachShader` — caller is expected to delete
/// the shaders separately once linking is done.
/// @param vs Compiled vertex-shader object name.
/// @param fs Compiled fragment-shader object name.
/// @return Linked program object name, or zero on creation or link failure.
static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = gl.CreateProgram();
    if (!program)
        return 0;
    if (gl.ProgramParameteri && gl.GetProgramBinary && gl.ProgramBinary)
        gl.ProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl.GetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        log[sizeof(log) - 1u] = '\0';
        {
            const char *debug = getenv("ZANNA_OPENGL_DEBUG");
            if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0)
                fprintf(stderr, "[OpenGL] program link failed: %s\n", log);
        }
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

/* Matches the Canvas3D antialiased-text raster LRU (512). */
#define GL_TEXTURE_CACHE_MAX_RESIDENT 512
#define GL_MAIN_UNIFORM_SNAPSHOT_CAPACITY 2048
#define GL_TEXTURE_CACHE_PRUNE_AGE 240u
#define GL_CUBEMAP_CACHE_MAX_RESIDENT 64
#define GL_CUBEMAP_CACHE_PRUNE_AGE 240u
#define GL_MORPH_CACHE_MAX_RESIDENT 64
#define GL_MORPH_CACHE_PRUNE_AGE 240u

/// @brief Geometric resize for a streaming GL buffer.
///
/// If the buffer is already large enough, no-op. Otherwise binds the
/// buffer and reallocates with a NULL data pointer to size it without
/// uploading. Capacity grows via the shared `vgfx3d_opengl_next_capacity`
/// helper.
/// @param target Buffer binding target.
/// @param buffer Existing OpenGL buffer object name.
/// @param[in,out] capacity Caller-owned current capacity, updated after successful growth.
/// @param needed Minimum required byte capacity.
/// @param initial_capacity Initial geometric-growth floor.
/// @param usage OpenGL usage hint supplied to `glBufferData`.
/// @return 0 when capacity is sufficient or growth succeeds, otherwise -1.
static int ensure_buffer_capacity(GLenum target,
                                  GLuint buffer,
                                  size_t *capacity,
                                  size_t needed,
                                  size_t initial_capacity,
                                  GLenum usage) {
    size_t new_capacity;

    if (!capacity || !buffer)
        return -1;
    if (needed == 0)
        needed = 4;
    if (*capacity >= needed)
        return 0;
    if (!vgfx3d_opengl_compute_buffer_capacity(
            *capacity, needed, initial_capacity, &new_capacity) ||
        new_capacity > (size_t)PTRDIFF_MAX) {
        return -1;
    }
    gl.BindBuffer(target, buffer);
    gl.BufferData(target, (GLsizeiptr)new_capacity, NULL, usage);
    if (!gl_upload_ok())
        return -1;
    *capacity = new_capacity;
    return 0;
}

/// @brief "Orphan" a streaming buffer and verify that the driver accepted the reallocation.
///
/// Calling `glBufferData(target, capacity, NULL, ...)` tells the driver
/// the existing storage may be reused for in-flight draws while we
/// build the next frame's data. Avoids the explicit-fence sync the
/// driver would otherwise have to insert on `BufferSubData`.
/// @param target Buffer binding target.
/// @param buffer Existing OpenGL buffer object name.
/// @param capacity Number of bytes to allocate for the replacement store.
/// @param usage OpenGL usage hint supplied to `glBufferData`.
/// @return 1 when the orphan succeeded without GL errors; otherwise 0.
static int orphan_stream_buffer(GLenum target, GLuint buffer, size_t capacity, GLenum usage) {
    if (!buffer || capacity == 0)
        return 0;
    gl.BindBuffer(target, buffer);
    gl.BufferData(target, (GLsizeiptr)capacity, NULL, usage);
    return gl_upload_ok();
}

/// @brief Whether the named OpenGL extension is present (checks both indexed and legacy queries).
/// @param name Borrowed exact extension name to locate.
/// @return Nonzero when the active context advertises @p name, otherwise zero.
static int gl_extension_supported(const char *name) {
    GLint count = 0;
    const char *extensions;

    if (!name || !*name || !gl.GetString)
        return 0;
    if (gl.GetStringi && gl.GetIntegerv) {
        gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; i++) {
            const char *ext = (const char *)gl.GetStringi(GL_EXTENSIONS, (GLuint)i);
            if (ext && strcmp(ext, name) == 0)
                return 1;
        }
        if (count > 0)
            return 0;
    }
    extensions = (const char *)gl.GetString(GL_EXTENSIONS);
    if (extensions) {
        size_t len = strlen(name);
        const char *p = extensions;
        while ((p = strstr(p, name)) != NULL) {
            if ((p == extensions || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
                return 1;
            p += len;
        }
    }
    return 0;
}

/// @brief Probe whether a color texture internal format can be used as an FBO attachment.
///
/// Some OpenGL drivers expose a version or extension that suggests support for a
/// format but still reject it for render targets on a specific context/profile.
/// This helper performs the actual framebuffer-completeness check used to decide
/// whether the backend may allocate scene or RTT targets in that format.
/// @param internal_format Candidate sized color internal format.
/// @param data_type Pixel data type used for the probe allocation.
/// @return Non-zero when a 1x1 framebuffer using @p internal_format is complete.
static int gl_probe_color_renderable_format(GLint internal_format, GLenum data_type) {
    gl_framebuffer_state_t saved;
    GLuint fbo = 0;
    GLuint tex = 0;
    int ok = 0;

    gl_capture_framebuffer_state(&saved);
    gl.GenFramebuffers(1, &fbo);
    gl.GenTextures(1, &tex);
    if (fbo && tex) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.TexImage2D(GL_TEXTURE_2D, 0, internal_format, 1, 1, 0, GL_RGBA, data_type, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (tex)
        gl.DeleteTextures(1, &tex);
    if (fbo)
        gl.DeleteFramebuffers(1, &fbo);
    gl_restore_framebuffer_state(&saved);
    return ok && gl_drain_errors("color format probe");
}

/// @brief Probe whether a depth texture internal format can be used as an FBO attachment.
///
/// Depth formats vary more across GL drivers than the GL version alone implies.
/// This live FBO probe prevents post-FX and shadow targets from selecting a
/// floating depth format that the context cannot actually attach.
/// @param internal_format Candidate sized depth internal format.
/// @param data_type Pixel data type used for the probe allocation.
/// @return Non-zero when a 1x1 depth-only framebuffer is complete.
static int gl_probe_depth_renderable_format(GLint internal_format, GLenum data_type) {
    gl_framebuffer_state_t saved;
    GLuint fbo = 0;
    GLuint tex = 0;
    int ok = 0;

    gl_capture_framebuffer_state(&saved);
    gl.GenFramebuffers(1, &fbo);
    gl.GenTextures(1, &tex);
    if (fbo && tex) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.TexImage2D(
            GL_TEXTURE_2D, 0, internal_format, 1, 1, 0, GL_DEPTH_COMPONENT, data_type, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
        gl.DrawBuffer(GL_NONE);
        gl.ReadBuffer(GL_NONE);
        ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (tex)
        gl.DeleteTextures(1, &tex);
    if (fbo)
        gl.DeleteFramebuffers(1, &fbo);
    gl_restore_framebuffer_state(&saved);
    return ok && gl_drain_errors("depth format probe");
}

/// @brief Query the active context's OpenGL version and coarse resource limits.
///
/// The backend requires the GL 3.3 feature set used by the embedded GLSL and the fixed vertex
/// layout. This function records the limits once, validates the minimum profile, and captures
/// optional compressed-texture support so TextureAsset3D can use native blocks when the driver
/// advertises them.
/// @param ctx Borrowed OpenGL backend context receiving version, limit, and feature results.
/// @return Nonzero when the active context satisfies all required limits and probes.
static int gl_query_context_capabilities(gl_context_t *ctx) {
    const char *version = NULL;

    if (!ctx || !gl.GetIntegerv || !gl.GetString)
        return 0;

    gl.GetIntegerv(GL_MAJOR_VERSION, &ctx->gl_major_version);
    gl.GetIntegerv(GL_MINOR_VERSION, &ctx->gl_minor_version);
    if (ctx->gl_major_version <= 0) {
        version = (const char *)gl.GetString(GL_VERSION);
        if (version &&
            sscanf(version, "%d.%d", &ctx->gl_major_version, &ctx->gl_minor_version) != 2) {
            ctx->gl_major_version = 0;
            ctx->gl_minor_version = 0;
        }
    }
    if (ctx->gl_major_version < 3 || (ctx->gl_major_version == 3 && ctx->gl_minor_version < 3)) {
        fprintf(stderr,
                "[OpenGL] GL 3.3 core backend unavailable on context version %d.%d\n",
                ctx->gl_major_version,
                ctx->gl_minor_version);
        return 0;
    }

    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &ctx->max_texture_size);
    gl.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &ctx->max_vertex_attribs);
    gl.GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &ctx->max_combined_texture_units);
    gl.GetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &ctx->max_vertex_texture_units);
    gl.GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &ctx->max_texture_buffer_size);
    if (!gl_drain_errors("capability query")) {
        ctx->max_texture_size = 0;
        ctx->max_vertex_attribs = 0;
        ctx->max_combined_texture_units = 0;
        ctx->max_vertex_texture_units = 0;
        ctx->max_texture_buffer_size = 0;
    }
    if (ctx->max_vertex_attribs < 16 ||
        ctx->max_combined_texture_units <= GL_TU_MORPH_NORMAL_DELTAS ||
        ctx->max_vertex_texture_units < 2) {
        fprintf(stderr,
                "[OpenGL] backend limits insufficient: attribs=%d combinedTex=%d vertexTex=%d\n",
                ctx->max_vertex_attribs,
                ctx->max_combined_texture_units,
                ctx->max_vertex_texture_units);
        return 0;
    }

    ctx->supports_hdr_color_target = gl_probe_color_renderable_format(GL_RGBA16F, GL_FLOAT) ? 1 : 0;
    ctx->supports_depth_float_target =
        gl_probe_depth_renderable_format(GL_DEPTH_COMPONENT32F, GL_FLOAT) ? 1 : 0;
    ctx->supports_bc7 = gl.CompressedTexImage2D && gl.CompressedTexSubImage2D &&
                        (gl_extension_supported("GL_ARB_texture_compression_bptc") ||
                         gl_extension_supported("GL_EXT_texture_compression_bptc"));
    ctx->supports_astc = gl.CompressedTexImage2D && gl.CompressedTexSubImage2D &&
                         gl_extension_supported("GL_KHR_texture_compression_astc_ldr");
    ctx->supports_etc2 =
        gl.CompressedTexImage2D && gl.CompressedTexSubImage2D &&
        (ctx->gl_major_version > 4 || (ctx->gl_major_version == 4 && ctx->gl_minor_version >= 3) ||
         gl_extension_supported("GL_ARB_ES3_compatibility"));
    return gl_drain_errors("capability query");
}

/// @brief Probe GL_EXT_texture_filter_anisotropic once for the active context.
/// @param ctx Borrowed OpenGL backend context receiving support and maximum-anisotropy values.
static void gl_query_anisotropy_support(gl_context_t *ctx) {
    GLfloat max_anisotropy = 1.0f;

    if (!ctx)
        return;
    ctx->anisotropy_supported = 0;
    ctx->max_texture_anisotropy = 1.0f;
    if (!gl.GetFloatv || !gl_extension_supported("GL_EXT_texture_filter_anisotropic"))
        return;
    gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
    if (!isfinite(max_anisotropy) || max_anisotropy <= 1.0f)
        return;
    if (max_anisotropy > (GLfloat)VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY)
        max_anisotropy = (GLfloat)VGFX3D_OPENGL_MAX_TEXTURE_ANISOTROPY;
    ctx->anisotropy_supported = 1;
    ctx->max_texture_anisotropy = max_anisotropy;
}

// Texture / cubemap / morph caches — keyed by stable host identity + generation.
// This avoids stale texture reuse when allocator address recycling hands a
// fresh Pixels object the same pointer as an older cached upload.

/// @brief Delete every cached 2D texture and reset the count.
#include "vgfx3d_backend_opengl_context.inc"
#include "vgfx3d_backend_opengl_frame.inc"
#include "vgfx3d_backend_opengl_material.inc"
#include "vgfx3d_backend_opengl_mesh.inc"
#include "vgfx3d_backend_opengl_targets.inc"
#include "vgfx3d_backend_opengl_texture.inc"

/// @brief Copy OpenGL backend telemetry into a backend-neutral diagnostics struct.
/// @details Canvas3D calls this optional vtable hook from user-facing debug getters. The function
///          never allocates and is NULL-safe so debug overlays can poll it every frame.
/// @param ctx_ptr Opaque OpenGL backend context.
/// @param out_stats Destination snapshot. Left untouched when NULL.
static void gl_get_backend_stats(void *ctx_ptr, vgfx3d_backend_stats_t *out_stats) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!out_stats)
        return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx)
        return;
    *out_stats = ctx->stats;
    out_stats->present_path = ctx->present_path;
    out_stats->default_framebuffer_writable = ctx->default_framebuffer_writable ? 1 : 0;
}

/// @brief Return OpenGL feature bits that depend on the probed device/context.
/// @details The post-FX hook can exist even when a driver cannot render to RGBA16F. HDR scene color
/// and TAA both allocate float scene/history targets, so they are advertised only after the context
/// probe has confirmed float color attachments are renderable.
/// @param ctx_ptr OpenGL backend context.
/// @return RT_CANVAS3D_BACKEND_CAP_* feature bits supported by this context.
static int64_t gl_get_feature_caps(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int64_t caps = 0;
    if (ctx && ctx->supports_hdr_color_target)
        caps |= RT_CANVAS3D_BACKEND_CAP_HDR_SCENE | RT_CANVAS3D_BACKEND_CAP_TAA;
    return caps;
}

const vgfx3d_backend_t vgfx3d_opengl_backend = {
    /* Scene passes render reversed-Z (Canvas3D negates the projection z row; clears
     * are 0 with Greater compares); the fixed [-1,1]->[0,1] window remap then stores
     * near = 1 / far = 0, giving the reversed float-depth distribution. Shadow maps
     * stay standard (Less, clear 1, pinned in gl_shadow_begin). */
    .reversed_z = 1,
    .name = "opengl",
    .gpu_skinning = 1,
    .particle_instancing = 1,
    .clustered_lighting = 1,
    .shadow_csm = 1,
    /* ADR 0246 step 1 of 2. Storage is done -- all slots live in one depth texture
     * array, layered FBOs report COMPLETE, a six-face omni light renders slots 0-6,
     * and the uniforms reach the shader correctly (type=1, shadowIndex=1,
     * projectionType=CUBE, shadowCount=7). The flag stays OFF because the main
     * fragment shader never invokes sampleShadowMap for these draws at all: forcing
     * every slot >= 1 to fully shadowed produces no visual change, so the scene is
     * shaded by a program or light loop other than the one carrying the shadow gate.
     * Until that path is found, advertising the atlas would report point shadows that
     * silently never darken anything. */
    .shadow_atlas_slots = 0,
    .create_ctx = gl_create_ctx,
    .destroy_ctx = gl_destroy_ctx,
    .clear = gl_clear,
    .resize = gl_resize,
    .begin_frame = gl_begin_frame,
    .submit_draw = gl_submit_draw,
    .end_frame = gl_end_frame,
    .set_render_target = gl_set_render_target,
    .shadow_begin = gl_shadow_begin,
    .shadow_draw = gl_shadow_draw,
    .shadow_end = gl_shadow_end,
    .shadow_reuse = gl_shadow_reuse,
    .draw_skybox = gl_draw_skybox,
    .submit_draw_instanced = gl_submit_draw_instanced,
    .present = gl_present,
    .readback_rgba = gl_readback_rgba,
    .set_capture_after_present = gl_set_capture_after_present,
    .present_postfx = gl_present_postfx,
    .resolve_opaque_targets = gl_resolve_opaque_targets,
    .apply_postfx = gl_apply_postfx,
    .set_gpu_postfx_enabled = gl_set_gpu_postfx_enabled,
    .set_gpu_postfx_snapshot = gl_set_gpu_postfx_snapshot,
    .set_texture_upload_budget = gl_set_texture_upload_budget,
    .get_texture_upload_pending_bytes = gl_get_texture_upload_pending_bytes,
    .get_texture_upload_bytes = gl_get_texture_upload_bytes,
    .get_frame_gpu_time_us = gl_get_frame_gpu_time_us,
    .get_native_texture_caps = gl_get_native_texture_caps,
    .get_feature_caps = gl_get_feature_caps,
    .get_backend_stats = gl_get_backend_stats,
    .set_vsync = gl_set_vsync,
    .set_render_scale = gl_set_render_scale,
    .queue_depth_probe = gl_queue_depth_probe,
    .read_depth_probe = gl_read_depth_probe,
};

#endif /* __linux__ && ZANNA_ENABLE_GRAPHICS */
