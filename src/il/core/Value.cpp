//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides the helper constructors and formatting routines that accompany the
// lightweight IL value type.  Callers primarily use these helpers when building
// or serialising IR, so concentrating the logic here keeps the header concise
// while documenting the canonical encodings for temporaries and constants.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements convenience constructors and printers for IL values.
/// @details The @ref il::core::Value type is a compact tagged union that
///          represents SSA temporaries and literals flowing through the
///          intermediate language.  This file defines ergonomic factory helpers
///          together with @ref toString so other subsystems can construct and
///          inspect values without repeating encoding knowledge.  Keeping the
///          logic out of the header minimises compile times while centralising
///          documentation for how each literal form should appear in textual IL.

#include "il/core/Value.hpp"
#include "il/io/StringEscape.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace il::core {

/// @brief Create a temporary value wrapper for SSA identifiers.
/// @details Temporaries appear as `%tN` in the textual IL.  They always use the
///          temp kind and record the numeric identifier in @ref Value::id.
/// @param t Zero-based identifier assigned by the surrounding builder.
/// @return Value representing the temporary reference.
Value Value::temp(unsigned t) {
    Value v;
    v.kind = Kind::Temp;
    v.id = t;
    return v;
}

/// @brief Create a signed integer constant value.
/// @details Preserves the exact @c long long bit pattern so two's-complement
///          wraparound semantics are maintained when consumed by handlers.
/// @param v Integer payload to embed in the value.
/// @return Value representing the integer literal.
Value Value::constInt(long long val) {
    Value v;
    v.kind = Kind::ConstInt;
    v.i64 = val;
    return v;
}

/// @brief Create a boolean literal backed by the integer constant encoding.
/// @details Booleans piggy-back on the integer constant representation but set
///          the @ref Value::isBool flag so printers render them as `true` /
///          `false` instead of numeric digits.
/// @param v Boolean payload to embed in the value.
/// @return Value representing the boolean literal.
Value Value::constBool(bool val) {
    Value v;
    v.kind = Kind::ConstInt;
    v.i64 = val ? 1 : 0;
    v.isBool = true;
    return v;
}

/// @brief Create a floating-point constant value.
/// @details Stores the exact @c double payload so NaNs and infinities propagate
///          through the IR unchanged.
/// @param v Floating-point payload to embed in the value.
/// @return Value representing the floating literal.
Value Value::constFloat(double val) {
    Value v;
    v.kind = Kind::ConstFloat;
    v.f64 = val;
    return v;
}

/// @brief Create a string literal value.
/// @details Moves the string payload into the @ref Value instance.  Literal
///          encoding (escaped or raw) is handled by callers so the helper merely
///          records the bytes.
/// @param s String contents of the literal.
/// @return Value representing the string literal.
Value Value::constStr(std::string s) {
    Value v;
    v.kind = Kind::ConstStr;
    v.str = std::move(s);
    return v;
}

/// @brief Create a global address value that refers to a named global symbol.
/// @details Stores the canonical name of the global and transfers ownership to
///          the resulting @ref Value instance.
/// @param s Name of the referenced global.
/// @return Value representing the global address literal.
Value Value::global(std::string s) {
    Value v;
    v.kind = Kind::GlobalAddr;
    v.str = std::move(s);
    return v;
}

/// @brief Create the null pointer literal used by pointer-typed values.
/// @details Null values always carry the @ref Kind::NullPtr tag and have empty
///          payloads.
/// @return Value representing the null literal.
Value Value::null() {
    return Value{}; // Default constructor creates NullPtr
}

/// @brief Render a value into its textual IL representation.
/// @details Mirrors the canonical format produced by the serializer: temporaries
///          appear as `%tN`, integers print in base 10 (with booleans spelled
///          out through the dedicated flag), floating-point values use a
///          precision high enough to round-trip IEEE-754 doubles before trimming
///          redundant zeros, strings are re-escaped through
///          @ref il::io::encodeEscapedString, and globals are prefixed with `@`.
///          Null pointers always render as the literal `null`.  Keeping the
///          implementation here avoids scattering formatting conventions across
///          the codebase and ensures debug output matches the canonical printer.
/// @param v Value to render.
/// @return String representation suitable for diagnostics or textual IL.
std::string toString(const Value &v) {
    switch (v.kind) {
        case Value::Kind::Temp:
            return "%t" + std::to_string(v.id);
        case Value::Kind::ConstInt:
            if (v.isBool)
                return v.i64 != 0 ? "true" : "false";
            return std::to_string(v.i64);
        case Value::Kind::ConstFloat: {
            // Canonicalise special values explicitly to ensure stable spelling
            // and avoid appending a fractional part to tokens like "nan".
            if (std::isnan(v.f64))
                return "NaN";
            if (std::isinf(v.f64))
                return std::signbit(v.f64) ? std::string("-Inf") : std::string("Inf");

            if (std::signbit(v.f64) && v.f64 == 0.0)
                return "-0.0";
            std::ostringstream oss;
            oss.imbue(std::locale::classic());
            oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
            oss << std::setprecision(std::numeric_limits<double>::digits10 + 2) << v.f64;
            std::string s = oss.str();
            if (v.f64 == 0.0)
                return "0.0";
            if (s.find('.') != std::string::npos) {
                while (!s.empty() && s.back() == '0')
                    s.pop_back();
                // Keep at least ".0" so float constants are unambiguous.
                if (!s.empty() && s.back() == '.')
                    s += '0';
            } else if (s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                // Integral-valued float with no decimal or exponent; append .0
                // so consumers can distinguish float from integer literals.
                s += ".0";
            }
            return s;
        }
        case Value::Kind::ConstStr:
            return std::string("\"") + il::io::encodeEscapedString(v.str) + "\"";
        case Value::Kind::GlobalAddr:
            return "@" + v.str;
        case Value::Kind::NullPtr:
            return "null";
    }
    return "";
}

//===----------------------------------------------------------------------===//
// Value Comparison and Hashing
//===----------------------------------------------------------------------===//

/// @brief Compare IL values by discriminant and semantic payload.
/// @param a First value.
/// @param b Second value.
/// @return True for identical temporary IDs, literal payloads/flags, global
///         names, or two null values.
/// @details Floating-point payloads compare by bits, preserving distinctions
///          such as signed zero and treating identical NaN encodings as equal.
bool valueEquals(const Value &a, const Value &b) noexcept {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case Value::Kind::Temp:
            return a.id == b.id;
        case Value::Kind::ConstInt:
            return a.i64 == b.i64 && a.isBool == b.isBool;
        case Value::Kind::ConstFloat: {
            // Use bitwise comparison to match valueHash() behavior.
            // IEEE 754 == treats -0.0 == 0.0 (true) and NaN != NaN (true),
            // but for CSE/hashing we need bitwise identity: -0.0 and 0.0 are
            // distinct constants, and identical NaN values must compare equal.
            static_assert(sizeof(double) == sizeof(unsigned long long));
            unsigned long long aBits, bBits;
            std::memcpy(&aBits, &a.f64, sizeof(double));
            std::memcpy(&bBits, &b.f64, sizeof(double));
            return aBits == bBits;
        }
        case Value::Kind::ConstStr:
        case Value::Kind::GlobalAddr:
            return a.str == b.str;
        case Value::Kind::NullPtr:
            return true;
    }
    return false;
}

/// @brief Hash an IL value consistently with valueEquals().
/// @param v Value to hash.
/// @return Mixed kind-and-payload hash suitable for unordered containers.
size_t valueHash(const Value &v) noexcept {
    const auto fold64 = [](std::uint64_t value) noexcept -> size_t {
        value ^= value >> 33;
        value *= UINT64_C(0xff51afd7ed558ccd);
        value ^= value >> 33;
        value *= UINT64_C(0xc4ceb9fe1a85ec53);
        value ^= value >> 33;
        if constexpr (sizeof(size_t) < sizeof(std::uint64_t))
            return static_cast<size_t>(value ^ (value >> 32));
        return static_cast<size_t>(value);
    };
    size_t h = static_cast<size_t>(v.kind) * kHashKindMix;
    switch (v.kind) {
        case Value::Kind::Temp:
            h ^= static_cast<size_t>(v.id) + kHashPhiMix;
            break;
        case Value::Kind::ConstInt:
            h ^= fold64(static_cast<std::uint64_t>(v.i64)) ^
                 (v.isBool ? kHashBoolFlag : 0);
            break;
        case Value::Kind::ConstFloat: {
            static_assert(sizeof(double) == sizeof(unsigned long long));
            unsigned long long bits{};
            std::memcpy(&bits, &v.f64, sizeof(double));
            h ^= fold64(bits);
            break;
        }
        case Value::Kind::ConstStr:
        case Value::Kind::GlobalAddr:
            h ^= std::hash<std::string>{}(v.str);
            break;
        case Value::Kind::NullPtr:
            h ^= kHashNullSentinel;
            break;
    }
    return h;
}

} // namespace il::core
