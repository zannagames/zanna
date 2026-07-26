//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file DebugExpr.hpp
/// @brief Defines the debug adapter's side-effect-free expression evaluator.
///
/// Conditional breakpoints and logpoint interpolation evaluate only against a supplied local-
/// variable snapshot. A resolver provides value/type strings, keeping the evaluator independent
/// of VM storage and types. The grammar supports scalar literals, identifiers, unary negation,
/// arithmetic, comparisons, Boolean operators, and parentheses with integer-to-float promotion.
///
/// Each Eval instance owns only its source, resolver, and cursor and retains no cross-call state.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace zanna::dbgexpr {

/// @brief A tagged evaluation result. Err propagates so callers can fail safe.
struct Value {
    /// @brief Runtime kind of a scalar evaluation result.
    enum class K { Int, Flt, Bool, Str, Err };
    K k = K::Err;
    int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;

    /// @brief Construct an integer result.
    /// @param v Signed integer value.
    /// @return Tagged integer result.
    static Value mkInt(int64_t v) { Value r; r.k = K::Int; r.i = v; return r; }

    /// @brief Construct a floating-point result.
    /// @param v Floating-point value.
    /// @return Tagged floating-point result.
    static Value mkFlt(double v) { Value r; r.k = K::Flt; r.f = v; return r; }

    /// @brief Construct a Boolean result.
    /// @param v Boolean value.
    /// @return Tagged Boolean result.
    static Value mkBool(bool v) { Value r; r.k = K::Bool; r.b = v; return r; }

    /// @brief Construct an owned string result.
    /// @param v String value to move into the result.
    /// @return Tagged string result.
    static Value mkStr(std::string v) { Value r; r.k = K::Str; r.s = std::move(v); return r; }

    /// @brief Construct the error sentinel.
    /// @return Default error-kind result.
    static Value err() { return Value{}; }

    /// @brief Test whether evaluation failed.
    /// @return @c true for the error sentinel.
    [[nodiscard]] bool isErr() const { return k == K::Err; }

    /// @brief Test whether this result participates in numeric arithmetic.
    /// @return @c true for integer or floating-point kinds.
    [[nodiscard]] bool isNum() const { return k == K::Int || k == K::Flt; }

    /// @brief Convert a numeric result to double precision.
    /// @return Stored float or integer promoted to @c double.
    [[nodiscard]] double num() const { return k == K::Flt ? f : static_cast<double>(i); }

    /// @brief Apply debugger-expression truthiness rules.
    /// @return Whether the stored scalar is nonzero, true, or nonempty.
    [[nodiscard]] bool truthy() const {
        switch (k) {
            case K::Int: return i != 0;
            case K::Flt: return f != 0.0;
            case K::Bool: return b;
            case K::Str: return !s.empty();
            default: return false;
        }
    }
    /// @brief Render for logpoint interpolation.
    /// @return Scalar text, or @c "<err>" for an error result.
    [[nodiscard]] std::string str() const {
        switch (k) {
            case K::Int: return std::to_string(i);
            case K::Flt: return std::to_string(f);
            case K::Bool: return b ? "true" : "false";
            case K::Str: return s;
            default: return "<err>";
        }
    }
};

/// @brief Resolve identifier @p name to its (value, type) strings from the stop's
///        locals. Returns false when the name is not in scope.
/// @param name Local identifier to resolve.
/// @param value Output display value.
/// @param type Output debugger type string.
/// @return @c true when the local exists and outputs were populated.
using Resolver = std::function<bool(const std::string &name, std::string &value, std::string &type)>;

/// @brief Recursive-descent evaluator over a single expression string.
class Eval {
  public:
    /// @brief Construct an evaluator for one expression and local snapshot.
    /// @param src Expression source to own.
    /// @param resolver Callback used to resolve local identifiers.
    Eval(std::string src, Resolver resolver) : s_(std::move(src)), r_(std::move(resolver)) {}

    /// @brief Evaluate the whole expression; trailing junk yields Err.
    /// @return Scalar result or the error sentinel.
    Value run() {
        pos_ = 0;
        Value v = parseOr();
        skip();
        if (pos_ != s_.size())
            return Value::err();
        return v;
    }

  private:
    std::string s_;
    Resolver r_;
    size_t pos_ = 0;

    /// @brief Advance past ASCII whitespace at the current cursor.
    void skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
            ++pos_;
    }
    /// @brief Inspect the current source byte.
    /// @return Current byte or NUL at end of input.
    [[nodiscard]] char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    /// @brief Inspect the byte after the current source position.
    /// @return Following byte or NUL when unavailable.
    [[nodiscard]] char peek2() const { return pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0'; }

    /// @brief Consume @p op (a 1- or 2-char operator) after skipping whitespace.
    /// @param op NUL-terminated one- or two-byte operator spelling.
    /// @return @c true when the exact non-prefix operator was consumed.
    bool eatOp(const char *op) {
        skip();
        const size_t n = op[1] ? 2 : 1;
        if (s_.compare(pos_, n, op) != 0)
            return false;
        // Disambiguate < from <=, ! from !=, = from ==, & from &&, | from ||.
        if (n == 1) {
            const char c = op[0];
            const char nx = peek2();
            if ((c == '<' || c == '>' || c == '!' || c == '=') && nx == '=')
                return false;
            if (c == '&' && nx == '&')
                return false;
            if (c == '|' && nx == '|')
                return false;
        }
        pos_ += n;
        return true;
    }

    /// @brief Test whether a byte may continue an identifier.
    /// @param c Byte to inspect.
    /// @return @c true for an alphanumeric byte or underscore.
    static bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    /// @brief Consume keyword @p kw on an identifier boundary (and/or/not/true/false).
    /// @param kw NUL-terminated keyword spelling.
    /// @return @c true when the keyword and following boundary were consumed.
    bool eatKeyword(const char *kw) {
        skip();
        size_t i = 0;
        while (kw[i] && pos_ + i < s_.size() && s_[pos_ + i] == kw[i])
            ++i;
        if (kw[i] != '\0')
            return false;
        if (pos_ + i < s_.size() && isIdent(s_[pos_ + i]))
            return false; // e.g. "android" must not match "and"
        pos_ += i;
        return true;
    }

    /// @brief Parse the lowest-precedence Boolean OR production.
    /// @return Combined Boolean result or propagated error-like truthiness.
    Value parseOr() {
        Value lhs = parseAnd();
        for (;;) {
            if (eatKeyword("or") || eatOp("||")) {
                Value rhs = parseAnd();
                lhs = Value::mkBool(lhs.truthy() || rhs.truthy());
            } else {
                return lhs;
            }
        }
    }

    /// @brief Parse the Boolean AND production.
    /// @return Combined Boolean result or propagated error-like truthiness.
    Value parseAnd() {
        Value lhs = parseCmp();
        for (;;) {
            if (eatKeyword("and") || eatOp("&&")) {
                Value rhs = parseCmp();
                lhs = Value::mkBool(lhs.truthy() && rhs.truthy());
            } else {
                return lhs;
            }
        }
    }

    /// @brief Parse an additive value followed by at most one comparison.
    /// @return Operand value or Boolean comparison result.
    Value parseCmp() {
        Value lhs = parseAdd();
        // At most one comparison (non-associative), matching common usage.
        const char *ops[] = {"==", "!=", "<=", ">=", "<", ">"};
        for (const char *op : ops) {
            if (eatOp(op)) {
                Value rhs = parseAdd();
                return compare(op, lhs, rhs);
            }
        }
        return lhs;
    }

    /// @brief Compare compatible numeric, string, or Boolean operands.
    /// @param op Supported comparison operator.
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return Boolean comparison result, or error for incompatible kinds.
    static Value compare(const std::string &op, const Value &a, const Value &b) {
        int cmp = 0; // -1,0,1
        if (a.isNum() && b.isNum()) {
            double x = a.num(), y = b.num();
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else if (a.k == Value::K::Str && b.k == Value::K::Str) {
            cmp = a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
        } else if (a.k == Value::K::Bool && b.k == Value::K::Bool) {
            cmp = (a.b ? 1 : 0) - (b.b ? 1 : 0);
        } else {
            return Value::err(); // incomparable types
        }
        if (op == "==") return Value::mkBool(cmp == 0);
        if (op == "!=") return Value::mkBool(cmp != 0);
        if (op == "<") return Value::mkBool(cmp < 0);
        if (op == ">") return Value::mkBool(cmp > 0);
        if (op == "<=") return Value::mkBool(cmp <= 0);
        return Value::mkBool(cmp >= 0); // ">="
    }

    /// @brief Parse left-associative addition and subtraction.
    /// @return Arithmetic or string-concatenation result.
    Value parseAdd() {
        Value lhs = parseMul();
        for (;;) {
            if (eatOp("+")) {
                Value rhs = parseMul();
                lhs = arith('+', lhs, rhs);
            } else if (eatOp("-")) {
                Value rhs = parseMul();
                lhs = arith('-', lhs, rhs);
            } else {
                return lhs;
            }
        }
    }

    /// @brief Parse left-associative multiplication, division, and remainder.
    /// @return Arithmetic result or error sentinel.
    Value parseMul() {
        Value lhs = parseUnary();
        for (;;) {
            if (eatOp("*")) {
                lhs = arith('*', lhs, parseUnary());
            } else if (eatOp("/")) {
                lhs = arith('/', lhs, parseUnary());
            } else if (eatOp("%")) {
                lhs = arith('%', lhs, parseUnary());
            } else {
                return lhs;
            }
        }
    }

    /// @brief Apply one arithmetic operator with integer-to-float promotion.
    /// @param op Addition, subtraction, multiplication, division, or remainder.
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return Numeric result, string concatenation, or error for invalid operands.
    static Value arith(char op, const Value &a, const Value &b) {
        // String concatenation with '+'.
        if (op == '+' && a.k == Value::K::Str && b.k == Value::K::Str)
            return Value::mkStr(a.s + b.s);
        if (!a.isNum() || !b.isNum())
            return Value::err();
        if (a.k == Value::K::Int && b.k == Value::K::Int) {
            int64_t x = a.i, y = b.i;
            switch (op) {
                case '+': return Value::mkInt(x + y);
                case '-': return Value::mkInt(x - y);
                case '*': return Value::mkInt(x * y);
                case '/': return y != 0 ? Value::mkInt(x / y) : Value::err();
                case '%': return y != 0 ? Value::mkInt(x % y) : Value::err();
            }
        }
        double x = a.num(), y = b.num();
        switch (op) {
            case '+': return Value::mkFlt(x + y);
            case '-': return Value::mkFlt(x - y);
            case '*': return Value::mkFlt(x * y);
            case '/': return y != 0.0 ? Value::mkFlt(x / y) : Value::err();
            default: return Value::err(); // % on floats unsupported
        }
    }

    /// @brief Parse recursive numeric negation or Boolean NOT.
    /// @return Unary result or the next primary expression.
    Value parseUnary() {
        if (eatOp("-")) {
            Value v = parseUnary();
            if (v.k == Value::K::Int) return Value::mkInt(-v.i);
            if (v.k == Value::K::Flt) return Value::mkFlt(-v.f);
            return Value::err();
        }
        if (eatKeyword("not") || eatOp("!")) {
            Value v = parseUnary();
            return Value::mkBool(!v.truthy());
        }
        return parsePrimary();
    }

    /// @brief Parse a parenthesized expression, literal, or identifier.
    /// @return Primary value or error when no valid production begins at the cursor.
    Value parsePrimary() {
        skip();
        const char c = peek();
        if (c == '(') {
            ++pos_;
            Value v = parseOr();
            skip();
            if (peek() == ')') { ++pos_; return v; }
            return Value::err();
        }
        if (c == '"')
            return parseString();
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek2()))))
            return parseNumber();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
            return parseIdentOrKeyword();
        return Value::err();
    }

    /// @brief Parse a double-quoted string with one-byte backslash escapes.
    /// @return Owned string value or error for an unterminated literal.
    Value parseString() {
        ++pos_; // opening quote
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char ch = s_[pos_++];
            if (ch == '\\' && pos_ < s_.size())
                ch = s_[pos_++];
            out.push_back(ch);
        }
        if (pos_ >= s_.size())
            return Value::err(); // unterminated
        ++pos_; // closing quote
        return Value::mkStr(std::move(out));
    }

    /// @brief Parse a decimal integer or floating-point literal.
    /// @return Numeric value or error when standard conversion rejects the token.
    Value parseNumber() {
        const size_t start = pos_;
        bool isFloat = false;
        while (pos_ < s_.size()) {
            const char ch = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                ++pos_;
            } else if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                // '+'/'-' only count as part of a number right after e/E.
                if ((ch == '+' || ch == '-') && !(pos_ > start && (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E')))
                    break;
                if (ch == '.' || ch == 'e' || ch == 'E')
                    isFloat = true;
                ++pos_;
            } else {
                break;
            }
        }
        const std::string tok = s_.substr(start, pos_ - start);
        try {
            if (isFloat)
                return Value::mkFlt(std::stod(tok));
            return Value::mkInt(static_cast<int64_t>(std::stoll(tok)));
        } catch (...) {
            return Value::err();
        }
    }

    /// @brief Parse a Boolean literal or resolve a local identifier.
    /// @return Boolean/local value or error when resolution fails.
    Value parseIdentOrKeyword() {
        const size_t start = pos_;
        while (pos_ < s_.size() && isIdent(s_[pos_]))
            ++pos_;
        const std::string id = s_.substr(start, pos_ - start);
        if (id == "true") return Value::mkBool(true);
        if (id == "false") return Value::mkBool(false);
        // Resolve from locals.
        std::string value, type;
        if (!r_ || !r_(id, value, type))
            return Value::err();
        return fromLocal(value, type);
    }

    /// @brief Build a Value from a local's (value, type) string pair.
    /// @param value Debugger display value.
    /// @param type Debugger type string.
    /// @return Parsed scalar, falling back to an owned string for unknown representations.
    static Value fromLocal(const std::string &value, const std::string &type) {
        if (type == "i1")
            return Value::mkBool(value == "1" || value == "true");
        if (!type.empty() && type[0] == 'i') {
            try { return Value::mkInt(static_cast<int64_t>(std::stoll(value))); }
            catch (...) { return Value::mkStr(value); }
        }
        if (!type.empty() && type[0] == 'f') {
            try { return Value::mkFlt(std::stod(value)); }
            catch (...) { return Value::mkStr(value); }
        }
        if (type == "str")
            return Value::mkStr(value);
        // Unknown type: best-effort int, then float, then string.
        try { size_t n; int64_t iv = std::stoll(value, &n); if (n == value.size()) return Value::mkInt(iv); }
        catch (...) {}
        try { size_t n; double dv = std::stod(value, &n); if (n == value.size()) return Value::mkFlt(dv); }
        catch (...) {}
        return Value::mkStr(value);
    }
};

/// @brief Evaluate @p expr against @p resolve; true unless it cleanly yields a
///        falsey value. Fail-safe: parse/type errors count as true so a malformed
///        condition still halts rather than silently skipping a breakpoint.
/// @param expr Conditional-breakpoint expression; empty means unconditional.
/// @param resolve Local-variable resolver for the current stop.
/// @return Evaluated truthiness, or @c true on parse/type error.
inline bool conditionHolds(const std::string &expr, const Resolver &resolve) {
    if (expr.empty())
        return true;
    Value v = Eval(expr, resolve).run();
    if (v.isErr())
        return true;
    return v.truthy();
}

/// @brief Interpolate `{expr}` segments of a logpoint message; non-brace text is
///        copied verbatim, an errored segment renders as "<err>". `{{`/`}}` escape.
/// @param msg Logpoint template.
/// @param resolve Local-variable resolver for the current stop.
/// @return Interpolated output with escaped braces and error placeholders.
inline std::string interpolate(const std::string &msg, const Resolver &resolve) {
    std::string out;
    for (size_t i = 0; i < msg.size();) {
        const char c = msg[i];
        if (c == '{' && i + 1 < msg.size() && msg[i + 1] == '{') { out.push_back('{'); i += 2; continue; }
        if (c == '}' && i + 1 < msg.size() && msg[i + 1] == '}') { out.push_back('}'); i += 2; continue; }
        if (c == '{') {
            const size_t end = msg.find('}', i + 1);
            if (end == std::string::npos) { out.append(msg, i, std::string::npos); break; }
            const std::string expr = msg.substr(i + 1, end - i - 1);
            Value v = Eval(expr, resolve).run();
            out += v.isErr() ? std::string("<err>") : v.str();
            i = end + 1;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

} // namespace zanna::dbgexpr
