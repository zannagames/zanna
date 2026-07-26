//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the self-contained JSON value, parser, and compact emitter
///        used by Zanna's language-server protocols.
///
/// JsonValue represents every RFC 8259 value category without external
/// dependencies. Objects use ordered key/value storage so protocol output is
/// deterministic, while strings and child values are always owned. Parsing
/// failures are reported as position-bearing @c std::runtime_error exceptions.
///
/// @par Invariants
/// - Object members retain insertion order.
/// - Parsed integers use signed 64-bit storage and fractional/exponent numbers
///   use double-precision storage.
/// - Successful parse/emit round trips retain the represented JSON value.
///
/// @see JsonRpc.hpp
/// @see Transport.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace zanna::server {

/// @brief Discriminants for the alternatives supported by JsonValue.
/// @details Enumerator order matches JsonValue's private variant alternatives.
enum class JsonType : uint8_t {
    Null,   ///< JSON null.
    Bool,   ///< JSON Boolean.
    Int,    ///< JSON number stored as a signed 64-bit integer.
    Double, ///< JSON number stored as a double.
    String, ///< Owned UTF-8 JSON string.
    Array,  ///< Ordered sequence of JSON values.
    Object, ///< Ordered sequence of string/value members.
};

/// @brief Owned value representation for null, Boolean, numeric, string,
///        array, and object JSON data.
///
/// Objects are stored as ordered vectors of (key, value) pairs to preserve
/// insertion order, which is important for deterministic protocol output.
/// O(n) key lookup is acceptable since protocol objects are small (<30 keys).
/// The type is copyable and movable, and returned fallback references point to
/// immutable process-lifetime sentinels.
class JsonValue {
  public:
    /// @brief Owned, ordered storage type for JSON array elements.
    using ArrayType = std::vector<JsonValue>;

    /// @brief Owned, insertion-ordered storage type for JSON object members.
    using ObjectType = std::vector<std::pair<std::string, JsonValue>>;

    /// @brief Construct a null JSON value.
    /// @post type() is JsonType::Null.
    JsonValue();

    /// @brief Construct a boolean JSON value.
    /// @param b Boolean value to store.
    explicit JsonValue(bool b);

    /// @brief Construct an integer JSON value.
    /// @param i Signed 64-bit integer to store exactly.
    explicit JsonValue(int64_t i);

    /// @brief Construct an integer JSON value from a native @c int.
    /// @param i Integer to widen to the signed 64-bit representation.
    explicit JsonValue(int i);

    /// @brief Construct a double-precision JSON number.
    /// @param d Value to store; non-finite values emit as JSON @c null.
    explicit JsonValue(double d);

    /// @brief Construct a JSON string by taking ownership of a string.
    /// @param s String whose contents are moved into this value.
    explicit JsonValue(std::string s);

    /// @brief Construct a JSON string by copying a character view.
    /// @param s View whose complete contents are copied.
    explicit JsonValue(std::string_view s);

    /// @brief Construct a JSON string from a nullable C string.
    /// @param s Null-terminated text to copy, or @c nullptr for an empty string.
    explicit JsonValue(const char *s);

    /// @brief Construct a JSON array by taking ownership of its elements.
    /// @param arr Ordered elements to move into this value.
    explicit JsonValue(ArrayType arr);

    /// @brief Construct a JSON object by taking ownership of its members.
    /// @param obj Ordered key/value members to move into this value.
    explicit JsonValue(ObjectType obj);

    /// @brief Report the active JSON alternative.
    /// @return Type corresponding to the active stored alternative.
    [[nodiscard]] JsonType type() const;

    /// @brief Test whether this value represents JSON null.
    /// @return @c true when type() is JsonType::Null.
    [[nodiscard]] bool isNull() const;

    // --- Accessors (return default on type mismatch) ---

    /// @brief Return the boolean value, or @p def when not a Bool.
    /// @param def Fallback for non-Boolean values.
    /// @return Stored Boolean or @p def.
    [[nodiscard]] bool asBool(bool def = false) const;

    /// @brief Return the integer value, or @p def when not integral.
    /// @details Int values are returned directly. Double values are accepted only
    ///          when finite, exactly whole-numbered, and within int64_t range.
    /// @param def Fallback when no exact integer conversion is available.
    /// @return Stored or exactly converted integer, otherwise @p def.
    [[nodiscard]] int64_t asInt(int64_t def = 0) const;

    /// @brief Return this value as a double, or @p def when nonnumeric.
    /// @param def Fallback for null, Boolean, string, array, and object values.
    /// @return Stored double, integer converted to double, or @p def.
    [[nodiscard]] double asDouble(double def = 0.0) const;

    /// @brief Access the string value.
    /// @return Stored string, or a shared empty string on a type mismatch.
    [[nodiscard]] const std::string &asString() const;

    /// @brief Access the array elements.
    /// @return Stored array, or a shared empty array on a type mismatch.
    [[nodiscard]] const ArrayType &asArray() const;

    /// @brief Access the ordered object members.
    /// @return Stored object, or a shared empty object on a type mismatch.
    [[nodiscard]] const ObjectType &asObject() const;

    // --- Object member access ---

    /// @brief Find an object member by key.
    /// @details Accepts a string view to avoid allocating temporary strings when
    ///          protocol handlers look up string-literal member names.
    /// @param key Member name to locate.
    /// @return Pointer to the first matching value, or @c nullptr when absent
    ///         or when this value is not an object.
    /// @warning The pointer is invalidated if the owning value is destroyed or
    ///          its object storage is modified.
    [[nodiscard]] const JsonValue *get(std::string_view key) const;

    /// @brief Access an object member with a shared null fallback.
    /// @param key Member name to locate.
    /// @return Matching value, or an immutable shared null value when absent.
    [[nodiscard]] const JsonValue &operator[](std::string_view key) const;

    /// @brief Test whether this object has a member with the given key.
    /// @param key Member name to locate.
    /// @return @c true only for an object containing @p key.
    [[nodiscard]] bool has(std::string_view key) const;

    // --- Array access ---

    /// @brief Return the number of array elements or object members.
    /// @return Container size, or zero for scalar values.
    [[nodiscard]] size_t size() const;

    /// @brief Access an array element by zero-based index.
    /// @param index Position of the requested element.
    /// @return Element at @p index, or an immutable shared null value when the
    ///         index is out of range or this value is not an array.
    [[nodiscard]] const JsonValue &at(size_t index) const;

    // --- Builders ---

    /// @brief Create a JSON object from an initializer list.
    /// @param members Members copied in initializer-list order.
    /// @return Object containing the supplied members.
    static JsonValue object(std::initializer_list<std::pair<std::string, JsonValue>> members);

    /// @brief Create a JSON array from an initializer list.
    /// @param elems Elements copied in initializer-list order.
    /// @return Array containing the supplied elements.
    static JsonValue array(std::initializer_list<JsonValue> elems);

    /// @brief Create a JSON object from owned member storage.
    /// @param members Ordered members to move into the result.
    /// @return Object containing the supplied members.
    static JsonValue object(ObjectType members);

    /// @brief Create a JSON array from owned element storage.
    /// @param elems Elements to move into the result.
    /// @return Array containing the supplied elements.
    static JsonValue array(ArrayType elems);

    // --- Serialization ---

    /// @brief Serialize this value without insignificant whitespace.
    /// @return Complete compact JSON document.
    /// @details Object member order is preserved and non-finite doubles are
    ///          encoded as JSON @c null.
    [[nodiscard]] std::string toCompactString() const;

    // --- Parsing ---

    /// @brief Parse one complete JSON document.
    /// @param input JSON text to parse; the returned value owns its data.
    /// @return Parsed JSON value.
    /// @throws std::runtime_error On malformed syntax or UTF-8, invalid or
    ///         out-of-range numbers, excessive nesting, or trailing input.
    static JsonValue parse(std::string_view input);

    // --- Comparison ---

    /// @brief Deep value equality (same type and recursively equal contents).
    /// @param other Value to compare with this value.
    /// @return @c true when both structures and all scalar contents match.
    bool operator==(const JsonValue &other) const;

    /// @brief Logical negation of operator==.
    /// @param other Value to compare with this value.
    /// @return @c true when the values are not structurally equal.
    bool operator!=(const JsonValue &other) const;

  private:
    using Storage =
        std::variant<std::nullptr_t, bool, int64_t, double, std::string, ArrayType, ObjectType>;
    Storage storage_;

    static const std::string kEmptyString;
    static const ArrayType kEmptyArray;
    static const ObjectType kEmptyObject;
    static const JsonValue kNull;

    /// @brief Recursively append this value's compact JSON encoding to a buffer.
    /// @param out Destination buffer to extend without clearing existing text.
    void emitTo(std::string &out) const;
};

} // namespace zanna::server
