//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_network_http_response.inc
/// @brief HTTP/1 response-line, header, and buffered-body parsing helpers.
/// @details This private translation-unit fragment shares the validated
///          connection primitives and protocol limits declared earlier in
///          rt_network_http.c. Keeping response parsing cohesive avoids a
///          mega-file without adding a separately linkable runtime surface.
///          Every returned native buffer transfers to its documented caller.
//
//===----------------------------------------------------------------------===//

/// @brief Read one bounded HTTP line from a connection.
/// @details Reads through LF, removes a preceding CR, and caps native storage at
///          64 KiB. EOF after some bytes returns the unterminated partial line;
///          EOF before any byte and allocation/transport failure return NULL.
/// @param conn Open HTTP connection.
/// @return Newly allocated NUL-terminated line without CRLF, or NULL.
static char *read_line_conn(http_conn_t *conn) {
    char *line = NULL;
    size_t len = 0;
    size_t cap = 256;
    line = (char *)calloc(cap, 1u);
    if (!line)
        return NULL;

    while (1) {
        uint8_t byte = 0;
        if (http_conn_recv_byte(conn, &byte) == 0) {
            // Connection closed
            if (len == 0) {
                free(line);
                return NULL;
            }
            break;
        }

        char c = (char)byte;

        if (c == '\n') {
            // Remove trailing CR if present
            if (len > 0 && line[len - 1] == '\r')
                len--;
            break;
        }

        if (len + 1 >= cap) {
            // Cap at 64 KB to prevent unbounded realloc from malicious servers
            if (cap >= 65536) {
                free(line);
                return NULL;
            }
            cap *= 2;
            if (cap > 65536)
                cap = 65536;
            char *new_line = (char *)realloc(line, cap);
            if (!new_line) {
                free(line);
                return NULL;
            }
            line = new_line;
        }
        line[len++] = c;
    }

    line[len] = '\0';
    return line;
}

/// @brief Parse a strict HTTP/1.0 or HTTP/1.1 response status line.
/// @details Requires an exact three-digit status in [100, 599] and optionally
///          duplicates the remaining reason phrase.
/// @param line NUL-terminated status line without CRLF.
/// @param http_minor_out Optional output receiving zero or one.
/// @param status_text_out Optional output receiving a newly allocated reason
///        phrase.
/// @return Parsed status code, or -1 on syntax or allocation failure.
static int parse_status_line(const char *line, int *http_minor_out, char **status_text_out) {
    if (http_minor_out)
        *http_minor_out = -1;
    if (status_text_out)
        *status_text_out = NULL;
    if (!line || strlen(line) < 12u)
        return -1;

    // Format: HTTP/1.x STATUS_CODE STATUS_TEXT
    if (strncmp(line, "HTTP/1.", 7) != 0)
        return -1;

    const char *p = line + 7;
    // Skip version digit
    if (*p != '0' && *p != '1')
        return -1;
    if (http_minor_out)
        *http_minor_out = *p - '0';
    p++;

    // Skip space
    if (*p != ' ')
        return -1;
    p++;

    // Parse exactly three status-code digits followed by SP or end-of-line.
    if (strlen(p) < 3)
        return -1;
    if (!(p[0] >= '0' && p[0] <= '9') || !(p[1] >= '0' && p[1] <= '9') ||
        !(p[2] >= '0' && p[2] <= '9'))
        return -1;
    int status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    p += 3;
    if (*p != '\0' && *p != ' ')
        return -1;

    if (status < 100 || status > 599)
        return -1;

    // Skip space and get status text
    if (*p == ' ')
        p++;

    if (status_text_out) {
        *status_text_out = strdup(p);
        if (!*status_text_out)
            return -1;
    }

    return status;
}

/// @brief Return whether an HTTP response header must not be combined or duplicated.
/// @details These fields influence framing, redirects, or content interpretation. Rejecting
///          duplicate instances avoids ambiguous response handling and request-smuggling style
///          inconsistencies between parsers.
/// @param lower_name Lowercase response header field name.
/// @return 1 if duplicates should fail the response parse; otherwise 0.
static int http_response_header_is_singleton(const char *lower_name) {
    return lower_name &&
           (strcmp(lower_name, "content-length") == 0 ||
            strcmp(lower_name, "transfer-encoding") == 0 || strcmp(lower_name, "location") == 0 ||
            strcmp(lower_name, "connection") == 0 || strcmp(lower_name, "content-encoding") == 0);
}

/// @brief Release every temporary used to build one response-header entry.
/// @details The box and value String hold independent references, so both are
///          balanced. Native lowercase/join buffers are always caller-owned.
/// @param lower_name Native lowercase field-name copy.
/// @param joined Native combined-value buffer.
/// @param name_string Managed lookup-key String.
/// @param value_string Managed candidate value String.
/// @param existing_string Retained prior value used during duplicate folding.
/// @param boxed Managed box retaining @p value_string.
static void http_response_header_build_cleanup(char *lower_name,
                                               char *joined,
                                               rt_string name_string,
                                               rt_string value_string,
                                               rt_string existing_string,
                                               void *boxed) {
    if (boxed && rt_obj_release_check0(boxed))
        rt_obj_free(boxed);
    if (existing_string)
        rt_string_unref(existing_string);
    if (value_string)
        rt_string_unref(value_string);
    if (name_string)
        rt_string_unref(name_string);
    free(joined);
    free(lower_name);
}

/// @brief Append or fold one response header into its canonical lowercase Map.
/// @details All native and managed staging values are covered by a recovery
///          frame. A managed allocation trap is re-raised only after cleanup;
///          native allocation failure and malformed singleton duplication are
///          reported as a zero parse result without mutating the Map.
/// @param headers_map Stable response Map receiving boxed String values.
/// @param name Valid HTTP field-name bytes.
/// @param value NUL-terminated field value.
/// @return One on success; zero for malformed input or native OOM.
static int append_response_header_value(void *headers_map, const char *name, const char *value) {
    char *volatile lower_name = NULL;
    char *volatile joined = NULL;
    rt_string volatile name_string = NULL;
    rt_string volatile value_string = NULL;
    rt_string volatile existing_string = NULL;
    void *volatile boxed = NULL;

    if (!headers_map || !name || !value)
        return 0;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response header allocation failed");
        rt_trap_clear_recovery();
        http_response_header_build_cleanup((char *)lower_name,
                                           (char *)joined,
                                           (rt_string)name_string,
                                           (rt_string)value_string,
                                           (rt_string)existing_string,
                                           (void *)boxed);
        rt_trap(saved_error);
        return 0;
    }

    lower_name = strdup(name);
    if (!lower_name) {
        rt_trap_clear_recovery();
        return 0;
    }
    for (char *p = (char *)lower_name; *p; p++) {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + ('a' - 'A'));
    }

    name_string = rt_string_from_bytes((const char *)lower_name, strlen((const char *)lower_name));
    value_string = rt_string_from_bytes(value, strlen(value));
    if (!name_string || !value_string) {
        rt_trap_clear_recovery();
        http_response_header_build_cleanup(
            (char *)lower_name, NULL, (rt_string)name_string, (rt_string)value_string, NULL, NULL);
        return 0;
    }

    {
        void *existing = rt_map_get(headers_map, (rt_string)name_string);
        if (existing && http_response_header_is_singleton((const char *)lower_name)) {
            rt_trap_clear_recovery();
            http_response_header_build_cleanup((char *)lower_name,
                                               NULL,
                                               (rt_string)name_string,
                                               (rt_string)value_string,
                                               NULL,
                                               NULL);
            return 0;
        }
        if (existing && rt_box_type(existing) == RT_BOX_STR) {
            existing_string = rt_unbox_str(existing);
            if (!existing_string)
                rt_trap("HTTP: invalid stored response header");
            const char *existing_cstr = rt_string_cstr((rt_string)existing_string);
            const char *value_cstr = rt_string_cstr((rt_string)value_string);
            const char *sep = strcmp((const char *)lower_name, "set-cookie") == 0
                                  ? HTTP_SET_COOKIE_JOIN_SEPARATOR
                                  : ", ";
            int64_t existing_len64 = rt_str_len((rt_string)existing_string);
            int64_t value_len64 = rt_str_len((rt_string)value_string);
            if (!existing_cstr || !value_cstr || existing_len64 < 0 || value_len64 < 0 ||
                (uint64_t)existing_len64 > (uint64_t)SIZE_MAX ||
                (uint64_t)value_len64 > (uint64_t)SIZE_MAX) {
                rt_trap("HTTP: invalid stored response header");
            }
            size_t existing_len = (size_t)existing_len64;
            size_t value_len = (size_t)value_len64;
            size_t sep_len = strlen(sep);
            if (existing_len > SIZE_MAX - sep_len ||
                existing_len + sep_len > SIZE_MAX - value_len ||
                existing_len + sep_len + value_len == SIZE_MAX) {
                rt_trap_clear_recovery();
                http_response_header_build_cleanup((char *)lower_name,
                                                   NULL,
                                                   (rt_string)name_string,
                                                   (rt_string)value_string,
                                                   (rt_string)existing_string,
                                                   NULL);
                return 0;
            }
            size_t joined_len = existing_len + sep_len + value_len;
            joined = (char *)malloc(joined_len + 1);
            if (!joined) {
                rt_trap_clear_recovery();
                http_response_header_build_cleanup((char *)lower_name,
                                                   NULL,
                                                   (rt_string)name_string,
                                                   (rt_string)value_string,
                                                   (rt_string)existing_string,
                                                   NULL);
                return 0;
            }
            memcpy((char *)joined, existing_cstr, existing_len);
            memcpy((char *)joined + existing_len, sep, sep_len);
            memcpy((char *)joined + existing_len + sep_len, value_cstr, value_len);
            ((char *)joined)[joined_len] = '\0';
            rt_string merged = rt_string_from_bytes((const char *)joined, joined_len);
            free((void *)joined);
            joined = NULL;
            if (!merged) {
                rt_trap_clear_recovery();
                http_response_header_build_cleanup((char *)lower_name,
                                                   NULL,
                                                   (rt_string)name_string,
                                                   (rt_string)value_string,
                                                   (rt_string)existing_string,
                                                   NULL);
                return 0;
            }
            rt_string_unref((rt_string)value_string);
            value_string = merged;
            rt_string_unref((rt_string)existing_string);
            existing_string = NULL;
        }
    }

    boxed = rt_box_str((rt_string)value_string);
    if (!boxed) {
        rt_trap_clear_recovery();
        http_response_header_build_cleanup(
            (char *)lower_name, NULL, (rt_string)name_string, (rt_string)value_string, NULL, NULL);
        return 0;
    }
    rt_map_set(headers_map, (rt_string)name_string, (void *)boxed);
    rt_trap_clear_recovery();
    http_response_header_build_cleanup((char *)lower_name,
                                       NULL,
                                       (rt_string)name_string,
                                       (rt_string)value_string,
                                       NULL,
                                       (void *)boxed);
    return 1;
}

/// @brief Parse a response header line into the header map.
/// @details Splits on the first colon, validates the field name as an HTTP token, trims leading
///          optional whitespace from the value, and delegates duplicate/combine rules to
///          append_response_header_value.
/// @param line NUL-terminated header line without the trailing CRLF.
/// @param headers_map Runtime map receiving lowercase header names and boxed string values.
/// @return 1 if the header was accepted; 0 if malformed or allocation failed.
static int parse_header_line(const char *line, void *headers_map) {
    const char *colon = strchr(line, ':');
    char *volatile name = NULL;
    if (!colon)
        return 0;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response header parse failed");
        rt_trap_clear_recovery();
        free((void *)name);
        rt_trap(saved_error);
        return 0;
    }

    name = (char *)malloc((size_t)(colon - line) + 1);
    if (!name) {
        rt_trap_clear_recovery();
        return 0;
    }
    memcpy((char *)name, line, (size_t)(colon - line));
    ((char *)name)[colon - line] = '\0';
    if (!http_header_field_name_is_token((const char *)name)) {
        rt_trap_clear_recovery();
        free((void *)name);
        return 0;
    }

    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;

    int ok = append_response_header_value(headers_map, (const char *)name, value);
    rt_trap_clear_recovery();
    free((void *)name);
    return ok;
}

/// @brief Release a partially staged HTTP/1 response head.
/// @param status_line Native status-line buffer.
/// @param line Native current header-line buffer.
/// @param status_text Native reason-phrase copy.
/// @param headers_map Managed response-header Map.
/// @param redirect_location Native Location copy.
static void http_response_head_cleanup(
    char *status_line, char *line, char *status_text, void *headers_map, char *redirect_location) {
    free(line);
    free(status_line);
    free(status_text);
    free(redirect_location);
    if (headers_map && rt_obj_release_check0(headers_map))
        rt_obj_free(headers_map);
}

/// @brief Read the status line and headers from an HTTP response.
/// @details Native line/reason/redirect buffers and the partially populated
///          managed Map are all registered with one recovery frame. Managed
///          allocation traps are re-raised after cleanup; malformed or short
///          wire input returns zero without transferring partial ownership.
///          Up to eight informational responses are skipped before publishing
///          the final head; aggregate bytes and field count are bounded.
/// @param conn Open connection positioned at a response status line.
/// @param status_out Optional output for the final status code.
/// @param http_minor_out Optional output for the HTTP/1 minor version.
/// @param status_text_out Optional output receiving the owned reason phrase.
/// @param headers_map_out Optional output receiving the owned lowercase Map.
/// @param redirect_location_out Optional output receiving an owned first
///        Location value.
/// @return One on success; zero on malformed/short response or native OOM.
static int read_response_head(http_conn_t *conn,
                              int *status_out,
                              int *http_minor_out,
                              char **status_text_out,
                              void **headers_map_out,
                              char **redirect_location_out) {
    if (status_out)
        *status_out = -1;
    if (http_minor_out)
        *http_minor_out = 1;
    if (status_text_out)
        *status_text_out = NULL;
    if (headers_map_out)
        *headers_map_out = NULL;
    if (redirect_location_out)
        *redirect_location_out = NULL;

    char *volatile status_line = NULL;
    char *volatile line = NULL;
    char *volatile status_text = NULL;
    void *volatile headers_map = NULL;
    char *volatile redirect_location = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "HTTP: response-head allocation failed");
        rt_trap_clear_recovery();
        http_response_head_cleanup((char *)status_line,
                                   (char *)line,
                                   (char *)status_text,
                                   (void *)headers_map,
                                   (char *)redirect_location);
        rt_trap(saved_error);
        return 0;
    }

    for (int informational_count = 0; informational_count < 8; informational_count++) {
        int status = -1;
        int header_count = 0;
        size_t header_bytes = 0;

        status_line = NULL;
        line = NULL;
        status_text = NULL;
        headers_map = NULL;
        redirect_location = NULL;

        if (!status_line)
            status_line = read_line_conn(conn);
        if (!status_line)
            goto fail;
        if (strlen((const char *)status_line) + 2u > HTTP_MAX_HEADER_BYTES)
            goto fail;
        header_bytes = strlen((const char *)status_line) + 2u;

        char *parsed_status_text = NULL;
        status = parse_status_line((const char *)status_line, http_minor_out, &parsed_status_text);
        status_text = parsed_status_text;
        free((void *)status_line);
        status_line = NULL;
        if (status < 0)
            goto fail;

        headers_map = rt_map_new();
        if (!headers_map)
            goto fail;

        while (1) {
            line = read_line_conn(conn);
            if (!line)
                goto fail;
            if (((const char *)line)[0] == '\0') {
                free((void *)line);
                line = NULL;
                break;
            }

            size_t line_len = strlen((const char *)line);
            if (line_len + 2u > HTTP_MAX_HEADER_BYTES - header_bytes) {
                goto fail;
            }
            header_bytes += line_len + 2u;

            header_count++;
            if (header_count > 256)
                goto fail;

            if (strncasecmp((const char *)line, "location:", 9) == 0 && !redirect_location) {
                const char *loc = (const char *)line + 9;
                while (*loc == ' ' || *loc == '\t')
                    loc++;
                redirect_location = strdup(loc);
                if (!redirect_location)
                    goto fail;
            }

            if (!parse_header_line((const char *)line, (void *)headers_map))
                goto fail;
            free((void *)line);
            line = NULL;
        }

        if (status >= 100 && status < 200 && status != 101) {
            http_response_head_cleanup(
                NULL, NULL, (char *)status_text, (void *)headers_map, (char *)redirect_location);
            status_text = NULL;
            headers_map = NULL;
            redirect_location = NULL;
            continue;
        }

        if (status_out)
            *status_out = status;
        if (status_text_out) {
            *status_text_out = (char *)status_text;
            status_text = NULL;
        }
        if (headers_map_out) {
            *headers_map_out = (void *)headers_map;
            headers_map = NULL;
        }
        if (redirect_location_out) {
            *redirect_location_out = (char *)redirect_location;
            redirect_location = NULL;
        }
        rt_trap_clear_recovery();
        http_response_head_cleanup(
            NULL, NULL, (char *)status_text, (void *)headers_map, (char *)redirect_location);
        return 1;

    fail:
        rt_trap_clear_recovery();
        http_response_head_cleanup((char *)status_line,
                                   (char *)line,
                                   (char *)status_text,
                                   (void *)headers_map,
                                   (char *)redirect_location);
        return 0;
    }

    rt_trap_clear_recovery();
    return 0;
}

/// @brief Read an exact Content-Length response body.
/// @param conn Open connection positioned at the body.
/// @param content_length Promised body byte count, bounded by the global limit.
/// @param out_len Receives the exact count, or zero on failure.
/// @return Newly allocated owned body buffer, or NULL for zero length,
///         oversized input, allocation failure, or premature EOF.
static uint8_t *read_body_content_length_conn(http_conn_t *conn,
                                              size_t content_length,
                                              size_t *out_len) {
    /* Cap to protect against server-controlled malloc(HUGE) (S-09 fix) */
    if (content_length > HTTP_MAX_BODY_SIZE) {
        *out_len = 0;
        return NULL;
    }
    if (content_length == 0) {
        *out_len = 0;
        return NULL;
    }

    uint8_t *body = (uint8_t *)malloc(content_length);
    if (!body)
        return NULL;

    size_t total_read = 0;
    while (total_read < content_length) {
        size_t remaining = content_length - total_read;
        size_t chunk_size = remaining < HTTP_BUFFER_SIZE ? remaining : HTTP_BUFFER_SIZE;

        long len = http_conn_recv(conn, body + total_read, chunk_size);
        if (len <= 0) {
            free(body);
            *out_len = 0;
            return NULL;
        }

        total_read += len;
    }

    *out_len = total_read;
    return body;
}

/// @brief Parse one HTTP chunk-size line.
/// @details Accepts one or more hexadecimal digits, optional whitespace, and
///          optional chunk extensions beginning with `;`, while rejecting
///          overflow and other trailing syntax.
/// @param size_line NUL-terminated line without CRLF.
/// @param chunk_size_out Receives the parsed size.
/// @return One on success, otherwise zero.
static int parse_http_chunk_size_line(const char *size_line, size_t *chunk_size_out) {
    size_t chunk_size = 0;
    int saw_digit = 0;
    const char *p = size_line;
    if (!size_line || !chunk_size_out)
        return 0;
    while (*p) {
        char c = *p;
        unsigned digit = 0;
        if (c >= '0' && c <= '9')
            digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A' + 10);
        else
            break;
        saw_digit = 1;
        if (chunk_size > (SIZE_MAX - digit) / 16)
            return 0;
        chunk_size = chunk_size * 16 + digit;
        p++;
    }
    if (!saw_digit)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '\0' && *p != ';')
        return 0;
    *chunk_size_out = chunk_size;
    return 1;
}

/// @brief Drain chunked-transfer trailer fields after the terminating zero-size chunk.
/// @details Reads trailer lines until the empty line while enforcing aggregate byte and field
///          count limits. Trailer contents are not exposed to callers, so malformed or oversized
///          trailers fail the body read.
/// @param conn Active HTTP connection to read from.
/// @return 1 when the trailer block is well-formed and fully drained; otherwise 0.
static int drain_chunk_trailers_conn(http_conn_t *conn) {
    size_t trailer_bytes = 0;
    unsigned trailer_count = 0;
    while (1) {
        char *trailer = read_line_conn(conn);
        if (!trailer)
            return 0;
        if (trailer[0] == '\0') {
            free(trailer);
            return 1;
        }
        size_t trailer_len = strlen(trailer);
        trailer_count++;
        if (trailer_count > HTTP_MAX_TRAILER_LINES ||
            trailer_len + 2u > HTTP_MAX_TRAILER_BYTES - trailer_bytes) {
            free(trailer);
            return 0;
        }
        trailer_bytes += trailer_len + 2u;
        free(trailer);
    }
}

/// @brief Decode a complete chunked-transfer response body.
/// @details Enforces aggregate body limits, exact chunk payload sizes, empty
///          chunk terminator lines, and bounded trailers. Empty bodies still
///          return a non-NULL owned sentinel buffer so success is distinguishable
///          from failure.
/// @param conn Open connection positioned at the first chunk-size line.
/// @param out_len Receives decoded payload length, or zero on failure.
/// @return Newly allocated decoded body buffer, or NULL on malformed input,
///         size violation, allocation failure, or premature EOF.
static uint8_t *read_body_chunked_conn(http_conn_t *conn, size_t *out_len) {
    size_t body_cap = HTTP_BUFFER_SIZE;
    size_t body_len = 0;
    uint8_t *body = (uint8_t *)malloc(body_cap);
    if (!body)
        return NULL;

    while (1) {
        // Read chunk size line
        char *size_line = read_line_conn(conn);
        if (!size_line) {
            free(body);
            *out_len = 0;
            return NULL;
        }

        // Parse hex chunk size — guard against overflow before each multiply (M-6)
        size_t chunk_size = 0;
        int parsed = parse_http_chunk_size_line(size_line, &chunk_size);
        free(size_line);

        if (!parsed) {
            free(body);
            *out_len = 0;
            return NULL;
        }

        if (chunk_size == 0) {
            // Last chunk — drain all trailer headers until empty line (RC-13 / RFC 7230 §4.1.2)
            if (!drain_chunk_trailers_conn(conn)) {
                free(body);
                *out_len = 0;
                return NULL;
            }
            break;
        }

        /* Reject chunks that would push the total body past the limit (S-09 fix) */
        if (body_len > HTTP_MAX_BODY_SIZE || chunk_size > HTTP_MAX_BODY_SIZE - body_len) {
            free(body);
            *out_len = 0;
            return NULL;
        }

        // Expand body buffer if needed
        while (body_len + chunk_size > body_cap) {
            if (body_cap > HTTP_MAX_BODY_SIZE / 2)
                body_cap = HTTP_MAX_BODY_SIZE;
            else
                body_cap *= 2;
            if (body_cap < body_len + chunk_size) {
                free(body);
                *out_len = 0;
                return NULL;
            }
            uint8_t *new_body = (uint8_t *)realloc(body, body_cap);
            if (!new_body) {
                free(body);
                return NULL;
            }
            body = new_body;
        }

        // Read chunk data
        size_t bytes_read = 0;
        while (bytes_read < chunk_size) {
            size_t remaining = chunk_size - bytes_read;
            size_t to_read = remaining < HTTP_BUFFER_SIZE ? remaining : HTTP_BUFFER_SIZE;

            long len = http_conn_recv(conn, body + body_len, to_read);
            if (len <= 0) {
                free(body);
                *out_len = 0;
                return NULL;
            }

            body_len += len;
            bytes_read += len;
        }

        // Read trailing CRLF after chunk
        char *chunk_end = read_line_conn(conn);
        if (!chunk_end || chunk_end[0] != '\0') {
            free(chunk_end);
            free(body);
            *out_len = 0;
            return NULL;
        }
        free(chunk_end);
    }

    if (body_len == 0) {
        free(body);
        *out_len = 0;
        body = (uint8_t *)malloc(1);
        if (body)
            body[0] = 0;
        return body;
    }

    *out_len = body_len;
    return body;
}
