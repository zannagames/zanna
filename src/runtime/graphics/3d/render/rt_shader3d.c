//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_shader3d.c
// Purpose: Shader3D — user-authored per-backend shader sources bound to a
//          Material3D (ADR 0370). Parses the `.zshader` header (params,
//          textures, mode, blend, cull) and keeps the raw [metal]/[hlsl]/[glsl]
//          sections for the backend that compiles them.
// Key invariants:
//   - The header is the only part the engine interprets; sections are opaque
//     text handed to exactly one backend each.
//   - Parameter packing follows the 16-byte block rules every backend shares
//     (float/int 1 lane, float2 2 lanes at even offsets, float3/float4 4 lanes
//     at multiples of 4), so a material's packed block is identical on Metal,
//     D3D11 and OpenGL.
//   - A shader that fails to parse is still a valid object: Status is FAILED,
//     Error names the line, and a material bound to it renders its built-in
//     shading model.
// Ownership/Lifetime:
//   - Section text, the load path and the error string are malloc-owned by the
//     shader and freed by its finalizer.
//   - Materials retain the shader (retain-then-release swap).
// Links: docs/adr/0370-custom-shader-sources.md, rt_material3d.c,
//        rt_canvas3d_deferred.inc (draw-command payload)
//
//===----------------------------------------------------------------------===//

#include "rt_asset.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Largest `.zshader` file accepted (sources are text; 4 MB is generous).
#define RT_SHADER3D_MAX_FILE_BYTES (4u * 1024u * 1024u)

/// @brief Validate @p obj as a Shader3D handle.
/// @param obj Candidate runtime object.
/// @return Typed shader pointer, or NULL for NULL / another class.
static rt_shader3d *shader3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_SHADER3D_CLASS_ID, sizeof(rt_shader3d))
               ? (rt_shader3d *)obj
               : NULL;
}

/// @brief Replace a malloc-owned string field.
/// @param slot Field to replace; the previous value is freed.
/// @param text Text to copy, or NULL to clear.
static void shader3d_set_owned_text(char **slot, const char *text) {
    if (!slot)
        return;
    free(*slot);
    *slot = NULL;
    if (!text)
        return;
    {
        size_t len = strlen(text);
        char *copy = (char *)malloc(len + 1u);
        if (!copy)
            return;
        memcpy(copy, text, len + 1u);
        *slot = copy;
    }
}

/// @brief Record a parse failure.
/// @param shader Shader receiving the failed status.
/// @param message Complete diagnostic text.
static void shader3d_fail(rt_shader3d *shader, const char *message) {
    if (!shader)
        return;
    shader->status = RT_SHADER3D_STATUS_FAILED;
    shader3d_set_owned_text(&shader->error, message);
}

/// @brief Reset every parsed field so a reparse starts from a clean object.
/// @param shader Shader to clear.
static void shader3d_clear_parsed(rt_shader3d *shader) {
    if (!shader)
        return;
    shader->name[0] = 0;
    shader->mode = RT_SHADER3D_MODE_SURFACE;
    shader->blend = -1;
    shader->cull = -1;
    memset(shader->params, 0, sizeof(shader->params));
    shader->param_count = 0;
    shader->param_floats = 0;
    memset(shader->texture_names, 0, sizeof(shader->texture_names));
    shader->texture_count = 0;
    for (int32_t i = 0; i < RT_SHADER3D_BACKEND_COUNT; i++) {
        free(shader->sources[i]);
        shader->sources[i] = NULL;
    }
    shader3d_set_owned_text(&shader->error, NULL);
    shader->status = RT_SHADER3D_STATUS_PENDING;
}

/// @brief Finalizer: free owned text.
/// @param obj Shader payload.
static void rt_shader3d_finalize(void *obj) {
    rt_shader3d *shader = (rt_shader3d *)obj;
    if (!shader)
        return;
    shader3d_clear_parsed(shader);
    free(shader->source_path);
    shader->source_path = NULL;
}

/// @brief Allocate an empty shader object.
/// @return Retained shader, or NULL after trapping on allocation failure.
static rt_shader3d *shader3d_alloc(void) {
    rt_shader3d *shader =
        (rt_shader3d *)rt_obj_new_i64(RT_G3D_SHADER3D_CLASS_ID, (int64_t)sizeof(rt_shader3d));
    if (!shader) {
        rt_trap("Shader3D: memory allocation failed");
        return NULL;
    }
    memset(shader, 0, sizeof(*shader));
    shader->identity_serial = rt_g3d_next_identity_serial();
    shader->blend = -1;
    shader->cull = -1;
    rt_obj_set_finalizer(shader, rt_shader3d_finalize);
    return shader;
}

/// @brief Lanes occupied by a parameter type inside the packed block.
static int32_t shader3d_type_lanes(int32_t type) {
    switch (type) {
        case RT_SHADER3D_PARAM_FLOAT2:
            return 2;
        case RT_SHADER3D_PARAM_FLOAT3:
        case RT_SHADER3D_PARAM_FLOAT4:
            return 4;
        default:
            return 1;
    }
}

/// @brief Components a parameter type carries (float3 is three of its four lanes).
int32_t rt_shader3d_type_components(int32_t type) {
    switch (type) {
        case RT_SHADER3D_PARAM_FLOAT2:
            return 2;
        case RT_SHADER3D_PARAM_FLOAT3:
            return 3;
        case RT_SHADER3D_PARAM_FLOAT4:
            return 4;
        default:
            return 1;
    }
}

/// @brief Parse a parameter type keyword.
/// @return RT_SHADER3D_PARAM_* or 0 for an unknown keyword.
static int32_t shader3d_parse_type(const char *word) {
    if (!word)
        return 0;
    if (strcmp(word, "float") == 0)
        return RT_SHADER3D_PARAM_FLOAT;
    if (strcmp(word, "float2") == 0)
        return RT_SHADER3D_PARAM_FLOAT2;
    if (strcmp(word, "float3") == 0)
        return RT_SHADER3D_PARAM_FLOAT3;
    if (strcmp(word, "float4") == 0)
        return RT_SHADER3D_PARAM_FLOAT4;
    if (strcmp(word, "int") == 0)
        return RT_SHADER3D_PARAM_INT;
    return 0;
}

/// @brief Keyword for a parameter type.
const char *rt_shader3d_type_name(int32_t type) {
    switch (type) {
        case RT_SHADER3D_PARAM_FLOAT:
            return "float";
        case RT_SHADER3D_PARAM_FLOAT2:
            return "float2";
        case RT_SHADER3D_PARAM_FLOAT3:
            return "float3";
        case RT_SHADER3D_PARAM_FLOAT4:
            return "float4";
        case RT_SHADER3D_PARAM_INT:
            return "int";
        default:
            return "";
    }
}

/// @brief Whether @p word is a usable parameter/texture identifier.
static int shader3d_identifier_ok(const char *word) {
    size_t len;
    if (!word || !*word)
        return 0;
    len = strlen(word);
    if (len >= RT_SHADER3D_NAME_MAX)
        return 0;
    if (!(isalpha((unsigned char)word[0]) || word[0] == '_'))
        return 0;
    for (size_t i = 1; i < len; i++) {
        if (!(isalnum((unsigned char)word[i]) || word[i] == '_'))
            return 0;
    }
    return 1;
}

/// @brief Map a section name (inside the brackets) to a backend index.
/// @return Backend index, or -1 for an unknown section.
static int32_t shader3d_section_backend(const char *name) {
    if (!name)
        return -1;
    if (strcmp(name, "metal") == 0 || strcmp(name, "msl") == 0)
        return RT_SHADER3D_BACKEND_METAL;
    if (strcmp(name, "hlsl") == 0 || strcmp(name, "d3d11") == 0)
        return RT_SHADER3D_BACKEND_HLSL;
    if (strcmp(name, "glsl") == 0 || strcmp(name, "opengl") == 0)
        return RT_SHADER3D_BACKEND_GLSL;
    return -1;
}

/// @brief Lower-case a token in place.
static void shader3d_lower(char *word) {
    for (; word && *word; word++)
        *word = (char)tolower((unsigned char)*word);
}

/// @brief Split a header line into whitespace-separated words (in place).
/// @return Word count (at most @p max_words).
static int32_t shader3d_split_words(char *line, char **words, int32_t max_words) {
    int32_t count = 0;
    char *p = line;
    while (*p && count < max_words) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        words[count++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = 0;
    }
    return count;
}

/// @brief Append a source line to a backend section buffer.
static int shader3d_append_section(char **section, size_t *len, size_t *cap, const char *line) {
    size_t line_len = strlen(line);
    size_t needed = *len + line_len + 2u;
    if (needed > *cap) {
        size_t next = *cap ? *cap : 1024u;
        char *grown;
        while (next < needed)
            next *= 2u;
        grown = (char *)realloc(*section, next);
        if (!grown)
            return 0;
        *section = grown;
        *cap = next;
    }
    memcpy(*section + *len, line, line_len);
    *len += line_len;
    (*section)[(*len)++] = '\n';
    (*section)[*len] = 0;
    return 1;
}

/// @brief Parse `.zshader` text into @p shader.
/// @details Header directives run until the first `[section]` line; every
///          section's lines are kept verbatim for its backend. Failure leaves
///          the object with status FAILED and a line-numbered message.
/// @param shader Shader receiving the parse (cleared first).
/// @param text Complete file text.
/// @return 1 on success, 0 on failure.
static int shader3d_parse(rt_shader3d *shader, const char *text) {
    char *buffer;
    char *cursor;
    int32_t line_no = 0;
    int32_t current_section = -1;
    char *sections[RT_SHADER3D_BACKEND_COUNT] = {NULL, NULL, NULL};
    size_t section_len[RT_SHADER3D_BACKEND_COUNT] = {0, 0, 0};
    size_t section_cap[RT_SHADER3D_BACKEND_COUNT] = {0, 0, 0};
    int saw_header = 0;
    char message[256];

    shader3d_clear_parsed(shader);
    if (!text) {
        shader3d_fail(shader, "Shader3D: empty source");
        return 0;
    }
    buffer = (char *)malloc(strlen(text) + 1u);
    if (!buffer) {
        shader3d_fail(shader, "Shader3D: memory allocation failed");
        return 0;
    }
    strcpy(buffer, text);
    cursor = buffer;
    while (cursor && *cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        char *trimmed;
        size_t tlen;
        if (newline) {
            *newline = 0;
            cursor = newline + 1;
        } else {
            cursor = NULL;
        }
        line_no++;
        tlen = strlen(line);
        if (tlen && line[tlen - 1] == '\r')
            line[--tlen] = 0;
        if (current_section >= 0) {
            /* A section runs until the next bracketed header line. */
            trimmed = line;
            while (*trimmed && isspace((unsigned char)*trimmed))
                trimmed++;
            if (*trimmed == '[' && trimmed[strlen(trimmed) - 1] == ']') {
                /* fall through to header handling below */
            } else {
                if (!shader3d_append_section(&sections[current_section],
                                             &section_len[current_section],
                                             &section_cap[current_section],
                                             line)) {
                    shader3d_fail(shader, "Shader3D: memory allocation failed");
                    goto fail;
                }
                continue;
            }
        }
        trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed))
            trimmed++;
        if (!*trimmed || *trimmed == '#')
            continue;
        if (*trimmed == '[') {
            char *close = strchr(trimmed, ']');
            int32_t backend;
            if (!close || close[1] != 0) {
                snprintf(message,
                         sizeof(message),
                         "Shader3D: line %d: malformed section header",
                         line_no);
                shader3d_fail(shader, message);
                goto fail;
            }
            *close = 0;
            shader3d_lower(trimmed + 1);
            backend = shader3d_section_backend(trimmed + 1);
            if (backend < 0) {
                snprintf(message,
                         sizeof(message),
                         "Shader3D: line %d: unknown section '%s'",
                         line_no,
                         trimmed + 1);
                shader3d_fail(shader, message);
                goto fail;
            }
            current_section = backend;
            continue;
        }
        {
            char *words[8];
            int32_t count = shader3d_split_words(trimmed, words, 8);
            if (count == 0)
                continue;
            shader3d_lower(words[0]);
            if (strcmp(words[0], "zshader") == 0) {
                if (count < 2 || strcmp(words[1], "1") != 0) {
                    snprintf(message,
                             sizeof(message),
                             "Shader3D: line %d: unsupported zshader version",
                             line_no);
                    shader3d_fail(shader, message);
                    goto fail;
                }
                saw_header = 1;
                continue;
            }
            if (!saw_header) {
                shader3d_fail(shader, "Shader3D: missing 'zshader 1' header");
                goto fail;
            }
            if (strcmp(words[0], "name") == 0) {
                size_t i;
                shader->name[0] = 0;
                for (i = 1; i < (size_t)count; i++) {
                    size_t cur = strlen(shader->name);
                    size_t wl = strlen(words[i]);
                    if (cur + wl + 2u >= RT_SHADER3D_NAME_MAX)
                        break;
                    if (cur)
                        shader->name[cur++] = ' ';
                    memcpy(shader->name + cur, words[i], wl + 1u);
                }
                continue;
            }
            if (strcmp(words[0], "mode") == 0) {
                if (count < 2)
                    goto bad_value;
                shader3d_lower(words[1]);
                if (strcmp(words[1], "surface") == 0)
                    shader->mode = RT_SHADER3D_MODE_SURFACE;
                else if (strcmp(words[1], "full") == 0)
                    shader->mode = RT_SHADER3D_MODE_FULL;
                else
                    goto bad_value;
                continue;
            }
            if (strcmp(words[0], "blend") == 0) {
                if (count < 2)
                    goto bad_value;
                shader3d_lower(words[1]);
                if (strcmp(words[1], "material") == 0)
                    shader->blend = -1;
                else if (strcmp(words[1], "opaque") == 0)
                    shader->blend = 0;
                else if (strcmp(words[1], "alpha") == 0)
                    shader->blend = 1;
                else if (strcmp(words[1], "additive") == 0)
                    shader->blend = 2;
                else
                    goto bad_value;
                continue;
            }
            if (strcmp(words[0], "cull") == 0) {
                if (count < 2)
                    goto bad_value;
                shader3d_lower(words[1]);
                if (strcmp(words[1], "material") == 0)
                    shader->cull = -1;
                else if (strcmp(words[1], "back") == 0)
                    shader->cull = 0;
                else if (strcmp(words[1], "front") == 0)
                    shader->cull = 1;
                else if (strcmp(words[1], "none") == 0)
                    shader->cull = 2;
                else
                    goto bad_value;
                continue;
            }
            if (strcmp(words[0], "texture") == 0) {
                if (count < 2 || !shader3d_identifier_ok(words[1]))
                    goto bad_value;
                if (shader->texture_count >= RT_SHADER3D_MAX_TEXTURES) {
                    shader3d_fail(shader, "Shader3D: more than 4 textures");
                    goto fail;
                }
                for (int32_t i = 0; i < shader->texture_count; i++) {
                    if (strcmp(shader->texture_names[i], words[1]) == 0) {
                        snprintf(message,
                                 sizeof(message),
                                 "Shader3D: texture '%s' redeclared",
                                 words[1]);
                        shader3d_fail(shader, message);
                        goto fail;
                    }
                }
                strcpy(shader->texture_names[shader->texture_count++], words[1]);
                continue;
            }
            if (strcmp(words[0], "param") == 0) {
                rt_shader3d_param *param;
                int32_t type;
                int32_t lanes;
                int32_t align;
                int32_t offset;
                if (count < 3)
                    goto bad_value;
                shader3d_lower(words[1]);
                type = shader3d_parse_type(words[1]);
                if (!type) {
                    snprintf(message,
                             sizeof(message),
                             "Shader3D: line %d: bad param type '%s'",
                             line_no,
                             words[1]);
                    shader3d_fail(shader, message);
                    goto fail;
                }
                if (!shader3d_identifier_ok(words[2]))
                    goto bad_value;
                for (int32_t i = 0; i < shader->param_count; i++) {
                    if (strcmp(shader->params[i].name, words[2]) == 0) {
                        snprintf(
                            message, sizeof(message), "Shader3D: param '%s' redeclared", words[2]);
                        shader3d_fail(shader, message);
                        goto fail;
                    }
                }
                if (shader->param_count >= RT_SHADER3D_MAX_PARAMS) {
                    shader3d_fail(shader, "Shader3D: more than 32 params");
                    goto fail;
                }
                lanes = shader3d_type_lanes(type);
                align = lanes;
                offset = (shader->param_floats + align - 1) / align * align;
                if (offset + lanes > RT_SHADER3D_PARAM_FLOATS) {
                    shader3d_fail(shader, "Shader3D: params exceed 64 floats");
                    goto fail;
                }
                param = &shader->params[shader->param_count++];
                strcpy(param->name, words[2]);
                param->type = type;
                param->offset = offset;
                param->lanes = lanes;
                for (int32_t lane = 0; lane < 4; lane++) {
                    double v = 0.0;
                    if (3 + lane < count) {
                        char *end = NULL;
                        v = strtod(words[3 + lane], &end);
                        if (!end || *end != 0 || !isfinite(v))
                            goto bad_value;
                    }
                    param->defaults[lane] = v;
                }
                shader->param_floats = offset + lanes;
                continue;
            }
            snprintf(message,
                     sizeof(message),
                     "Shader3D: line %d: unknown directive '%s'",
                     line_no,
                     words[0]);
            shader3d_fail(shader, message);
            goto fail;
        bad_value:
            snprintf(message,
                     sizeof(message),
                     "Shader3D: line %d: bad value for '%s'",
                     line_no,
                     words[0]);
            shader3d_fail(shader, message);
            goto fail;
        }
    }
    if (!saw_header) {
        shader3d_fail(shader, "Shader3D: missing 'zshader 1' header");
        goto fail;
    }
    for (int32_t i = 0; i < RT_SHADER3D_BACKEND_COUNT; i++)
        shader->sources[i] = sections[i];
    free(buffer);
    shader->revision++;
    /* No backend compiles user sources yet (ADR 0370 phase 1): a parsed shader
     * reports UNSUPPORTED until a canvas backend with compile_shader adopts it. */
    shader->status = RT_SHADER3D_STATUS_UNSUPPORTED;
    return 1;

fail:
    for (int32_t i = 0; i < RT_SHADER3D_BACKEND_COUNT; i++)
        free(sections[i]);
    free(buffer);
    return 0;
}

/// @brief Read a whole file into a NUL-terminated malloc buffer.
static char *shader3d_read_file(const char *path) {
    FILE *file;
    long size;
    char *bytes;
    if (!path || !*path)
        return NULL;
    file = rt_file_stdio_open_utf8(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0 ||
        (unsigned long)size > RT_SHADER3D_MAX_FILE_BYTES) {
        fclose(file);
        return NULL;
    }
    bytes = (char *)malloc((size_t)size + 1u);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[size] = 0;
    return bytes;
}

/// @brief Load and parse the shader's recorded source (file or asset).
static void shader3d_load_from_path(rt_shader3d *shader) {
    char *text = NULL;
    char message[300];
    if (!shader || !shader->source_path)
        return;
    if (shader->source_is_asset) {
        size_t size = 0;
        rt_string name = rt_const_cstr(shader->source_path);
        uint8_t *raw = rt_asset_load_raw(name, &size);
        if (raw) {
            text = (char *)malloc(size + 1u);
            if (text) {
                memcpy(text, raw, size);
                text[size] = 0;
            }
            free(raw);
        }
    } else {
        text = shader3d_read_file(shader->source_path);
    }
    if (!text) {
        shader3d_clear_parsed(shader);
        snprintf(message,
                 sizeof(message),
                 "Shader3D.%s: cannot read '%s'",
                 shader->source_is_asset ? "LoadAsset" : "Load",
                 shader->source_path);
        shader3d_fail(shader, message);
        return;
    }
    (void)shader3d_parse(shader, text);
    free(text);
}

void *rt_shader3d_load(rt_string path) {
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    rt_shader3d *shader;
    if (!cpath || !*cpath) {
        rt_trap("Shader3D.Load: invalid path");
        return NULL;
    }
    shader = shader3d_alloc();
    if (!shader)
        return NULL;
    shader3d_set_owned_text(&shader->source_path, cpath);
    shader->source_is_asset = 0;
    shader3d_load_from_path(shader);
    return shader;
}

void *rt_shader3d_load_asset(rt_string asset_path) {
    const char *cpath = asset_path ? rt_string_cstr(asset_path) : NULL;
    rt_shader3d *shader;
    if (!cpath || !*cpath) {
        rt_trap("Shader3D.LoadAsset: invalid asset path");
        return NULL;
    }
    shader = shader3d_alloc();
    if (!shader)
        return NULL;
    shader3d_set_owned_text(&shader->source_path, cpath);
    shader->source_is_asset = 1;
    shader3d_load_from_path(shader);
    return shader;
}

void *rt_shader3d_from_source(rt_string text) {
    const char *ctext = text ? rt_string_cstr(text) : NULL;
    rt_shader3d *shader = shader3d_alloc();
    if (!shader)
        return NULL;
    (void)shader3d_parse(shader, ctext ? ctext : "");
    return shader;
}

void rt_shader3d_reload(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    if (!shader)
        return;
    if (!shader->source_path) {
        rt_trap("Shader3D.Reload: shader was created from source text");
        return;
    }
    shader3d_load_from_path(shader);
}

rt_string rt_shader3d_get_name(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return rt_const_cstr(shader ? shader->name : "");
}

int64_t rt_shader3d_get_mode(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return shader ? shader->mode : 0;
}

int64_t rt_shader3d_get_status(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return shader ? shader->status : RT_SHADER3D_STATUS_FAILED;
}

int8_t rt_shader3d_get_is_ready(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return (shader && shader->status == RT_SHADER3D_STATUS_READY) ? 1 : 0;
}

rt_string rt_shader3d_get_error(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return rt_const_cstr(shader && shader->error ? shader->error : "");
}

int64_t rt_shader3d_get_param_count(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return shader ? shader->param_count : 0;
}

rt_string rt_shader3d_param_name(void *obj, int64_t index) {
    rt_shader3d *shader = shader3d_checked(obj);
    if (!shader || index < 0 || index >= shader->param_count)
        return rt_const_cstr("");
    return rt_const_cstr(shader->params[index].name);
}

rt_string rt_shader3d_param_type(void *obj, int64_t index) {
    rt_shader3d *shader = shader3d_checked(obj);
    if (!shader || index < 0 || index >= shader->param_count)
        return rt_const_cstr("");
    return rt_const_cstr(rt_shader3d_type_name(shader->params[index].type));
}

int64_t rt_shader3d_get_texture_count(void *obj) {
    rt_shader3d *shader = shader3d_checked(obj);
    return shader ? shader->texture_count : 0;
}

rt_string rt_shader3d_texture_name(void *obj, int64_t index) {
    rt_shader3d *shader = shader3d_checked(obj);
    if (!shader || index < 0 || index >= shader->texture_count)
        return rt_const_cstr("");
    return rt_const_cstr(shader->texture_names[index]);
}

int8_t rt_shader3d_has_backend(void *obj, rt_string backend) {
    rt_shader3d *shader = shader3d_checked(obj);
    const char *name = backend ? rt_string_cstr(backend) : NULL;
    char lowered[16];
    int32_t index;
    size_t len;
    if (!shader || !name)
        return 0;
    len = strlen(name);
    if (len == 0 || len >= sizeof(lowered))
        return 0;
    memcpy(lowered, name, len + 1u);
    shader3d_lower(lowered);
    index = shader3d_section_backend(lowered);
    return (index >= 0 && shader->sources[index] && shader->sources[index][0]) ? 1 : 0;
}

int32_t rt_shader3d_find_param(const rt_shader3d *shader, const char *name) {
    if (!shader || !name)
        return -1;
    for (int32_t i = 0; i < shader->param_count; i++) {
        if (strcmp(shader->params[i].name, name) == 0)
            return i;
    }
    return -1;
}

int32_t rt_shader3d_find_texture(const rt_shader3d *shader, const char *name) {
    if (!shader || !name)
        return -1;
    for (int32_t i = 0; i < shader->texture_count; i++) {
        if (strcmp(shader->texture_names[i], name) == 0)
            return i;
    }
    return -1;
}

const char *rt_shader3d_backend_source(const rt_shader3d *shader, int32_t backend) {
    if (!shader || backend < 0 || backend >= RT_SHADER3D_BACKEND_COUNT)
        return NULL;
    return shader->sources[backend];
}

/// @brief Append text to a growable buffer.
static int shader3d_append_text(char **buf, size_t *len, size_t *cap, const char *text) {
    size_t tl = strlen(text);
    if (*len + tl + 1u > *cap) {
        size_t next = *cap ? *cap : 256u;
        char *grown;
        while (next < *len + tl + 1u)
            next *= 2u;
        grown = (char *)realloc(*buf, next);
        if (!grown)
            return 0;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, tl + 1u);
    *len += tl;
    return 1;
}

char *rt_shader3d_params_declaration(const rt_shader3d *shader, int32_t backend) {
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    int32_t cursor = 0;
    int32_t pad_index = 0;
    char line[160];
    const char *type_names[3][5] = {
        {"float", "float2", "float3", "float4", "int"},
        {"float", "float2", "float3", "float4", "int"},
        {"float", "vec2", "vec3", "vec4", "int"},
    };
    const char *head;
    if (!shader || backend < 0 || backend >= RT_SHADER3D_BACKEND_COUNT)
        return NULL;
    head = backend == RT_SHADER3D_BACKEND_METAL  ? "struct ZsParams {\n"
           : backend == RT_SHADER3D_BACKEND_HLSL ? "cbuffer ZsParams : register(b11)\n{\n"
                                                 : "layout(std140) uniform ZsParams\n{\n";
    if (!shader3d_append_text(&out, &len, &cap, head))
        goto fail;
    for (int32_t i = 0; i < shader->param_count; i++) {
        const rt_shader3d_param *p = &shader->params[i];
        int32_t consumed;
        while (cursor < p->offset) {
            snprintf(line, sizeof(line), "    float _zsPad%d;\n", pad_index++);
            if (!shader3d_append_text(&out, &len, &cap, line))
                goto fail;
            cursor++;
        }
        snprintf(line, sizeof(line), "    %s %s;\n", type_names[backend][p->type - 1], p->name);
        if (!shader3d_append_text(&out, &len, &cap, line))
            goto fail;
        /* Metal's float3 is 16 bytes in a constant block; HLSL and std140
         * pack the next float into the fourth lane, so they get an explicit pad. */
        consumed = (p->type == RT_SHADER3D_PARAM_FLOAT3 && backend != RT_SHADER3D_BACKEND_METAL)
                       ? 3
                       : p->lanes;
        cursor += consumed;
    }
    while (cursor < shader->param_floats) {
        snprintf(line, sizeof(line), "    float _zsPad%d;\n", pad_index++);
        if (!shader3d_append_text(&out, &len, &cap, line))
            goto fail;
        cursor++;
    }
    if (shader->param_count == 0 &&
        !shader3d_append_text(&out, &len, &cap, "    float _zsUnused;\n"))
        goto fail;
    if (!shader3d_append_text(&out, &len, &cap, "};\n"))
        goto fail;
    return out;
fail:
    free(out);
    return NULL;
}
