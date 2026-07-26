//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/Json.cpp
// Purpose: JSON value type implementation — parser and emitter.
// Key invariants:
//   - Parser is recursive descent, handles all JSON types per RFC 8259
//   - Strings handle all standard escape sequences (\", \\, \/, \b, \f, \n, \r, \t, \uXXXX)
//   - Numbers: integers stored as int64_t, floats as double
// Ownership/Lifetime:
//   - All allocations via std::string/std::vector (RAII)
// Links: tools/lsp-common/Json.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Json.cpp
 * @brief Implements the language-server JSON value, parser, and emitter.
 * @details JsonValue stores null, Boolean, integer, floating, String, array,
 *          and object alternatives with RAII ownership. The recursive-descent
 *          parser validates complete RFC 8259 input and Unicode escapes, while
 *          serialization escapes Strings and emits compact JSON text.
 */

#include "tools/lsp-common/Json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace zanna::server {

// Static members
const std::string JsonValue::kEmptyString;
const JsonValue::ArrayType JsonValue::kEmptyArray;
const JsonValue::ObjectType JsonValue::kEmptyObject;
const JsonValue JsonValue::kNull;

// --- Constructors ---

/// @brief Construct a JSON null value.
/// @details Initializes the variant to its null alternative.
JsonValue::JsonValue() : storage_(nullptr) {}

/// @brief Construct a JSON Boolean value.
/// @param b Boolean value to store.
JsonValue::JsonValue(bool b) : storage_(b) {}

/// @brief Construct a JSON integer value.
/// @param i Signed 64-bit integer to store without conversion.
JsonValue::JsonValue(int64_t i) : storage_(i) {}

/// @brief Construct a JSON integer value from a native @c int.
/// @param i Integer to widen to the value type's signed 64-bit representation.
JsonValue::JsonValue(int i) : storage_(static_cast<int64_t>(i)) {}

/// @brief Construct a JSON floating-point value.
/// @param d Double-precision value to store.
/// @details Non-finite values can be represented in memory, but the emitter
///          serializes them as JSON @c null because JSON has no NaN or infinity.
JsonValue::JsonValue(double d) : storage_(d) {}

/// @brief Construct a JSON string by taking ownership of a string.
/// @param s String whose contents are moved into this value.
JsonValue::JsonValue(std::string s) : storage_(std::move(s)) {}

/// @brief Construct a JSON string by copying a character view.
/// @param s View whose complete contents are copied into owned storage.
JsonValue::JsonValue(std::string_view s) : storage_(std::string(s)) {}

/// @brief Construct a JSON string from a nullable C string.
/// @param s Null-terminated string to copy, or @c nullptr to create an empty string.
JsonValue::JsonValue(const char *s) : storage_(std::string(s ? s : "")) {}

/// @brief Construct a JSON array by taking ownership of its elements.
/// @param arr Ordered array elements to move into this value.
JsonValue::JsonValue(ArrayType arr) : storage_(std::move(arr)) {}

/// @brief Construct a JSON object by taking ownership of its members.
/// @param obj Ordered key/value members to move into this value.
JsonValue::JsonValue(ObjectType obj) : storage_(std::move(obj)) {}

// --- Type inspection ---

/// @brief Report the active JSON alternative.
/// @return Type corresponding to the active variant alternative.
JsonType JsonValue::type() const {
    return static_cast<JsonType>(storage_.index());
}

/// @brief Test whether this value represents JSON null.
/// @return @c true when the null variant alternative is active.
bool JsonValue::isNull() const {
    return storage_.index() == 0;
}

// --- Accessors ---

/// @brief Read this value as a Boolean.
/// @param def Fallback returned when this value is not a JSON Boolean.
/// @return Stored Boolean, or @p def on a type mismatch.
bool JsonValue::asBool(bool def) const {
    if (auto *p = std::get_if<bool>(&storage_))
        return *p;
    return def;
}

/// @brief Read this value as a signed 64-bit integer.
/// @param def Fallback returned when no exact integer conversion is available.
/// @return The stored integer, an exactly integral in-range double converted to
///         @c int64_t, or @p def for all other values.
int64_t JsonValue::asInt(int64_t def) const {
    if (auto *p = std::get_if<int64_t>(&storage_))
        return *p;
    if (auto *p = std::get_if<double>(&storage_)) {
        if (!std::isfinite(*p))
            return def;
        const double value = *p;
        if (std::trunc(value) != value)
            return def;
        if (value < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int64_t>::max())) {
            return def;
        }
        return static_cast<int64_t>(value);
    }
    return def;
}

/// @brief Read this value as a double-precision number.
/// @param def Fallback returned when this value is not numeric.
/// @return The stored double, the stored integer converted to @c double, or
///         @p def for nonnumeric values.
double JsonValue::asDouble(double def) const {
    if (auto *p = std::get_if<double>(&storage_))
        return *p;
    if (auto *p = std::get_if<int64_t>(&storage_))
        return static_cast<double>(*p);
    return def;
}

/// @brief Read this value as a string.
/// @return Reference to the stored string, or a process-lifetime empty string
///         when this value is not a JSON string.
const std::string &JsonValue::asString() const {
    if (auto *p = std::get_if<std::string>(&storage_))
        return *p;
    return kEmptyString;
}

/// @brief Read this value as an array.
/// @return Reference to the stored elements, or a process-lifetime empty array
///         when this value is not a JSON array.
const JsonValue::ArrayType &JsonValue::asArray() const {
    if (auto *p = std::get_if<ArrayType>(&storage_))
        return *p;
    return kEmptyArray;
}

/// @brief Read this value as an object.
/// @return Reference to the ordered members, or a process-lifetime empty object
///         when this value is not a JSON object.
const JsonValue::ObjectType &JsonValue::asObject() const {
    if (auto *p = std::get_if<ObjectType>(&storage_))
        return *p;
    return kEmptyObject;
}

// --- Object access ---

/// @brief Find an object member by key.
/// @param key Member name to compare without allocating a temporary string.
/// @return Pointer to the first matching value, or @c nullptr when this value
///         is not an object or no member has that name.
/// @note The pointer remains valid only while the owning JsonValue and its
///       object storage remain unmodified.
const JsonValue *JsonValue::get(std::string_view key) const {
    if (auto *obj = std::get_if<ObjectType>(&storage_)) {
        for (const auto &[k, v] : *obj) {
            if (k == key)
                return &v;
        }
    }
    return nullptr;
}

/// @brief Access an object member by key with a null-value fallback.
/// @param key Member name to locate.
/// @return Reference to the matching value, or a shared immutable JSON null
///         value when the object or member is absent.
const JsonValue &JsonValue::operator[](std::string_view key) const {
    if (const auto *v = get(key))
        return *v;
    return kNull;
}

/// @brief Test whether an object contains a named member.
/// @param key Member name to locate.
/// @return @c true when this value is an object containing @p key.
bool JsonValue::has(std::string_view key) const {
    return get(key) != nullptr;
}

// --- Array access ---

/// @brief Return the number of immediate array elements or object members.
/// @return Container size for arrays and objects; zero for every scalar type.
size_t JsonValue::size() const {
    if (auto *arr = std::get_if<ArrayType>(&storage_))
        return arr->size();
    if (auto *obj = std::get_if<ObjectType>(&storage_))
        return obj->size();
    return 0;
}

/// @brief Access an array element by zero-based index.
/// @param index Position of the requested element.
/// @return Reference to the element, or a shared immutable JSON null value when
///         this value is not an array or @p index is out of range.
const JsonValue &JsonValue::at(size_t index) const {
    if (auto *arr = std::get_if<ArrayType>(&storage_)) {
        if (index < arr->size())
            return (*arr)[index];
    }
    return kNull;
}

// --- Builders ---

/// @brief Build an ordered JSON object from initializer-list members.
/// @param members Key/value pairs copied in initializer-list order.
/// @return Object value containing the supplied members.
JsonValue JsonValue::object(std::initializer_list<std::pair<std::string, JsonValue>> members) {
    return JsonValue(ObjectType(members.begin(), members.end()));
}

/// @brief Build a JSON array from initializer-list elements.
/// @param elems Values copied in initializer-list order.
/// @return Array value containing the supplied elements.
JsonValue JsonValue::array(std::initializer_list<JsonValue> elems) {
    return JsonValue(ArrayType(elems.begin(), elems.end()));
}

/// @brief Build an ordered JSON object from owned member storage.
/// @param members Key/value pairs to move into the resulting object.
/// @return Object value containing the supplied members.
JsonValue JsonValue::object(ObjectType members) {
    return JsonValue(std::move(members));
}

/// @brief Build a JSON array from owned element storage.
/// @param elems Elements to move into the resulting array.
/// @return Array value containing the supplied elements.
JsonValue JsonValue::array(ArrayType elems) {
    return JsonValue(std::move(elems));
}

// --- Comparison ---

/// @brief Compare two JSON values for exact structural equality.
/// @param other Value to compare with this value.
/// @return @c true when both values have the same type and recursively equal
///         scalar data, array elements, or ordered object members.
bool JsonValue::operator==(const JsonValue &other) const {
    return storage_ == other.storage_;
}

/// @brief Compare two JSON values for structural inequality.
/// @param other Value to compare with this value.
/// @return @c true when exact structural equality does not hold.
bool JsonValue::operator!=(const JsonValue &other) const {
    return storage_ != other.storage_;
}

// --- Emitter ---

/// @brief Append @p s to @p out as a quoted, escaped JSON string literal.
/// @details Escapes the standard control/quote characters and encodes any other
///          byte below 0x20 as a \\u00XX sequence; the surrounding quotes are
///          added by this function.
/// @param out Destination buffer to extend.
/// @param s Unquoted string bytes to encode.
static void emitString(std::string &out, const std::string &s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control characters as \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    out += '"';
}

/// @brief Append this value's compact JSON representation to a buffer.
/// @param out Destination buffer to extend without clearing existing contents.
/// @details Arrays and objects are emitted recursively and object insertion
///          order is retained. Non-finite doubles are emitted as JSON @c null.
void JsonValue::emitTo(std::string &out) const {
    switch (type()) {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Bool:
            out += asBool() ? "true" : "false";
            break;
        case JsonType::Int: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(asInt()));
            out += buf;
            break;
        }
        case JsonType::Double: {
            double d = asDouble();
            if (std::isnan(d) || std::isinf(d)) {
                out += "null"; // JSON has no NaN/Inf
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                out += buf;
                // Ensure it looks like a float (has . or e)
                if (std::string_view(buf).find_first_of(".eE") == std::string_view::npos)
                    out += ".0";
            }
            break;
        }
        case JsonType::String:
            emitString(out, asString());
            break;
        case JsonType::Array: {
            out += '[';
            const auto &arr = asArray();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0)
                    out += ',';
                arr[i].emitTo(out);
            }
            out += ']';
            break;
        }
        case JsonType::Object: {
            out += '{';
            const auto &obj = asObject();
            for (size_t i = 0; i < obj.size(); ++i) {
                if (i > 0)
                    out += ',';
                emitString(out, obj[i].first);
                out += ':';
                obj[i].second.emitTo(out);
            }
            out += '}';
            break;
        }
    }
}

/// @brief Serialize this value without insignificant whitespace.
/// @return Complete compact JSON document representing this value.
std::string JsonValue::toCompactString() const {
    std::string out;
    out.reserve(128);
    emitTo(out);
    return out;
}

// ==========================================================================
// JSON Parser — recursive descent
// ==========================================================================

namespace {

/// @brief Recursive-descent JSON parser over a borrowed string_view (RFC 8259).
/// @details Tracks a cursor into the input and exposes a single parse() entry
///          point; malformed input is reported by throwing std::runtime_error.
class JsonParser {
  public:
    /// @brief Construct a parser positioned at the start of an input view.
    /// @param input JSON text borrowed for the lifetime of this parser.
    explicit JsonParser(std::string_view input) : src_(input), pos_(0) {}

    /// @brief Parse the entire input as a single JSON value.
    /// @details Skips surrounding whitespace and rejects trailing characters.
    /// @return Parsed, independently owned JSON value.
    /// @throws std::runtime_error on any syntax error.
    JsonValue parse() {
        skipWhitespace();
        auto val = parseValue();
        skipWhitespace();
        if (pos_ < src_.size())
            error("unexpected trailing characters");
        return val;
    }

  private:
    static constexpr int kMaxDepth = 512;

    std::string_view src_;
    size_t pos_;

    /// @brief Report a parse failure at the current cursor position.
    /// @param msg Human-readable reason appended to the position prefix.
    /// @throws std::runtime_error Always; this function does not return.
    [[noreturn]] void error(const char *msg) const {
        std::string err = "JSON parse error at position ";
        err += std::to_string(pos_);
        err += ": ";
        err += msg;
        throw std::runtime_error(err);
    }

    /// @brief Inspect the current input character without consuming it.
    /// @return Current character, or @c '\0' when the cursor is at end of input.
    char peek() const {
        if (pos_ >= src_.size())
            return '\0';
        return src_[pos_];
    }

    /// @brief Consume and return the current input character.
    /// @return Character that occupied the current cursor position.
    /// @throws std::runtime_error when the cursor is already at end of input.
    char advance() {
        if (pos_ >= src_.size())
            error("unexpected end of input");
        return src_[pos_++];
    }

    /// @brief Consume an expected punctuation character.
    /// @param c Required character at the current cursor position.
    /// @throws std::runtime_error when the input ends or a different character
    ///         occupies the current position.
    void expect(char c) {
        char got = advance();
        if (got != c) {
            std::string msg = "expected '";
            msg += c;
            msg += "', got '";
            msg += got;
            msg += "'";
            error(msg.c_str());
        }
    }

    /// @brief Advance past a run of RFC 8259 whitespace.
    /// @details Recognizes space, tab, line feed, and carriage return only.
    void skipWhitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    /// @brief Conditionally consume a literal at the current cursor.
    /// @param literal Exact byte sequence to match.
    /// @return @c true and advances the cursor on a match; @c false without
    ///         consuming input otherwise.
    bool tryConsume(std::string_view literal) {
        if (src_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    /// @brief Parse a single JSON value, dispatching on the leading character.
    /// @param depth Current nesting depth, bounded by kMaxDepth to stop runaway
    ///        recursion on deeply nested or malicious input.
    /// @return Parsed null, Boolean, number, string, array, or object value.
    /// @throws std::runtime_error for malformed input or excessive nesting.
    JsonValue parseValue(int depth = 0) {
        if (depth > kMaxDepth)
            error("maximum nesting depth exceeded");
        skipWhitespace();
        char c = peek();
        if (c == '\0')
            error("unexpected end of input");

        switch (c) {
            case 'n':
                if (tryConsume("null"))
                    return JsonValue();
                error("invalid literal");
            case 't':
                if (tryConsume("true"))
                    return JsonValue(true);
                error("invalid literal");
            case 'f':
                if (tryConsume("false"))
                    return JsonValue(false);
                error("invalid literal");
            case '"':
                return JsonValue(parseString());
            case '[':
                return parseArray(depth + 1);
            case '{':
                return parseObject(depth + 1);
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parseNumber();
                error("unexpected character");
        }
    }

    /// @brief Parse a quoted JSON string and decode its escapes.
    /// @return Owned UTF-8 string contents without the surrounding quotes.
    /// @throws std::runtime_error for invalid escapes, surrogate pairs, control
    ///         characters, UTF-8 sequences, or an unterminated string.
    std::string parseString() {
        expect('"');
        std::string result;
        result.reserve(16);
        while (true) {
            if (pos_ >= src_.size())
                error("unterminated string");
            char c = src_[pos_++];
            if (c == '"')
                return result;
            if (c == '\\') {
                if (pos_ >= src_.size())
                    error("unterminated escape");
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':
                        result += '"';
                        break;
                    case '\\':
                        result += '\\';
                        break;
                    case '/':
                        result += '/';
                        break;
                    case 'b':
                        result += '\b';
                        break;
                    case 'f':
                        result += '\f';
                        break;
                    case 'n':
                        result += '\n';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'u': {
                        uint32_t cp = parseHex4();
                        // Handle surrogate pairs
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos_ + 1 < src_.size() && src_[pos_] == '\\' &&
                                src_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                uint32_t lo = parseHex4();
                                if (lo >= 0xDC00 && lo <= 0xDFFF)
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                else
                                    error("invalid surrogate pair");
                            } else {
                                error("missing low surrogate");
                            }
                        }
                        if (cp >= 0xDC00 && cp <= 0xDFFF)
                            error("lone low surrogate");
                        encodeUtf8(result, cp);
                        break;
                    }
                    default:
                        error("invalid escape sequence");
                }
            } else {
                const auto uc = static_cast<unsigned char>(c);
                if (uc < 0x20)
                    error("unescaped control character in string");
                if (uc < 0x80) {
                    result += c;
                } else {
                    appendValidatedUtf8(result, uc);
                }
            }
        }
    }

    /// @brief Append a raw non-ASCII UTF-8 sequence after validating RFC 3629 form.
    /// @details JSON input is UTF-8. This helper consumes the continuation bytes belonging to
    /// @p lead, rejects malformed, overlong, surrogate, and out-of-range encodings, and appends
    /// the original byte sequence to @p out when valid.
    /// @param out Decoded string buffer to extend.
    /// @param lead Already-consumed leading byte of the UTF-8 sequence.
    /// @throws std::runtime_error when the sequence is truncated or invalid.
    void appendValidatedUtf8(std::string &out, unsigned char lead) {
        int length = 0;
        uint32_t cp = 0;
        uint32_t minCodePoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
            minCodePoint = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
            minCodePoint = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
            minCodePoint = 0x10000;
        } else {
            error("invalid UTF-8 sequence in string");
        }

        std::string bytes;
        bytes.reserve(static_cast<std::size_t>(length));
        bytes.push_back(static_cast<char>(lead));
        for (int i = 1; i < length; ++i) {
            if (pos_ >= src_.size())
                error("truncated UTF-8 sequence in string");
            const auto cont = static_cast<unsigned char>(src_[pos_++]);
            if ((cont & 0xC0u) != 0x80u)
                error("invalid UTF-8 continuation byte in string");
            cp = (cp << 6) | static_cast<uint32_t>(cont & 0x3Fu);
            bytes.push_back(static_cast<char>(cont));
        }

        if (cp < minCodePoint || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            error("invalid UTF-8 code point in string");
        out += bytes;
    }

    /// @brief Parse exactly four hexadecimal digits after a @c \\u escape.
    /// @return Unsigned 16-bit code-unit value represented by the four digits.
    /// @throws std::runtime_error when fewer than four characters remain or any
    ///         character is not hexadecimal.
    uint32_t parseHex4() {
        if (pos_ + 4 > src_.size())
            error("incomplete \\u escape");
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            val <<= 4;
            if (c >= '0' && c <= '9')
                val |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                val |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                val |= static_cast<uint32_t>(c - 'A' + 10);
            else
                error("invalid hex digit in \\u escape");
        }
        return val;
    }

    /// @brief Append a Unicode code point to a string as one to four UTF-8 bytes.
    /// @param out Destination string to extend.
    /// @param cp Scalar value to encode; values at or above 0x110000 append nothing.
    static void encodeUtf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x110000) {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    /// @brief Parse a JSON number, returning an Int when integral or a Double when
    ///        it has a fraction/exponent; rejects out-of-range or malformed values.
    /// @return Numeric JSON value whose representation matches the parsed syntax.
    /// @throws std::runtime_error for malformed, out-of-range, or non-finite numbers.
    JsonValue parseNumber() {
        size_t start = pos_;
        bool isFloat = false;

        if (peek() == '-')
            ++pos_;

        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        } else {
            error("invalid number");
        }

        if (pos_ < src_.size() && src_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                error("expected digit after decimal point");
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                error("expected digit in exponent");
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        const char *begin = src_.data() + start;
        const char *end = src_.data() + pos_;

        if (isFloat) {
            double d = 0.0;
            auto parsed = std::from_chars(begin, end, d);
            if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(d))
                error("invalid number");
            return JsonValue(d);
        } else {
            int64_t value = 0;
            auto parsed = std::from_chars(begin, end, value, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                error("invalid number");
            return JsonValue(value);
        }
    }

    /// @brief Parse a JSON array at the given nesting depth.
    /// @param depth Nesting depth passed to each child-value parse.
    /// @return Array containing parsed elements in source order.
    /// @throws std::runtime_error when separators, elements, or closing syntax
    ///         are malformed.
    JsonValue parseArray(int depth) {
        expect('[');
        skipWhitespace();
        JsonValue::ArrayType arr;
        if (peek() == ']') {
            ++pos_;
            return JsonValue(std::move(arr));
        }
        while (true) {
            arr.push_back(parseValue(depth));
            skipWhitespace();
            if (peek() == ']') {
                ++pos_;
                return JsonValue(std::move(arr));
            }
            expect(',');
        }
    }

    /// @brief Parse a JSON object `{ "key": value, ... }`, preserving member order.
    /// @details Duplicate keys are accepted with last-value-wins semantics. This
    ///          matches common JSON protocol behavior while preserving the first
    ///          key's position for deterministic re-emission.
    /// @param depth Nesting depth passed to each member-value parse.
    /// @return Object containing parsed members in first-occurrence order.
    /// @throws std::runtime_error when keys, separators, values, or closing
    ///         syntax are malformed.
    JsonValue parseObject(int depth) {
        expect('{');
        skipWhitespace();
        JsonValue::ObjectType obj;
        std::unordered_map<std::string, std::size_t> indexByKey;
        if (peek() == '}') {
            ++pos_;
            return JsonValue(std::move(obj));
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"')
                error("expected string key");
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            auto val = parseValue(depth);
            auto existing = indexByKey.find(key);
            if (existing != indexByKey.end()) {
                obj[existing->second].second = std::move(val);
            } else {
                indexByKey.emplace(key, obj.size());
                obj.emplace_back(std::move(key), std::move(val));
            }
            skipWhitespace();
            if (peek() == '}') {
                ++pos_;
                return JsonValue(std::move(obj));
            }
            expect(',');
        }
    }
};

} // anonymous namespace

/// @brief Parse a complete JSON document.
/// @param input JSON text to parse; the result owns all copied data.
/// @return Parsed JSON value.
/// @throws std::runtime_error for invalid syntax, encoding, numeric values,
///         excessive nesting, or trailing non-whitespace input.
JsonValue JsonValue::parse(std::string_view input) {
    JsonParser parser(input);
    return parser.parse();
}

} // namespace zanna::server
