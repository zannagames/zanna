//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_ascii.inc
// Purpose: Length-bounded, brace-scoped ASCII FBX lexer and typed graph parser. This file is
//   included from rt_fbx_loader.c and is not a standalone translation unit.
//
// Key invariants:
//   - Every cursor is bounded by an explicit exclusive end pointer; input NUL bytes are rejected.
//   - Comments and quoted strings are tokenized before node matching, so decoy text cannot escape
//     its lexical scope.
//   - ASCII nodes produce the same fbx_node_t/fbx_prop_t graph consumed by the binary extractor.
//   - Declared array counts must exactly match their `a:` payload and are charged before
//   allocation.
//
// Ownership/Lifetime:
//   - The destination root owns all decoded names, properties, arrays, and child nodes.
//   - fbx_free_node() releases a complete or partially parsed graph after any failure.
//
// Links: rt_fbx_loader.c, rt_fbx_loader_parse.inc, docs/adr/0139-graphics3d-transactional-
//   hardening-and-retained-work.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_ascii.inc
 * @brief Implements a bounded lexer and typed parser for ASCII FBX documents.
 *
 * The lexer operates entirely on explicit byte spans, preserves line endings
 * for grammar decisions, skips semicolon comments, validates decimal syntax,
 * and borrows token payloads from the input. The recursive parser converts
 * those tokens into the same owned `fbx_node_t` and `fbx_prop_t` graph used by
 * the binary reader while charging every retained allocation to the active
 * load budget. This file is included by `rt_fbx_loader.c`.
 */
/*==========================================================================
 * Length-bounded ASCII tokenization
 *=========================================================================*/

/// @brief Token categories recognized by the ASCII FBX lexer.
typedef enum {
    /// End of the bounded input span.
    FBX_ASCII_TOKEN_EOF = 0,
    /// One logical CR, LF, or CRLF line ending.
    FBX_ASCII_TOKEN_EOL,
    /// Unquoted nonnumeric payload.
    FBX_ASCII_TOKEN_IDENTIFIER,
    /// Quoted payload with quote delimiters excluded.
    FBX_ASCII_TOKEN_STRING,
    /// Strict signed decimal or exponent spelling.
    FBX_ASCII_TOKEN_NUMBER,
    /// Colon punctuation.
    FBX_ASCII_TOKEN_COLON,
    /// Comma punctuation.
    FBX_ASCII_TOKEN_COMMA,
    /// Array-count asterisk.
    FBX_ASCII_TOKEN_STAR,
    /// Opening child-scope brace.
    FBX_ASCII_TOKEN_LBRACE,
    /// Closing child-scope brace.
    FBX_ASCII_TOKEN_RBRACE
} fbx_ascii_token_kind_t;

/// @brief One borrowed token span inside an ASCII FBX input buffer.
typedef struct {
    fbx_ascii_token_kind_t kind; ///< Lexical category.
    const char *data;            ///< First payload byte (quotes excluded for strings).
    size_t length;               ///< Exact payload byte count.
    int has_escape;              ///< Nonzero when a quoted string contains a backslash escape.
} fbx_ascii_token_t;

/// @brief Cursor, current token, error state, and budget for one ASCII FBX parse.
typedef struct {
    const char *begin;           ///< First input byte.
    const char *cursor;          ///< Next byte to tokenize.
    const char *end;             ///< Exclusive input boundary.
    fbx_ascii_token_t token;     ///< Current token.
    fbx_load_context_t *context; ///< Aggregate allocation budget.
    int error;                   ///< Sticky lexical or syntactic failure flag.
} fbx_ascii_parser_t;

/// @brief Return whether a byte terminates an unquoted ASCII FBX token.
/// @param ch Unsigned byte promoted to int, or EOF-style negative input.
/// @return Nonzero for whitespace, punctuation, or a comment introducer.
static int fbx_ascii_is_token_delimiter(int ch) {
    return ch < 0 || isspace((unsigned char)ch) || ch == ':' || ch == ',' || ch == '*' ||
           ch == '{' || ch == '}' || ch == '"' || ch == ';';
}

/// @brief Lex the next length-bounded token into @p parser->token.
/// @details Spaces are skipped, line endings are retained as grammar tokens, and semicolon comments
///          are discarded through (but not including) their line ending. Quoted strings honor
///          escaped quotes/backslashes without interpreting their payload at this stage. Numeric
///          tokens require a complete decimal/exponent grammar and a delimiter at the end.
/// @param parser Parser whose cursor should advance.
/// @return Nonzero when a token (including EOF) was produced, zero on malformed input.
static int fbx_ascii_lex_next(fbx_ascii_parser_t *parser) {
    const char *p;
    if (!parser || parser->error)
        return 0;
    p = parser->cursor;
    for (;;) {
        while (p < parser->end && (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f'))
            p++;
        if (p < parser->end && *p == ';') {
            while (p < parser->end && *p != '\r' && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    memset(&parser->token, 0, sizeof(parser->token));
    if (p >= parser->end) {
        parser->cursor = parser->end;
        parser->token.kind = FBX_ASCII_TOKEN_EOF;
        parser->token.data = parser->end;
        return 1;
    }
    if (*p == '\0') {
        parser->error = 1;
        return 0;
    }
    if (*p == '\r' || *p == '\n') {
        parser->token.kind = FBX_ASCII_TOKEN_EOL;
        parser->token.data = p;
        parser->token.length = 1u;
        if (*p == '\r' && p + 1 < parser->end && p[1] == '\n')
            p++;
        parser->cursor = p + 1;
        return 1;
    }
    switch (*p) {
        case ':':
            parser->token.kind = FBX_ASCII_TOKEN_COLON;
            break;
        case ',':
            parser->token.kind = FBX_ASCII_TOKEN_COMMA;
            break;
        case '*':
            parser->token.kind = FBX_ASCII_TOKEN_STAR;
            break;
        case '{':
            parser->token.kind = FBX_ASCII_TOKEN_LBRACE;
            break;
        case '}':
            parser->token.kind = FBX_ASCII_TOKEN_RBRACE;
            break;
        default:
            parser->token.kind = FBX_ASCII_TOKEN_EOF;
            break;
    }
    if (parser->token.kind != FBX_ASCII_TOKEN_EOF) {
        parser->token.data = p;
        parser->token.length = 1u;
        parser->cursor = p + 1;
        return 1;
    }
    if (*p == '"') {
        const char *start = ++p;
        int escaped = 0;
        while (p < parser->end) {
            if (*p == '\0' || *p == '\r' || *p == '\n') {
                parser->error = 1;
                return 0;
            }
            if (*p == '"') {
                parser->token.kind = FBX_ASCII_TOKEN_STRING;
                parser->token.data = start;
                parser->token.length = (size_t)(p - start);
                parser->token.has_escape = escaped;
                parser->cursor = p + 1;
                return 1;
            }
            if (*p == '\\') {
                escaped = 1;
                p++;
                if (p >= parser->end || *p == '\r' || *p == '\n' || *p == '\0') {
                    parser->error = 1;
                    return 0;
                }
            }
            p++;
        }
        parser->error = 1;
        return 0;
    }
    if ((*p >= '0' && *p <= '9') ||
        ((*p == '+' || *p == '-') && p + 1 < parser->end &&
         ((p[1] >= '0' && p[1] <= '9') || p[1] == '.')) ||
        (*p == '.' && p + 1 < parser->end && p[1] >= '0' && p[1] <= '9')) {
        const char *start = p;
        int digits = 0;
        if (*p == '+' || *p == '-')
            p++;
        while (p < parser->end && *p >= '0' && *p <= '9') {
            digits++;
            p++;
        }
        if (p < parser->end && *p == '.') {
            p++;
            while (p < parser->end && *p >= '0' && *p <= '9') {
                digits++;
                p++;
            }
        }
        if (digits == 0) {
            parser->error = 1;
            return 0;
        }
        if (p < parser->end && (*p == 'e' || *p == 'E')) {
            const char *exponent = ++p;
            if (p < parser->end && (*p == '+' || *p == '-'))
                p++;
            exponent = p;
            while (p < parser->end && *p >= '0' && *p <= '9')
                p++;
            if (p == exponent) {
                parser->error = 1;
                return 0;
            }
        }
        if (p < parser->end && !fbx_ascii_is_token_delimiter((unsigned char)*p)) {
            parser->error = 1;
            return 0;
        }
        parser->token.kind = FBX_ASCII_TOKEN_NUMBER;
        parser->token.data = start;
        parser->token.length = (size_t)(p - start);
        parser->cursor = p;
        return 1;
    }
    {
        const char *start = p;
        while (p < parser->end && !fbx_ascii_is_token_delimiter((unsigned char)*p)) {
            if (*p == '\0') {
                parser->error = 1;
                return 0;
            }
            p++;
        }
        if (p == start) {
            parser->error = 1;
            return 0;
        }
        parser->token.kind = FBX_ASCII_TOKEN_IDENTIFIER;
        parser->token.data = start;
        parser->token.length = (size_t)(p - start);
        parser->cursor = p;
        return 1;
    }
}

/// @brief Compare the current token payload to one case-sensitive C string.
/// @param token Borrowed token span.
/// @param text NUL-terminated comparison text.
/// @return Nonzero only when byte lengths and contents match exactly.
static int fbx_ascii_token_equals(const fbx_ascii_token_t *token, const char *text) {
    size_t length = text ? strlen(text) : 0u;
    return token && text && token->length == length && memcmp(token->data, text, length) == 0;
}

/// @brief Parse an integer-only numeric token without locale or overflow ambiguity.
/// @param token NUMBER token containing optional sign and decimal digits only.
/// @param out_value Receives the exact signed 64-bit result.
/// @return Nonzero on success; zero for fractions, exponents, empty digits, or overflow.
static int fbx_ascii_token_i64(const fbx_ascii_token_t *token, int64_t *out_value) {
    size_t i = 0;
    int negative = 0;
    uint64_t value = 0;
    uint64_t limit;
    if (!token || !out_value || token->kind != FBX_ASCII_TOKEN_NUMBER || token->length == 0)
        return 0;
    if (token->data[i] == '+' || token->data[i] == '-') {
        negative = token->data[i] == '-';
        i++;
    }
    if (i >= token->length)
        return 0;
    limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < token->length; i++) {
        uint64_t digit;
        if (token->data[i] < '0' || token->data[i] > '9')
            return 0;
        digit = (uint64_t)(token->data[i] - '0');
        if (value > (limit - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    if (negative && value == (uint64_t)INT64_MAX + 1u)
        *out_value = INT64_MIN;
    else
        *out_value = negative ? -(int64_t)value : (int64_t)value;
    return 1;
}

/// @brief Compute a bounded integral power of ten for locale-independent decimal conversion.
/// @param exponent Signed exponent in the parser-audited finite range.
/// @return Approximate `10^exponent`, potentially overflowing/underflowing for later rejection.
static double fbx_ascii_power10(int exponent) {
    unsigned int remaining = (unsigned int)(exponent < 0 ? -exponent : exponent);
    double base = exponent < 0 ? 0.1 : 10.0;
    double result = 1.0;
    while (remaining != 0u) {
        if ((remaining & 1u) != 0u)
            result *= base;
        remaining >>= 1u;
        if (remaining != 0u)
            base *= base;
    }
    return result;
}

/// @brief Convert a complete decimal NUMBER token to one finite double.
/// @details Conversion uses explicit `.` semantics and never consults process locale. Exponents
///          outside a conservative finite-double window and intermediate overflow are rejected.
/// @param token NUMBER token produced by the strict lexer.
/// @param out_value Receives the finite result.
/// @return Nonzero on success, zero on malformed input or a non-finite result.
static int fbx_ascii_token_double(const fbx_ascii_token_t *token, double *out_value) {
    size_t i = 0;
    int sign = 1;
    int fractional_digits = 0;
    int exponent_sign = 1;
    int exponent = 0;
    double value = 0.0;
    double result;
    if (!token || !out_value || token->kind != FBX_ASCII_TOKEN_NUMBER || token->length == 0)
        return 0;
    if (token->data[i] == '+' || token->data[i] == '-') {
        sign = token->data[i] == '-' ? -1 : 1;
        i++;
    }
    while (i < token->length && token->data[i] >= '0' && token->data[i] <= '9') {
        value = value * 10.0 + (double)(token->data[i] - '0');
        if (!isfinite(value))
            return 0;
        i++;
    }
    if (i < token->length && token->data[i] == '.') {
        i++;
        while (i < token->length && token->data[i] >= '0' && token->data[i] <= '9') {
            value = value * 10.0 + (double)(token->data[i] - '0');
            if (!isfinite(value) || fractional_digits >= 1000)
                return 0;
            fractional_digits++;
            i++;
        }
    }
    if (i < token->length && (token->data[i] == 'e' || token->data[i] == 'E')) {
        i++;
        if (i < token->length && (token->data[i] == '+' || token->data[i] == '-')) {
            exponent_sign = token->data[i] == '-' ? -1 : 1;
            i++;
        }
        while (i < token->length && token->data[i] >= '0' && token->data[i] <= '9') {
            if (exponent > 1000)
                return 0;
            exponent = exponent * 10 + (token->data[i] - '0');
            i++;
        }
    }
    if (i != token->length)
        return 0;
    exponent = exponent_sign * exponent - fractional_digits;
    if (exponent < -1000 || exponent > 1000)
        return 0;
    result = (double)sign * value * fbx_ascii_power10(exponent);
    if (!isfinite(result))
        return 0;
    *out_value = result;
    return 1;
}

/*==========================================================================
 * Typed property construction
 *=========================================================================*/

/// @brief Append one exact ASCII string/identifier token as an owned `S` property.
/// @details Quoted `\\`, `\"`, `\n`, `\r`, and `\t` escapes are decoded. Unknown escapes retain
///          both bytes so Windows paths and exporter-specific text are not silently rewritten.
/// @param node Destination node.
/// @param token STRING or IDENTIFIER token to retain.
/// @param context Active load context charged for property capacity and decoded storage.
/// @return Nonzero on success, zero on overflow, budget exhaustion, or allocation failure.
static int fbx_ascii_append_string_property(fbx_node_t *node,
                                            const fbx_ascii_token_t *token,
                                            fbx_load_context_t *context) {
    char *decoded;
    size_t decoded_length = 0;
    fbx_prop_t *prop;
    if (!node || !token ||
        (token->kind != FBX_ASCII_TOKEN_STRING && token->kind != FBX_ASCII_TOKEN_IDENTIFIER)) {
        return 0;
    }
    for (size_t i = 0; i < token->length; i++) {
        if (token->kind == FBX_ASCII_TOKEN_STRING && token->data[i] == '\\' &&
            i + 1u < token->length) {
            char escaped = token->data[i + 1u];
            decoded_length += (escaped == '\\' || escaped == '"' || escaped == 'n' ||
                               escaped == 'r' || escaped == 't')
                                  ? 1u
                                  : 2u;
            i++;
        } else {
            decoded_length++;
        }
    }
    if (decoded_length > UINT32_MAX || decoded_length == SIZE_MAX ||
        !fbx_node_reserve_property(node, context) ||
        !fbx_budget_charge(context, (uint64_t)decoded_length + 1u, 1u)) {
        return 0;
    }
    decoded = (char *)malloc(decoded_length + 1u);
    if (!decoded)
        return 0;
    decoded_length = 0;
    for (size_t i = 0; i < token->length; i++) {
        char ch = token->data[i];
        if (token->kind == FBX_ASCII_TOKEN_STRING && ch == '\\' && i + 1u < token->length) {
            char escaped = token->data[++i];
            if (escaped == '\\' || escaped == '"')
                decoded[decoded_length++] = escaped;
            else if (escaped == 'n')
                decoded[decoded_length++] = '\n';
            else if (escaped == 'r')
                decoded[decoded_length++] = '\r';
            else if (escaped == 't')
                decoded[decoded_length++] = '\t';
            else {
                decoded[decoded_length++] = '\\';
                decoded[decoded_length++] = escaped;
            }
        } else {
            decoded[decoded_length++] = ch;
        }
    }
    decoded[decoded_length] = '\0';
    prop = &node->props[node->prop_count];
    prop->type = 'S';
    prop->v.string.str = decoded;
    prop->v.string.len = (uint32_t)decoded_length;
    node->prop_count++;
    return 1;
}

/// @brief Append one numeric ASCII token using exact int64 storage when possible.
/// @details Integral spelling is retained as `L`; decimal/exponent spelling becomes `D`. Shared
///          coercion helpers allow ordinary FBX scalar consumers to accept either representation.
/// @param node Destination node.
/// @param token NUMBER token.
/// @param context Active load context charged for property capacity.
/// @return Nonzero on success, zero on malformed/non-finite input or allocation failure.
static int fbx_ascii_append_number_property(fbx_node_t *node,
                                            const fbx_ascii_token_t *token,
                                            fbx_load_context_t *context) {
    int64_t integer;
    double number;
    fbx_prop_t *prop;
    if (!node || !token || !fbx_node_reserve_property(node, context))
        return 0;
    prop = &node->props[node->prop_count];
    if (fbx_ascii_token_i64(token, &integer)) {
        prop->type = 'L';
        prop->v.i64 = integer;
    } else if (fbx_ascii_token_double(token, &number)) {
        prop->type = 'D';
        prop->v.f64 = number;
    } else {
        return 0;
    }
    node->prop_count++;
    return 1;
}

/// @brief Choose the binary-compatible array property type for one ASCII node name.
/// @details Geometry/skin/material index arrays use int32, animation times use int64, and all
///          remaining numeric payloads use double so the existing binary extraction paths consume
///          them without a format-specific branch.
/// @param node_name NUL-terminated structural node name.
/// @return One of `i`, `l`, or `d`.
static char fbx_ascii_array_type(const char *node_name) {
    size_t length;
    if (!node_name)
        return 'd';
    if (strcmp(node_name, "KeyTime") == 0)
        return 'l';
    if (strcmp(node_name, "PolygonVertexIndex") == 0 || strcmp(node_name, "Indexes") == 0 ||
        strcmp(node_name, "Materials") == 0 || strcmp(node_name, "KeyAttrFlags") == 0 ||
        strcmp(node_name, "Edges") == 0 || strcmp(node_name, "UVIndex0") == 0 ||
        strcmp(node_name, "UVIndex1") == 0) {
        return 'i';
    }
    length = strlen(node_name);
    if (length >= 5u && strcmp(node_name + length - 5u, "Index") == 0)
        return 'i';
    return 'd';
}

/// @brief Parse an ASCII `*count { a: ... }` payload into one binary-compatible array property.
/// @details The declared count is validated before allocation, the destination is allocated once,
///          every element must match the selected integer/double type, and the closing brace is
///          accepted only after exactly the declared number of values.
/// @param parser Parser positioned on the STAR token.
/// @param node Destination array node, already carrying its structural name.
/// @return Nonzero on success; zero on syntax, range, count, budget, or allocation failure.
static int fbx_ascii_parse_array_property(fbx_ascii_parser_t *parser, fbx_node_t *node) {
    int64_t declared_i64;
    uint32_t declared;
    uint32_t parsed = 0;
    char type;
    size_t element_size;
    void *data;
    fbx_prop_t *prop;
    if (!parser || !node || parser->token.kind != FBX_ASCII_TOKEN_STAR ||
        !fbx_ascii_lex_next(parser) || !fbx_ascii_token_i64(&parser->token, &declared_i64) ||
        declared_i64 < 0 || (uint64_t)declared_i64 > UINT32_MAX) {
        return 0;
    }
    declared = (uint32_t)declared_i64;
    type = fbx_ascii_array_type(node->name);
    element_size = type == 'i' ? sizeof(int32_t) : type == 'l' ? sizeof(int64_t) : sizeof(double);
    if (!rt_untrusted_count_ok((int64_t)declared, element_size, FBX_MAX_DECOMPRESSED_ARRAY_BYTES) ||
        !fbx_ascii_lex_next(parser)) {
        return 0;
    }
    while (parser->token.kind == FBX_ASCII_TOKEN_EOL)
        if (!fbx_ascii_lex_next(parser))
            return 0;
    if (parser->token.kind != FBX_ASCII_TOKEN_LBRACE || !fbx_ascii_lex_next(parser))
        return 0;
    while (parser->token.kind == FBX_ASCII_TOKEN_EOL)
        if (!fbx_ascii_lex_next(parser))
            return 0;
    if (!fbx_ascii_token_equals(&parser->token, "a") || !fbx_ascii_lex_next(parser) ||
        parser->token.kind != FBX_ASCII_TOKEN_COLON || !fbx_ascii_lex_next(parser)) {
        return 0;
    }
    if (!fbx_node_reserve_property(node, parser->context) ||
        !fbx_budget_charge(
            parser->context, declared > 0 ? declared : 1u, declared > 0 ? element_size : 1u)) {
        return 0;
    }
    data = calloc(declared > 0 ? declared : 1u, declared > 0 ? element_size : 1u);
    if (!data)
        return 0;
    for (;;) {
        while (parser->token.kind == FBX_ASCII_TOKEN_EOL)
            if (!fbx_ascii_lex_next(parser))
                goto fail;
        if (parser->token.kind == FBX_ASCII_TOKEN_RBRACE)
            break;
        if (parser->token.kind == FBX_ASCII_TOKEN_COMMA) {
            if (!fbx_ascii_lex_next(parser))
                goto fail;
            continue;
        }
        if (parser->token.kind != FBX_ASCII_TOKEN_NUMBER || parsed >= declared)
            goto fail;
        if (type == 'i') {
            int64_t value;
            if (!fbx_ascii_token_i64(&parser->token, &value) || value < INT32_MIN ||
                value > INT32_MAX) {
                goto fail;
            }
            ((int32_t *)data)[parsed] = (int32_t)value;
        } else if (type == 'l') {
            if (!fbx_ascii_token_i64(&parser->token, &((int64_t *)data)[parsed]))
                goto fail;
        } else if (!fbx_ascii_token_double(&parser->token, &((double *)data)[parsed])) {
            goto fail;
        }
        parsed++;
        if (!fbx_ascii_lex_next(parser))
            goto fail;
    }
    if (parsed != declared || !fbx_ascii_lex_next(parser))
        goto fail;
    prop = &node->props[node->prop_count];
    prop->type = type;
    prop->v.array.data = data;
    prop->v.array.count = declared;
    prop->v.array.elem_type = type;
    node->prop_count++;
    return 1;
fail:
    free(data);
    return 0;
}

/*==========================================================================
 * Brace-scoped typed graph parsing
 *=========================================================================*/

/// @brief Parse one named ASCII FBX node and its optional child block recursively.
/// @details A newline terminates ordinary property lists, while braces establish the only child
///          scope. Array declarations use @ref fbx_ascii_parse_array_property and may span lines.
///          The destination remains safely freeable after every failure path.
/// @param parser Parser positioned on an IDENTIFIER node name.
/// @param node Zeroed destination node.
/// @param depth Current recursive depth, bounded by @ref FBX_MAX_NODE_DEPTH.
/// @return Nonzero on success, zero on lexical/syntactic/allocation failure.
static int fbx_ascii_parse_node(fbx_ascii_parser_t *parser, fbx_node_t *node, int depth) {
    fbx_ascii_token_t name;
    int expect_value = 1;
    if (!parser || !node || depth > FBX_MAX_NODE_DEPTH ||
        parser->token.kind != FBX_ASCII_TOKEN_IDENTIFIER) {
        return 0;
    }
    memset(node, 0, sizeof(*node));
    name = parser->token;
    if (!fbx_node_set_name(node, name.data, name.length, parser->context) ||
        !fbx_ascii_lex_next(parser) || parser->token.kind != FBX_ASCII_TOKEN_COLON ||
        !fbx_ascii_lex_next(parser)) {
        return 0;
    }
    if (parser->token.kind == FBX_ASCII_TOKEN_STAR)
        return fbx_ascii_parse_array_property(parser, node);

    while (
        parser->token.kind != FBX_ASCII_TOKEN_LBRACE && parser->token.kind != FBX_ASCII_TOKEN_EOL &&
        parser->token.kind != FBX_ASCII_TOKEN_RBRACE && parser->token.kind != FBX_ASCII_TOKEN_EOF) {
        if (parser->token.kind == FBX_ASCII_TOKEN_COMMA) {
            if (expect_value || !fbx_ascii_lex_next(parser))
                return 0;
            expect_value = 1;
            continue;
        }
        if (!expect_value)
            return 0;
        if (parser->token.kind == FBX_ASCII_TOKEN_NUMBER) {
            if (!fbx_ascii_append_number_property(node, &parser->token, parser->context))
                return 0;
        } else if (parser->token.kind == FBX_ASCII_TOKEN_STRING ||
                   parser->token.kind == FBX_ASCII_TOKEN_IDENTIFIER) {
            if (!fbx_ascii_append_string_property(node, &parser->token, parser->context))
                return 0;
        } else {
            return 0;
        }
        expect_value = 0;
        if (!fbx_ascii_lex_next(parser))
            return 0;
    }
    if (expect_value && node->prop_count > 0)
        return 0;
    if (parser->token.kind != FBX_ASCII_TOKEN_LBRACE)
        return 1;
    if (!fbx_ascii_lex_next(parser))
        return 0;
    for (;;) {
        while (parser->token.kind == FBX_ASCII_TOKEN_EOL)
            if (!fbx_ascii_lex_next(parser))
                return 0;
        if (parser->token.kind == FBX_ASCII_TOKEN_RBRACE)
            return fbx_ascii_lex_next(parser);
        if (parser->token.kind == FBX_ASCII_TOKEN_EOF ||
            parser->token.kind != FBX_ASCII_TOKEN_IDENTIFIER ||
            !fbx_node_reserve_child(node, parser->context)) {
            return 0;
        }
        if (!fbx_ascii_parse_node(parser, &node->children[node->child_count], depth + 1))
            return 0;
        node->child_count++;
    }
}

/// @brief Parse a complete ASCII FBX byte span into the shared typed node/property graph.
/// @details The function never calls C-string search routines on input. Embedded NUL bytes,
///          unmatched braces, trailing punctuation, empty/comment-only documents, and partial
///          final nodes are rejected. On failure the partially built root is released and zeroed.
/// @param data Source bytes, not required to carry a trailing NUL.
/// @param size Exact input byte count.
/// @param root Zeroed destination virtual root.
/// @param context Active load context charged for every retained parse allocation.
/// @return Nonzero when at least one complete top-level node was parsed.
static int fbx_parse_ascii_document(const uint8_t *data,
                                    size_t size,
                                    fbx_node_t *root,
                                    fbx_load_context_t *context) {
    fbx_ascii_parser_t parser;
    if (!data || size == 0 || !root || !context || memchr(data, '\0', size) != NULL)
        return 0;
    memset(root, 0, sizeof(*root));
    if (!fbx_node_set_name(root, "_root_", 6u, context))
        return 0;
    memset(&parser, 0, sizeof(parser));
    parser.begin = (const char *)data;
    parser.cursor = (const char *)data;
    parser.end = (const char *)data + size;
    parser.context = context;
    if (!fbx_ascii_lex_next(&parser))
        goto fail;
    while (parser.token.kind != FBX_ASCII_TOKEN_EOF) {
        while (parser.token.kind == FBX_ASCII_TOKEN_EOL)
            if (!fbx_ascii_lex_next(&parser))
                goto fail;
        if (parser.token.kind == FBX_ASCII_TOKEN_EOF)
            break;
        if (parser.token.kind != FBX_ASCII_TOKEN_IDENTIFIER ||
            !fbx_node_reserve_child(root, context) ||
            !fbx_ascii_parse_node(&parser, &root->children[root->child_count], 0)) {
            goto fail;
        }
        root->child_count++;
    }
    if (parser.error || root->child_count == 0)
        goto fail;
    return 1;
fail:
    fbx_free_node(root);
    return 0;
}
