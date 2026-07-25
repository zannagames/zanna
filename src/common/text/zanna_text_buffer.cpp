//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/text/zanna_text_buffer.cpp
// Purpose: Implement the C ABI facade for Zanna's shared piece-table buffer.
// Key invariants: No C++ exception crosses the exported C ABI; all returned
//                 strings are NUL-terminated copies with explicit byte lengths.
// Ownership/Lifetime: The opaque handle owns a zanna::tui::text::TextBuffer.
//                     C callers release handles and strings through this API.
// Links: src/common/text/zanna_text_buffer.h,
//        src/tui/include/tui/text/text_buffer.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file zanna_text_buffer.cpp
 * @brief Implements the C ABI wrapper around the C++ piece-table text buffer.
 *
 * Exported entry points translate invalid handles and caught C++ exceptions
 * into C-compatible false, zero, or null results. Returned strings use malloc
 * and are paired with zanna_text_buffer_free_string().
 */

#include "zanna_text_buffer.h"

#include "tui/text/text_buffer.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief Concrete storage hidden behind the C ABI handle.
 *
 * @details The struct intentionally lives only in this translation unit so C
 * users cannot observe, allocate, or depend on the underlying C++ text buffer
 * layout.
 */
struct zanna_text_buffer {
    zanna::tui::text::TextBuffer buffer; ///< Owned shared text engine.
};

/**
 * @brief Copy a C++ string into an allocator-stable C string.
 *
 * @details The returned buffer is allocated with malloc and always contains one
 * trailing NUL byte after the copied payload. The payload itself may contain
 * embedded NUL bytes, so callers must use the returned length when binary-safe
 * handling matters.
 *
 * @param value String payload to copy.
 * @param out_len Optional output for the payload byte length.
 * @return Caller-owned C allocation, or NULL on allocation failure.
 */
static char *duplicate_string_for_c(const std::string &value, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (value.size() == static_cast<size_t>(-1)) {
        return nullptr;
    }

    char *copy = static_cast<char *>(std::malloc(value.size() + 1));
    if (!copy) {
        return nullptr;
    }
    if (!value.empty()) {
        std::memcpy(copy, value.data(), value.size());
    }
    copy[value.size()] = '\0';
    if (out_len) {
        *out_len = value.size();
    }
    return copy;
}

/** @copydoc zanna_text_buffer_new */
zanna_text_buffer_t *zanna_text_buffer_new(void) {
    try {
        return new zanna_text_buffer();
    } catch (...) {
        return nullptr;
    }
}

/** @copydoc zanna_text_buffer_free */
void zanna_text_buffer_free(zanna_text_buffer_t *buffer) {
    delete buffer;
}

/** @copydoc zanna_text_buffer_load_bytes */
bool zanna_text_buffer_load_bytes(zanna_text_buffer_t *buffer, const char *bytes, size_t len) {
    if (!buffer || (!bytes && len != 0)) {
        return false;
    }
    try {
        std::string text;
        if (len != 0) {
            text.assign(bytes, len);
        }
        buffer->buffer.load(std::move(text));
        return true;
    } catch (...) {
        return false;
    }
}

/** @copydoc zanna_text_buffer_insert_bytes */
bool zanna_text_buffer_insert_bytes(zanna_text_buffer_t *buffer,
                                    size_t pos,
                                    const char *bytes,
                                    size_t len) {
    if (!buffer || (!bytes && len != 0)) {
        return false;
    }
    try {
        const std::string_view text(bytes ? bytes : "", len);
        buffer->buffer.insert(pos, text);
        return true;
    } catch (...) {
        return false;
    }
}

/** @copydoc zanna_text_buffer_erase */
bool zanna_text_buffer_erase(zanna_text_buffer_t *buffer, size_t pos, size_t len) {
    if (!buffer) {
        return false;
    }
    try {
        buffer->buffer.erase(pos, len);
        return true;
    } catch (...) {
        return false;
    }
}

/** @copydoc zanna_text_buffer_begin_transaction */
void zanna_text_buffer_begin_transaction(zanna_text_buffer_t *buffer) {
    if (!buffer) {
        return;
    }
    try {
        buffer->buffer.beginTxn();
    } catch (...) {
    }
}

/** @copydoc zanna_text_buffer_end_transaction */
void zanna_text_buffer_end_transaction(zanna_text_buffer_t *buffer) {
    if (!buffer) {
        return;
    }
    try {
        buffer->buffer.endTxn();
    } catch (...) {
    }
}

/** @copydoc zanna_text_buffer_undo */
bool zanna_text_buffer_undo(zanna_text_buffer_t *buffer) {
    if (!buffer) {
        return false;
    }
    try {
        return buffer->buffer.undo();
    } catch (...) {
        return false;
    }
}

/** @copydoc zanna_text_buffer_redo */
bool zanna_text_buffer_redo(zanna_text_buffer_t *buffer) {
    if (!buffer) {
        return false;
    }
    try {
        return buffer->buffer.redo();
    } catch (...) {
        return false;
    }
}

/** @copydoc zanna_text_buffer_size */
size_t zanna_text_buffer_size(const zanna_text_buffer_t *buffer) {
    return buffer ? buffer->buffer.size() : 0;
}

/** @copydoc zanna_text_buffer_line_count */
size_t zanna_text_buffer_line_count(const zanna_text_buffer_t *buffer) {
    return buffer ? buffer->buffer.lineCount() : 0;
}

/** @copydoc zanna_text_buffer_line_start */
size_t zanna_text_buffer_line_start(const zanna_text_buffer_t *buffer, size_t line_no) {
    return buffer ? buffer->buffer.lineStart(line_no) : 0;
}

/** @copydoc zanna_text_buffer_line_end */
size_t zanna_text_buffer_line_end(const zanna_text_buffer_t *buffer, size_t line_no) {
    return buffer ? buffer->buffer.lineEnd(line_no) : 0;
}

/** @copydoc zanna_text_buffer_line_length */
size_t zanna_text_buffer_line_length(const zanna_text_buffer_t *buffer, size_t line_no) {
    return buffer ? buffer->buffer.lineLength(line_no) : 0;
}

/** @copydoc zanna_text_buffer_text_dup */
char *zanna_text_buffer_text_dup(const zanna_text_buffer_t *buffer, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!buffer) {
        return nullptr;
    }
    try {
        return duplicate_string_for_c(buffer->buffer.str(), out_len);
    } catch (...) {
        return nullptr;
    }
}

/** @copydoc zanna_text_buffer_line_dup */
char *zanna_text_buffer_line_dup(const zanna_text_buffer_t *buffer,
                                 size_t line_no,
                                 size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!buffer) {
        return nullptr;
    }
    try {
        return duplicate_string_for_c(buffer->buffer.getLine(line_no), out_len);
    } catch (...) {
        return nullptr;
    }
}

/** @copydoc zanna_text_buffer_free_string */
void zanna_text_buffer_free_string(char *text) {
    std::free(text);
}
