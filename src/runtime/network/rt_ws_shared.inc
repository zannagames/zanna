//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ws_shared.inc
// Purpose: Shared strict HTTP/WebSocket syntax, authority, frame-length, and
//          UTF-8 helpers included by the plain and TLS WebSocket servers.
// Key invariants:
//   - HTTP token comparison is locale-independent ASCII.
//   - Comma lists reject empty elements and invalid token bytes.
//   - Authority parsing is overflow-safe and rejects ambiguous unbracketed IPv6.
//   - RFC 6455 64-bit lengths reject the forbidden high bit.
// Ownership/Lifetime:
//   - Native strings returned by helpers are caller-owned and freed with free().
//   - All other inputs are borrowed for the duration of a call.
// Links: rt_ws_server.c, rt_wss_server.c, rt_websocket.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ws_shared.inc
 * @brief Supplies strict WebSocket server parsing helpers to both transports.
 * @details The included static routines implement locale-independent HTTP
 *          token processing, authority validation, upgrade-header checks,
 *          canonical frame-length decoding, and incremental UTF-8 state. Any
 *          returned native string is caller-owned; other inputs are borrowed.
 */

/// @brief Fold one ASCII uppercase byte without consulting the process locale.
/// @param[in] value Byte to fold.
/// @return Lowercase ASCII equivalent for `A` through `Z`; otherwise @p value unchanged.
static unsigned char ws_ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

/// @brief Compare exactly @p length bytes using ASCII case-insensitive semantics.
/// @param[in] left First byte span.
/// @param[in] right Second byte span.
/// @param[in] length Exact number of bytes to compare.
/// @return 1 when both non-null spans match under ASCII folding; otherwise 0.
static int ws_ascii_equal_n(const char *left, const char *right, size_t length) {
    if (!left || !right)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        if (ws_ascii_lower((unsigned char)left[index]) !=
            ws_ascii_lower((unsigned char)right[index]))
            return 0;
    }
    return 1;
}

/// @brief Compare two NUL-terminated strings using ASCII case-insensitive semantics.
/// @param[in] left First null-terminated string.
/// @param[in] right Second null-terminated string.
/// @return 1 when both strings are non-null, equal length, and ASCII-case equivalent;
///         otherwise 0.
static int ws_ascii_equal(const char *left, const char *right) {
    return left && right && strlen(left) == strlen(right) &&
           ws_ascii_equal_n(left, right, strlen(left));
}

/// @brief Validate one HTTP token represented by a non-NUL-terminated span.
/// @param[in] value Token bytes.
/// @param[in] length Number of bytes at @p value.
/// @return 1 for a nonempty span without controls, whitespace, DEL, or HTTP separators;
///         otherwise 0.
static int ws_token_span_is_valid(const char *value, size_t length) {
    static const char *kSeparators = "()<>@,;:\\\"/[]?={} \t";
    if (!value || length == 0)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte <= 0x20 || byte == 0x7Fu || strchr(kSeparators, (int)byte) != NULL)
            return 0;
    }
    return 1;
}

/// @brief Validate one null-terminated HTTP token.
/// @param[in] value Token candidate.
/// @return 1 for a valid nonempty token; otherwise 0.
static int ws_token_is_valid(const char *value) {
    return value ? ws_token_span_is_valid(value, strlen(value)) : 0;
}

/// @brief Validate a comma-separated token list and search it for one token.
/// @details Empty list members, trailing commas, and invalid token bytes reject
///          the entire value rather than merely producing a non-match.
/// @param[in] value Null-terminated HTTP token-list field value.
/// @param[in] token Nonempty null-terminated token to locate.
/// @param[in] case_sensitive Nonzero for byte-exact comparison; zero for ASCII folding.
/// @return 1 when the whole list is valid and contains @p token; otherwise 0.
static int ws_header_has_token_impl(const char *value, const char *token, int case_sensitive) {
    const char *p = value;
    size_t token_len = token ? strlen(token) : 0;
    int found = 0;
    int saw_any = 0;
    if (!value || !token || token_len == 0)
        return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == ',')
            return 0;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (!ws_token_span_is_valid(start, (size_t)(end - start)))
            return 0;
        if ((size_t)(end - start) == token_len &&
            (case_sensitive ? memcmp(start, token, token_len) == 0
                            : ws_ascii_equal_n(start, token, token_len)))
            found = 1;
        saw_any = 1;
        if (*p == ',') {
            p++;
            const char *next = p;
            while (*next == ' ' || *next == '\t')
                next++;
            if (!*next)
                return 0;
        }
    }
    return saw_any && found;
}

/// @brief Check a Connection field for the case-insensitive `Upgrade` token.
/// @param[in] value Null-terminated Connection field value.
/// @return 1 when @p value is a valid token list containing `Upgrade`; otherwise 0.
static int ws_header_has_upgrade_token(const char *value) {
    return ws_header_has_token_impl(value, "Upgrade", 0);
}

/// @brief Validate a Sec-WebSocket-Protocol comma-separated token list.
/// @details Each trimmed member must be nonempty, fit the bounded parser, and
///          satisfy the HTTP token grammar.
/// @param[in] value Null-terminated protocol-list field value.
/// @return 1 for a valid nonempty list; otherwise 0.
static int ws_protocol_header_is_valid(const char *value) {
    const char *p = value;
    int saw_any = 0;

    if (!value)
        return 0;

    while (*p) {
        char token[256];
        size_t len = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == ',')
            return 0;
        while (*p && *p != ',') {
            if (len + 1 >= sizeof(token))
                return 0;
            token[len++] = *p++;
        }
        while (len > 0 && (token[len - 1] == ' ' || token[len - 1] == '\t'))
            len--;
        token[len] = '\0';
        if (!ws_token_is_valid(token))
            return 0;
        saw_any = 1;
        if (*p == ',') {
            p++;
            const char *next = p;
            while (*next == ' ' || *next == '\t')
                next++;
            if (!*next)
                return 0;
        }
    }

    return saw_any;
}

/// @brief Search a valid Sec-WebSocket-Protocol list using case-sensitive token semantics.
/// @param[in] value Null-terminated offered protocol list.
/// @param[in] token Exact protocol token required by the server.
/// @return 1 when both inputs are valid and the list contains @p token; otherwise 0.
static int ws_protocol_list_contains(const char *value, const char *token) {
    if (!ws_protocol_header_is_valid(value) || !ws_token_is_valid(token))
        return 0;
    return ws_header_has_token_impl(value, token, 1);
}

/// @brief Duplicate a string after trimming leading and trailing HTTP optional whitespace.
/// @param[in] text Null-terminated source text.
/// @return Newly allocated trimmed string, or NULL for null input or allocation failure.
///         The caller must release the result with free().
static char *ws_strdup_trimmed(const char *text) {
    if (!text)
        return NULL;
    while (*text == ' ' || *text == '\t')
        text++;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Copy an HTTP header value into a fixed handshake buffer.
/// @details Trims leading and trailing SP/HTAB before copying. Returns 0 when
///          the destination is invalid, the source is NULL, or the trimmed
///          value would not fit including the trailing NUL. Callers reject the
///          handshake on 0 so security-sensitive header validation never runs
///          against a truncated value.
/// @param dst Destination buffer.
/// @param dst_cap Capacity of @p dst in bytes.
/// @param text Raw header value after the colon.
/// @return 1 when the full trimmed value was copied, 0 on invalid input or truncation.
static int ws_copy_trimmed_header_value(char *dst, size_t dst_cap, const char *text) {
    if (!dst || dst_cap == 0 || !text)
        return 0;
    while (*text == ' ' || *text == '\t')
        text++;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    if (len >= dst_cap)
        return 0;
    memcpy(dst, text, len);
    dst[len] = '\0';
    return 1;
}

/// @brief Parsed singleton and token-list state for one WebSocket upgrade.
/// @details Both plain and TLS servers use this exact state machine so their
///          duplicate-header, origin, version, and subprotocol policies cannot
///          drift. Fixed buffers deliberately bound all unauthenticated input.
typedef struct {
    char key[128];
    char host[256];
    char origin[256];
    char protocol[256];
    unsigned key_count;
    unsigned host_count;
    unsigned origin_count;
    unsigned protocol_count;
    unsigned version_count;
    unsigned content_length_count;
    int saw_upgrade;
    int saw_connection;
} ws_handshake_headers_t;

// Strict completion validation is defined before the authority/key helpers so
// the shared parser stays contiguous; these private declarations preserve C11
// prototype checking for the later definitions.
static int ws_sec_key_is_valid(const char *key);
static int ws_host_header_is_valid(const char *host_header);
static int ws_origin_matches_expected(const char *origin,
                                      const char *host_header,
                                      const char *expected_scheme,
                                      int default_port);

/// @brief Compare an exact parsed field-name span with an ASCII header name.
/// @param name Non-NUL-terminated field-name span.
/// @param name_len Length of @p name.
/// @param expected NUL-terminated canonical header name.
/// @return Nonzero only for an exact ASCII case-insensitive match.
static int ws_header_name_is(const char *name, size_t name_len, const char *expected) {
    size_t expected_len = expected ? strlen(expected) : 0;
    return name && expected && name_len == expected_len &&
           ws_ascii_equal_n(name, expected, name_len);
}

/// @brief Split and validate one mutable HTTP/1.1 header line.
/// @details Rejects obsolete folding, empty/invalid field names, and control
///          bytes in field values. Leading/trailing optional whitespace is
///          removed in place and the returned value is NUL terminated.
/// @param line Mutable NUL-terminated line without CRLF.
/// @param name_out Receives the borrowed field-name start.
/// @param name_len_out Receives the exact field-name byte length.
/// @param value_out Receives the borrowed trimmed field value.
/// @return Nonzero for syntactically valid field syntax.
static int ws_http_header_split(char *line,
                                const char **name_out,
                                size_t *name_len_out,
                                char **value_out) {
    if (!line || !*line || !name_out || !name_len_out || !value_out || line[0] == ' ' ||
        line[0] == '\t')
        return 0;

    char *colon = strchr(line, ':');
    if (!colon || colon == line || !ws_token_span_is_valid(line, (size_t)(colon - line)))
        return 0;

    char *value = colon + 1;
    for (char *cursor = value; *cursor; ++cursor) {
        unsigned char byte = (unsigned char)*cursor;
        if ((byte < 0x20u && byte != '\t') || byte == 0x7Fu)
            return 0;
    }
    while (*value == ' ' || *value == '\t')
        ++value;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    *end = '\0';

    *name_out = line;
    *name_len_out = (size_t)(colon - line);
    *value_out = value;
    return 1;
}

/// @brief Parse an unsigned decimal HTTP Content-Length without overflow.
/// @param value Trimmed NUL-terminated field value.
/// @param result_out Receives the parsed value.
/// @return Nonzero when @p value is nonempty, decimal, and fits uint64_t.
static int ws_parse_content_length(const char *value, uint64_t *result_out) {
    uint64_t result = 0;
    if (!value || !*value || !result_out)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9')
            return 0;
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10u)
            return 0;
        result = result * 10u + digit;
    }
    *result_out = result;
    return 1;
}

/// @brief Incorporate one strict HTTP header into WebSocket handshake state.
/// @details Security-sensitive singleton headers reject duplicates. Upgrade
///          and Connection may repeat as HTTP list fields but every occurrence
///          must be a valid token list. Request bodies and transfer codings are
///          rejected so post-upgrade bytes cannot be reinterpreted as frames.
/// @param state Handshake state to update.
/// @param line Mutable header line without CRLF.
/// @return Nonzero when the field is syntactically and semantically acceptable.
static int ws_handshake_parse_header(ws_handshake_headers_t *state, char *line) {
    const char *name = NULL;
    size_t name_len = 0;
    char *value = NULL;
    if (!state || !ws_http_header_split(line, &name, &name_len, &value))
        return 0;

    if (ws_header_name_is(name, name_len, "Sec-WebSocket-Key")) {
        if (++state->key_count != 1)
            return 0;
        return ws_copy_trimmed_header_value(state->key, sizeof(state->key), value);
    }
    if (ws_header_name_is(name, name_len, "Host")) {
        if (++state->host_count != 1)
            return 0;
        return ws_copy_trimmed_header_value(state->host, sizeof(state->host), value);
    }
    if (ws_header_name_is(name, name_len, "Origin")) {
        if (++state->origin_count != 1)
            return 0;
        return ws_copy_trimmed_header_value(state->origin, sizeof(state->origin), value);
    }
    if (ws_header_name_is(name, name_len, "Sec-WebSocket-Protocol")) {
        if (++state->protocol_count != 1 || !ws_protocol_header_is_valid(value))
            return 0;
        return ws_copy_trimmed_header_value(state->protocol, sizeof(state->protocol), value);
    }
    if (ws_header_name_is(name, name_len, "Upgrade")) {
        if (!ws_protocol_header_is_valid(value))
            return 0;
        if (ws_header_has_token_impl(value, "websocket", 0))
            state->saw_upgrade = 1;
        return 1;
    }
    if (ws_header_name_is(name, name_len, "Connection")) {
        if (!ws_protocol_header_is_valid(value))
            return 0;
        if (ws_header_has_upgrade_token(value))
            state->saw_connection = 1;
        return 1;
    }
    if (ws_header_name_is(name, name_len, "Sec-WebSocket-Version")) {
        return ++state->version_count == 1 && strcmp(value, "13") == 0;
    }
    if (ws_header_name_is(name, name_len, "Content-Length")) {
        uint64_t content_length = 0;
        return ++state->content_length_count == 1 &&
               ws_parse_content_length(value, &content_length) && content_length == 0;
    }
    if (ws_header_name_is(name, name_len, "Transfer-Encoding"))
        return 0;

    return 1;
}

/// @brief Validate the complete required WebSocket handshake header set.
/// @param state Fully parsed header state.
/// @param required_subprotocol Optional exact server-required protocol token.
/// @param expected_scheme Same-origin scheme (`http` or `https`).
/// @param default_port Default port associated with @p expected_scheme.
/// @return Nonzero when every RFC/security policy requirement is satisfied.
static int ws_handshake_headers_are_valid(const ws_handshake_headers_t *state,
                                          const char *required_subprotocol,
                                          const char *expected_scheme,
                                          int default_port) {
    if (!state || state->key_count != 1 || state->host_count != 1 || state->version_count != 1 ||
        !state->saw_upgrade || !state->saw_connection ||
        (state->origin_count != 0 && state->origin[0] == '\0') ||
        !ws_sec_key_is_valid(state->key) || !ws_host_header_is_valid(state->host) ||
        !ws_origin_matches_expected(state->origin, state->host, expected_scheme, default_port))
        return 0;
    return !required_subprotocol || !*required_subprotocol ||
           ws_protocol_list_contains(state->protocol, required_subprotocol);
}

/// @brief Test one ASCII hexadecimal digit.
/// @param[in] byte Byte to classify.
/// @return 1 for `0-9`, `A-F`, or `a-f`; otherwise 0.
static int ws_ascii_is_hex(unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') ||
           (byte >= 'a' && byte <= 'f');
}

/// @brief Validate a URI registered-name host without locale dependence.
/// @details Accepts RFC 3986 unreserved/sub-delimiter characters and canonical
///          percent triplets; rejects raw non-ASCII bytes and structural
///          delimiters that would make Host/Origin authority ambiguous.
/// @param[in] host Null-terminated registered-name candidate.
/// @return 1 for a nonempty syntactically valid registered name; otherwise 0.
static int ws_reg_name_is_valid(const char *host) {
    if (!host || !*host)
        return 0;
    for (size_t index = 0; host[index] != '\0'; ++index) {
        unsigned char byte = (unsigned char)host[index];
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || strchr("-._~!$&'()*+,;=", byte))
            continue;
        if (byte == '%' && host[index + 1] != '\0' && host[index + 2] != '\0' &&
            ws_ascii_is_hex((unsigned char)host[index + 1]) &&
            ws_ascii_is_hex((unsigned char)host[index + 2])) {
            index += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

/// @brief Validate a bracketed IP-literal host.
/// @details Native `inet_pton` provides exact IPv6 syntax. RFC 3986 IPvFuture
///          literals are also accepted using their `vHEXDIG+.payload` grammar.
/// @param[in] host Null-terminated literal contents with surrounding brackets removed.
/// @return 1 for valid IPv6 or IPvFuture syntax; otherwise 0.
static int ws_ip_literal_is_valid(const char *host) {
    struct in6_addr ipv6;
    if (!host || !*host)
        return 0;
    if (inet_pton(AF_INET6, host, &ipv6) == 1)
        return 1;
    if (host[0] != 'v' && host[0] != 'V')
        return 0;
    size_t index = 1;
    size_t hex_start = index;
    while (ws_ascii_is_hex((unsigned char)host[index]))
        ++index;
    if (index == hex_start || host[index++] != '.' || host[index] == '\0')
        return 0;
    for (; host[index] != '\0'; ++index) {
        unsigned char byte = (unsigned char)host[index];
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || strchr("-._~!$&'()*+,;=:", byte))
            continue;
        return 0;
    }
    return 1;
}

/// @brief Parse and validate an HTTP authority without user-info.
/// @details Trims optional whitespace, requires IPv6/IPvFuture literals to be
///          bracketed, validates registered names and decimal ports, rejects
///          ambiguous unbracketed colons, and strips brackets from the
///          returned host. Output fields are cleared before parsing.
/// @param[in] authority Null-terminated authority text.
/// @param[out] host_out Optional destination for an allocated host string.
/// @param[out] port_out Optional destination for the port, or -1 when omitted.
/// @return 1 on success, or 0 for malformed input or allocation failure. The
///         caller must free the published host.
static int ws_parse_authority(const char *authority, char **host_out, int *port_out) {
    char *trimmed = NULL;
    char *host = NULL;
    int port = -1;
    int saw_port = 0;
    int bracketed = 0;
    char *cursor;

    if (host_out)
        *host_out = NULL;
    if (port_out)
        *port_out = -1;
    if (!authority || !*authority)
        return 0;

    trimmed = ws_strdup_trimmed(authority);
    if (!trimmed || !*trimmed) {
        free(trimmed);
        return 0;
    }

    cursor = trimmed;
    if (*cursor == '[') {
        bracketed = 1;
        char *end = strchr(cursor + 1, ']');
        char *after = NULL;
        size_t host_len;
        if (!end) {
            free(trimmed);
            return 0;
        }
        host_len = (size_t)(end - (cursor + 1));
        if (host_len == 0) {
            free(trimmed);
            return 0;
        }
        host = (char *)malloc(host_len + 1);
        if (!host) {
            free(trimmed);
            return 0;
        }
        memcpy(host, cursor + 1, host_len);
        host[host_len] = '\0';
        after = end + 1;
        if (*after == ':') {
            char *port_text = after + 1;
            port = 0;
            if (!*port_text) {
                free(host);
                free(trimmed);
                return 0;
            }
            while (*port_text) {
                if (*port_text < '0' || *port_text > '9') {
                    free(host);
                    free(trimmed);
                    return 0;
                }
                int digit = *port_text - '0';
                if (port > (65535 - digit) / 10) {
                    free(host);
                    free(trimmed);
                    return 0;
                }
                saw_port = 1;
                port = port * 10 + digit;
                port_text++;
            }
            if (!saw_port) {
                free(host);
                free(trimmed);
                return 0;
            }
        } else if (*after != '\0') {
            free(host);
            free(trimmed);
            return 0;
        }
    } else {
        char *colon = strchr(cursor, ':');
        if (colon) {
            if (colon != strrchr(cursor, ':')) {
                free(trimmed);
                return 0;
            }
            char *port_text = colon + 1;
            port = 0;
            if (!*port_text) {
                free(trimmed);
                return 0;
            }
            while (*port_text) {
                if (*port_text < '0' || *port_text > '9') {
                    free(trimmed);
                    return 0;
                }
                int digit = *port_text - '0';
                if (port > (65535 - digit) / 10) {
                    free(trimmed);
                    return 0;
                }
                saw_port = 1;
                port = port * 10 + digit;
                port_text++;
            }
            *colon = '\0';
        }
        if (*cursor == '\0') {
            free(trimmed);
            return 0;
        }
        host = strdup(cursor);
        if (!host) {
            free(trimmed);
            return 0;
        }
    }

    free(trimmed);
    if (!host || !*host) {
        free(host);
        return 0;
    }
    if ((bracketed && !ws_ip_literal_is_valid(host)) ||
        (!bracketed && !ws_reg_name_is_valid(host))) {
        free(host);
        return 0;
    }
    for (const unsigned char *byte = (const unsigned char *)host; *byte; ++byte) {
        if (*byte <= 0x20 || *byte == 0x7Fu || *byte == '/' || *byte == '?' || *byte == '#' ||
            *byte == '@') {
            free(host);
            return 0;
        }
    }

    if (host_out)
        *host_out = host;
    else
        free(host);
    if (port_out)
        *port_out = port;
    return 1;
}

/// @brief Validate a Host field as one complete authority.
/// @param[in] host_header Null-terminated Host field value.
/// @return 1 for a valid host with an omitted port or a port in 1..65535;
///         otherwise 0.
static int ws_host_header_is_valid(const char *host_header) {
    char *trimmed = NULL;
    char *host = NULL;
    int port = -1;
    int ok = 0;

    trimmed = ws_strdup_trimmed(host_header);
    if (!trimmed || !*trimmed) {
        free(trimmed);
        return 0;
    }

    if (strchr(trimmed, ':') != strrchr(trimmed, ':') && trimmed[0] != '[') {
        free(trimmed);
        return 0;
    }

    if (!ws_parse_authority(trimmed, &host, &port)) {
        free(trimmed);
        return 0;
    }

    ok = host && *host && (port < 0 || (port >= 1 && port <= 65535));
    free(trimmed);
    free(host);
    return ok;
}

/// @brief Validate the strict WebSocket HTTP/1.1 request line.
/// @details Requires `GET`, one nonempty origin-form target beginning with `/`,
///          exact `HTTP/1.1`, and no controls, DEL, or fragment marker in the target.
/// @param[in] request_line Null-terminated line without CRLF.
/// @return 1 when the line satisfies the server handshake grammar; otherwise 0.
static int ws_request_line_is_valid(const char *request_line) {
    const char *target = NULL;
    const char *version = NULL;

    if (!request_line || strncmp(request_line, "GET ", 4) != 0)
        return 0;

    target = request_line + 4;
    version = strchr(target, ' ');
    if (!version || version == target || strcmp(version, " HTTP/1.1") != 0)
        return 0;

    if (*target != '/')
        return 0;

    for (const char *p = target; p < version; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7F || ch == '#')
            return 0;
    }

    return 1;
}

/// @brief Validate the canonical Base64 shape of a 16-byte Sec-WebSocket-Key.
/// @details Requires exactly 24 characters, two trailing padding bytes, and
///          zero unused low bits in the final data symbol.
/// @param[in] key Null-terminated key field value.
/// @return 1 for the canonical 16-byte nonce encoding; otherwise 0.
static int ws_sec_key_is_valid(const char *key) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = 0;

    if (!key)
        return 0;

    len = strlen(key);
    if (len != 24)
        return 0;

    for (size_t i = 0; i < 22; ++i) {
        if (!strchr(base64_chars, key[i]))
            return 0;
    }

    // Sixteen decoded bytes leave only the high two bits meaningful in the
    // twenty-second base64 symbol. Requiring the low four bits to be zero
    // rejects non-canonical encodings that merely have the right shape.
    const char *last_symbol = strchr(base64_chars, key[21]);
    return last_symbol && (((size_t)(last_symbol - base64_chars)) & 0x0Fu) == 0 && key[22] == '=' &&
           key[23] == '=';
}

/// @brief Enforce same-origin scheme, host, and effective port for an upgrade.
/// @details An absent Origin is accepted. A present Origin must contain only
///          `scheme://authority`, use the expected scheme, and match the Host
///          field after ASCII host folding and default-port substitution.
/// @param[in] origin Trimmed Origin field, or an empty string when absent.
/// @param[in] host_header Validated request Host field.
/// @param[in] expected_scheme Expected `http` or `https` scheme.
/// @param[in] default_port Port substituted when either authority omits one.
/// @return 1 when Origin is absent or matches exactly; otherwise 0.
static int ws_origin_matches_expected(const char *origin,
                                      const char *host_header,
                                      const char *expected_scheme,
                                      int default_port) {
    const char *scheme_sep = NULL;
    const char *authority = NULL;
    const char *authority_end = NULL;
    size_t scheme_len = 0;
    size_t authority_len = 0;
    char *origin_authority = NULL;
    char *origin_host = NULL;
    char *request_host = NULL;
    int origin_port = -1;
    int request_port = -1;
    int matches = 0;

    if (!origin || !*origin)
        return 1;
    if (!host_header || !*host_header || !expected_scheme || default_port <= 0)
        return 0;

    scheme_sep = strstr(origin, "://");
    if (!scheme_sep)
        return 0;
    scheme_len = (size_t)(scheme_sep - origin);
    if (scheme_len != strlen(expected_scheme) ||
        !ws_ascii_equal_n(origin, expected_scheme, scheme_len)) {
        return 0;
    }

    authority = scheme_sep + 3;
    authority_end = authority;
    while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
           *authority_end != '#')
        authority_end++;
    authority_len = (size_t)(authority_end - authority);
    if (*authority_end != '\0')
        return 0;
    origin_authority = (char *)malloc(authority_len + 1);
    if (!origin_authority)
        return 0;
    memcpy(origin_authority, authority, authority_len);
    origin_authority[authority_len] = '\0';

    if (!ws_parse_authority(origin_authority, &origin_host, &origin_port) ||
        !ws_parse_authority(host_header, &request_host, &request_port)) {
        free(request_host);
        free(origin_host);
        free(origin_authority);
        return 0;
    }

    if (origin_port < 0)
        origin_port = default_port;
    if (request_port < 0)
        request_port = default_port;

    if (ws_ascii_equal(origin_host, request_host) && origin_port == request_port)
        matches = 1;

    free(request_host);
    free(origin_host);
    free(origin_authority);
    return matches;
}

/// @brief Validate an RFC 6455 frame opcode.
/// @param[in] opcode Four-bit opcode value.
/// @return 1 for continuation, text, binary, close, ping, or pong; otherwise 0.
static int ws_is_valid_opcode(uint8_t opcode) {
    switch (opcode) {
        case WS_OP_CONTINUATION:
        case WS_OP_TEXT:
        case WS_OP_BINARY:
        case WS_OP_CLOSE:
        case WS_OP_PING:
        case WS_OP_PONG:
            return 1;
        default:
            return 0;
    }
}

/// @brief Encode a payload length as an eight-byte network-order field.
/// @param[out] out Eight-byte destination.
/// @param[in] len Host payload length.
static void ws_encode_u64_len(uint8_t out[8], size_t len) {
    uint64_t value = (uint64_t)len;
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

/// @brief Decode an RFC 6455 63-bit network-order payload length.
/// @param[in] in Eight-byte encoded field.
/// @param[out] len_out Receives the host-representable length.
/// @return 1 on success, or 0 when arguments are null, the forbidden high bit
///         is set, or the value exceeds SIZE_MAX.
static int ws_decode_u64_len(const uint8_t in[8], size_t *len_out) {
    if (!in || !len_out || (in[0] & 0x80u) != 0)
        return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 8) | in[i];
    if (value > (uint64_t)SIZE_MAX)
        return 0;
    *len_out = (size_t)value;
    return 1;
}

/// @brief Validate an RFC 6455 close status received on the wire.
/// @param[in] code Unsigned close status.
/// @return 1 for defined protocol codes below 1016 or application/private codes
///         3000..4999, excluding wire-reserved statuses; otherwise 0.
static int ws_close_code_is_valid(uint16_t code) {
    if (code < 1000 || code >= 5000)
        return 0;
    if (code == 1004 || code == 1005 || code == 1006 || code == 1015)
        return 0;
    return code < 1016 || code >= 3000;
}

/// @brief Incremental RFC 3629 UTF-8 validation state for fragmented messages.
/// @details A WebSocket code point may span frame boundaries, so validating
///          each continuation independently is incorrect. This compact state
///          retains only the partial scalar and never needs to concatenate the
///          message solely for validation.
typedef struct {
    uint32_t codepoint;
    uint32_t minimum;
    uint8_t remaining;
} ws_utf8_state_t;

/// @brief Reset an incremental UTF-8 validator to the code-point boundary state.
/// @param[out] state Validator to clear; NULL is a no-op.
static void ws_utf8_state_init(ws_utf8_state_t *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

/// @brief Feed one exact byte span into an incremental UTF-8 validator.
/// @param state Validator retained across all fragments of one text message.
/// @param data Byte span; NULL is accepted only for zero length.
/// @param len Number of bytes in @p data.
/// @return Nonzero while the prefix is potentially valid UTF-8.
static int ws_utf8_state_feed(ws_utf8_state_t *state, const uint8_t *data, size_t len) {
    if (!state || (!data && len > 0))
        return 0;
    for (size_t index = 0; index < len; ++index) {
        uint8_t byte = data[index];
        if (state->remaining == 0) {
            if (byte <= 0x7Fu)
                continue;
            if (byte >= 0xC2u && byte <= 0xDFu) {
                state->codepoint = (uint32_t)(byte & 0x1Fu);
                state->minimum = 0x80u;
                state->remaining = 1;
                continue;
            }
            if (byte >= 0xE0u && byte <= 0xEFu) {
                state->codepoint = (uint32_t)(byte & 0x0Fu);
                state->minimum = 0x800u;
                state->remaining = 2;
                continue;
            }
            if (byte >= 0xF0u && byte <= 0xF4u) {
                state->codepoint = (uint32_t)(byte & 0x07u);
                state->minimum = 0x10000u;
                state->remaining = 3;
                continue;
            }
            return 0;
        }

        if ((byte & 0xC0u) != 0x80u)
            return 0;
        state->codepoint = (state->codepoint << 6) | (uint32_t)(byte & 0x3Fu);
        --state->remaining;
        if (state->remaining == 0) {
            if (state->codepoint < state->minimum || state->codepoint > 0x10FFFFu ||
                (state->codepoint >= 0xD800u && state->codepoint <= 0xDFFFu))
                return 0;
            state->codepoint = 0;
            state->minimum = 0;
        }
    }
    return 1;
}

/// @brief Test whether an incremental UTF-8 prefix ends at a code-point boundary.
/// @param[in] state Validator after feeding the complete message prefix.
/// @return 1 when @p state is non-null and no continuation bytes remain; otherwise 0.
static int ws_utf8_state_is_complete(const ws_utf8_state_t *state) {
    return state && state->remaining == 0;
}

/// @brief Validate one complete byte sequence as RFC 3629 UTF-8.
/// @param[in] data Input bytes; NULL is permitted only when @p len is zero.
/// @param[in] len Number of bytes to validate.
/// @return 1 when the complete sequence is valid and ends at a code-point
///         boundary; otherwise 0.
static int ws_is_valid_utf8(const uint8_t *data, size_t len) {
    ws_utf8_state_t state;
    ws_utf8_state_init(&state);
    return ws_utf8_state_feed(&state, data, len) && ws_utf8_state_is_complete(&state);
}
