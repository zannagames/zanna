//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_action_io.c
/// @file
/// @brief Implements JSON serialization and transactional parsing for the
///        global input-action registry.
// Purpose: Input-action serialization: save the current action/binding set to JSON
//   and load it back, reconstructing actions and bindings via the action core.
//
// Persistence model:
//   - Save emits one top-level "actions" array in current linked-list order.
//   - Load constructs a temporary registry while retaining the previous list.
//     A syntactically/structurally invalid document destroys the temporary
//     nodes and restores the previous registry; successful parsing replaces it.
//   - Unknown object members and binding type tags are ignored for forward
//     compatibility. Recognized numeric fields receive defensive conversion.
//
// Ownership/Lifetime:
//   - Save returns a newly allocated runtime string.
//   - Load borrows its input string, owns its parser and pending-binding buffer,
//     and transfers successfully materialized nodes to the global registry.
//
// Links: rt_action.h (public API), rt_action_internal.h (shared model),
//        rt_json_stream.h (streaming JSON parser), rt_action.c (action core)
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_action_internal.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_json_stream.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief Extract an int64 value from a borrowed runtime box.
extern int64_t rt_unbox_i64(void *box);

/// @brief Map a `BindingType` enum to the JSON serialization tag.
/// @param type Internal binding kind.
/// @return Borrowed static tag string, or `"unknown"` for unsupported values.
static const char *binding_type_name(BindingType type) {
    switch (type) {
        case BIND_KEY:
            return "key";
        case BIND_MOUSE_BUTTON:
            return "mouse";
        case BIND_MOUSE_X:
            return "mouse_x";
        case BIND_MOUSE_Y:
            return "mouse_y";
        case BIND_SCROLL_X:
            return "scroll_x";
        case BIND_SCROLL_Y:
            return "scroll_y";
        case BIND_PAD_BUTTON:
            return "pad_button";
        case BIND_PAD_AXIS:
            return "pad_axis";
        case BIND_PAD_BUTTON_AXIS:
            return "pad_button_axis";
        case BIND_CHORD:
            return "chord";
        default:
            return "unknown";
    }
}

/// @brief Reverse map: JSON tag string → `BindingType` enum.
///
/// Returns `BIND_NONE` for unknown tags so the loader can skip them.
/// @param name Borrowed non-null JSON tag string.
/// @return Corresponding binding kind, or BIND_NONE when unrecognized.
static BindingType binding_type_from_name(const char *name) {
    if (strcmp(name, "key") == 0)
        return BIND_KEY;
    if (strcmp(name, "mouse") == 0)
        return BIND_MOUSE_BUTTON;
    if (strcmp(name, "mouse_x") == 0)
        return BIND_MOUSE_X;
    if (strcmp(name, "mouse_y") == 0)
        return BIND_MOUSE_Y;
    if (strcmp(name, "scroll_x") == 0)
        return BIND_SCROLL_X;
    if (strcmp(name, "scroll_y") == 0)
        return BIND_SCROLL_Y;
    if (strcmp(name, "pad_button") == 0)
        return BIND_PAD_BUTTON;
    if (strcmp(name, "pad_axis") == 0)
        return BIND_PAD_AXIS;
    if (strcmp(name, "pad_button_axis") == 0)
        return BIND_PAD_BUTTON_AXIS;
    if (strcmp(name, "chord") == 0)
        return BIND_CHORD;
    return BIND_NONE;
}

/// @brief Release a streaming JSON parser object created by rt_json_stream_new.
/// @details Releases one reference and frees the object if that was the final
///          reference.
/// @param parser Owned parser handle; `NULL` is ignored.
static void action_release_json_parser(void *parser) {
    if (parser && rt_obj_release_check0(parser))
        rt_obj_free(parser);
}

/// @brief Append a JSON-quoted-string-literal version of `str` to a builder.
///
/// Handles the standard JSON escape characters (`"`, `\\`, `\\n`,
/// `\\r`, `\\t`) and emits `\\u00XX` escapes for the remaining control bytes.
/// Non-ASCII bytes pass through as-is — the persistence format assumes UTF-8
/// throughout.
/// @param sb Borrowed destination builder.
/// @param str Borrowed non-null, null-terminated text to quote.
/// @return `1` on success; `0` when a builder append fails.
static int sb_append_json_string(rt_string_builder *sb, const char *str) {
    static const char hex[] = "0123456789ABCDEF";
    if (rt_sb_append_cstr(sb, "\"") != RT_SB_OK)
        return 0;
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        switch (c) {
            case '"':
                if (rt_sb_append_cstr(sb, "\\\"") != RT_SB_OK)
                    return 0;
                break;
            case '\\':
                if (rt_sb_append_cstr(sb, "\\\\") != RT_SB_OK)
                    return 0;
                break;
            case '\n':
                if (rt_sb_append_cstr(sb, "\\n") != RT_SB_OK)
                    return 0;
                break;
            case '\r':
                if (rt_sb_append_cstr(sb, "\\r") != RT_SB_OK)
                    return 0;
                break;
            case '\t':
                if (rt_sb_append_cstr(sb, "\\t") != RT_SB_OK)
                    return 0;
                break;
            default:
                if (c < 0x20u) {
                    char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xFu], hex[c & 0xFu]};
                    if (rt_sb_append_bytes(sb, esc, sizeof(esc)) != RT_SB_OK)
                        return 0;
                } else {
                    char byte = (char)c;
                    if (rt_sb_append_bytes(sb, &byte, 1) != RT_SB_OK)
                        return 0;
                }
                break;
        }
    }
    return rt_sb_append_cstr(sb, "\"") == RT_SB_OK;
}

/// @brief `Action.Save` — serialize all action+binding state to JSON.
///
/// Format: `{"actions":[{"name","type","bindings":[{"type","code","pad","value","keys":[...]}]}]}`.
/// `keys` is only present for chord bindings. The result is a fresh
/// `rt_string` that can be saved to disk and round-tripped via
/// `rt_action_load`. Actions and bindings retain current linked-list order.
/// @return Newly owned JSON runtime string. Builder failure returns a newly
///         allocated empty string when possible; final string allocation can
///         return `NULL`.
rt_string rt_action_save(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_string_builder sb;
    int8_t first_action;
    rt_sb_init(&sb);

    first_action = 1;
    if (rt_sb_append_cstr(&sb, "{\"actions\":[") != RT_SB_OK)
        goto save_error;

    {
        Action *a = g_actions;
        while (a) {
            int8_t first_binding;
            if (!first_action) {
                if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                    goto save_error;
            }
            first_action = 0;

            if (rt_sb_append_cstr(&sb, "{\"name\":") != RT_SB_OK ||
                !sb_append_json_string(&sb, a->name) ||
                rt_sb_append_cstr(&sb, ",\"type\":") != RT_SB_OK ||
                rt_sb_append_cstr(&sb, a->is_axis ? "\"axis\"" : "\"button\"") != RT_SB_OK ||
                rt_sb_append_cstr(&sb, ",\"bindings\":[") != RT_SB_OK)
                goto save_error;

            first_binding = 1;
            {
                Binding *b = a->bindings;
                while (b) {
                    if (!first_binding) {
                        if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                            goto save_error;
                    }
                    first_binding = 0;

                    if (rt_sb_append_cstr(&sb, "{\"type\":") != RT_SB_OK ||
                        !sb_append_json_string(&sb, binding_type_name(b->type)) ||
                        rt_sb_append_cstr(&sb, ",\"code\":") != RT_SB_OK ||
                        rt_sb_append_int(&sb, b->code) != RT_SB_OK ||
                        rt_sb_append_cstr(&sb, ",\"pad\":") != RT_SB_OK ||
                        rt_sb_append_int(&sb, b->pad_index) != RT_SB_OK ||
                        rt_sb_append_cstr(&sb, ",\"value\":") != RT_SB_OK ||
                        rt_sb_append_double(&sb, b->value) != RT_SB_OK)
                        goto save_error;
                    if (b->type == BIND_CHORD && b->chord_len > 0) {
                        int32_t ci;
                        if (rt_sb_append_cstr(&sb, ",\"keys\":[") != RT_SB_OK)
                            goto save_error;
                        for (ci = 0; ci < b->chord_len; ci++) {
                            if (ci > 0) {
                                if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                                    goto save_error;
                            }
                            if (rt_sb_append_int(&sb, b->chord_keys[ci]) != RT_SB_OK)
                                goto save_error;
                        }
                        if (rt_sb_append_cstr(&sb, "]") != RT_SB_OK)
                            goto save_error;
                    }
                    if (rt_sb_append_cstr(&sb, "}") != RT_SB_OK)
                        goto save_error;
                    b = b->next;
                }
            }

            if (rt_sb_append_cstr(&sb, "]}") != RT_SB_OK)
                goto save_error;
            a = a->next;
        }
    }

    if (rt_sb_append_cstr(&sb, "]}") != RT_SB_OK)
        goto save_error;

    {
        rt_string result = rt_string_from_bytes(sb.data, sb.len);
        rt_sb_free(&sb);
        return result;
    }

save_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

/// @brief Clamp a JSON number to a valid int64 without undefined behavior.
/// @details rt_json_stream_number_value returns a raw double parsed from an
///   external, user-editable bindings file. A non-finite value (or one outside the
///   int64 range) makes a direct (int64_t) cast undefined behavior (C11 6.3.1.4p1),
///   so reject/saturate before casting. Finite fractional values truncate
///   toward zero.
/// @param v Parsed JSON number.
/// @return Truncated int64, zero for non-finite input, or the nearest int64
///         endpoint for out-of-range input.
static int64_t action_clamp_json_i64(double v) {
    if (!isfinite(v))
        return 0;
    if (v >= 9223372036854775808.0)
        return INT64_MAX;
    if (v < -9223372036854775808.0)
        return INT64_MIN;
    return (int64_t)v;
}

/// @brief One parsed binding, buffered until the enclosing action object closes so
///        that "name"/"type"/"bindings" may appear in any order (JSON is unordered).
typedef struct {
    /// Parsed source kind; BIND_NONE causes the object to be ignored.
    BindingType btype;
    /// Parsed key, button, or axis identifier.
    int64_t code;
    /// Parsed controller index.
    int64_t pad;
    /// Parsed digital contribution or analog scale.
    double value;
    /// Parsed ordered chord keys.
    int64_t chord_keys[MAX_CHORD_KEYS];
    /// Number of populated @ref chord_keys entries.
    int32_t chord_len;
} action_pending_binding;

/// @brief `Action.Load(json)` — restore action and binding state from JSON.
/// @details Uses the streaming parser rather than constructing a JSON tree.
///          The previous global list remains recoverable until the complete
///          top-level object and its single `"actions"` array parse cleanly.
///          Unknown members are skipped, unknown binding types are omitted,
///          absent binding fields retain defaults, and action names must fit
///          in 255 bytes. Parsed actions are inserted at the global list head.
///          Allocation failure while growing the pending buffer fails the
///          transaction; individual action/binding allocation failures are
///          silently omitted by the underlying creation helpers.
/// @param json Borrowed runtime string containing the JSON document.
/// @return `1` after a structurally valid complete document replaces the
///         registry; `0` on null input, parser creation failure, malformed
///         structure, excessive chord/name length, or pending-buffer failure.
int8_t rt_action_load(rt_string json) {
    RT_ASSERT_MAIN_THREAD();
    void *parser = NULL;
    int64_t tok = RT_JSON_TOK_ERROR;
    int8_t success = 0;
    int8_t saw_actions = 0;
    Action *saved_actions = NULL;
    /* Per-action-object binding buffer, reused across objects and freed once at
     * cleanup so the many `goto cleanup` sites need no per-branch free. */
    action_pending_binding *pending = NULL;
    int pending_count = 0;
    int pending_cap = 0;

    if (!json)
        return 0;

    parser = rt_json_stream_new(json);
    if (!parser)
        return 0;

    if (!g_initialized)
        rt_action_init();

    saved_actions = g_actions;
    g_actions = NULL;

    tok = rt_json_stream_next(parser);
    if (tok != RT_JSON_TOK_OBJECT_START)
        goto cleanup;

    tok = rt_json_stream_next(parser);
    while (tok == RT_JSON_TOK_KEY) {
        rt_string top_key = rt_json_stream_string_value(parser);
        int is_actions_key = (strcmp(rt_string_cstr(top_key), "actions") == 0);
        rt_string_unref(top_key);
        tok = rt_json_stream_next(parser);
        if (!is_actions_key) {
            rt_json_stream_skip(parser);
            tok = rt_json_stream_next(parser);
            continue;
        }
        if (saw_actions || tok != RT_JSON_TOK_ARRAY_START)
            goto cleanup;
        saw_actions = 1;

        tok = rt_json_stream_next(parser);
        while (tok == RT_JSON_TOK_OBJECT_START) {
            char action_name[256];
            int8_t is_axis = 0;
            action_name[0] = '\0';
            /* Buffer this object's bindings and materialize the action at OBJECT_END,
             * so "name"/"type"/"bindings" may appear in any order — JSON members are
             * unordered, and a third-party file need not match rt_action_save's order.
             * Reuse the function-scope buffer across objects. */
            pending_count = 0;

            tok = rt_json_stream_next(parser);
            while (tok == RT_JSON_TOK_KEY) {
                rt_string key = rt_json_stream_string_value(parser);
                const char *key_cstr = rt_string_cstr(key);
                int key_name = (strcmp(key_cstr, "name") == 0);
                int key_type = (strcmp(key_cstr, "type") == 0);
                int key_bindings = (strcmp(key_cstr, "bindings") == 0);
                rt_string_unref(key);
                tok = rt_json_stream_next(parser);

                if (key_name) {
                    if (tok != RT_JSON_TOK_STRING)
                        goto cleanup;
                    rt_string val = rt_json_stream_string_value(parser);
                    const char *val_cstr = rt_string_cstr(val);
                    size_t len = strlen(val_cstr);
                    if (len >= sizeof(action_name)) {
                        rt_string_unref(val);
                        goto cleanup;
                    }
                    memcpy(action_name, val_cstr, len);
                    action_name[len] = '\0';
                    rt_string_unref(val);
                } else if (key_type) {
                    if (tok != RT_JSON_TOK_STRING)
                        goto cleanup;
                    rt_string val = rt_json_stream_string_value(parser);
                    is_axis = (strcmp(rt_string_cstr(val), "axis") == 0) ? 1 : 0;
                    rt_string_unref(val);
                } else if (key_bindings) {
                    if (tok != RT_JSON_TOK_ARRAY_START)
                        goto cleanup;

                    tok = rt_json_stream_next(parser);
                    while (tok == RT_JSON_TOK_OBJECT_START) {
                        action_pending_binding pb;
                        pb.btype = BIND_NONE;
                        pb.code = 0;
                        pb.pad = 0;
                        pb.value = 0.0;
                        pb.chord_len = 0;

                        tok = rt_json_stream_next(parser);
                        while (tok == RT_JSON_TOK_KEY) {
                            rt_string bkey = rt_json_stream_string_value(parser);
                            const char *bkey_cstr = rt_string_cstr(bkey);
                            int bk_type = (strcmp(bkey_cstr, "type") == 0);
                            int bk_code = (strcmp(bkey_cstr, "code") == 0);
                            int bk_pad = (strcmp(bkey_cstr, "pad") == 0);
                            int bk_value = (strcmp(bkey_cstr, "value") == 0);
                            int bk_keys = (strcmp(bkey_cstr, "keys") == 0);
                            rt_string_unref(bkey);
                            tok = rt_json_stream_next(parser);

                            if (bk_type) {
                                if (tok != RT_JSON_TOK_STRING)
                                    goto cleanup;
                                rt_string bval = rt_json_stream_string_value(parser);
                                pb.btype = binding_type_from_name(rt_string_cstr(bval));
                                rt_string_unref(bval);
                            } else if (bk_code) {
                                if (tok != RT_JSON_TOK_NUMBER)
                                    goto cleanup;
                                pb.code = action_clamp_json_i64(rt_json_stream_number_value(parser));
                            } else if (bk_pad) {
                                if (tok != RT_JSON_TOK_NUMBER)
                                    goto cleanup;
                                pb.pad = action_clamp_json_i64(rt_json_stream_number_value(parser));
                            } else if (bk_value) {
                                if (tok != RT_JSON_TOK_NUMBER)
                                    goto cleanup;
                                pb.value = rt_json_stream_number_value(parser);
                            } else if (bk_keys) {
                                if (tok != RT_JSON_TOK_ARRAY_START)
                                    goto cleanup;
                                pb.chord_len = 0;
                                tok = rt_json_stream_next(parser);
                                while (tok == RT_JSON_TOK_NUMBER) {
                                    if (pb.chord_len >= MAX_CHORD_KEYS)
                                        goto cleanup;
                                    pb.chord_keys[pb.chord_len++] =
                                        action_clamp_json_i64(rt_json_stream_number_value(parser));
                                    tok = rt_json_stream_next(parser);
                                }
                                if (tok != RT_JSON_TOK_ARRAY_END)
                                    goto cleanup;
                            } else {
                                rt_json_stream_skip(parser);
                            }

                            tok = rt_json_stream_next(parser);
                        }
                        if (tok != RT_JSON_TOK_OBJECT_END)
                            goto cleanup;

                        if (pb.btype != BIND_NONE) {
                            if (pending_count >= pending_cap) {
                                int new_cap = pending_cap ? pending_cap * 2 : 8;
                                action_pending_binding *grown = (action_pending_binding *)realloc(
                                    pending, (size_t)new_cap * sizeof(*pending));
                                if (!grown)
                                    goto cleanup;
                                pending = grown;
                                pending_cap = new_cap;
                            }
                            pending[pending_count++] = pb;
                        }
                        tok = rt_json_stream_next(parser);
                    }
                    if (tok != RT_JSON_TOK_ARRAY_END)
                        goto cleanup;
                } else {
                    rt_json_stream_skip(parser);
                }
                tok = rt_json_stream_next(parser);
            }
            if (tok != RT_JSON_TOK_OBJECT_END)
                goto cleanup;

            /* Materialize now that name and type are known regardless of key order. */
            if (action_name[0] != '\0') {
                rt_string name_str = rt_const_cstr(action_name);
                if (is_axis)
                    rt_action_define_axis(name_str);
                else
                    rt_action_define(name_str);
                rt_string_unref(name_str);
                Action *a = find_action(action_name);
                if (a) {
                    for (int pi = 0; pi < pending_count; pi++) {
                        Binding *b = create_binding(
                            pending[pi].btype, pending[pi].code, pending[pi].pad, pending[pi].value);
                        if (b) {
                            if (pending[pi].btype == BIND_CHORD) {
                                b->chord_len = pending[pi].chord_len;
                                for (int32_t ci = 0; ci < pending[pi].chord_len; ci++)
                                    b->chord_keys[ci] = pending[pi].chord_keys[ci];
                            }
                            add_binding(a, b);
                        }
                    }
                }
            }
            tok = rt_json_stream_next(parser);
        }
        if (tok != RT_JSON_TOK_ARRAY_END)
            goto cleanup;
        tok = rt_json_stream_next(parser);
    }
    if (tok != RT_JSON_TOK_OBJECT_END)
        goto cleanup;
    tok = rt_json_stream_next(parser);
    if (tok == RT_JSON_TOK_END && saw_actions)
        success = 1;

cleanup:
    free(pending);
    action_release_json_parser(parser);
    if (success) {
        Action *loaded_actions = g_actions;
        g_actions = saved_actions;
        rt_action_clear();
        g_actions = loaded_actions;
    } else {
        rt_action_clear();
        g_actions = saved_actions;
    }
    return success;
}
